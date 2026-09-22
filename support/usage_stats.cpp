#include "usage_stats.h"

#include "config.h"
#include "kiwi.h"
#include "rx.h"
#include "rx_util.h"
#include "ext_int.h"
#include "mode.h"
#include "peri.h"
#include "cfg.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

namespace {

const int USAGE_SCHEMA = 1;
const int HLL_REGS = 256;
const int FREQ_CAP = 128;
const int EXT_CAP = 32;
const int GEO_CAP = 32;
const int DETAIL_CAP = 256;
const int CALLSIGN_LEN = 64;
const int GEO_LEN = 96;
const int EXT_NAME_LEN = 32;
const int HASH_LEN = 32;
const int HOURLY_RETENTION_DAYS = 8;

struct hll_t {
    u1_t reg[HLL_REGS];
};

struct frequency_t {
    bool used;
    int bucket_kHz;
    int mode;
    u4_t entries;
    u64_t seconds;
    u64_t error;
};

struct extension_t {
    bool used;
    char name[EXT_NAME_LEN];
    u4_t starts;
    u64_t seconds;
};

struct geography_t {
    bool used;
    char label[GEO_LEN];
    u4_t sessions;
    u64_t seconds;
};

struct session_detail_t {
    u1_t visitor_hash[HASH_LEN];
    char callsign[CALLSIGN_LEN];
    char geo[GEO_LEN];
    char client[16];
    s64_t start_utc;
    s64_t end_utc;
    int start_freq_kHz;
    int end_freq_kHz;
    int start_mode;
    int end_mode;
    char close_reason[24];
};

struct hour_t {
    s64_t hour_start;
    u4_t session_starts;
    u4_t peak_concurrent;
    u4_t detail_dropped;
    u4_t frequency_overflow;
    u4_t geography_overflow;
    u64_t listener_seconds;
    u64_t concurrency_samples;
    u64_t concurrency_sum;
    hll_t unique;
    frequency_t freq[FREQ_CAP];
    extension_t ext[EXT_CAP];
    geography_t geo[GEO_CAP];
    session_detail_t detail[DETAIL_CAP];
    int detail_count;
};

struct active_t {
    bool active;
    conn_t* conn;
    u64_t conn_tstamp;
    u1_t visitor_hash[HASH_LEN];
    char callsign[CALLSIGN_LEN];
    char geo[GEO_LEN];
    char client[16];
    s64_t start_utc;
    int start_freq_kHz;
    int start_mode;
    int last_freq_kHz;
    int last_mode;
    char last_ext[EXT_NAME_LEN];
};

struct persisted_hour_t {
    bool valid;
    s64_t hour_start;
    u64_t listener_seconds;
    u64_t concurrency_sum;
    u64_t concurrency_samples;
    u4_t sessions;
    u4_t unique;
    u4_t peak;
    u4_t detail_dropped;
};

static hour_t current_hour;
static active_t active[MAX_RX_CHANS];
static u1_t identity_key[HASH_LEN];
static bool identity_key_valid;
static bool enabled = true;
static bool initialized;
static char last_error[160];

static bool valid_date(const char* date);
static bool valid_month(const char* month);
static bool remove_tree_contents(const std::string& path);

static const char* usage_root()
{
#ifdef NATIVE_HARNESS
    return DIR_CFG "/usage";
#else
    return "/media/mmcblk0p1/config/usage";
#endif
}

static void set_error(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_error, sizeof(last_error), fmt, ap);
    va_end(ap);
    lprintf("USAGE: %s\n", last_error);
}

static bool mkdir_one(const char* path, mode_t mode)
{
    if (mkdir(path, mode) == 0 || errno == EEXIST) return true;
    set_error("mkdir %s failed: %s", path, strerror(errno));
    return false;
}

static bool mkdir_tree(const std::string& path)
{
    if (path.empty()) return false;
    std::string cur;
    if (path[0] == '/') cur = "/";
    size_t pos = 1;
    while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!part.empty()) {
            if (cur.size() > 1) cur += "/";
            cur += part;
            if (!mkdir_one(cur.c_str(), 0700)) return false;
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return true;
}

class sd_write_guard {
public:
    sd_write_guard() : active_(true) { sd_enable(true); }
    ~sd_write_guard() { if (active_) sd_enable(false); }
    void close() { if (active_) { sd_enable(false); active_ = false; } }
private:
    bool active_;
};

static std::string path_join(const char* a, const char* b)
{
    return std::string(a) + "/" + b;
}

static bool read_exact(const char* path, void* buf, size_t len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, (char*)buf + off, len - off);
        if (n <= 0) {
            close(fd);
            return false;
        }
        off += (size_t)n;
    }
    char extra;
    bool exact = read(fd, &extra, 1) == 0;
    close(fd);
    return exact;
}

static bool write_atomic_bytes(const std::string& path, const void* data, size_t len, mode_t mode)
{
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        set_error("open %s failed: %s", tmp.c_str(), strerror(errno));
        return false;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, (const char*)data + off, len - off);
        if (n <= 0) {
            set_error("write %s failed: %s", tmp.c_str(), strerror(errno));
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        off += (size_t)n;
    }
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (ok && rename(tmp.c_str(), path.c_str()) != 0) ok = false;
    if (!ok) {
        set_error("commit %s failed: %s", path.c_str(), strerror(errno));
        unlink(tmp.c_str());
    }
    return ok;
}

typedef bool (*json_writer_t)(FILE*, void*);

static bool write_atomic_json(const std::string& path, json_writer_t writer, void* param)
{
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    FILE* fp = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!fp) {
        set_error("fopen %s failed: %s", tmp.c_str(), strerror(errno));
        if (fd >= 0) close(fd);
        return false;
    }
    bool ok = writer(fp, param);
    if (ok && fflush(fp) != 0) ok = false;
    if (ok && fsync(fileno(fp)) != 0) ok = false;
    if (fclose(fp) != 0) ok = false;
    if (ok && rename(tmp.c_str(), path.c_str()) != 0) ok = false;
    if (!ok) {
        set_error("write %s failed: %s", path.c_str(), strerror(errno));
        unlink(tmp.c_str());
    }
    return ok;
}

static void write_json_string(FILE* fp, const char* s)
{
    fputc('"', fp);
    if (s) {
        for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
            switch (*p) {
            case '"': fputs("\\\"", fp); break;
            case '\\': fputs("\\\\", fp); break;
            case '\b': fputs("\\b", fp); break;
            case '\f': fputs("\\f", fp); break;
            case '\n': fputs("\\n", fp); break;
            case '\r': fputs("\\r", fp); break;
            case '\t': fputs("\\t", fp); break;
            default:
                if (*p < 0x20) fprintf(fp, "\\u%04x", *p);
                else fputc(*p, fp);
                break;
            }
        }
    }
    fputc('"', fp);
}

static void format_hour(s64_t epoch, char* date, size_t date_len, char* hour, size_t hour_len)
{
    time_t t = (time_t)epoch;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(date, date_len, "%Y-%m-%d", &tm);
    strftime(hour, hour_len, "%H", &tm);
}

static s64_t hour_floor(s64_t now)
{
    return now - (now % 3600);
}

static void hash_hex(const u1_t* hash, char* out, int bytes)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < bytes; i++) {
        out[i * 2] = hex[hash[i] >> 4];
        out[i * 2 + 1] = hex[hash[i] & 0xf];
    }
    out[bytes * 2] = '\0';
}

static bool normalize_ip(const char* ip, u1_t* out, int* out_len)
{
    if (inet_pton(AF_INET, ip, out) == 1) {
        *out_len = 4;
        return true;
    }
    if (inet_pton(AF_INET6, ip, out) == 1) {
        *out_len = 16;
        return true;
    }
    return false;
}

static void visitor_hash(const char* ip, u1_t* out)
{
    u1_t normalized[16];
    int len;
    const void* data = ip;
    if (normalize_ip(ip, normalized, &len)) data = normalized;
    else len = strlen(ip);
    unsigned out_len = 0;
    if (!HMAC(EVP_sha256(), identity_key, sizeof(identity_key),
            (const unsigned char*)data, len, out, &out_len) || out_len != HASH_LEN) {
        memset(out, 0, HASH_LEN);
    }
}

static int hll_rank(u64_t value)
{
    if (value == 0) return 57;
    return __builtin_clzll(value) + 1;
}

static void hll_add(hll_t* hll, const u1_t* hash)
{
    int idx = hash[0];
    u64_t value = 0;
    memcpy(&value, hash + 1, sizeof(value));
    int rank = hll_rank(value);
    if (rank > 63) rank = 63;
    if (hll->reg[idx] < rank) hll->reg[idx] = rank;
}

static void hll_merge(hll_t* dst, const hll_t* src)
{
    for (int i = 0; i < HLL_REGS; i++) {
        if (dst->reg[i] < src->reg[i]) dst->reg[i] = src->reg[i];
    }
}

static u4_t hll_estimate(const hll_t* hll)
{
    double sum = 0;
    int zeros = 0;
    for (int i = 0; i < HLL_REGS; i++) {
        sum += 1.0 / (double)(1ULL << MIN(hll->reg[i], 62));
        if (hll->reg[i] == 0) zeros++;
    }
    const double alpha = 0.7213 / (1.0 + 1.079 / HLL_REGS);
    double estimate = alpha * HLL_REGS * HLL_REGS / sum;
    if (estimate <= 2.5 * HLL_REGS && zeros) {
        estimate = HLL_REGS * log((double)HLL_REGS / zeros);
    }
    return (u4_t)(estimate + 0.5);
}

static void hll_hex(const hll_t* hll, char* out)
{
    hash_hex(hll->reg, out, HLL_REGS);
}

static bool hll_from_hex(const char* s, hll_t* hll)
{
    if (!s || strlen(s) != HLL_REGS * 2) return false;
    for (int i = 0; i < HLL_REGS; i++) {
        unsigned v;
        if (sscanf(&s[i * 2], "%2x", &v) != 1) return false;
        hll->reg[i] = (u1_t)v;
    }
    return true;
}

static void frequency_add(hour_t* hour, int bucket_kHz, int mode, bool entry)
{
    frequency_t* free_slot = NULL;
    frequency_t* min_slot = NULL;
    for (int i = 0; i < FREQ_CAP; i++) {
        frequency_t* f = &hour->freq[i];
        if (f->used && f->bucket_kHz == bucket_kHz && f->mode == mode) {
            f->seconds++;
            if (entry) f->entries++;
            return;
        }
        if (!f->used && !free_slot) free_slot = f;
        if (f->used && (!min_slot || f->seconds < min_slot->seconds)) min_slot = f;
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->used = true;
        free_slot->bucket_kHz = bucket_kHz;
        free_slot->mode = mode;
        free_slot->seconds = 1;
        free_slot->entries = entry ? 1 : 0;
        return;
    }
    hour->frequency_overflow++;
    if (min_slot) {
        min_slot->error = min_slot->seconds;
        min_slot->bucket_kHz = bucket_kHz;
        min_slot->mode = mode;
        min_slot->seconds++;
        min_slot->entries = entry ? 1 : 0;
    }
}

static void extension_add(hour_t* hour, const char* name, bool started)
{
    if (!name || !*name) return;
    extension_t* free_slot = NULL;
    for (int i = 0; i < EXT_CAP; i++) {
        extension_t* ext = &hour->ext[i];
        if (ext->used && strcmp(ext->name, name) == 0) {
            ext->seconds++;
            if (started) ext->starts++;
            return;
        }
        if (!ext->used && !free_slot) free_slot = ext;
    }
    if (free_slot) {
        free_slot->used = true;
        kiwi_strncpy(free_slot->name, name, sizeof(free_slot->name));
        free_slot->seconds = 1;
        free_slot->starts = started ? 1 : 0;
    }
}

static void geography_add(hour_t* hour, const char* label, bool new_session, bool add_second)
{
    if (!label || !*label) label = "Unknown";
    geography_t* free_slot = NULL;
    geography_t* min_slot = NULL;
    for (int i = 0; i < GEO_CAP; i++) {
        geography_t* geo = &hour->geo[i];
        if (geo->used && strcmp(geo->label, label) == 0) {
            if (add_second) geo->seconds++;
            if (new_session) geo->sessions++;
            return;
        }
        if (!geo->used && !free_slot) free_slot = geo;
        if (geo->used && (!min_slot || geo->seconds < min_slot->seconds)) min_slot = geo;
    }
    if (free_slot) {
        free_slot->used = true;
        kiwi_strncpy(free_slot->label, label, sizeof(free_slot->label));
        free_slot->seconds = add_second ? 1 : 0;
        free_slot->sessions = new_session ? 1 : 0;
        return;
    }
    hour->geography_overflow++;
    if (min_slot) {
        kiwi_strncpy(min_slot->label, label, sizeof(min_slot->label));
        if (add_second) min_slot->seconds++;
        min_slot->sessions = new_session ? 1 : 0;
    }
}

static const char* client_type(conn_t* conn)
{
    if (conn->ext_api) return "external_api";
    return "browser";
}

static bool trackable(conn_t* conn)
{
    if (!conn || !conn->valid || !conn->arrived || !conn->isMaster) return false;
    if (conn->internal_connection || conn->rx_channel < 0 || conn->rx_channel >= rx_chans) return false;
    return conn->type == STREAM_SOUND || conn->type == STREAM_WATERFALL;
}

static void start_session(int ch, conn_t* conn, s64_t now)
{
    active_t* a = &active[ch];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->conn = conn;
    a->conn_tstamp = conn->tstamp;
    visitor_hash(conn->remote_ip, a->visitor_hash);
    kiwi_strncpy(a->callsign, conn->isUserIP ? "" : (conn->ident_user ? conn->ident_user : ""), sizeof(a->callsign));
    kiwi_strncpy(a->geo, conn->geo ? conn->geo : "Unknown", sizeof(a->geo));
    kiwi_strncpy(a->client, client_type(conn), sizeof(a->client));
    a->start_utc = now;
    a->start_freq_kHz = conn->freqHz / 1000;
    a->start_mode = conn->mode;
    a->last_freq_kHz = a->start_freq_kHz;
    a->last_mode = a->start_mode;
    if (ext_users[ch].ext)
        kiwi_strncpy(a->last_ext, ext_users[ch].ext->name, sizeof(a->last_ext));

    current_hour.session_starts++;
    hll_add(&current_hour.unique, a->visitor_hash);
    geography_add(&current_hour, a->geo, true, false);
}

static void finish_session(active_t* a, s64_t now, const char* reason)
{
    if (!a->active) return;
    if (current_hour.detail_count < DETAIL_CAP) {
        session_detail_t* d = &current_hour.detail[current_hour.detail_count++];
        memset(d, 0, sizeof(*d));
        memcpy(d->visitor_hash, a->visitor_hash, sizeof(d->visitor_hash));
        kiwi_strncpy(d->callsign, a->callsign, sizeof(d->callsign));
        kiwi_strncpy(d->geo, a->geo, sizeof(d->geo));
        kiwi_strncpy(d->client, a->client, sizeof(d->client));
        d->start_utc = a->start_utc;
        d->end_utc = now;
        d->start_freq_kHz = a->start_freq_kHz;
        d->end_freq_kHz = a->last_freq_kHz;
        d->start_mode = a->start_mode;
        d->end_mode = a->last_mode;
        kiwi_strncpy(d->close_reason, reason ? reason : "closed", sizeof(d->close_reason));
    } else {
        current_hour.detail_dropped++;
    }
    memset(a, 0, sizeof(*a));
}

static bool write_hour_json(FILE* fp, void* param)
{
    hour_t* hour = (hour_t*)param;
    char hll[HLL_REGS * 2 + 1];
    hll_hex(&hour->unique, hll);
    fprintf(fp, "{\"schema\":%d,\"complete\":true,\"hour_start\":%lld,"
                "\"listener_seconds\":%llu,\"sessions\":%u,\"unique_estimate\":%u,"
                "\"peak_concurrent\":%u,\"concurrency_sum\":%llu,\"concurrency_samples\":%llu,"
                "\"detail_dropped\":%u,\"frequency_overflow\":%u,\"geography_overflow\":%u,"
                "\"hll\":\"%s\",\"frequencies\":[",
            USAGE_SCHEMA, hour->hour_start, hour->listener_seconds, hour->session_starts,
            hll_estimate(&hour->unique), hour->peak_concurrent, hour->concurrency_sum,
            hour->concurrency_samples, hour->detail_dropped, hour->frequency_overflow,
            hour->geography_overflow, hll);
    bool comma = false;
    for (int i = 0; i < FREQ_CAP; i++) {
        frequency_t* f = &hour->freq[i];
        if (!f->used) continue;
        fprintf(fp, "%s{\"khz\":%d,\"mode\":", comma ? "," : "", f->bucket_kHz);
        write_json_string(fp, rx_enum2mode(f->mode));
        fprintf(fp, ",\"seconds\":%llu,\"entries\":%u,\"error\":%llu}",
                f->seconds, f->entries, f->error);
        comma = true;
    }
    fputs("],\"extensions\":[", fp);
    comma = false;
    for (int i = 0; i < EXT_CAP; i++) {
        extension_t* ext = &hour->ext[i];
        if (!ext->used) continue;
        fprintf(fp, "%s{\"name\":", comma ? "," : "");
        write_json_string(fp, ext->name);
        fprintf(fp, ",\"seconds\":%llu,\"starts\":%u}", ext->seconds, ext->starts);
        comma = true;
    }
    fputs("],\"geography\":[", fp);
    comma = false;
    for (int i = 0; i < GEO_CAP; i++) {
        geography_t* geo = &hour->geo[i];
        if (!geo->used) continue;
        fprintf(fp, "%s{\"label\":", comma ? "," : "");
        write_json_string(fp, geo->label);
        fprintf(fp, ",\"seconds\":%llu,\"sessions\":%u}", geo->seconds, geo->sessions);
        comma = true;
    }
    fputs("]}\n", fp);
    return ferror(fp) == 0;
}

static bool write_sessions_json(FILE* fp, void* param)
{
    hour_t* hour = (hour_t*)param;
    fprintf(fp, "{\"schema\":%d,\"hour_start\":%lld,\"detail_dropped\":%u,\"sessions\":[",
            USAGE_SCHEMA, hour->hour_start, hour->detail_dropped);
    for (int i = 0; i < hour->detail_count; i++) {
        session_detail_t* d = &hour->detail[i];
        char hash[HASH_LEN * 2 + 1];
        hash_hex(d->visitor_hash, hash, HASH_LEN);
        fprintf(fp, "%s{\"visitor_hash\":\"%s\",\"callsign\":", i ? "," : "", hash);
        write_json_string(fp, d->callsign);
        fputs(",\"geo\":", fp);
        write_json_string(fp, d->geo);
        fputs(",\"client\":", fp);
        write_json_string(fp, d->client);
        fprintf(fp, ",\"start\":%lld,\"end\":%lld,\"duration\":%lld,"
                    "\"start_khz\":%d,\"end_khz\":%d,\"start_mode\":",
                d->start_utc, d->end_utc, MAX(0LL, d->end_utc - d->start_utc),
                d->start_freq_kHz, d->end_freq_kHz);
        write_json_string(fp, rx_enum2mode(d->start_mode));
        fputs(",\"end_mode\":", fp);
        write_json_string(fp, rx_enum2mode(d->end_mode));
        fputs(",\"close_reason\":", fp);
        write_json_string(fp, d->close_reason);
        fputc('}', fp);
    }
    fputs("]}\n", fp);
    return ferror(fp) == 0;
}

static bool extract_u64(const char* json, const char* key, u64_t* value)
{
    std::string needle = std::string("\"") + key + "\":";
    const char* p = strstr(json, needle.c_str());
    if (!p) return false;
    p += needle.size();
    unsigned long long v;
    if (sscanf(p, "%llu", &v) != 1) return false;
    *value = v;
    return true;
}

static bool extract_string(const char* json, const char* key, char* out, size_t out_len)
{
    std::string needle = std::string("\"") + key + "\":\"";
    const char* p = strstr(json, needle.c_str());
    if (!p) return false;
    p += needle.size();
    size_t len = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            if (!c) return false;
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
        }
        if (len + 1 < out_len) out[len++] = c;
    }
    if (*p != '"') return false;
    out[len] = '\0';
    return true;
}

static char* read_file_alloc(const std::string& path)
{
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 1024 * 1024) {
        close(fd);
        return NULL;
    }
    char* buf = (char*)malloc(st.st_size + 1);
    if (!buf) {
        close(fd);
        return NULL;
    }
    ssize_t n = read(fd, buf, st.st_size);
    close(fd);
    if (n != st.st_size) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

static bool load_hour(s64_t epoch, persisted_hour_t* out, hll_t* hll = NULL)
{
    memset(out, 0, sizeof(*out));
    if (hll) memset(hll, 0, sizeof(*hll));
    char date[16], hour[4];
    format_hour(epoch, date, sizeof(date), hour, sizeof(hour));
    std::string path = std::string(usage_root()) + "/hourly/" + date + "/" + hour + ".json";
    char* json = read_file_alloc(path);
    if (!json) return false;
    u64_t v = 0;
    bool ok = extract_u64(json, "hour_start", &v);
    out->hour_start = v;
    ok &= extract_u64(json, "listener_seconds", &out->listener_seconds);
    v = 0;
    ok &= extract_u64(json, "sessions", &v); out->sessions = v;
    v = 0;
    ok &= extract_u64(json, "unique_estimate", &v); out->unique = v;
    v = 0;
    ok &= extract_u64(json, "peak_concurrent", &v); out->peak = v;
    extract_u64(json, "concurrency_sum", &out->concurrency_sum);
    extract_u64(json, "concurrency_samples", &out->concurrency_samples);
    v = 0;
    extract_u64(json, "detail_dropped", &v); out->detail_dropped = v;
    if (hll) {
        char hex[HLL_REGS * 2 + 1];
        ok &= extract_string(json, "hll", hex, sizeof(hex)) && hll_from_hex(hex, hll);
    }
    out->valid = ok;
    free(json);
    return ok;
}

static bool load_rollup_file(const std::string& path, persisted_hour_t* out, hll_t* hll)
{
    memset(out, 0, sizeof(*out));
    if (hll) memset(hll, 0, sizeof(*hll));
    char* json = read_file_alloc(path);
    if (!json) return false;
    u64_t v = 0;
    bool ok = extract_u64(json, "start", &v);
    out->hour_start = v;
    ok &= extract_u64(json, "listener_seconds", &out->listener_seconds);
    v = 0;
    ok &= extract_u64(json, "sessions", &v); out->sessions = v;
    v = 0;
    ok &= extract_u64(json, "unique_estimate", &v); out->unique = v;
    v = 0;
    ok &= extract_u64(json, "peak_concurrent", &v); out->peak = v;
    extract_u64(json, "concurrency_sum", &out->concurrency_sum);
    extract_u64(json, "concurrency_samples", &out->concurrency_samples);
    v = 0;
    extract_u64(json, "detail_dropped", &v); out->detail_dropped = v;
    char hex[HLL_REGS * 2 + 1];
    ok &= extract_string(json, "hll", hex, sizeof(hex)) && hll_from_hex(hex, hll);
    out->valid = ok;
    free(json);
    return ok;
}

struct rollup_param_t {
    s64_t start;
    int count;
    const char* period_name;
};

static bool write_rollup_json(FILE* fp, void* param)
{
    rollup_param_t* rp = (rollup_param_t*)param;
    u64_t listener = 0, concurrency_sum = 0, concurrency_samples = 0;
    u4_t sessions = 0, peak = 0, dropped = 0;
    hll_t hll;
    memset(&hll, 0, sizeof(hll));
    int present = 0;
    for (int i = 0; i < rp->count; i++) {
        persisted_hour_t hour;
        hll_t hh;
        if (!load_hour(rp->start + (s64_t)i * 3600, &hour, &hh)) continue;
        present++;
        listener += hour.listener_seconds;
        sessions += hour.sessions;
        peak = MAX(peak, hour.peak);
        dropped += hour.detail_dropped;
        concurrency_sum += hour.concurrency_sum;
        concurrency_samples += hour.concurrency_samples;
        hll_merge(&hll, &hh);
    }
    char hex[HLL_REGS * 2 + 1];
    hll_hex(&hll, hex);
    fprintf(fp, "{\"schema\":%d,\"period\":\"%s\",\"start\":%lld,\"source_hours\":%d,"
                "\"listener_seconds\":%llu,\"sessions\":%u,\"unique_estimate\":%u,"
                "\"peak_concurrent\":%u,\"concurrency_sum\":%llu,\"concurrency_samples\":%llu,"
                "\"detail_dropped\":%u,\"hll\":\"%s\"}\n",
            USAGE_SCHEMA, rp->period_name, rp->start, present, listener, sessions,
            hll_estimate(&hll), peak, concurrency_sum, concurrency_samples, dropped, hex);
    return ferror(fp) == 0;
}

static void write_day_rollup(s64_t completed_hour)
{
    time_t t = completed_hour;
    struct tm tm;
    gmtime_r(&t, &tm);
    if (tm.tm_hour != 23) return;
    s64_t day_start = completed_hour - 23 * 3600;
    char date[16], ignored[4];
    format_hour(day_start, date, sizeof(date), ignored, sizeof(ignored));
    std::string dir = path_join(usage_root(), "daily");
    if (!mkdir_tree(dir)) return;
    rollup_param_t rp = { day_start, 24, "day" };
    write_atomic_json(dir + "/" + date + ".json", write_rollup_json, &rp);
}

struct month_rollup_param_t {
    s64_t month_start;
    int days;
};

static bool write_month_rollup_json(FILE* fp, void* param)
{
    month_rollup_param_t* rp = (month_rollup_param_t*)param;
    u64_t listener = 0, concurrency_sum = 0, concurrency_samples = 0;
    u4_t sessions = 0, peak = 0, dropped = 0;
    hll_t hll;
    memset(&hll, 0, sizeof(hll));
    int present = 0;
    for (int i = 0; i < rp->days; i++) {
        s64_t epoch = rp->month_start + (s64_t)i * 86400;
        char date[16], ignored[4];
        format_hour(epoch, date, sizeof(date), ignored, sizeof(ignored));
        std::string path = std::string(usage_root()) + "/daily/" + date + ".json";
        persisted_hour_t day;
        hll_t dh;
        if (!load_rollup_file(path, &day, &dh)) continue;
        present++;
        listener += day.listener_seconds;
        sessions += day.sessions;
        peak = MAX(peak, day.peak);
        dropped += day.detail_dropped;
        concurrency_sum += day.concurrency_sum;
        concurrency_samples += day.concurrency_samples;
        hll_merge(&hll, &dh);
    }
    char hex[HLL_REGS * 2 + 1];
    hll_hex(&hll, hex);
    fprintf(fp, "{\"schema\":%d,\"period\":\"month\",\"start\":%lld,\"source_days\":%d,"
                "\"listener_seconds\":%llu,\"sessions\":%u,\"unique_estimate\":%u,"
                "\"peak_concurrent\":%u,\"concurrency_sum\":%llu,\"concurrency_samples\":%llu,"
                "\"detail_dropped\":%u,\"hll\":\"%s\"}\n",
            USAGE_SCHEMA, rp->month_start, present, listener, sessions, hll_estimate(&hll),
            peak, concurrency_sum, concurrency_samples, dropped, hex);
    return ferror(fp) == 0;
}

static void write_month_rollup(s64_t completed_hour)
{
    time_t current_t = completed_hour;
    time_t next_t = completed_hour + 3600;
    struct tm current_tm, next_tm;
    gmtime_r(&current_t, &current_tm);
    gmtime_r(&next_t, &next_tm);
    if (current_tm.tm_mon == next_tm.tm_mon) return;

    struct tm start_tm = current_tm;
    start_tm.tm_mday = 1;
    start_tm.tm_hour = start_tm.tm_min = start_tm.tm_sec = 0;
    s64_t month_start = timegm(&start_tm);
    int days = current_tm.tm_mday;
    char month[16];
    strftime(month, sizeof(month), "%Y-%m", &current_tm);
    std::string dir = path_join(usage_root(), "monthly");
    if (!mkdir_tree(dir)) return;
    month_rollup_param_t rp = { month_start, days };
    write_atomic_json(dir + "/" + month + ".json", write_month_rollup_json, &rp);
}

static bool date_name_epoch(const char* name, const char* suffix, s64_t* epoch)
{
    size_t suffix_len = suffix ? strlen(suffix) : 0;
    size_t len = strlen(name);
    if (suffix_len && (len <= suffix_len || strcmp(name + len - suffix_len, suffix) != 0))
        return false;
    size_t date_len = len - suffix_len;
    if (date_len != 10) return false;
    char date[11];
    memcpy(date, name, 10);
    date[10] = '\0';
    if (!valid_date(date)) return false;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    int year, month, day;
    sscanf(date, "%d-%d-%d", &year, &month, &day);
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    *epoch = timegm(&tm);
    return true;
}

static void cleanup_dated_directories(const char* category, s64_t cutoff)
{
    std::string base = path_join(usage_root(), category);
    DIR* dir = opendir(base.c_str());
    if (!dir) return;
    struct dirent* de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        s64_t epoch;
        if (!date_name_epoch(de->d_name, NULL, &epoch) || epoch >= cutoff) continue;
        std::string child = base + "/" + de->d_name;
        if (remove_tree_contents(child)) rmdir(child.c_str());
    }
    closedir(dir);
}

static void cleanup_daily_files(s64_t cutoff)
{
    std::string base = path_join(usage_root(), "daily");
    DIR* dir = opendir(base.c_str());
    if (!dir) return;
    struct dirent* de;
    while ((de = readdir(dir)) != NULL) {
        s64_t epoch;
        if (!date_name_epoch(de->d_name, ".json", &epoch) || epoch >= cutoff) continue;
        unlink((base + "/" + de->d_name).c_str());
    }
    closedir(dir);
}

static void cleanup_monthly_files(const struct tm* cutoff_tm)
{
    std::string base = path_join(usage_root(), "monthly");
    DIR* dir = opendir(base.c_str());
    if (!dir) return;
    int cutoff_value = (cutoff_tm->tm_year + 1900) * 12 + cutoff_tm->tm_mon;
    struct dirent* de;
    while ((de = readdir(dir)) != NULL) {
        int year, month;
        char tail;
        if (sscanf(de->d_name, "%d-%d.json%c", &year, &month, &tail) != 2) continue;
        int value = year * 12 + month - 1;
        if (value < cutoff_value) unlink((base + "/" + de->d_name).c_str());
    }
    closedir(dir);
}

static void cleanup_retention(s64_t now)
{
    s64_t day_start = now - (now % 86400);
    cleanup_dated_directories("hourly", day_start - (HOURLY_RETENTION_DAYS - 1) * 86400LL);
    cleanup_dated_directories("sessions", day_start - 30 * 86400LL);

    time_t t = now;
    struct tm cutoff_tm;
    gmtime_r(&t, &cutoff_tm);
    cutoff_tm.tm_mday = 1;
    cutoff_tm.tm_hour = cutoff_tm.tm_min = cutoff_tm.tm_sec = 0;
    cutoff_tm.tm_mon -= 24;
    s64_t cutoff = timegm(&cutoff_tm);
    time_t cutoff_t = (time_t)cutoff;
    gmtime_r(&cutoff_t, &cutoff_tm);
    cleanup_daily_files(cutoff);
    cleanup_monthly_files(&cutoff_tm);
}

static bool write_state_json(FILE* fp, void* param)
{
    hour_t* hour = (hour_t*)param;
    fprintf(fp, "{\"schema\":%d,\"last_completed_hour\":%lld,\"last_write_ok\":true,"
                "\"memory_bytes\":%zu}\n",
            USAGE_SCHEMA, hour->hour_start, sizeof(current_hour) + sizeof(active));
    return ferror(fp) == 0;
}

static void persist_hour(hour_t* hour)
{
    char date[16], hh[4];
    format_hour(hour->hour_start, date, sizeof(date), hh, sizeof(hh));
    sd_write_guard guard;
    std::string hourly_dir = std::string(usage_root()) + "/hourly/" + date;
    std::string session_dir = std::string(usage_root()) + "/sessions/" + date;
    if (!mkdir_tree(hourly_dir) || !mkdir_tree(session_dir)) return;
    bool ok = write_atomic_json(hourly_dir + "/" + hh + ".json", write_hour_json, hour);
    if (ok) ok = write_atomic_json(session_dir + "/" + hh + ".json", write_sessions_json, hour);
    if (ok) write_day_rollup(hour->hour_start);
    if (ok) write_month_rollup(hour->hour_start);
    if (ok) cleanup_retention(hour->hour_start + 3600);
    if (ok) write_atomic_json(path_join(usage_root(), "state.json"), write_state_json, hour);
}

static void rollover(s64_t new_hour)
{
    if (!current_hour.hour_start) {
        memset(&current_hour, 0, sizeof(current_hour));
        current_hour.hour_start = new_hour;
        return;
    }
    persist_hour(&current_hour);
    memset(&current_hour, 0, sizeof(current_hour));
    current_hour.hour_start = new_hour;
}

static void load_or_create_key()
{
    std::string root = usage_root();
    std::string path = root + "/.identity-key";
    if (read_exact(path.c_str(), identity_key, sizeof(identity_key))) {
        identity_key_valid = true;
        return;
    }
    if (RAND_bytes(identity_key, sizeof(identity_key)) != 1) {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0 || read(fd, identity_key, sizeof(identity_key)) != sizeof(identity_key)) {
            if (fd >= 0) close(fd);
            set_error("unable to generate identity key");
            return;
        }
        close(fd);
    }
    sd_write_guard guard;
    if (!mkdir_tree(root)) return;
    if (!write_atomic_bytes(path, identity_key, sizeof(identity_key), 0600)) return;
    identity_key_valid = true;
}

static std::string json_escape(const char* s)
{
    std::string out;
    if (!s) return out;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        switch (*p) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (*p >= 0x20) out += (char)*p;
            break;
        }
    }
    return out;
}

static bool valid_date(const char* date)
{
    if (!date || strlen(date) != 10) return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (date[i] != '-') return false;
        } else if (date[i] < '0' || date[i] > '9') {
            return false;
        }
    }
    int year, month, day;
    if (sscanf(date, "%d-%d-%d", &year, &month, &day) != 3) return false;
    if (year < 2020 || month < 1 || month > 12 || day < 1 || day > 31) return false;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    time_t t = timegm(&tm);
    struct tm check;
    gmtime_r(&t, &check);
    return check.tm_year == tm.tm_year && check.tm_mon == tm.tm_mon && check.tm_mday == tm.tm_mday;
}

static bool valid_month(const char* month)
{
    if (!month || strlen(month) != 7 || month[4] != '-') return false;
    for (int i = 0; i < 7; i++) {
        if (i != 4 && (month[i] < '0' || month[i] > '9')) return false;
    }
    int year, value;
    return sscanf(month, "%d-%d", &year, &value) == 2 &&
        year >= 2020 && value >= 1 && value <= 12;
}

static void appendf(std::string& out, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) out.append(buf, MIN(n, (int)sizeof(buf) - 1));
}

static bool remove_tree_contents(const std::string& path)
{
    DIR* dir = opendir(path.c_str());
    if (!dir) return errno == ENOENT;
    bool ok = true;
    struct dirent* de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        std::string child = path + "/" + de->d_name;
        struct stat st;
        if (lstat(child.c_str(), &st) != 0) {
            ok = false;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!remove_tree_contents(child) || rmdir(child.c_str()) != 0) ok = false;
        } else if (unlink(child.c_str()) != 0) {
            ok = false;
        }
    }
    closedir(dir);
    return ok;
}

}

void usage_stats_init()
{
    memset(&current_hour, 0, sizeof(current_hour));
    memset(active, 0, sizeof(active));
    last_error[0] = '\0';
    bool err;
    enabled = admcfg_bool("usage_stats", &err, CFG_OPTIONAL);
    if (err) enabled = true;
    load_or_create_key();
    current_hour.hour_start = hour_floor(time(NULL));
    initialized = identity_key_valid;
    if (atexit(usage_stats_flush_partial) != 0)
        set_error("unable to register usage statistics shutdown flush");
    lprintf("USAGE: fixed memory %zu bytes, enabled=%d root=%s\n",
            sizeof(current_hour) + sizeof(active), enabled, usage_root());
}

void usage_stats_tick()
{
    if (!initialized || !enabled) return;
    s64_t now = time(NULL);
    s64_t hour = hour_floor(now);
    if (hour != current_hour.hour_start) rollover(hour);

    u4_t concurrent = 0;
    for (int ch = 0; ch < rx_chans; ch++) {
        conn_t* conn = rx_channels[ch].conn;
        active_t* a = &active[ch];
        bool tracked = trackable(conn);
        if (a->active && (!tracked || conn != a->conn || conn->tstamp != a->conn_tstamp)) {
            finish_session(a, now, "closed");
        }
        if (!tracked) continue;
        if (!a->active) start_session(ch, conn, now);

        concurrent++;
        current_hour.listener_seconds++;
        hll_add(&current_hour.unique, a->visitor_hash);

        if (!conn->isUserIP && conn->ident_user)
            kiwi_strncpy(a->callsign, conn->ident_user, sizeof(a->callsign));
        if (conn->geo) kiwi_strncpy(a->geo, conn->geo, sizeof(a->geo));

        int freq_kHz = conn->freqHz / 1000;
        bool freq_entry = freq_kHz != a->last_freq_kHz || conn->mode != a->last_mode;
        frequency_add(&current_hour, freq_kHz, conn->mode, freq_entry);
        a->last_freq_kHz = freq_kHz;
        a->last_mode = conn->mode;

        const char* ext_name = ext_users[ch].ext ? ext_users[ch].ext->name : "";
        bool ext_started = strcmp(a->last_ext, ext_name) != 0 && *ext_name;
        extension_add(&current_hour, ext_name, ext_started);
        kiwi_strncpy(a->last_ext, ext_name, sizeof(a->last_ext));
        geography_add(&current_hour, a->geo, false, true);
    }
    current_hour.peak_concurrent = MAX(current_hour.peak_concurrent, concurrent);
    current_hour.concurrency_sum += concurrent;
    current_hour.concurrency_samples++;
}

void usage_stats_connection_closed(conn_t* conn)
{
    if (!initialized || !conn) return;
    for (int ch = 0; ch < MAX_RX_CHANS; ch++) {
        if (active[ch].active && active[ch].conn == conn) {
            finish_session(&active[ch], time(NULL), conn->kick ? "kicked" : "closed");
            return;
        }
    }
}

void usage_stats_flush_partial()
{
    if (!initialized || !enabled) return;
    persist_hour(&current_hour);
}

bool usage_stats_enabled()
{
    return enabled;
}

void usage_stats_set_enabled(bool value)
{
    if (enabled == value) return;
    if (!value) {
        s64_t now = time(NULL);
        for (int i = 0; i < MAX_RX_CHANS; i++) finish_session(&active[i], now, "disabled");
        usage_stats_flush_partial();
    }
    enabled = value;
}

bool usage_stats_delete_all()
{
    sd_write_guard guard;
    bool ok = remove_tree_contents(usage_root());
    if (!ok) {
        set_error("delete usage data failed: %s", strerror(errno));
        return false;
    }
    memset(&current_hour, 0, sizeof(current_hour));
    memset(active, 0, sizeof(active));
    identity_key_valid = false;
    current_hour.hour_start = hour_floor(time(NULL));
    load_or_create_key();
    initialized = identity_key_valid;
    return initialized;
}

char* usage_stats_heatmap_json(int days, const char* metric)
{
    days = CLAMP(days, 1, 7);
    if (!metric || (strcmp(metric, "listener_minutes") != 0 &&
                    strcmp(metric, "sessions") != 0 &&
                    strcmp(metric, "unique") != 0))
        metric = "listener_minutes";
    s64_t now = time(NULL);
    s64_t current = hour_floor(now);
    s64_t today_start = current - (current % 86400);
    s64_t start = today_start - (days - 1) * 86400LL;
    std::string out = "{\"metric\":\"";
    out += json_escape(metric ? metric : "listener_minutes");
    out += "\",\"estimated_unique\":true,\"cells\":[";
    for (int i = 0; i < days * 24; i++) {
        s64_t epoch = start + i * 3600LL;
        persisted_hour_t h;
        bool partial = epoch == current;
        if (partial) {
            memset(&h, 0, sizeof(h));
            h.valid = true;
            h.hour_start = current_hour.hour_start;
            h.listener_seconds = current_hour.listener_seconds;
            h.sessions = current_hour.session_starts;
            h.unique = hll_estimate(&current_hour.unique);
            h.peak = current_hour.peak_concurrent;
            h.concurrency_sum = current_hour.concurrency_sum;
            h.concurrency_samples = current_hour.concurrency_samples;
            h.detail_dropped = current_hour.detail_dropped;
        } else {
            load_hour(epoch, &h);
        }
        appendf(out, "%s{\"start\":%lld,\"available\":%s,\"partial\":%s,"
                     "\"listener_minutes\":%.2f,\"sessions\":%u,\"unique\":%u,"
                     "\"average_concurrent\":%.2f,\"peak\":%u,\"detail_dropped\":%u}",
                i ? "," : "", epoch, h.valid ? "true" : "false", partial ? "true" : "false",
                h.listener_seconds / 60.0, h.sessions, h.unique,
                h.concurrency_samples ? (double)h.concurrency_sum / h.concurrency_samples : 0,
                h.peak, h.detail_dropped);
    }
    out += "]}";
    return strdup(out.c_str());
}

char* usage_stats_summary_json()
{
    s64_t now = time(NULL);
    s64_t current = hour_floor(now);
    s64_t today_start = current - (current % 86400);
    u64_t today_listener = current_hour.listener_seconds;
    u64_t today_concurrency_sum = current_hour.concurrency_sum;
    u64_t today_concurrency_samples = current_hour.concurrency_samples;
    u4_t today_sessions = current_hour.session_starts;
    u4_t today_peak = current_hour.peak_concurrent;
    hll_t today_hll = current_hour.unique;
    for (s64_t epoch = today_start; epoch < current; epoch += 3600) {
        persisted_hour_t ph;
        hll_t hh;
        if (!load_hour(epoch, &ph, &hh)) continue;
        today_listener += ph.listener_seconds;
        today_sessions += ph.sessions;
        today_peak = MAX(today_peak, ph.peak);
        today_concurrency_sum += ph.concurrency_sum;
        today_concurrency_samples += ph.concurrency_samples;
        hll_merge(&today_hll, &hh);
    }

    time_t now_t = now;
    struct tm now_tm;
    gmtime_r(&now_t, &now_tm);
    struct tm month_tm = now_tm;
    month_tm.tm_mday = 1;
    month_tm.tm_hour = month_tm.tm_min = month_tm.tm_sec = 0;
    s64_t month_start = timegm(&month_tm);
    u64_t month_listener = today_listener;
    u4_t month_sessions = today_sessions;
    u4_t month_peak = today_peak;
    hll_t month_hll = today_hll;
    for (s64_t epoch = month_start; epoch < today_start; epoch += 86400) {
        char date[16], ignored[4];
        format_hour(epoch, date, sizeof(date), ignored, sizeof(ignored));
        persisted_hour_t day;
        hll_t dh;
        if (!load_rollup_file(std::string(usage_root()) + "/daily/" + date + ".json", &day, &dh))
            continue;
        month_listener += day.listener_seconds;
        month_sessions += day.sessions;
        month_peak = MAX(month_peak, day.peak);
        hll_merge(&month_hll, &dh);
    }

    std::string out;
    appendf(out, "{\"enabled\":%s,\"estimated_unique\":true,\"hour_start\":%lld,"
                 "\"listener_seconds\":%llu,\"sessions\":%u,\"unique\":%u,"
                 "\"peak\":%u,\"average_concurrent\":%.2f,\"detail_dropped\":%u,"
                 "\"today_listener_seconds\":%llu,\"today_sessions\":%u,\"today_unique\":%u,"
                 "\"today_peak\":%u,\"today_average_concurrent\":%.2f,"
                 "\"month_listener_seconds\":%llu,\"month_sessions\":%u,\"month_unique\":%u,"
                 "\"month_peak\":%u,"
                 "\"memory_bytes\":%zu,\"last_error\":\"%s\"}",
            enabled ? "true" : "false", current_hour.hour_start, current_hour.listener_seconds,
            current_hour.session_starts, hll_estimate(&current_hour.unique),
            current_hour.peak_concurrent,
            current_hour.concurrency_samples ?
                (double)current_hour.concurrency_sum / current_hour.concurrency_samples : 0,
            current_hour.detail_dropped,
            today_listener, today_sessions, hll_estimate(&today_hll), today_peak,
            today_concurrency_samples ?
                (double)today_concurrency_sum / today_concurrency_samples : 0,
            month_listener, month_sessions, hll_estimate(&month_hll), month_peak,
            sizeof(current_hour) + sizeof(active),
            json_escape(last_error).c_str());
    return strdup(out.c_str());
}

char* usage_stats_month_json(const char* month)
{
    if (!valid_month(month)) return strdup("{\"error\":\"invalid month\"}");
    int year, month_number;
    sscanf(month, "%d-%d", &year, &month_number);
    struct tm start_tm;
    memset(&start_tm, 0, sizeof(start_tm));
    start_tm.tm_year = year - 1900;
    start_tm.tm_mon = month_number - 1;
    start_tm.tm_mday = 1;
    s64_t start = timegm(&start_tm);

    struct tm end_tm = start_tm;
    end_tm.tm_mon++;
    s64_t end = timegm(&end_tm);
    s64_t today = hour_floor(time(NULL));
    today -= today % 86400;

    std::string out = "{\"month\":\"" + json_escape(month) + "\",\"days\":[";
    int emitted = 0;
    for (s64_t epoch = start; epoch < end; epoch += 86400) {
        char date[16], ignored[4];
        format_hour(epoch, date, sizeof(date), ignored, sizeof(ignored));
        persisted_hour_t day;
        memset(&day, 0, sizeof(day));
        bool partial = epoch == today;
        if (partial) {
            hll_t hll;
            memset(&hll, 0, sizeof(hll));
            s64_t current = hour_floor(time(NULL));
            for (s64_t hour = epoch; hour <= current; hour += 3600) {
                if (hour == current) {
                    day.listener_seconds += current_hour.listener_seconds;
                    day.sessions += current_hour.session_starts;
                    day.peak = MAX(day.peak, current_hour.peak_concurrent);
                    day.concurrency_sum += current_hour.concurrency_sum;
                    day.concurrency_samples += current_hour.concurrency_samples;
                    day.detail_dropped += current_hour.detail_dropped;
                    hll_merge(&hll, &current_hour.unique);
                } else {
                    persisted_hour_t ph;
                    hll_t hh;
                    if (!load_hour(hour, &ph, &hh)) continue;
                    day.listener_seconds += ph.listener_seconds;
                    day.sessions += ph.sessions;
                    day.peak = MAX(day.peak, ph.peak);
                    day.concurrency_sum += ph.concurrency_sum;
                    day.concurrency_samples += ph.concurrency_samples;
                    day.detail_dropped += ph.detail_dropped;
                    hll_merge(&hll, &hh);
                }
            }
            day.unique = hll_estimate(&hll);
            day.valid = true;
        } else {
            hll_t unused;
            day.valid = load_rollup_file(
                std::string(usage_root()) + "/daily/" + date + ".json", &day, &unused);
        }
        if (!day.valid && epoch > today) continue;
        appendf(out, "%s{\"date\":\"%s\",\"available\":%s,\"partial\":%s,"
                     "\"listener_minutes\":%.2f,\"sessions\":%u,\"unique\":%u,"
                     "\"average_concurrent\":%.2f,\"peak\":%u,\"detail_dropped\":%u}",
                emitted++ ? "," : "", date, day.valid ? "true" : "false",
                partial ? "true" : "false", day.listener_seconds / 60.0,
                day.sessions, day.unique,
                day.concurrency_samples ? (double)day.concurrency_sum / day.concurrency_samples : 0,
                day.peak, day.detail_dropped);
    }
    out += "]}";
    return strdup(out.c_str());
}

struct geo_report_t {
    geography_t geo[GEO_CAP];
    u4_t overflow;
};

static void geo_report_add(geo_report_t* report, const char* label, u4_t sessions, u64_t seconds)
{
    if (!label || !*label) label = "Unknown";
    geography_t* free_slot = NULL;
    geography_t* min_slot = NULL;
    for (int i = 0; i < GEO_CAP; i++) {
        geography_t* geo = &report->geo[i];
        if (geo->used && strcmp(geo->label, label) == 0) {
            geo->sessions += sessions;
            geo->seconds += seconds;
            return;
        }
        if (!geo->used && !free_slot) free_slot = geo;
        if (geo->used && (!min_slot || geo->seconds < min_slot->seconds)) min_slot = geo;
    }
    if (free_slot) {
        free_slot->used = true;
        kiwi_strncpy(free_slot->label, label, sizeof(free_slot->label));
        free_slot->sessions = sessions;
        free_slot->seconds = seconds;
        return;
    }
    report->overflow++;
    if (min_slot) {
        kiwi_strncpy(min_slot->label, label, sizeof(min_slot->label));
        min_slot->sessions = sessions;
        min_slot->seconds += seconds;
    }
}

static bool geo_report_merge_json(geo_report_t* report, const char* json)
{
    u64_t overflow = 0;
    extract_u64(json, "geography_overflow", &overflow);
    report->overflow += overflow;
    const char* p = strstr(json, "\"geography\":[");
    if (!p) return false;
    p += strlen("\"geography\":[");
    while (*p && *p != ']') {
        const char* begin = strchr(p, '{');
        if (!begin) break;
        const char* end = strchr(begin, '}');
        if (!end) return false;
        std::string item(begin, end - begin + 1);
        char label[GEO_LEN];
        u64_t sessions = 0, seconds = 0;
        if (!extract_string(item.c_str(), "label", label, sizeof(label)) ||
                !extract_u64(item.c_str(), "sessions", &sessions) ||
                !extract_u64(item.c_str(), "seconds", &seconds)) {
            return false;
        }
        geo_report_add(report, label, sessions, seconds);
        p = end + 1;
    }
    return true;
}

char* usage_stats_geo_json(int days)
{
    days = CLAMP(days, 1, 7);
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    s64_t start = timegm(&tm) - (s64_t)(days - 1) * 86400;
    s64_t current = hour_floor(now);
    geo_report_t* report = (geo_report_t*)calloc(1, sizeof(*report));
    if (!report) return strdup("{\"error\":\"out of memory\"}");

    int available = 0;
    for (s64_t epoch = start; epoch < current; epoch += 3600) {
        char date[16], hour[4];
        format_hour(epoch, date, sizeof(date), hour, sizeof(hour));
        std::string path = std::string(usage_root()) + "/hourly/" + date + "/" + hour + ".json";
        char* json = read_file_alloc(path);
        if (!json) continue;
        if (geo_report_merge_json(report, json)) available++;
        free(json);
    }
    for (int i = 0; i < GEO_CAP; i++) {
        geography_t* geo = &current_hour.geo[i];
        if (geo->used) geo_report_add(report, geo->label, geo->sessions, geo->seconds);
    }
    available++;

    u64_t total_seconds = 0;
    u4_t total_sessions = 0;
    for (int i = 0; i < GEO_CAP; i++) {
        geography_t* geo = &report->geo[i];
        if (!geo->used) continue;
        total_seconds += geo->seconds;
        total_sessions += geo->sessions;
    }

    std::string out;
    appendf(out, "{\"days\":%d,\"start\":%lld,\"end\":%lld,\"hours_available\":%d,"
            "\"hours_expected\":%lld,\"total_seconds\":%llu,\"total_sessions\":%u,"
            "\"overflow\":%u,\"regions\":[",
            days, start, (s64_t)now, available, (current - start) / 3600 + 1,
            total_seconds, total_sessions, report->overflow);
    bool comma = false;
    for (int i = 0; i < GEO_CAP; i++) {
        geography_t* geo = &report->geo[i];
        if (!geo->used) continue;
        appendf(out, "%s{\"label\":\"%s\",\"seconds\":%llu,\"sessions\":%u}",
                comma ? "," : "", json_escape(geo->label).c_str(), geo->seconds, geo->sessions);
        comma = true;
    }
    out += "]}";
    free(report);
    return strdup(out.c_str());
}

char* usage_stats_day_json(const char* date)
{
    if (!valid_date(date)) return strdup("{\"error\":\"invalid date\"}");
    std::string out = "{\"date\":\"" + json_escape(date) + "\",\"hours\":[";
    for (int h = 0; h < 24; h++) {
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        int year, month, day;
        sscanf(date, "%d-%d-%d", &year, &month, &day);
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = h;
        s64_t epoch = timegm(&tm);
        persisted_hour_t ph;
        bool partial = epoch == current_hour.hour_start;
        if (partial) {
            memset(&ph, 0, sizeof(ph));
            ph.valid = true;
            ph.hour_start = epoch;
            ph.listener_seconds = current_hour.listener_seconds;
            ph.sessions = current_hour.session_starts;
            ph.unique = hll_estimate(&current_hour.unique);
            ph.peak = current_hour.peak_concurrent;
            ph.concurrency_sum = current_hour.concurrency_sum;
            ph.concurrency_samples = current_hour.concurrency_samples;
            ph.detail_dropped = current_hour.detail_dropped;
        } else {
            load_hour(epoch, &ph);
        }
        appendf(out, "%s{\"hour\":%d,\"available\":%s,\"partial\":%s,"
                     "\"listener_minutes\":%.2f,\"sessions\":%u,\"unique\":%u,"
                     "\"average_concurrent\":%.2f,\"peak\":%u,\"detail_dropped\":%u}",
                h ? "," : "", h, ph.valid ? "true" : "false", partial ? "true" : "false",
                ph.listener_seconds / 60.0, ph.sessions, ph.unique,
                ph.concurrency_samples ? (double)ph.concurrency_sum / ph.concurrency_samples : 0,
                ph.peak, ph.detail_dropped);
    }
    out += "]}";
    return strdup(out.c_str());
}

char* usage_stats_hour_json(const char* date, int hour)
{
    if (!valid_date(date) || hour < 0 || hour > 23)
        return strdup("{\"error\":\"invalid hour\"}");

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    int year, month, day;
    sscanf(date, "%d-%d-%d", &year, &month, &day);
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    s64_t epoch = timegm(&tm);

    if (epoch != current_hour.hour_start) {
        char path[256];
        snprintf(path, sizeof(path), "%s/hourly/%s/%02d.json", usage_root(), date, hour);
        char* json = read_file_alloc(path);
        return json ? json : strdup("{\"available\":false}");
    }

    std::string out;
    appendf(out, "{\"schema\":%d,\"complete\":false,\"hour_start\":%lld,"
                 "\"listener_seconds\":%llu,\"sessions\":%u,\"unique_estimate\":%u,"
                 "\"peak_concurrent\":%u,\"concurrency_sum\":%llu,\"concurrency_samples\":%llu,"
                 "\"detail_dropped\":%u,\"frequency_overflow\":%u,\"geography_overflow\":%u,"
                 "\"frequencies\":[",
            USAGE_SCHEMA, current_hour.hour_start, current_hour.listener_seconds,
            current_hour.session_starts, hll_estimate(&current_hour.unique),
            current_hour.peak_concurrent, current_hour.concurrency_sum,
            current_hour.concurrency_samples, current_hour.detail_dropped,
            current_hour.frequency_overflow, current_hour.geography_overflow);
    bool comma = false;
    for (int i = 0; i < FREQ_CAP; i++) {
        frequency_t* f = &current_hour.freq[i];
        if (!f->used) continue;
        appendf(out, "%s{\"khz\":%d,\"mode\":\"%s\",\"seconds\":%llu,\"entries\":%u,\"error\":%llu}",
                comma ? "," : "", f->bucket_kHz,
                json_escape(rx_enum2mode(f->mode)).c_str(), f->seconds, f->entries, f->error);
        comma = true;
    }
    out += "],\"extensions\":[";
    comma = false;
    for (int i = 0; i < EXT_CAP; i++) {
        extension_t* ext = &current_hour.ext[i];
        if (!ext->used) continue;
        appendf(out, "%s{\"name\":\"%s\",\"seconds\":%llu,\"starts\":%u}",
                comma ? "," : "", json_escape(ext->name).c_str(), ext->seconds, ext->starts);
        comma = true;
    }
    out += "],\"geography\":[";
    comma = false;
    for (int i = 0; i < GEO_CAP; i++) {
        geography_t* geo = &current_hour.geo[i];
        if (!geo->used) continue;
        appendf(out, "%s{\"label\":\"%s\",\"seconds\":%llu,\"sessions\":%u}",
                comma ? "," : "", json_escape(geo->label).c_str(), geo->seconds, geo->sessions);
        comma = true;
    }
    out += "]}";
    return strdup(out.c_str());
}

char* usage_stats_recent_json(const char* date, int page, int limit)
{
    if (!valid_date(date)) return strdup("{\"error\":\"invalid date\"}");
    page = MAX(page, 0);
    limit = CLAMP(limit, 1, 100);
    int skip = page * limit;
    int emitted = 0, seen = 0;
    std::string out = "{\"date\":\"" + json_escape(date) + "\",\"sessions\":[";
    char current_date[16], current_hour_name[4];
    format_hour(current_hour.hour_start, current_date, sizeof(current_date),
            current_hour_name, sizeof(current_hour_name));
    bool current_date_selected = strcmp(date, current_date) == 0;
    if (current_date_selected) {
        for (int i = current_hour.detail_count - 1; i >= 0; i--) {
            session_detail_t* d = &current_hour.detail[i];
            if (seen++ < skip || emitted >= limit) continue;
            char hash[HASH_LEN * 2 + 1];
            hash_hex(d->visitor_hash, hash, HASH_LEN);
            hash[12] = '\0';
            if (emitted++) out += ",";
            out += "{\"visitor_hash\":\"";
            out += hash;
            out += "\",\"callsign\":\"";
            out += json_escape(d->callsign);
            out += "\",\"geo\":\"";
            out += json_escape(d->geo);
            out += "\",\"client\":\"";
            out += json_escape(d->client);
            appendf(out, "\",\"start\":%lld,\"end\":%lld,\"duration\":%lld,"
                    "\"start_khz\":%d,\"end_khz\":%d,\"start_mode\":\"%s\","
                    "\"end_mode\":\"%s\",\"close_reason\":\"%s\"}",
                    d->start_utc, d->end_utc, MAX(0LL, d->end_utc - d->start_utc),
                    d->start_freq_kHz, d->end_freq_kHz,
                    json_escape(rx_enum2mode(d->start_mode)).c_str(),
                    json_escape(rx_enum2mode(d->end_mode)).c_str(),
                    json_escape(d->close_reason).c_str());
        }
    }

    int newest_hour = current_date_selected ? atoi(current_hour_name) - 1 : 23;
    for (int h = newest_hour; h >= 0; h--) {
        char path[256];
        snprintf(path, sizeof(path), "%s/sessions/%s/%02d.json", usage_root(), date, h);
        char* json = read_file_alloc(path);
        if (!json) continue;
        const char* p = strstr(json, "\"sessions\":[");
        if (p) {
            p += strlen("\"sessions\":[");
            while (*p && *p != ']') {
                const char* begin = strchr(p, '{');
                if (!begin) break;
                int depth = 0;
                const char* end = begin;
                bool quoted = false;
                for (; *end; end++) {
                    if (*end == '"' && (end == begin || end[-1] != '\\')) quoted = !quoted;
                    if (quoted) continue;
                    if (*end == '{') depth++;
                    if (*end == '}' && --depth == 0) { end++; break; }
                }
                if (!*end && depth) break;
                if (seen++ >= skip && emitted < limit) {
                    if (emitted++) out += ",";
                    std::string item(begin, end - begin);
                    const char* key = "\"visitor_hash\":\"";
                    size_t hash_pos = item.find(key);
                    if (hash_pos != std::string::npos) {
                        size_t value = hash_pos + strlen(key);
                        size_t value_end = item.find('"', value);
                        if (value_end != std::string::npos && value_end > value + 12)
                            item.erase(value + 12, value_end - (value + 12));
                    }
                    out += item;
                }
                p = end;
            }
        }
        free(json);
    }
    appendf(out, "],\"page\":%d,\"limit\":%d,\"has_more\":%s}", page, limit,
            seen > skip + emitted ? "true" : "false");
    return strdup(out.c_str());
}

const char* usage_stats_last_error()
{
    return last_error;
}

#ifdef NATIVE_HARNESS
void usage_stats_test_stress(int iterations)
{
    iterations = CLAMP(iterations, 1, 100000);
    for (int i = 0; i < iterations; i++) {
        char synthetic[64];
        snprintf(synthetic, sizeof(synthetic), "198.51.100.%d-%d", i & 255, i);
        u1_t hash[HASH_LEN];
        visitor_hash(synthetic, hash);
        hll_add(&current_hour.unique, hash);
        current_hour.session_starts++;
        current_hour.listener_seconds++;
        frequency_add(&current_hour, 1000 + i, i % 8, true);
        geography_add(&current_hour, "Stress test", true, true);
        extension_add(&current_hour, "stress", true);
        if (current_hour.detail_count < DETAIL_CAP) {
            session_detail_t* d = &current_hour.detail[current_hour.detail_count++];
            memset(d, 0, sizeof(*d));
            memcpy(d->visitor_hash, hash, sizeof(d->visitor_hash));
            bool xss_probe = current_hour.detail_count == DETAIL_CAP;
            kiwi_strncpy(d->callsign, xss_probe ? "<img id=usage-xss-probe>" : "STRESS",
                    sizeof(d->callsign));
            kiwi_strncpy(d->geo, xss_probe ? "<script>usage-xss</script>" : "Stress test",
                    sizeof(d->geo));
            kiwi_strncpy(d->client, "test", sizeof(d->client));
            d->start_utc = current_hour.hour_start;
            d->end_utc = current_hour.hour_start + 1;
            d->start_freq_kHz = d->end_freq_kHz = 1000 + i;
            d->start_mode = d->end_mode = i % 8;
            kiwi_strncpy(d->close_reason, "test", sizeof(d->close_reason));
        } else {
            current_hour.detail_dropped++;
        }
    }
}
#endif

/*
--------------------------------------------------------------------------------
This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public License as published
by the Free Software Foundation; either version 2 of the License, or (at your
option) any later version.
--------------------------------------------------------------------------------
*/

#include "types.h"
#include "config.h"
#include "kiwi.h"
#include "cfg.h"
#include "conn.h"
#include "rx.h"
#include "printf.h"
#include "rx_server_ajax.h"
#include "security.h"
#include "sha256.h"
#include "str.h"
#include "coroutines.h"
#include "restrict.h"

#include <algorithm>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RESTRICT_MAX_RANGES 128
#define RESTRICT_SALT_BYTES 16
#define RESTRICT_SALT_HEX_LEN (RESTRICT_SALT_BYTES * 2)
#define RESTRICT_HASH_HEX_LEN (SHA256_BLOCK_SIZE * 2)
#define RESTRICT_PASSWORD_PREFIX "sha256$"
#define RESTRICT_PASSWORD_VALUE_LEN \
    (sizeof(RESTRICT_PASSWORD_PREFIX) - 1 + RESTRICT_SALT_HEX_LEN + 1 + RESTRICT_HASH_HEX_LEN)

typedef struct {
    int lo_Hz, hi_Hz;
} restrict_range_t;

typedef struct {
    lock_t lock;
    bool init, valid;
    int nranges;
    restrict_range_t ranges[RESTRICT_MAX_RANGES];
} restrict_policy_t;

static restrict_policy_t restrict_policy;

static void restrict_mode_bin_to_hex(const u1_t* bin, int bin_len, char* hex)
{
    static const char digits[] = "0123456789abcdef";

    for (int i = 0; i < bin_len; i++) {
        hex[i * 2] = digits[bin[i] >> 4];
        hex[i * 2 + 1] = digits[bin[i] & 0xf];
    }
    hex[bin_len * 2] = '\0';
}

static int restrict_mode_hex_to_bin(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static bool restrict_mode_password_format_valid(const char* configured, u1_t* salt, char* hash)
{
    if (configured == NULL || strlen(configured) != RESTRICT_PASSWORD_VALUE_LEN ||
        strncmp(configured, RESTRICT_PASSWORD_PREFIX, sizeof(RESTRICT_PASSWORD_PREFIX) - 1) != 0)
        return false;

    const char* salt_s = configured + sizeof(RESTRICT_PASSWORD_PREFIX) - 1;
    const char* hash_s = salt_s + RESTRICT_SALT_HEX_LEN + 1;
    if (salt_s[RESTRICT_SALT_HEX_LEN] != '$')
        return false;

    for (int i = 0; i < RESTRICT_SALT_BYTES; i++) {
        int hi = restrict_mode_hex_to_bin(salt_s[i * 2]);
        int lo = restrict_mode_hex_to_bin(salt_s[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        salt[i] = (hi << 4) | lo;
    }

    for (int i = 0; i < RESTRICT_HASH_HEX_LEN; i++) {
        if (restrict_mode_hex_to_bin(hash_s[i]) < 0)
            return false;
        hash[i] = hash_s[i];
    }
    hash[RESTRICT_HASH_HEX_LEN] = '\0';
    return true;
}

static void restrict_mode_password_hash(const char* password, const u1_t* salt, char* hash)
{
    SHA256_CTX ctx;
    u1_t digest[SHA256_BLOCK_SIZE];

    sha256_init(&ctx);
    sha256_update(&ctx, salt, RESTRICT_SALT_BYTES);
    sha256_update(&ctx, (const u1_t*) password, strlen(password));
    sha256_final(&ctx, digest);
    restrict_mode_bin_to_hex(digest, sizeof(digest), hash);

    memset(&ctx, 0, sizeof(ctx));
    memset(digest, 0, sizeof(digest));
}

static bool restrict_mode_hash_equal(const char* a, const char* b)
{
    u1_t diff = 0;
    for (int i = 0; i < RESTRICT_HASH_HEX_LEN; i++)
        diff |= a[i] ^ b[i];
    return diff == 0;
}

static void restrict_mode_set_policy_invalid()
{
    if (!restrict_policy.init) {
        lock_init_recursive(&restrict_policy.lock);
        restrict_policy.init = true;
    }

    lock_holder holder(restrict_policy.lock);
    restrict_policy.valid = false;
    restrict_policy.nranges = 0;
}

static bool token_equals(cfg_t* cfg, jsmntok_t* token, const char* s)
{
    int len = strlen(s);
    return token->type == JSMN_STRING && token->end - token->start == len &&
        strncmp(&cfg->json[token->start], s, len) == 0;
}

static bool token_int(cfg_t* cfg, jsmntok_t* token, int* value)
{
    if (token->type != JSMN_PRIMITIVE)
        return false;

    char buf[32];
    int len = token->end - token->start;
    if (len <= 0 || len >= (int) sizeof(buf))
        return false;

    memcpy(buf, &cfg->json[token->start], len);
    buf[len] = '\0';
    char* end;
    long parsed = strtol(buf, &end, 10);
    if (*end != '\0' || parsed < 0 || parsed > INT_MAX)
        return false;

    *value = parsed;
    return true;
}

static bool token_service(cfg_t* cfg, jsmntok_t* token, char* service)
{
    if (token->type != JSMN_STRING || token->end - token->start != 1)
        return false;

    *service = cfg->json[token->start];
    return true;
}

static bool collect_band(cfg_t* cfg, jsmntok_t* object, int object_i, restrict_range_t* ranges, int* nranges)
{
    bool have_min = false, have_max = false, have_itu = false, have_svc = false;
    int min_kHz = 0, max_kHz = 0, itu = 0;
    char service = '\0';

    for (int i = object_i + 1; i + 1 < cfg->ntok; i++) {
        jsmntok_t* key = &cfg->tokens[i];
        if (key->start >= object->end)
            break;
        jsmntok_t* value = &cfg->tokens[i + 1];
        if (value->end > object->end || key->type != JSMN_STRING)
            continue;

        if (token_equals(cfg, key, "min")) {
            have_min = token_int(cfg, value, &min_kHz);
        } else
        if (token_equals(cfg, key, "max")) {
            have_max = token_int(cfg, value, &max_kHz);
        } else
        if (token_equals(cfg, key, "itu")) {
            have_itu = token_int(cfg, value, &itu);
        } else
        if (token_equals(cfg, key, "svc")) {
            have_svc = token_service(cfg, value, &service);
        }
    }

    if (!have_min || !have_max || !have_itu || !have_svc || min_kHz > max_kHz)
        return false;

    int current_itu = cfg_int("init.ITU_region", NULL, CFG_REQUIRED) + 1;
    if ((service != 'A' && service != 'B') || (itu != 0 && itu != current_itu))
        return true;

    if (*nranges == RESTRICT_MAX_RANGES)
        return false;

    ranges[*nranges].lo_Hz = min_kHz * 1000;
    ranges[*nranges].hi_Hz = max_kHz * 1000;
    (*nranges)++;
    return true;
}

bool restrict_mode_reload_policy()
{
    char* dxcfg_json = dxcfg_get_json(NULL);
    if (dxcfg_json == NULL) {
        lprintf("RESTRICT: dx_config.json is unavailable\n");
        restrict_mode_set_policy_invalid();
        return false;
    }

    char* json_copy = strdup(dxcfg_json);
    if (json_copy == NULL) {
        lprintf("RESTRICT: unable to allocate dx_config.json copy\n");
        restrict_mode_set_policy_invalid();
        return false;
    }

    cfg_t parsed = { 0 };
    if (!json_init(&parsed, json_copy, "restrict_dx_config")) {
        lprintf("RESTRICT: unable to parse dx_config.json\n");
        free(json_copy);
        restrict_mode_set_policy_invalid();
        return false;
    }
    free(json_copy);

    jsmntok_t* bands = NULL;
    for (int i = 0; i + 1 < parsed.ntok; i++) {
        if (token_equals(&parsed, &parsed.tokens[i], "bands") && JSMN_IS_ARRAY(&parsed.tokens[i + 1])) {
            bands = &parsed.tokens[i + 1];
            break;
        }
    }

    restrict_range_t ranges[RESTRICT_MAX_RANGES];
    int nranges = 0;
    bool valid = bands != NULL;
    if (valid) {
        for (int i = 0; i < parsed.ntok; i++) {
            jsmntok_t* token = &parsed.tokens[i];
            if (!JSMN_IS_OBJECT(token) || token->start < bands->start || token->end > bands->end)
                continue;
            if (!collect_band(&parsed, token, i, ranges, &nranges)) {
                valid = false;
                break;
            }
        }
    }

    json_release(&parsed);
    if (!valid || nranges == 0) {
        lprintf("RESTRICT: dx_config.json has no valid Amateur/Broadcast bands\n");
        restrict_mode_set_policy_invalid();
        return false;
    }

    std::sort(ranges, ranges + nranges, [](const restrict_range_t& a, const restrict_range_t& b) {
        return a.lo_Hz < b.lo_Hz;
    });

    int merged = 0;
    for (int i = 0; i < nranges; i++) {
        if (merged == 0 || ranges[i].lo_Hz > ranges[merged - 1].hi_Hz + 1) {
            ranges[merged++] = ranges[i];
        } else {
            ranges[merged - 1].hi_Hz = MAX(ranges[merged - 1].hi_Hz, ranges[i].hi_Hz);
        }
    }

    if (!restrict_policy.init) {
        lock_init_recursive(&restrict_policy.lock);
        restrict_policy.init = true;
    }

    {
        lock_holder holder(restrict_policy.lock);
        memcpy(restrict_policy.ranges, ranges, merged * sizeof(ranges[0]));
        restrict_policy.nranges = merged;
        restrict_policy.valid = true;
    }

    lprintf("RESTRICT: loaded %d Amateur/Broadcast frequency ranges\n", merged);
    return true;
}

static bool restrict_mode_enabled()
{
    bool error;
    bool enabled = admcfg_bool("restrict_mode_enabled", &error, CFG_OPTIONAL);
    return !error && enabled;
}

bool restrict_mode_tune_allowed(conn_t* conn, double freq_kHz)
{
    if (!restrict_mode_enabled() || conn->auth_admin || conn->restrict_unlocked)
        return true;

    if (!isfinite(freq_kHz) || freq_kHz < 0 ||
        freq_kHz > (double) INT_MAX / 1000.0)
        return false;

    int freq_Hz = (int) round(freq_kHz * 1000.0);
    if (!restrict_policy.init)
        return false;

    lock_holder holder(restrict_policy.lock);
    if (!restrict_policy.valid)
        return false;

    for (int i = 0; i < restrict_policy.nranges; i++) {
        restrict_range_t* range = &restrict_policy.ranges[i];
        if (freq_Hz < range->lo_Hz)
            return false;
        if (freq_Hz <= range->hi_Hz)
            return true;
    }
    return false;
}

static void restrict_mode_set_unlocked(conn_t* conn)
{
    int rx_channel = conn->rx_channel;
    for (conn_t* c = conns; c < &conns[N_CONNS]; c++) {
        if (!c->valid)
            continue;
        if (c->rx_channel == rx_channel &&
            (c->type == STREAM_SOUND || c->type == STREAM_WATERFALL || c->type == STREAM_EXT)) {
            c->restrict_unlocked = true;
        }
    }
}

bool restrict_mode_unlock(conn_t* conn, char* password)
{
    if (conn->auth_admin || !restrict_mode_enabled()) {
        restrict_mode_set_unlocked(conn);
        return true;
    }

    const char* configured = admcfg_string("restrict_mode_password", NULL, CFG_REQUIRED);
    bool allowed = false;
    u1_t salt[RESTRICT_SALT_BYTES];
    char expected_hash[RESTRICT_HASH_HEX_LEN + 1];
    char actual_hash[RESTRICT_HASH_HEX_LEN + 1];
    if (password != NULL &&
        restrict_mode_password_format_valid(configured, salt, expected_hash)) {
        restrict_mode_password_hash(password, salt, actual_hash);
        allowed = restrict_mode_hash_equal(expected_hash, actual_hash);
    }
    memset(salt, 0, sizeof(salt));
    memset(expected_hash, 0, sizeof(expected_hash));
    memset(actual_hash, 0, sizeof(actual_hash));
    cfg_string_free(configured);
    if (allowed)
        restrict_mode_set_unlocked(conn);
    return allowed;
}

bool restrict_mode_set_password(const char* password)
{
    if (password == NULL || *password == '\0')
        return false;

    u1_t salt[RESTRICT_SALT_BYTES];
    if (kiwi_file_read("restrict_mode_set_password", "/dev/urandom", (char*) salt, sizeof(salt)) != sizeof(salt)) {
        lprintf("RESTRICT: unable to read password salt\n");
        return false;
    }

    char salt_hex[RESTRICT_SALT_HEX_LEN + 1];
    char hash_hex[RESTRICT_HASH_HEX_LEN + 1];
    char configured[RESTRICT_PASSWORD_VALUE_LEN + 1];
    restrict_mode_bin_to_hex(salt, sizeof(salt), salt_hex);
    restrict_mode_password_hash(password, salt, hash_hex);
    snprintf(configured, sizeof(configured), "%s%s$%s", RESTRICT_PASSWORD_PREFIX, salt_hex, hash_hex);
    admcfg_set_string_save("restrict_mode_password", configured);

    memset(salt, 0, sizeof(salt));
    memset(salt_hex, 0, sizeof(salt_hex));
    memset(hash_hex, 0, sizeof(hash_hex));
    memset(configured, 0, sizeof(configured));
    return true;
}

void restrict_mode_send_state(conn_t* conn)
{
    send_msg(conn, false, "MSG restrict=%d,%d,%d", restrict_mode_enabled(), conn->restrict_unlocked, conn->auth_admin);
}

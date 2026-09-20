#pragma once

#include "types.h"

struct conn_st;
typedef struct conn_st conn_t;

void usage_stats_init();
void usage_stats_tick();
void usage_stats_connection_closed(conn_t* conn);
void usage_stats_flush_partial();

bool usage_stats_enabled();
void usage_stats_set_enabled(bool enabled);

char* usage_stats_heatmap_json(int days, const char* metric);
char* usage_stats_summary_json();
char* usage_stats_day_json(const char* date);
char* usage_stats_recent_json(const char* date, int page, int limit);
const char* usage_stats_last_error();

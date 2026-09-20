/*
--------------------------------------------------------------------------------
This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public License as published
by the Free Software Foundation; either version 2 of the License, or (at your
option) any later version.
--------------------------------------------------------------------------------
*/

#pragma once

typedef struct conn_st conn_t;

bool restrict_mode_reload_policy();
bool restrict_mode_tune_allowed(conn_t* conn, double freq_kHz);
bool restrict_mode_unlock(conn_t* conn, char* password);
void restrict_mode_send_state(conn_t* conn);

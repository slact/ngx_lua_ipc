#ifndef NGX_LUA_IPC_H
#define NGX_LUA_IPC_H

#include <ngx_http.h>
#include <nginx.h>

extern ngx_module_t ngx_lua_ipc_module;

typedef struct {
  ngx_pid_t     sender_pid;
  ngx_int_t     sender_slot;
  ngx_str_t    *name;
  ngx_str_t    *data;
} lua_ipc_alert_t;

#endif //NGX_LUA_IPC_H

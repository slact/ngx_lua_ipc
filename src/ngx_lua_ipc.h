#ifndef NGX_LUA_IPC_H
#define NGX_LUA_IPC_H

#include <ngx_http.h>
#include <ngx_lua_ipc_util.h>
#include <nginx.h>

extern ngx_module_t ngx_lua_ipc_module;

typedef struct {
  void            *ptr;
} shm_data_t;

#endif //NGX_LUA_IPC_H

#ifndef NGX_LUA_IPC_H
#define NGX_LUA_IPC_H

#include <ngx_http.h>
#include <nginx.h>

extern ngx_module_t ngx_lua_ipc_module;

typedef struct {
  ngx_int_t      pid;
  ngx_int_t      slot;
} worker_slot_t;

typedef struct {
  worker_slot_t   *worker_slots;
  void            *ptr;
} shm_data_t;

#endif //NGX_LUA_IPC_H

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

typedef struct ipc_alert_waiting_s ipc_alert_waiting_t;
struct ipc_alert_waiting_s {
  ngx_int_t             sender_slot;
  ngx_pid_t             sender_pid;
  ngx_str_t             name;
  ngx_str_t             data;
  ipc_alert_waiting_t  *next;
}; //ipc_alert_waiting_t

typedef struct {
  ipc_alert_waiting_t  *head;
  ipc_alert_waiting_t  *tail;
} received_buffered_alerts_t;

#endif //NGX_LUA_IPC_H

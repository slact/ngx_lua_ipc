#include <ngx_lua_ipc.h>
#include <ipc.h>
#include <lauxlib.h>
#include "ngx_http_lua_api.h"

#include <shmem.h>

#define DEBUG_LEVEL NGX_LOG_DEBUG
//#define DEBUG_LEVEL NGX_LOG_WARN

#define DBG(fmt, args...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "IPC:" fmt, ##args)
#define ERR(fmt, args...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "IPC:" fmt, ##args)

static shmem_t         *shm = NULL;
static shm_data_t      *shdata = NULL;

static ipc_t            ipc_data;
static ipc_t           *ipc = NULL;

static lua_State       *alert_L = NULL;

static ngx_int_t        max_workers;


static void ngx_lua_ipc_alert_handler(ngx_int_t sender, ngx_uint_t code, void *data);

static ngx_int_t initialize_shm(ngx_shm_zone_t *zone, void *data) {
  shm_data_t         *d;
  if(data) { //zone being passed after restart
    zone->data = data;
    d = zone->data;
    shm_reinit(shm);
  }
  else {
    shm_init(shm);
    
    if((d = shm_calloc(shm, sizeof(*d), "root shared data")) == NULL) {
      return NGX_ERROR;
    }
    
    zone->data = d;
    shdata = d;
    
  }
  
  if(shdata->worker_slots) {
    shm_free(shm, shdata->worker_slots);
  }
  shdata->worker_slots = shm_calloc(shm, sizeof(worker_slot_t) * max_workers, "worker slots");
    
  return NGX_OK;
}


static int ngx_http_lua_ipc_send_alert(lua_State *L) {
  int            target_worker = luaL_checknumber(L, 1);
  int            alert_code = luaL_checknumber(L, 2);
  size_t         data_sz;
  const char    *data = luaL_checklstring(L, 3, &data_sz);
  int            i;
  
  for(i=0; i<max_workers; i++) {
    if(shdata->worker_slots[i].pid == target_worker) {
      ipc_alert(ipc, shdata->worker_slots[i].slot, alert_code, (void *)data, data_sz);
      break;
    }
  }
  return 0;
}

static int ngx_http_lua_ipc_broadcast_alert(lua_State * L) {
  int            alert_code = luaL_checknumber(L, 1);
  size_t         data_sz;
  const char    *data = luaL_checklstring(L, 2, &data_sz);
  int            i;
  
  for(i=0; i<max_workers; i++) {
    ipc_alert(ipc, shdata->worker_slots[i].slot, alert_code, (void *)data, data_sz);
  }
  
  return 0;
}

static int ngx_http_lua_ipc_add_event_handler(lua_State * L) {
  int            alert_code = luaL_checknumber(L, 1);
  luaL_checktype (L, 2, LUA_TFUNCTION);
  
  lua_getglobal(L, "_ipc_alert_handlers"); ///ugly!!!
  
  lua_pushvalue(L, 2);
  
  lua_rawseti(L, -2, alert_code);
  
  if(!alert_L) {
    alert_L = L;
  }
  
  return 0;
}

static int ngx_http_lua_ipc_init_lua_code(lua_State * L) {
  lua_createtable(L, 0, 3);
  lua_pushcfunction(L, ngx_http_lua_ipc_send_alert);
  lua_setfield(L, -2, "send");

  lua_pushcfunction(L, ngx_http_lua_ipc_broadcast_alert);
  lua_setfield(L, -2, "broadcast");

  lua_pushcfunction(L, ngx_http_lua_ipc_add_event_handler);
  lua_setfield(L, -2, "receive");
  
  lua_newtable(L);
  lua_setglobal(L, "_ipc_alert_handlers");  //ugly
  return 1;
}



static ngx_int_t ngx_lua_ipc_init_postconfig(ngx_conf_t *cf) {
  ngx_str_t              name = ngx_string("ngx_lua_ipc");

  shm = shm_create(&name, &ngx_lua_ipc_module, cf, 1024*1024, initialize_shm, &ngx_lua_ipc_module);
  
  if (ngx_http_lua_add_package_preload(cf, "ngx.ipc", ngx_http_lua_ipc_init_lua_code) != NGX_OK) {
    return NGX_ERROR;
  }
  
  return NGX_OK;
}


static void ngx_lua_ipc_alert_handler(ngx_int_t sender_slot, ngx_uint_t code, void *data) {
  lua_State *L = alert_L;
  int i, sender_pid;
  int found = 0;
  
  if(!L) {
    //no alert handlers here
    return;
  }
  
  lua_getglobal(L, "_ipc_alert_handlers"); ///ugly!!!
  
  lua_rawgeti(L, -1, code);
  
  for(i=0; i<max_workers; i++) {
    if(shdata->worker_slots[i].slot == sender_slot) {
      sender_pid = shdata->worker_slots[i].pid;
      found = 1;
      break;
    }
  }
  if(found) {
    lua_pushinteger(L, sender_pid);
    lua_pushstring(L, (char *)data);
    lua_call(L, 2, 0);
  }
  else {
    //uuh....
  }
}

static ngx_int_t ngx_lua_ipc_init_module(ngx_cycle_t *cycle) {
  ngx_core_conf_t                *ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
  
  max_workers = ccf->worker_processes;
  
  //initialize our little IPC
  if(ipc == NULL) {
    ipc = &ipc_data;
    ipc_init(ipc);
    ipc_set_handler(ipc, ngx_lua_ipc_alert_handler);
  }
  ipc_open(ipc, cycle, ccf->worker_processes, NULL);
  return NGX_OK;
}

static ngx_int_t ngx_lua_ipc_init_worker(ngx_cycle_t *cycle) {
  int i, found = 0;
  shmtx_lock(shm);
  for(i=0; i<max_workers; i++) {
    if(shdata->worker_slots[i].pid == 0) {
      shdata->worker_slots[i].pid = ngx_pid;
      shdata->worker_slots[i].slot = ngx_process_slot; 
      found = 1;
      break;
    }
  }
  shmtx_unlock(shm);
  
  if(!found) {
    return NGX_ERROR;
  }
  ipc_register_worker(ipc, cycle);
  
  return NGX_OK;
}

static void ngx_lua_ipc_exit_worker(ngx_cycle_t *cycle) { 
  ipc_close(ipc, cycle);
}

static void ngx_lua_ipc_exit_master(ngx_cycle_t *cycle) {
  ipc_close(ipc, cycle);
  shm_free(shm, shdata);
  shm_destroy(shm);
}

static ngx_command_t  ngx_lua_ipc_commands[] = {
  ngx_null_command
};

static ngx_http_module_t  ngx_lua_ipc_ctx = {
  NULL,                          /* preconfiguration */
  ngx_lua_ipc_init_postconfig,   /* postconfiguration */
  NULL,                          /* create main configuration */
  NULL,                          /* init main configuration */
  NULL,                          /* create server configuration */
  NULL,                          /* merge server configuration */
  NULL,                          /* create location configuration */
  NULL,                          /* merge location configuration */
};

ngx_module_t  ngx_lua_ipc_module = {
  NGX_MODULE_V1,
  &ngx_lua_ipc_ctx,              /* module context */
  ngx_lua_ipc_commands,          /* module directives */
  NGX_HTTP_MODULE,               /* module type */
  NULL,                          /* init master */
  ngx_lua_ipc_init_module,       /* init module */
  ngx_lua_ipc_init_worker,       /* init process */
  NULL,                          /* init thread */
  NULL,                          /* exit thread */
  ngx_lua_ipc_exit_worker,       /* exit process */
  ngx_lua_ipc_exit_master,       /* exit master */
  NGX_MODULE_V1_PADDING
};

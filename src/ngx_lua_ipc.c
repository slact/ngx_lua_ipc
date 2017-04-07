#include <ngx_lua_ipc.h>
#include <ipc.h>
#include <lauxlib.h>
#include "ngx_http_lua_api.h"

#include "ngx_lua_ipc_scripts.h"

#include <assert.h>

#define DEBUG_LEVEL NGX_LOG_DEBUG
//#define DEBUG_LEVEL NGX_LOG_WARN

#define LOAD_SCRIPTS_AS_NAMED_CHUNKS

#define DBG(fmt, args...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "IPC:" fmt, ##args)
#define ERR(fmt, args...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "IPC:" fmt, ##args)

#ifndef container_of
#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})
#endif


#ifdef LOAD_SCRIPTS_AS_NAMED_CHUNKS
  #define ngx_lua_ipc_loadscript(lua_state, name)                 \
          luaL_loadbuffer(lua_state, ngx_ipc_lua_scripts.name, strlen(ngx_ipc_lua_scripts.name), #name); \
          lua_call(lua_state, 0, LUA_MULTRET)
#else
  #define ngx_lua_ipc_loadscript(lua_state, name)                 \
          luaL_dostring(lua_state, ngx_ipc_lua_scripts.name)
#endif

static ipc_t                        *ipc = NULL;

// lua for the ipc alert handlers will be run from this timer's context
static ngx_event_t                  *hacktimer = NULL;
static int                           running_hacked_timer_handler = 0;

//received but yet-unprocessed alerts are quered here
static received_buffered_alerts_t   received_alerts = {NULL, NULL};


static void ngx_lua_ipc_alert_handler(ngx_int_t sender, ngx_str_t *name, ngx_str_t *data);


ngx_int_t luaL_checklstring_as_ngx_str(lua_State *L, int n, ngx_str_t *str) {
  size_t         data_sz;
  const char    *data = luaL_checklstring(L, n, &data_sz);
  
  str->data = (u_char *)data;
  str->len = data_sz;
  return NGX_OK;
}

static ngx_int_t ngx_lua_ipc_get_alert_args(lua_State *L, int stack_offset, ngx_str_t *name, ngx_str_t *data) {
  luaL_checklstring_as_ngx_str(L, 1+stack_offset, name);
  if(lua_gettop(L) >= 2+stack_offset) {
    luaL_checklstring_as_ngx_str(L, 2+stack_offset, data);
  }
  else {
    data->len=0;
    data->data=NULL;
  }
  
  return NGX_OK;
}

static int ngx_lua_ipc_hacktimer_alert_iterator(lua_State *L) {
  ipc_alert_waiting_t  *cur = received_alerts.head;
  if(cur) {
    received_alerts.head = cur->next;
    if(received_alerts.tail == cur) {
      assert(cur->next == NULL);
      received_alerts.tail = NULL;
    }
    
    lua_pushinteger(L, cur->sender_slot);
    lua_pushinteger(L, cur->sender_pid);
    lua_pushlstring (L, (const char *)cur->name.data, cur->name.len);
    lua_pushlstring (L, (const char *)cur->data.data, cur->data.len);
    
    ngx_free(cur);
  }
  else {
    lua_pushnil(L);
    lua_pushnil(L);
    lua_pushnil(L);
    lua_pushnil(L);
  }
  return 4;
}

static int ngx_lua_ipc_hacktimer_run_handler(lua_State *L) {
  luaL_checktype (L, 1, LUA_TFUNCTION); //handler
  luaL_checktype (L, 2, LUA_TSTRING); //event name
  luaL_checktype (L, 3, LUA_TSTRING); //event data
  luaL_checktype (L, 4, LUA_TBOOLEAN); //should name be passed to handler as first argument?
  
  int pass_name = lua_toboolean(L, 4);
  
  hacktimer = NULL; //it's about to be deleted (and a new one added), might as well clear it right now.
  running_hacked_timer_handler = 1;
  if(!pass_name) {
    lua_pushvalue(L, 1); //callback function
    lua_pushvalue(L, 3); //event data
    lua_call(L, 1, 0);
  }
  else {
    lua_pushvalue(L, 1); //callback function
    lua_pushvalue(L, 2); //event name
    lua_pushvalue(L, 3); //event data
    lua_call(L, 2, 0);
  }
  running_hacked_timer_handler = 0;
  return 0;
}

ngx_event_t *ngx_lua_ipc_get_hacktimer(ngx_msec_t magic_key) {
  
  ngx_event_t    *ev;
  ngx_rbtree_node_t **p, *sentinel=ngx_event_timer_rbtree.sentinel, *temp=ngx_event_timer_rbtree.root;
  if(temp == sentinel) {
    return NULL;
  }
  while(1) {
    p = ((ngx_rbtree_key_int_t) (magic_key - temp->key) < 0) ? &temp->left : &temp->right;
    if (*p == sentinel) {
      break;
    }
    temp = *p;
  }
  
  if(temp != sentinel && temp->key == magic_key) {
    ev = container_of(temp, ngx_event_t, timer);
    return ev;
  }
  else {
    return NULL;
  }
}

static int ngx_lua_ipc_hacktimer_unique_timeout(lua_State *L, uint32_t *unique_timeout, ngx_msec_t *unique_timeout_key) {
  uint32_t i;
  ngx_msec_t timeout_msec, timeout_key;
  for(i=4596313; i != 0; i++) {
    lua_pushinteger(L, i);
    timeout_msec = (ngx_msec_t)(luaL_checknumber(L, -1) * 1000); 
    //generate the timeout just like the lua call would
    lua_pop(L, 1);
    timeout_key = timeout_msec + ngx_current_msec;
    if(ngx_lua_ipc_get_hacktimer(timeout_key) == NULL) {
      //no timer exists with this timeout
      *unique_timeout = i;
      *unique_timeout_key = timeout_key;
      return 1;
    }
    
    DBG("timer with timeout %i exists, try again", timeout_msec);
  }
  return 0;
}

static int ngx_lua_ipc_hacktimer_add_and_hack(lua_State *L) {
  uint32_t        unique_timeout;
  ngx_msec_t      unique_timeout_key;
  ngx_event_t    *ev;
  
  if(ngx_quit || ngx_exiting) {
    //do nothing
    return 0;
  }
  
  if(!ngx_event_timer_rbtree.sentinel) { //tree not ready yet
    luaL_error(L, "Can't register receive handlers in init_by_lua, too early in Nginx start. Try init_worker_by_lua.");
  }
  
  DBG("ngx_lua_ipc_hacktimer_add_and_hack");
  assert(hacktimer == NULL);
  
  luaL_checktype (L, 1, LUA_TFUNCTION);
  
  lua_getglobal(L, "ngx");
  lua_getfield(L, -1, "timer");
  lua_getfield(L, -1, "at");
  
  if(!ngx_lua_ipc_hacktimer_unique_timeout(L, &unique_timeout, &unique_timeout_key)) {
    ERR("couldn't find unique timeout. hack failed.");
    return 0;
  }
  
  lua_pushinteger(L, unique_timeout);
  lua_pushvalue(L, 1); //callback function
  lua_call(L, 2, 0);
  
  //now find that crazy timer
  ev = ngx_lua_ipc_get_hacktimer(unique_timeout_key);
  assert(ev);
  
  ngx_del_timer(ev);
  ngx_add_timer(ev, 10000000);
  DBG("set hacked timer %p (prev %p)", ev, hacktimer);
  hacktimer = ev;
  
  return 0;
}

static int ngx_lua_ipc_send_alert(lua_State *L) {
  int            target_worker_pid = luaL_checknumber(L, 1);
  ngx_int_t      rc;
  ngx_str_t      name, data;
  ngx_lua_ipc_get_alert_args(L, 1, &name, &data);
  
  rc = ipc_alert_pid(ipc, target_worker_pid, &name, &data);
  
  lua_pushboolean(L, rc == NGX_OK);
  return 1;
}

static int ngx_lua_ipc_broadcast_alert(lua_State * L) {
  ngx_str_t      name, data;
  ngx_int_t      rc;
  ngx_lua_ipc_get_alert_args(L, 0, &name, &data);

  rc = ipc_alert_all_workers(ipc, &name, &data);
  
  lua_pushboolean(L, rc == NGX_OK);
  return 1;
}

static void ngx_lua_ipc_alert_handler(ngx_int_t sender_slot, ngx_str_t *name, ngx_str_t *data) {
  
  ipc_alert_waiting_t *alert;
  
  if(!hacktimer && !running_hacked_timer_handler) {
    //no alert handlers here
    return;
  }
  
  alert = ngx_alloc(sizeof(*alert) + name->len + data->len, ngx_cycle->log);
  assert(alert);
  
  alert->sender_slot = sender_slot;
  alert->sender_pid = ipc_get_pid(ipc, sender_slot);
  
  alert->name.data = (u_char *)&alert[1];
  alert->name.len = name->len;
  ngx_memcpy(alert->name.data, name->data, name->len);
  
  alert->data.data = alert->name.data + alert->name.len;
  alert->data.len = data->len;
  ngx_memcpy(alert->data.data, data->data, data->len);
  
  alert->next = NULL;
  
  if(received_alerts.tail) {
    received_alerts.tail->next = alert;
  }
  received_alerts.tail = alert;
  if(!received_alerts.head) {
    received_alerts.head = alert;
  }
  
  //listener timer now!!
  if(hacktimer && hacktimer->timer.key > ngx_current_msec) {
    DBG("run hacked timer right now: %p", hacktimer);
    
    ngx_del_timer(hacktimer);
    hacktimer->handler(hacktimer);
    //ngx_add_timer(hacktimer, 0); //the slow way to do it -- run it next cycle
  }
  else if(!hacktimer) {
    DBG("timer handler running right now");
    assert(running_hacked_timer_handler == 1);
  }
  else {
    DBG("hacked timer %p already set to run on next cycle", hacktimer);
  }
  return;
  
}

static int ngx_lua_ipc_init_lua_code(lua_State * L) {
  
  int t = lua_gettop(L) + 1;
  lua_createtable(L, 0, 5);
  lua_pushcfunction(L, ngx_lua_ipc_send_alert);
  lua_setfield(L, t, "send");

  lua_pushcfunction(L, ngx_lua_ipc_broadcast_alert);
  lua_setfield(L, t, "broadcast");

  ngx_lua_ipc_loadscript(L, reply);
  lua_pushvalue(L, t);
  lua_call(L, 1, 1);
  lua_setfield(L, t, "reply");
  
  ngx_lua_ipc_loadscript(L, register_receive_handler);
  lua_pushvalue(L, t); //ipc table
  lua_pushcfunction(L, ngx_lua_ipc_hacktimer_run_handler);
  lua_pushcfunction(L, ngx_lua_ipc_hacktimer_add_and_hack);
  lua_pushcfunction(L, ngx_lua_ipc_hacktimer_alert_iterator);
  lua_call(L, 4, 1);
  
  lua_setfield(L, t, "receive");
  
  lua_newtable(L); //handlers table
  lua_setfield(L, t, "handlers");
  return 1;
}

static ngx_int_t ngx_lua_ipc_init_postconfig(ngx_conf_t *cf) {
  
  if (ngx_http_lua_add_package_preload(cf, "ngx.ipc", ngx_lua_ipc_init_lua_code) != NGX_OK) {
    return NGX_ERROR;
  }
  
  return NGX_OK;
}
static ngx_int_t ngx_lua_ipc_init_module(ngx_cycle_t *cycle) {
  
  if(ipc) { //ipc already exists. destroy it!
    ipc_destroy(ipc);
  }
  ipc = ipc_init_module("ngx_lua_ipc", cycle);
  ipc_set_worker_alert_handler(ipc, ngx_lua_ipc_alert_handler);
  
  return NGX_OK;
}

static ngx_int_t ngx_lua_ipc_init_worker(ngx_cycle_t *cycle) {
  return ipc_init_worker(ipc, cycle);
}

static void ngx_lua_ipc_exit_worker(ngx_cycle_t *cycle) { 
  ipc_destroy(ipc);
}

static void ngx_lua_ipc_exit_master(ngx_cycle_t *cycle) {
  ipc_destroy(ipc);
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

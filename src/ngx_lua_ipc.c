#include <ngx_lua_ipc.h>
#include <ipc.h>
//static shmem_t         *shm = NULL;
//static shm_data_t      *shdata = NULL;

static ipc_t            ipc_data;
static ipc_t           *ipc = NULL;


void ngx_lua_ipc_alert_handler(ngx_int_t sender, ngx_uint_t code, void *data) {
  
  
}

/*
static ngx_int_t initialize_shm(ngx_shm_zone_t *zone, void *data) {
  shm_data_t         *d;
  ngx_int_t           i;
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
  
  return NGX_OK;
}
*/

static ngx_int_t ngx_lua_ipc_init_postconfig(ngx_conf_t *cf) {
  /*
  nchan_main_conf_t     *conf = ngx_http_conf_get_module_main_conf(cf, &ngx_lua_ipc_module);
  ngx_str_t              name = ngx_string("ngx_lua_ipc");

  shm = shm_create(&name, cf, 32*1024, initialize_shm, &ngx_lua_ipc_module);
  */
  return NGX_OK;
}

static ngx_int_t ngx_lua_ipc_init_module(ngx_cycle_t *cycle) {
  ngx_core_conf_t                *ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
  
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
  ipc_register_worker(ipc, cycle);
  return NGX_OK;
}

static void ngx_lua_ipc_exit_worker(ngx_cycle_t *cycle) { 
  ipc_close(ipc, cycle);
}

static void ngx_lua_ipc_exit_master(ngx_cycle_t *cycle) {
  ipc_close(ipc, cycle);
  //shm_free(shm, shdata);
  //shm_destroy(shm);
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

#include <ngx_http.h>
#include <nginx.h>


extern ngx_module_t ngx_lua_ipc_module;

typedef struct {
  void            *ptr;
} shm_data_t;

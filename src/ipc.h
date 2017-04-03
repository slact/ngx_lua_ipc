typedef struct ipc_alert_link_s ipc_alert_link_t;
struct ipc_alert_link_s {
  ipc_alert_link_t *next;
  ngx_str_t         buf;
  uint16_t          sent;
};

typedef struct ipc_writebuf_s ipc_writebuf_t;
struct ipc_writebuf_s {
  ipc_alert_link_t         *head;
  ipc_alert_link_t         *tail;
  uint16_t                  n;
}; //ipc_writebuf_t

typedef struct {
  struct {
    size_t   code;
    size_t   name_len;
    uint16_t src_slot;
    uint8_t  separators_seen;
    unsigned complete:1;
  }           header;
  
  ngx_str_t   body;
  
  unsigned    complete:1;
  
  char       *buf;
  char       *buf_last;
  char       *cur;
  char       *last;
  
  size_t      read_next_bytes;
} ipc_readbuf_t;


typedef struct ipc_alert_waiting_s ipc_alert_waiting_t;
struct ipc_alert_waiting_s {
  ngx_int_t             sender_slot;
  ngx_pid_t             sender_pid;
  ngx_str_t             name;
  ngx_str_t             data;
  ipc_alert_waiting_t  *next;
}; //ipc_alert_waiting_t

typedef struct ipc_s ipc_t;

typedef struct {
  ipc_t                 *ipc; //need this backrerefence for write events
  ngx_socket_t           pipe[2];
  ngx_connection_t      *c;
  ipc_writebuf_t         wbuf;
  ipc_readbuf_t          rbuf;
  unsigned               active:1;
} ipc_comm_t;

typedef enum {NGX_IPC_NOTREADY, NGX_IPC_INIT_CONFIG, NGX_IPC_INIT_MODULE, NGX_IPC_INIT_WORKER, NGX_IPC_READY} ipc_status_t;

struct ipc_s {
  ipc_status_t           status;
  const char            *name;
  ngx_shm_zone_t        *shm_zone;
  ipc_comm_t             process[NGX_MAX_PROCESSES];
  ngx_int_t              configured_worker_process_count;
  void                  (*handler)(ngx_int_t, ngx_str_t *, ngx_str_t *);
}; //ipc_t


ngx_int_t ipc_init_config(ipc_t *ipc, ngx_conf_t *cf, ngx_module_t *ipc_owner_module, char *ipc_name);
ngx_int_t ipc_init_module(ipc_t *ipc, ngx_cycle_t *cycle);
ngx_int_t ipc_init_worker(ipc_t *ipc, ngx_cycle_t *cycle);

ngx_int_t ipc_exit_worker(ipc_t *ipc, ngx_cycle_t *cycle);
ngx_int_t ipc_exit_master(ipc_t *ipc, ngx_cycle_t *cycle);

ngx_pid_t ipc_get_pid(ipc_t *ipc, int process_slot);
ngx_int_t ipc_get_slot(ipc_t *ipc, ngx_pid_t pid);

ngx_int_t ipc_set_handler(ipc_t *ipc, void (*alert_handler)(ngx_int_t, ngx_str_t *, ngx_str_t *));

ngx_int_t ipc_alert_slot(ipc_t *ipc, ngx_int_t slot, ngx_str_t *name, ngx_str_t *data);
ngx_int_t ipc_alert_pid(ipc_t *ipc, ngx_pid_t pid, ngx_str_t *name, ngx_str_t *data);
ngx_int_t ipc_alert_all_workers(ipc_t *ipc, ngx_str_t *name, ngx_str_t *data); //poor man's broadcast


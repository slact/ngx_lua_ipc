#define IPC_DATA_SIZE 512
//#define IPC_DATA_SIZE 80

typedef struct {
  uint16_t        sz;
  int16_t         src_slot;
  char           *data;
  uint8_t         code;
} ipc_alert_t;

typedef struct ipc_alert_link_s ipc_alert_link_t;
struct ipc_alert_link_s {
  ipc_alert_t       alert;
  ipc_alert_link_t *next;
  uint16_t          sent;
  unsigned          header_sent:1;
};

#define IPC_WRITEBUF_SIZE 32

typedef struct ipc_writebuf_overflow_s ipc_writebuf_overflow_t;
struct ipc_writebuf_overflow_s {
  ipc_alert_t               alert;
  ipc_writebuf_overflow_t  *next;
};

typedef struct ipc_writebuf_s ipc_writebuf_t;
struct ipc_writebuf_s {
  ipc_alert_link_t         *head;
  ipc_alert_link_t         *tail;
  uint16_t                  n;
}; //ipc_writebuf_t

typedef struct {
  ipc_alert_t alert;
  uint16_t    received;
  unsigned    header_received:1;
  unsigned    complete:1;
} ipc_readbuf_t;

typedef struct ipc_s ipc_t;

typedef struct {
  ipc_t                 *ipc; //useful for write events
  ngx_socket_t           pipe[2];
  ngx_connection_t      *c;
  ipc_writebuf_t         wbuf;
  ipc_readbuf_t          rbuf;
  unsigned               active:1;
} ipc_process_t;

struct ipc_s {
  const char            *name;
  
  ipc_process_t         process[NGX_MAX_PROCESSES];
  
  void                  (*handler)(ngx_int_t, ngx_uint_t, void*, size_t sz);
}; //ipc_t

ngx_int_t ipc_init(ipc_t *ipc);
ngx_int_t ipc_open(ipc_t *ipc, ngx_cycle_t *cycle, ngx_int_t workers, void (*slot_callback)(int slot, int worker));
ngx_int_t ipc_set_handler(ipc_t *ipc, void (*alert_handler)(ngx_int_t, ngx_uint_t , void *data, size_t));
ngx_int_t ipc_register_worker(ipc_t *ipc, ngx_cycle_t *cycle);
ngx_int_t ipc_close(ipc_t *ipc, ngx_cycle_t *cycle);

ngx_int_t ipc_alert(ipc_t *ipc, ngx_int_t slot, ngx_uint_t code,  void *data, size_t data_size);

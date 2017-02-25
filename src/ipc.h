#define IPC_UINT16_MAXLEN (sizeof("65536")-1)
#define IPC_UINT32_MAXLEN (sizeof("4294967295")-1)
#define IPC_MAX_HEADER_LEN (3   + 1 + IPC_UINT16_MAXLEN + 1 + IPC_UINT16_MAXLEN + 1)
#define IPC_MAX_READBUF_LEN 512
// <CODE(uint8)>|<SRC_SLOT(uint16)>|<DATA_LEN(uint16)>|<DATA>
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
  unsigned    complete:1;
  unsigned    header_complete:1;
  uint8_t     separators_seen;
  
  char       *buf;
  char       *buf_last;
  char       *cur;
  char       *last;
  
  size_t      read_next_bytes;
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

//worker processes of the world, unite.
#include <ngx_http.h>

#include <nginx.h>
#include <ngx_channel.h>
#include <assert.h>
#include "ipc.h"

#define IPC_UINT16_MAXLEN (sizeof("65536")-1)
#define IPC_UINT32_MAXLEN (sizeof("4294967295")-1)
#define IPC_MAX_HEADER_LEN (IPC_UINT16_MAXLEN   + 1 + IPC_UINT32_MAXLEN + 1 + IPC_UINT16_MAXLEN + 1)
#define IPC_MAX_READBUF_LEN 512
// <SRC_SLOT(uint16)>|<NAME&DATA_LEN(uint32)>|<NAME_LEN(uint16)>|<NAME><DATA>


#define DEBUG_LEVEL NGX_LOG_DEBUG
//#define DEBUG_LEVEL NGX_LOG_WARN

#define DBG(fmt, args...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "IPC:" fmt, ##args)
#define ERR(fmt, args...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "IPC:" fmt, ##args)



//shared memory stuff
typedef struct {
  ngx_int_t      pid;
  ngx_int_t      slot;
} worker_slot_tracking_t;

typedef struct {
  worker_slot_tracking_t   *worker_slots;
} ipc_shm_data_t;

static ngx_int_t shm_init_callback(ngx_shm_zone_t *zone, void *data) {
  ipc_shm_data_t         *d;
  if(data) { //zone being passed after restart
    zone->data = data;
    d = zone->data;
  }
  else {
    ngx_slab_pool_t    *shpool = (ngx_slab_pool_t *)zone->shm.addr;
    ngx_slab_init(shpool);
    
    
    if((d = ngx_slab_alloc(shpool, sizeof(*d))) != NULL) {
      ngx_memzero(d, sizeof(*d));
    }
    else {
      return NGX_ERROR;
    }
    
    zone->data = d;
  }
  return NGX_OK;
}

static ngx_shm_zone_t *ipc_shm_create(char *name, ngx_module_t *module, ngx_conf_t *cf, size_t shm_size, ngx_int_t (*init)(ngx_shm_zone_t *, void *)) {
  u_char                          zone_name_buf[1024];
  ngx_snprintf(zone_name_buf, 1024, "ngx_ipc: %s", name);
  ngx_str_t                       zone_name = ngx_string(zone_name_buf);

  ngx_shm_zone_t    *shm_zone;

  shm_size = ngx_align(shm_size, ngx_pagesize);
  if (shm_size < ngx_pagesize) {
    shm_size = ngx_pagesize;
  }
  shm_zone = ngx_shared_memory_add(cf, &zone_name, shm_size, module);
  if (shm_zone == NULL) {
    return NULL;
  }
  shm_zone->init = shm_init_callback;
  shm_zone->data = (void *) 1;
  return shm_zone;
}
//end shared memory stuff



static void ipc_worker_read_handler(ngx_event_t *ev);
static ngx_int_t ipc_free_buffered_alert(ipc_alert_link_t *alert_link);
static ngx_int_t parsebuf_reset_readbuf(ipc_readbuf_t *rbuf);

static ngx_int_t ipc_open(ipc_t *ipc, ngx_cycle_t *cycle, ngx_int_t workers, void (*slot_callback)(int slot, int worker));
static ngx_int_t ipc_register_worker(ipc_t *ipc, ngx_cycle_t *cycle);
static ngx_int_t ipc_close(ipc_t *ipc, ngx_cycle_t *cycle);



static ngx_int_t ipc_init_channel(ipc_t *ipc, ipc_channel_t *chan) {
  chan->ipc = ipc;
  chan->pipe[0]=NGX_INVALID_FILE;
  chan->pipe[1]=NGX_INVALID_FILE;
  chan->read_conn = NULL;
  chan->write_conn = NULL;
  chan->active = 0;
  chan->wbuf.head = NULL;
  chan->wbuf.tail = NULL;
  chan->wbuf.n = 0;
  ngx_memzero(&chan->rbuf, sizeof(chan->rbuf));
  return NGX_OK;
}

ngx_int_t ipc_init_config(ipc_t *ipc, ngx_conf_t *cf, ngx_module_t *module, char *name) {
  int                             i = 0;
  for(i=0; i< NGX_MAX_PROCESSES; i++) {
    ipc_init_channel(ipc, &ipc->worker_channel[i]);
  }
  
  ipc->shm_zone = ipc_shm_create(name, module, cf, NGX_MAX_PROCESSES * sizeof(worker_slot_tracking_t) * 2, shm_init_callback);
  if(!ipc->shm_zone) { 
    return NGX_ERROR;
  }
  
  ipc->name = name;
  ipc->worker_process_count = NGX_ERROR;
  
  return NGX_OK;
}

ngx_int_t ipc_init_module(ipc_t *ipc, ngx_cycle_t *cycle) {
  ngx_core_conf_t                *ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
  ngx_int_t                       max_workers = ccf->worker_processes;
  ngx_slab_pool_t                *shpool = (ngx_slab_pool_t *)ipc->shm_zone->shm.addr;
  ipc_shm_data_t                 *shdata = ipc->shm_zone->data;
  size_t                          workerslots_sz = sizeof(*shdata->worker_slots) * max_workers;
  
  ipc->worker_process_count = max_workers;
  
  ngx_shmtx_lock(&shpool->mutex);
  if(shdata->worker_slots) {
    ngx_slab_free_locked(shpool, shdata->worker_slots);
    shdata->worker_slots = NULL;
  }
  
  if((shdata->worker_slots = ngx_slab_alloc_locked(shpool, workerslots_sz)) != NULL) {
    ngx_memzero(shdata->worker_slots, workerslots_sz);
  }
  
  ngx_shmtx_unlock(&shpool->mutex);
  
  ipc_open(ipc, cycle, max_workers, NULL);
  return NGX_OK;
}

ngx_int_t ipc_init_worker(ipc_t *ipc, ngx_cycle_t *cycle) {
  ngx_slab_pool_t                *shpool = (ngx_slab_pool_t *)ipc->shm_zone->shm.addr;
  ipc_shm_data_t                 *shdata = ipc->shm_zone->data;
  ngx_int_t                       max_workers = ipc->worker_process_count;
  int                             i, found = 0;
  worker_slot_tracking_t         *workerslot;
  
  if (ngx_process != NGX_PROCESS_WORKER && ngx_process != NGX_PROCESS_SINGLE) {
    //not a worker, stop initializing stuff.
    return NGX_OK;
  }
  
  ngx_shmtx_lock(&shpool->mutex);
  
  for(i=0; !found && i<max_workers; i++) {
    workerslot = &shdata->worker_slots[i];
    if( workerslot->pid == 0) {
      // empty workerslot
      found = 1;
    }
    else if(workerslot->slot == ngx_process_slot) {
      // replacing previously crashed(?) worker
      found = 1;
    }
  }
  
  ngx_shmtx_unlock(&shpool->mutex);
  
  if(found) {
    workerslot->pid = ngx_pid;
    workerslot->slot = ngx_process_slot; 
  }
  else {
    return NGX_ERROR;
  }
  ipc_register_worker(ipc, cycle);
  
  return NGX_OK;  
}


ngx_int_t ipc_exit_worker(ipc_t *ipc, ngx_cycle_t *cycle) {
  return ipc_close(ipc, cycle);
}
ngx_int_t ipc_exit_master(ipc_t *ipc, ngx_cycle_t *cycle) {
  ngx_int_t            rc;
  ngx_slab_pool_t     *shpool = (ngx_slab_pool_t *)ipc->shm_zone->shm.addr;
  ipc_shm_data_t      *shdata = ipc->shm_zone->data;
  
  rc = ipc_close(ipc, cycle);
  ngx_slab_free(shpool, shdata);
  return rc;
}

ngx_pid_t ipc_get_pid(ipc_t *ipc, int process_slot) {
  ipc_shm_data_t         *shdata = ipc->shm_zone->data;
  int                     max_workers = ipc->worker_process_count;
  int                     i;
  worker_slot_tracking_t *worker_slots = shdata->worker_slots;
  
  for(i=0; i<max_workers; i++) {
    if(worker_slots[i].slot == process_slot) {
      return worker_slots[i].pid;
    }
  }
  return NGX_INVALID_PID;
}
ngx_int_t ipc_get_slot(ipc_t *ipc, ngx_pid_t pid) {
  ipc_shm_data_t         *shdata = ipc->shm_zone->data;
  int                     max_workers = ipc->worker_process_count;
  int                     i;
  worker_slot_tracking_t *worker_slots = shdata->worker_slots;
  
  for(i=0; i<max_workers; i++) {
    if(worker_slots[i].pid == pid) {
      return worker_slots[i].slot;
    }
  }
  return NGX_ERROR;
}

ngx_int_t ipc_set_worker_alert_handler(ipc_t *ipc, ipc_alert_handler_pt alert_handler) {
  ipc->worker_alert_handler=alert_handler;
  return NGX_OK;
}

static void ipc_try_close_fd(ngx_socket_t *fd) {
  if(*fd != NGX_INVALID_FILE) {
    ngx_close_socket(*fd);
    *fd=NGX_INVALID_FILE;
  }
}

static ngx_int_t ipc_activate_channel(ipc_t *ipc, ngx_cycle_t *cycle, ipc_channel_t *channel, ipc_socket_type_t socktype) {
  int                             rc;
  ngx_socket_t                   *socks = channel->pipe;
  if(channel->active) {
    // reinitialize already active pipes. This is done to prevent IPC alerts
    // from a previous restart that were never read from being received by
    // a newly restarted worker
    ipc_try_close_fd(&socks[0]);
    ipc_try_close_fd(&socks[1]);
    channel->active = 0;
  }
  
  assert(socks[0] == NGX_INVALID_FILE && socks[1] == NGX_INVALID_FILE);
  
  channel->socket_type = socktype;
  if(socktype == IPC_PIPE) {
    //make-a-pipe
    rc = pipe(socks);
  }
  else if(socktype == IPC_SOCKETPAIR) {
    rc = socketpair(AF_LOCAL, SOCK_STREAM, 0, socks);
  }
  
  if(rc == -1) {
    ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno, "pipe() failed while initializing IPC %s", ipc->name);
    return NGX_ERROR;
  }
  //make both ends nonblocking
  if (ngx_nonblocking(socks[0]) == -1 || ngx_nonblocking(socks[1]) == -1) {
    ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno, ngx_nonblocking_n " failed on pipe socket %i while initializing IPC %s", ipc->name);
    ipc_try_close_fd(&socks[0]);
    ipc_try_close_fd(&socks[1]);
    return NGX_ERROR;
  }
  //It's ALIIIIIVE! ... erm.. active...
  channel->active = 1;
  
  return NGX_OK;
}

static ngx_int_t ipc_open(ipc_t *ipc, ngx_cycle_t *cycle, ngx_int_t workers, void (*slot_callback)(int slot, int worker)) {
//initialize pipes for workers in advance.
  int                             i, s = 0;
  ngx_int_t                       last_expected_process = ngx_last_process;
  ipc_channel_t                  *worker_channel;
  
  /* here's the deal: we have no control over fork()ing, nginx's internal 
    * socketpairs are unusable for our purposes (as of nginx 0.8 -- check the 
    * code to see why), and the module initialization callbacks occur before
    * any workers are spawned. Rather than futzing around with existing 
    * socketpairs, we make our own pipes array. 
    * Trouble is, ngx_spawn_process() creates them one-by-one, and we need to 
    * do it all at once. So we must guess all the workers' ngx_process_slots in 
    * advance. Meaning the spawning logic must be copied to the T.
    * ... with some allowances for already-opened sockets...
    */
  for(i=0; i < workers; i++) {
    //copypasta from os/unix/ngx_process.c (ngx_spawn_process)
    while (s < last_expected_process && ngx_processes[s].pid != -1) {
      //find empty existing slot
      s++;
    }
    
    if(slot_callback) {
      slot_callback(s, i);
    }
    
    worker_channel = &ipc->worker_channel[s];

    if(ipc_activate_channel(ipc, cycle, worker_channel, IPC_PIPE) != NGX_OK) {
      return NGX_ERROR;
    }
    
    s++; //NEXT!!
  }
  
  return NGX_OK;
}

static ngx_int_t ipc_close_channel(ipc_channel_t *chan) {
  ipc_alert_link_t         *cur, *cur_next;
  if(!chan->active) {
    return NGX_OK;
  }
    
  if(chan->read_conn) {
    ngx_close_connection(chan->read_conn);
    chan->read_conn = NULL;
  }
  if(chan->write_conn) {
    ngx_close_connection(chan->write_conn);
    chan->write_conn = NULL;
  }
  
  for(cur = chan->wbuf.head; cur != NULL; cur = cur_next) {
    cur_next = cur->next;
    ipc_free_buffered_alert(cur);
  }
  
  if(chan->rbuf.buf) {
    free(chan->rbuf.buf);
    ngx_memzero(&chan->rbuf, sizeof(chan->rbuf));
  }
  
  ipc_try_close_fd(&chan->pipe[0]);
  ipc_try_close_fd(&chan->pipe[1]);
  
  return NGX_OK;
}

ngx_int_t ipc_close(ipc_t *ipc, ngx_cycle_t *cycle) {
  int i;
  
  for (i=0; i<NGX_MAX_PROCESSES; i++) {
    ipc_close_channel(&ipc->worker_channel[i]);
    ipc->worker_channel[i].active = 0;
  }
  return NGX_OK;
}


static ngx_int_t ipc_write_buffered_alert(ngx_socket_t fd, ipc_alert_link_t *alert) {
  int          n;
  ngx_int_t    err;
  uint16_t     unsent;
  u_char      *data;
 
  unsent = alert->buf.len - alert->sent;
  data = &alert->buf.data[alert->sent];
  
  n = write(fd, data, unsent);
  if (n == -1) {
    err = ngx_errno;
    if (err == NGX_EAGAIN) {
      //ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, err, "write() EAGAINED...");
      return NGX_AGAIN;
    }
    
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, err, "write() failed");
    assert(0);
    return NGX_ERROR;
  }
  else if (n < unsent) {
    alert->sent += n;
    return NGX_AGAIN;
  }
  
  return NGX_OK;
}

static ngx_int_t ipc_free_buffered_alert(ipc_alert_link_t *alert_link) {
  //ngx_free(alert_link->alert.data);
  ngx_free(alert_link);
  return NGX_OK;
}

static void ipc_write_handler(ngx_event_t *ev) {
  ngx_connection_t        *c = ev->data;
  ngx_socket_t             fd = c->fd;
  
  ipc_channel_t           *chan = c->data;
  ipc_alert_link_t        *cur;
  
  ngx_int_t                rc;
  
  uint8_t                  write_aborted = 0;
  while((cur = chan->wbuf.head) != NULL) {
    rc = ipc_write_buffered_alert(fd, cur);
    
    if(rc == NGX_EAGAIN) {
      write_aborted = 1;
      break;
    }
    else if(rc == NGX_OK) {
      chan->wbuf.head = cur->next;
      if(chan->wbuf.tail == cur) {
        chan->wbuf.tail = NULL;
      }
      ipc_free_buffered_alert(cur);
    }
    else {
      //we got other problems
      write_aborted = 1;
      break;
    }
  }
  
  if(write_aborted) {
    //re-add event because the write failed
    ngx_handle_write_event(c->write, 0);
  }
  else {
    assert(chan->wbuf.head == NULL);
    assert(chan->wbuf.tail == NULL);
  }
}


typedef enum {IPC_CONN_READ, IPC_CONN_WRITE} ipc_conn_type_t;
static ngx_int_t ipc_channel_setup_conn(ipc_channel_t *chan, ngx_cycle_t *cycle, ipc_conn_type_t conn_type, void (*event_handler)(ngx_event_t *), void *data) {
  ngx_connection_t      *c; 
  //set up read connection
  c = ngx_get_connection(chan->pipe[conn_type == IPC_CONN_READ ? 0 : 1], cycle->log);
  c->data = data;
  
  if(conn_type == IPC_CONN_READ) {
    c->read->handler = event_handler;
    c->read->log = cycle->log;
    c->write->handler = NULL;
    ngx_add_event(c->read, NGX_READ_EVENT, 0);
    chan->read_conn=c;
    parsebuf_reset_readbuf(&chan->rbuf);
  }
  else if(conn_type == IPC_CONN_WRITE) {
    c->read->handler = NULL;
    c->write->log = cycle->log;
    c->write->handler = ipc_write_handler;
    chan->write_conn=c;
  }
  else {
    return NGX_ERROR;
  }
  return NGX_OK;
}

static ngx_int_t ipc_register_worker(ipc_t *ipc, ngx_cycle_t *cycle) {
  int                    i;    
  ipc_channel_t         *chan;
  
  for(i=0; i< NGX_MAX_PROCESSES; i++) {
    
    chan = &ipc->worker_channel[i];
    
    if(!chan->active) continue;
    
    assert(chan->pipe[0] != NGX_INVALID_FILE);
    assert(chan->pipe[1] != NGX_INVALID_FILE);
    
    if(i==ngx_process_slot) {
      //set up read connection
      ipc_channel_setup_conn(chan, cycle, IPC_CONN_READ, ipc_worker_read_handler, ipc);
    }
    else {
      //set up write connection
      ipc_channel_setup_conn(chan, cycle, IPC_CONN_WRITE, ipc_write_handler, chan);
    }
  }
  
  return NGX_OK;
}

static void alloc_buf(ipc_readbuf_t *rbuf, size_t size) {
  rbuf->buf = malloc(size);
  rbuf->buf_last = rbuf->buf + size;
  rbuf->cur = rbuf->buf;
  rbuf->last = rbuf->buf;
}

static void alloc_buf_copy(ipc_readbuf_t *rbuf, size_t size) {
  char   *oldbuf = rbuf->buf, *cur = rbuf->cur;
  size_t  oldsz =rbuf->last - cur;
  assert(size > oldsz);
  alloc_buf(rbuf, size);
  if(oldsz > 0) {
    memcpy(rbuf->cur, cur, oldsz);
  }
  rbuf->last = rbuf->cur + oldsz;  
  free(oldbuf);
}

static ngx_int_t parsebuf_reset_readbuf(ipc_readbuf_t *rbuf) {
  
  ngx_memzero(&rbuf->header, sizeof(rbuf->header));
  ngx_memzero(&rbuf->body, sizeof(rbuf->body));
  
  rbuf->complete = 0;
  
  if(rbuf->buf) {
    if(rbuf->last == rbuf->cur) {
      if(rbuf->buf_last - rbuf->buf > IPC_MAX_READBUF_LEN) {
        DBG("parsebuf_reset %p: remove large old buf & rewind", rbuf);
        free(rbuf->buf);
        alloc_buf(rbuf, IPC_MAX_READBUF_LEN);
      }
      else if(rbuf->buf_last - rbuf->buf < IPC_MAX_READBUF_LEN) {
        DBG("parsebuf_reset %p: remove small old buf & rewind", rbuf);
        free(rbuf->buf);
        alloc_buf(rbuf, IPC_MAX_READBUF_LEN);
      }
      else {
        DBG("parsebuf_reset %p: rewind buf", rbuf);
        rbuf->cur = rbuf->buf;
        rbuf->last = rbuf->buf;
      }
      rbuf->read_next_bytes = IPC_MAX_READBUF_LEN;
    }
    else {
      DBG("parsebuf_reset %p: there's more data to parse", rbuf);
      assert(rbuf->last > rbuf->cur);
      rbuf->read_next_bytes = rbuf->buf_last - rbuf->last;
      return NGX_AGAIN;
    }
  }
  else {
    DBG("parsebuf_reset %p: intialize buf", rbuf);
    rbuf->read_next_bytes = IPC_MAX_READBUF_LEN;
    alloc_buf(rbuf, IPC_MAX_READBUF_LEN);
  }
  
  return NGX_OK;
}

static ngx_int_t parsebuf_need_data(ipc_readbuf_t *rbuf) {
  assert(rbuf->header.complete);
  
  size_t sz = rbuf->last - rbuf->cur, freesz = rbuf->last - rbuf->buf_last;
  
  if(rbuf->body.len <= sz){
    //we already have the alert data
    rbuf->read_next_bytes = 0;
    return NGX_AGAIN;
  }
  else if(rbuf->body.len <= sz + freesz) {
    // maybe have some data, but not enough
    rbuf->read_next_bytes = rbuf->body.len - sz;
    return NGX_OK;
  }
  else {
    alloc_buf_copy(rbuf, rbuf->body.len);
    rbuf->read_next_bytes = rbuf->body.len - (rbuf->last - rbuf->cur);
    return NGX_OK;
  }
}


static ngx_int_t parsebuf(ipc_readbuf_t *rbuf, ipc_alert_handler_pt handler) {
  char *cur = rbuf->cur;
  char *last = rbuf->last;
  size_t used;
  
  if(!rbuf->header.complete) {
    cur = (char *)ngx_strlchr((u_char *)cur, (u_char *)last, '|');
    if(!cur) {
      //need more data
      rbuf->read_next_bytes = rbuf->buf_last - rbuf->last;
      assert(rbuf->buf_last - rbuf->last >= (ssize_t )rbuf->read_next_bytes);
      if(rbuf->read_next_bytes == 0) { // no space to read data
        alloc_buf_copy(rbuf, IPC_MAX_READBUF_LEN);
        rbuf->read_next_bytes = rbuf->buf_last - rbuf->last;
        assert(rbuf->read_next_bytes != 0);
      }
      return NGX_OK;
    }
    else if(cur) {
      *cur='\0';
      if(rbuf->header.separators_seen == 0){
        rbuf->header.src_slot = atoi(rbuf->cur);
      }
      else if(rbuf->header.separators_seen == 1){
        rbuf->body.len = atoi(rbuf->cur);
      }
      else if(rbuf->header.separators_seen == 2){
        rbuf->header.name_len = atoi(rbuf->cur);
        rbuf->header.complete = 1;
      }
      *cur='|'; //change it back for debugging
      rbuf->header.separators_seen ++;
      
      used = (cur+1) - rbuf->cur;
      rbuf->cur += used;
      
      return NGX_AGAIN;
    }
  }
  else {
    if((ssize_t )rbuf->body.len <= rbuf->last - rbuf->cur) {
      ngx_str_t name, data;
      
      rbuf->body.data = (u_char *)cur;
      
      name.data = rbuf->body.data;
      name.len = rbuf->header.name_len;
      
      data.data = name.data + name.len;
      data.len = rbuf->body.len - name.len;
      
      handler(rbuf->header.src_slot, &name, &data);
      rbuf->cur += rbuf->body.len;
      return parsebuf_reset_readbuf(rbuf);
    }
    else {
      return parsebuf_need_data(rbuf);
    }
  }
  return NGX_OK;
}

static ngx_int_t ipc_read(ipc_channel_t *ipc_channel, ipc_readbuf_t *rbuf, ipc_alert_handler_pt handler, ngx_log_t *log) {
  ssize_t             n;
  ngx_err_t           err;
  ngx_int_t           rc;
  ngx_socket_t        s = ipc_channel->read_conn->fd;
  
  DBG("IPC read at most %i bytes", rbuf->read_next_bytes);
  
  while(rbuf->read_next_bytes > 0) {
    assert(rbuf->buf_last - rbuf->last >= (ssize_t )rbuf->read_next_bytes);
    
    n = read(s, rbuf->last, rbuf->read_next_bytes);
    DBG("...actually read %i", n);
    if (n == -1) {
      err = ngx_errno;
      if (err == NGX_EAGAIN) {
        return NGX_AGAIN;
      }
      
      ngx_log_error(NGX_LOG_ERR, log, err, "nchan IPC: read() failed");
      return NGX_ERROR;
    } 
    else if (n == 0) {
      ngx_log_debug0(NGX_LOG_ERR, log, 0, "nchan IPC: read() returned zero");
      return NGX_ERROR;
    }
    else {
      rbuf->last += n;
      do {
        rc = parsebuf(&ipc_channel->rbuf, handler);
      } while(rc == NGX_AGAIN);
    }
  }
  
  return NGX_OK;
}

static void ipc_worker_read_handler(ngx_event_t *ev) {
  ngx_int_t          rc;
  ngx_connection_t  *c;
  ipc_channel_t     *ipc_channel;
  ipc_t             *ipc;
  
  if (ev->timedout) {
    ev->timedout = 0;
    return;
  }
  c = ev->data;
  ipc = c->data;
  ipc_channel = &ipc->worker_channel[ngx_process_slot];
  
  rc = ipc_read(ipc_channel, &ipc_channel->rbuf, ipc->worker_alert_handler, ev->log);
  if (rc == NGX_ERROR) {
    ERR("IPC_READ_SOCKET failed: bad connection. This should never have happened, yet here we are...");
    assert(0);
    return;
  }
  else if (rc == NGX_AGAIN) {
    return;
  }
}
  
// This is what an alert string looks like:
// <SRC_SLOT(uint16)>|<NAME&DATA_LEN(uint32)>|<NAME_LEN(uint16)>|<NAME><DATA>
static ngx_int_t ipc_alert_channel(ipc_channel_t *chan, ngx_str_t *name, ngx_str_t *data) {
  ipc_alert_link_t   *alert;
  ipc_writebuf_t     *wb = &chan->wbuf;
  size_t              alert_str_size = 0;
  u_char             *end;
  
  if(!chan->active) {
    return NGX_ERROR;
  }

  alert_str_size +=   IPC_MAX_HEADER_LEN + data->len + name->len;
  if((alert = ngx_alloc(sizeof(*alert) + alert_str_size, ngx_cycle->log)) == NULL) {
    // nomem
    return NGX_ERROR;
  }
  alert->next = NULL;
  alert->sent = 0;
  
  alert->buf.data = (u_char *)&alert[1];
  
  end = ngx_snprintf(alert->buf.data, alert_str_size, "%i|%i|%i|%V%V", ngx_process_slot, data->len + name->len, name->len, name, data);
  
  alert->buf.len = end - alert->buf.data;
  
  if(wb->tail != NULL) {
    wb->tail->next = alert;
  }
  wb->tail = alert;
  if(wb->head == NULL) {
    wb->head = alert;
  }
  ipc_write_handler(chan->write_conn->write);
  
  //ngx_handle_write_event(ipc->c[slot]->write, 0);
  //ngx_add_event(ipc->c[slot]->write, NGX_WRITE_EVENT, NGX_CLEAR_EVENT);

  return NGX_OK;
}

ngx_int_t ipc_alert_slot(ipc_t *ipc, ngx_int_t slot, ngx_str_t *name, ngx_str_t *data) {
  DBG("IPC send alert '%V' to slot %i", name, slot);
  
  ngx_str_t           empty = {0, NULL};
  if(!name) name = &empty;
  if(!data) data = &empty;
  
  if(slot == ngx_process_slot) {
    ipc->worker_alert_handler(slot, name, data);
    return NGX_OK;
  }
  return ipc_alert_channel(&ipc->worker_channel[slot], name, data);
}


ngx_int_t ipc_alert_pid(ipc_t *ipc, ngx_pid_t worker_pid, ngx_str_t *name, ngx_str_t *data) {
  ngx_int_t slot = ipc_get_slot(ipc, worker_pid);
  if(slot == NGX_ERROR) {
    return NGX_ERROR;
  }
  return ipc_alert_slot(ipc, slot, name, data);
}

ngx_int_t ipc_alert_all_workers(ipc_t *ipc, ngx_str_t *name, ngx_str_t *data) {
  ipc_shm_data_t         *shdata = ipc->shm_zone->data;
  int                     max_workers = ipc->worker_process_count;
  int                     i;
  worker_slot_tracking_t *worker_slots = shdata->worker_slots;
  
  for(i=0; i<max_workers; i++) {
    ipc_alert_slot(ipc, worker_slots[i].slot, name, data);
  }
  return NGX_OK;
}


//worker processes of the world, unite.
#include <ngx_lua_ipc.h>
#include <ngx_channel.h>
#include <assert.h>
#include "ipc.h"

#define DEBUG_LEVEL NGX_LOG_DEBUG
//#define DEBUG_LEVEL NGX_LOG_WARN

#define DBG(fmt, args...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "IPC:" fmt, ##args)
#define ERR(fmt, args...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "IPC:" fmt, ##args)


static void ipc_read_handler(ngx_event_t *ev);
static ngx_int_t ipc_free_buffered_alert(ipc_alert_link_t *alert_link);
static ngx_int_t parsebuf_reset_readbuf(ipc_readbuf_t *rbuf);

ngx_int_t ipc_init(ipc_t *ipc) {
  int                             i = 0;
  ipc_process_t                  *proc;
  
  for(i=0; i< NGX_MAX_PROCESSES; i++) {
    proc = &ipc->process[i];
    proc->ipc = ipc;
    proc->pipe[0]=NGX_INVALID_FILE;
    proc->pipe[1]=NGX_INVALID_FILE;
    proc->c=NULL;
    proc->active = 0;
    proc->wbuf.head = NULL;
    proc->wbuf.tail = NULL;
    proc->wbuf.n = 0;
    parsebuf_reset_readbuf(&proc->rbuf);
  }
  return NGX_OK;
}

ngx_int_t ipc_set_handler(ipc_t *ipc, void (*alert_handler)(ngx_int_t, ngx_uint_t, void *, size_t)) {
  ipc->handler=alert_handler;
  return NGX_OK;
}

static void ipc_try_close_fd(ngx_socket_t *fd) {
  if(*fd != NGX_INVALID_FILE) {
    ngx_close_socket(*fd);
    *fd=NGX_INVALID_FILE;
  }
}

ngx_int_t ipc_open(ipc_t *ipc, ngx_cycle_t *cycle, ngx_int_t workers, void (*slot_callback)(int slot, int worker)) {
//initialize pipes for workers in advance.
  int                             i, j, s = 0;
  ngx_int_t                       last_expected_process = ngx_last_process;
  ipc_process_t                  *proc;
  ngx_socket_t                   *socks;
  
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
    
    proc = &ipc->process[s];

    socks = proc->pipe;
    
    if(proc->active) {
      // reinitialize already active pipes. This is done to prevent IPC alerts
      // from a previous restart that were never read from being received by
      // a newly restarted worker
      ipc_try_close_fd(&socks[0]);
      ipc_try_close_fd(&socks[1]);
      proc->active = 0;
    }
    
    assert(socks[0] == NGX_INVALID_FILE && socks[1] == NGX_INVALID_FILE);
    
    //make-a-pipe
    if (pipe(socks) == -1) {
      ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno, "pipe() failed while initializing nchan IPC");
      return NGX_ERROR;
    }
    //make both ends nonblocking
    for(j=0; j <= 1; j++) {
      if (ngx_nonblocking(socks[j]) == -1) {
	ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno, ngx_nonblocking_n " failed on pipe socket %i while initializing nchan", j);
	ipc_try_close_fd(&socks[0]);
	ipc_try_close_fd(&socks[1]);
	return NGX_ERROR;
      }
    }
    //It's ALIIIIIVE! ... erm.. active...
    proc->active = 1;
    
    s++; //NEXT!!
  }
  
  //ERR("ipc_alert_t size %i bytes", sizeof(ipc_alert_t));
  
  return NGX_OK;
}



ngx_int_t ipc_close(ipc_t *ipc, ngx_cycle_t *cycle) {
  int i;
  ipc_process_t            *proc;
  ipc_alert_link_t         *cur, *cur_next;
  
  for (i=0; i<NGX_MAX_PROCESSES; i++) {
    proc = &ipc->process[i];
    if(!proc->active) continue;
    
    if(proc->c) {
      ngx_close_connection(proc->c);
      proc->c = NULL;
    }
    
    for(cur = proc->wbuf.head; cur != NULL; cur = cur_next) {
      cur_next = cur->next;
      ipc_free_buffered_alert(cur);
    }
    
    if(proc->rbuf.buf) {
      free(proc->rbuf.buf);
      ngx_memzero(&proc->rbuf, sizeof(proc->rbuf));
    }
    
    ipc_try_close_fd(&proc->pipe[0]);
    ipc_try_close_fd(&proc->pipe[1]);
    ipc->process[i].active = 0;
  }
  return NGX_OK;
}


static ngx_int_t ipc_write_buffered_alert(ngx_socket_t fd, ipc_alert_link_t *alert_link) {
  int          n;
  ngx_int_t    err;
  uint16_t     unsent;
  ipc_alert_t *alert = &alert_link->alert;
  char        *data;
 
  unsent = alert->sz - alert_link->sent;
  data = &alert->data[alert_link->sent];
  
  n = write(fd, data, unsent);
  if (n == -1) {
    err = ngx_errno;
    if (err == NGX_EAGAIN) {
      ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, err, "write() EAGAINED...");
      return NGX_AGAIN;
    }
    
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, err, "write() failed");
    assert(0);
    return NGX_ERROR;
  }
  else if (n < unsent) {
    alert_link->sent += n;
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
  
  ipc_process_t           *proc = (ipc_process_t *) c->data;
  ipc_alert_link_t        *cur;
  
  ngx_int_t                rc;
  
  uint8_t                  write_aborted = 0;
  
  while((cur = proc->wbuf.head) != NULL) {
    rc = ipc_write_buffered_alert(fd, cur);
    
    if(rc == NGX_EAGAIN) {
      write_aborted = 1;
      break;
    }
    else if(rc == NGX_OK) {
      proc->wbuf.head = cur->next;
      if(proc->wbuf.tail == cur) {
        proc->wbuf.tail = NULL;
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
    //DBG("re-add event because the write failed");
    ngx_handle_write_event(c->write, 0);
  }
  else {
    assert(proc->wbuf.head == NULL);
    assert(proc->wbuf.tail == NULL);
  }
}

ngx_int_t ipc_register_worker(ipc_t *ipc, ngx_cycle_t *cycle) {
  int                    i;    
  ngx_connection_t      *c;
  ipc_process_t         *proc;
  
  for(i=0; i< NGX_MAX_PROCESSES; i++) {
    
    proc = &ipc->process[i];
    
    if(!proc->active) continue;
    
    assert(proc->pipe[0] != NGX_INVALID_FILE);
    assert(proc->pipe[1] != NGX_INVALID_FILE);
    
    if(i==ngx_process_slot) {
      //set up read connection
      c = ngx_get_connection(proc->pipe[0], cycle->log);
      c->data = ipc;
      
      c->read->handler = ipc_read_handler;
      c->read->log = cycle->log;
      c->write->handler = NULL;
      
      ngx_add_event(c->read, NGX_READ_EVENT, 0);
      proc->c=c;
    }
    else {
      //set up write connection
      c = ngx_get_connection(proc->pipe[1], cycle->log);
      
      c->data = proc;
      
      c->read->handler = NULL;
      c->write->log = cycle->log;
      c->write->handler = ipc_write_handler;
      
      proc->c=c;
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
  memcpy(rbuf->cur, cur, oldsz);
  rbuf->last = rbuf->cur + oldsz;  
  free(oldbuf);
}

static ngx_int_t parsebuf_reset_readbuf(ipc_readbuf_t *rbuf) {
  
  rbuf->alert.sz = 0;
  rbuf->alert.src_slot = 0;
  rbuf->alert.data = NULL;
  rbuf->alert.code = 0;
  
  rbuf->received = 0;
  rbuf->complete = 0;
  rbuf->header_complete = 0;
  rbuf->separators_seen = 0;
  
  if(rbuf->buf) {
    if(rbuf->last == rbuf->cur) {
      if(rbuf->buf_last - rbuf->buf > IPC_MAX_READBUF_LEN) {
        DBG("parsebuf_reset %p: remove large old buf & rewind", rbuf);
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
  assert(rbuf->header_complete);
  
  size_t sz = rbuf->last - rbuf->cur, freesz = rbuf->last - rbuf->buf_last;
  
  if(rbuf->alert.sz <= sz){
    //we already have the alert data
    rbuf->read_next_bytes = 0;
    return NGX_AGAIN;
  }
  else if(rbuf->alert.sz <= sz + freesz) {
    // maybe have some data, but not enough
    rbuf->read_next_bytes = rbuf->alert.sz - sz;
    return NGX_OK;
  }
  else {
    alloc_buf_copy(rbuf, rbuf->alert.sz);
    rbuf->read_next_bytes = rbuf->alert.sz - (rbuf->last - rbuf->cur);
    return NGX_OK;
  }
}


static ngx_int_t parsebuf(ipc_t *ipc, ipc_readbuf_t *rbuf) {
  char *cur = rbuf->cur;
  char *last = rbuf->last;
  size_t used;
  
  if(!rbuf->header_complete) {
    cur = (char *)ngx_strlchr((u_char *)cur, (u_char *)last, '|');
    if(!cur) {
      //need more data
      rbuf->read_next_bytes = rbuf->buf_last - rbuf->last;
      assert(rbuf->read_next_bytes > 0);
      return NGX_OK;
    }
    else if(cur) {
      *cur='\0';
      if(rbuf->separators_seen == 0){
        rbuf->alert.code = atoi(rbuf->cur);
      }
      else if(rbuf->separators_seen == 1){
        rbuf->alert.src_slot = atoi(rbuf->cur);
      }
      else if(rbuf->separators_seen == 2){
        rbuf->alert.sz = atoi(rbuf->cur);
        rbuf->header_complete = 1;
      }
      *cur='|'; //change it back for debugging
      rbuf->separators_seen ++;
      
      used = (cur+1) - rbuf->cur;
      rbuf->cur += used;
      
      return NGX_AGAIN;
    }
  }
  else {
    if(rbuf->alert.sz <= rbuf->last - rbuf->cur) {
      ipc->handler(rbuf->alert.src_slot, rbuf->alert.code, rbuf->cur, rbuf->alert.sz);
      rbuf->cur += rbuf->alert.sz;
      return parsebuf_reset_readbuf(rbuf);
    }
    else {
      return parsebuf_need_data(rbuf);
    }
  }
  return NGX_OK;
}

static ngx_int_t ipc_read(ipc_process_t *ipc_proc, ipc_readbuf_t *rbuf, ngx_log_t *log) {
  ssize_t             n;
  ngx_err_t           err;
  ngx_int_t           rc;
  ngx_socket_t        s = ipc_proc->c->fd;
  
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
        rc = parsebuf(ipc_proc->ipc, &ipc_proc->rbuf);
      } while(rc == NGX_AGAIN);
    }
  }
  
  return NGX_OK;
}

static void ipc_read_handler(ngx_event_t *ev) {
  DBG("IPC channel handler");
  //copypasta from os/unix/ngx_process_cycle.c (ngx_channel_handler)
  ngx_int_t          rc;
  ngx_connection_t  *c;
  ipc_process_t     *ipc_proc;
  
  if (ev->timedout) {
    ev->timedout = 0;
    return;
  }
  c = ev->data;
  ipc_proc = &((ipc_t *)c->data)->process[ngx_process_slot];
  
  rc = ipc_read(ipc_proc, &ipc_proc->rbuf, ev->log);
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
// <CODE(uint8)>|<SRC_SLOT(uint16)>|<DATA_LEN(uint16)>|<DATA>



ngx_int_t ipc_alert(ipc_t *ipc, ngx_int_t slot, ngx_uint_t code, void *data, size_t data_size) {
  
  ipc_alert_link_t   *alert_link;
  ipc_process_t      *proc = &ipc->process[slot];
  ipc_writebuf_t     *wb = &proc->wbuf;
  ipc_alert_t        *alert;
  size_t              alert_str_size = 0;
  u_char             *end;
  
  ngx_str_t           data_str;
  
  DBG("IPC send alert code %i to slot %i", code, slot);
  
  if(slot == ngx_process_slot) {
    ipc->handler(slot, code, data, data_size);
    return NGX_OK;
  }
  
  if(!proc->active) {
    return NGX_ERROR;
  }

  alert_str_size +=   IPC_MAX_HEADER_LEN + data_size;
  if((alert_link = ngx_alloc(sizeof(*alert_link) + alert_str_size, ngx_cycle->log)) == NULL) {
    // nomem
    return NGX_ERROR;
  }
  alert_link->next = NULL;
  alert_link->sent = 0;
  
  alert = &alert_link->alert;
  
  alert->data = (char *)&alert_link[1];
  
  data_str.len = data_size;
  data_str.data = (u_char *)data;
  
  end = ngx_snprintf((u_char *)alert->data, alert_str_size, "%i|%i|%i|%V", code, ngx_process_slot, data_str.len, &data_str);
  
  alert->sz = end - (u_char *)alert->data;
  
  if(wb->tail != NULL) {
    wb->tail->next = alert_link;
  }
  wb->tail = alert_link;
  if(wb->head == NULL) {
    wb->head = alert_link;
  }
  ipc_write_handler(proc->c->write);
  
  //ngx_handle_write_event(ipc->c[slot]->write, 0);
  //ngx_add_event(ipc->c[slot]->write, NGX_WRITE_EVENT, NGX_CLEAR_EVENT);

  return NGX_OK;
}


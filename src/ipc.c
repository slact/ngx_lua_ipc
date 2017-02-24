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
  
  if(!alert_link->header_sent) {
    assert(write(fd, &alert->sz, sizeof(alert->sz)) == sizeof(alert->sz));
    assert(write(fd, &alert->src_slot, sizeof(alert->src_slot)) == sizeof(alert->src_slot));
    alert_link->header_sent = 1;
  }
 
  unsent = alert->sz - alert_link->sent;
  data = &alert->data[alert_link->sent];
  
  n = write(fd, data, unsent);
  if (n == -1) {
    err = ngx_errno;
    if (err == NGX_EAGAIN) {
      return NGX_AGAIN;
    }
    
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, err, "write() failed");
    assert(0);
    return NGX_ERROR;
  }
  else if (n < unsent) {
    alert_link->sent += n;
    return NGX_EAGAIN;
  }
  
  return NGX_OK;
}

static ngx_int_t ipc_free_buffered_alert(ipc_alert_link_t *alert_link) {
  ngx_free(alert_link->alert.data);
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

static ngx_int_t ipc_read_socket(ngx_socket_t s, ipc_readbuf_t *rbuf, ngx_log_t *log) {
  DBG("IPC read channel");
  ssize_t             n;
  ngx_err_t           err;
  ssize_t             unreceived;
  
  if(!rbuf->header_received) {
    assert(rbuf->received == 0);
    assert(read(s, &rbuf->alert.sz, sizeof(rbuf->alert.sz)) == sizeof(rbuf->alert.sz));
    assert(read(s, &rbuf->alert.src_slot, sizeof(rbuf->alert.src_slot)) == sizeof(rbuf->alert.src_slot));
    rbuf->header_received = 1;
    
    assert(rbuf->alert.data == NULL);
    rbuf->alert.data = ngx_alloc(rbuf->alert.sz, ngx_cycle->log);
    assert(rbuf->alert.data != NULL);
  }

  unreceived = rbuf->alert.sz - rbuf->received;
  
  n = read(s, rbuf->alert.data, unreceived);
 
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
  else if (n < unreceived) {
    rbuf->received += n;
    return NGX_AGAIN;
  }
  else {
    rbuf->received += n;
    assert(rbuf->received == rbuf->alert.sz);
    return NGX_OK;
  }
}

#if DEBUG_DELAY_IPC_RECEIVE_ALERT_MSEC
typedef struct {
  ngx_event_t   timer;
  ipc_alert_t   alert;
  ipc_t        *ipc;
} delayed_alert_glob_t;
static void fake_ipc_alert_delay_handler(ngx_event_t *ev) {
  delayed_alert_glob_t *glob = (delayed_alert_glob_t *)ev->data;
  
  glob->ipc->handler(glob->alert.src_slot, glob->alert.code, glob->alert.data);
  ngx_free(glob);
}
#endif

static void ipc_read_handler(ngx_event_t *ev) {
  DBG("IPC channel handler");
  //copypasta from os/unix/ngx_process_cycle.c (ngx_channel_handler)
  ngx_int_t          rc;
  ngx_connection_t  *c;
  ipc_process_t     *ipc_proc;
  ipc_alert_t       *alert;
  
  if (ev->timedout) {
    ev->timedout = 0;
    return;
  }
  c = ev->data;
  ipc_proc = &((ipc_t *)c->data)->process[ngx_process_slot];
  
  while(1) {
    rc = ipc_read_socket(c->fd, &ipc_proc->rbuf, ev->log);
    if (rc == NGX_ERROR) {
      ERR("IPC_READ_SOCKET failed: bad connection. This should never have happened, yet here we are...");
      assert(0);
      return;
    }
    else if (rc == NGX_AGAIN) {
      return;
    }
    
    assert(ipc_proc->rbuf.complete == 1);
    
    alert = &ipc_proc->rbuf.alert;
    ((ipc_t *)c->data)->handler(alert->src_slot, alert->code, alert->data, alert->sz);
    
    ipc_proc->rbuf.complete = 0;
    ipc_proc->rbuf.header_received = 0;
    ipc_proc->rbuf.received = 0;
    ngx_free(ipc_proc->rbuf.alert.data);
    ipc_proc->rbuf.alert.data = NULL;
  }
}


ngx_int_t ipc_alert(ipc_t *ipc, ngx_int_t slot, ngx_uint_t code, void *data, size_t data_size) {
  
  ipc_alert_link_t   *alert_link;
  ipc_process_t      *proc = &ipc->process[slot];
  ipc_writebuf_t     *wb = &proc->wbuf;
  ipc_alert_t        *alert;
  
  DBG("IPC send alert code %i to slot %i", code, slot);
  
  if(slot == ngx_process_slot) {
    ipc->handler(slot, code, data, data_size);
    return NGX_OK;
  }
  
  if(!proc->active) {
    return NGX_ERROR;
  }
  
  if((alert_link = ngx_alloc(sizeof(*alert_link) + data_size, ngx_cycle->log)) == NULL) {
    // nomem
    return NGX_ERROR;
  }
  alert_link->next = NULL;
  alert_link->sent = 0;
  alert_link->header_sent = 0;
  
  alert = &alert_link->alert;
  alert->sz = data_size;
  alert->src_slot = ngx_process_slot;
  alert->code = code;
  alert->data = (char *)&alert_link[1];
  ngx_memcpy(&alert->data, data, data_size);
  
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


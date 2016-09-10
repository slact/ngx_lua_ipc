Interprocess communication for lua_nginx_module and openresty. 

This is a **proof of concept**. There are severe limitations on the use of this thing.
Interprocess messages can be no more than 512 bytes, and can't have null characters.
There's no worker crash recovery. The receiving lua VM does not yet run in a proper coroutine. Also it may spawn gremlins.

It does, however, reliably send messages between worker processes.

I wrote this as a quick hack to separate the [interprocess code](https://github.com/slact/nchan/tree/master/src/store/memory) out of [Nchan](https://github.com/slact/nchan) mostly on a flight back from Nginx Conf 2016.


API:
```lua

local ipc = require "ngx.ipc"

ipc.receive(ipc_alert_code, function(sender, data)
  --ipc receiver function for all alerts with integer code ipc_alert_code
end)

ipc.send(destination_worker_pid, ipc_alert_code, data_string)

ipc.broadcast(ipc_alert_code, data_string)


```






usage:

```nginx

http {

  init_by_lua '
    local ipc = require "ngx.ipc"
    ipc.receive(0, function(sender, data)
      ngx.log(ngx.ALERT, ("%d says %s"):format(sender, data))
    end)
    
  ';
  
  server {
    listen       80;
    
    location ~ /send/(\d+)/(.*)$ {
      set $dst_pid $1;
      set $data $2;
      content_by_lua '
        local ipc = require "ngx.ipc"
        ipc.send(ngx.var.dst_pid, 0, ngx.var.data)
      ';
    }
    
    location ~ /broadcast/(.*)$ {
      set $data $1;
      content_by_lua '
        local ipc = require "ngx.ipc"
        ipc.broadcast(0, ngx.var.data)
      ';
    }
    
  }
}

```

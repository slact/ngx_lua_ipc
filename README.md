Interprocess communication for lua_nginx_module and openresty. 

I wrote this as a quick hack to separate the [interprocess code](https://github.com/slact/nchan/tree/master/src/store/memory) out of [Nchan](https://github.com/slact/nchan) mostly on a flight back from Nginx Conf 2016.

The completion of this module was generously sponsored by [ring.com](https://ring.com). Thanks guys!

This is a **work-in-progress**. There are limitations on the use of this thing, and the API is liable to change.
The receiving lua VM does not yet run in a proper coroutine.



API:
```lua

local ipc = require "ngx.ipc"

ipc.receive(ipc_alert_name, function(sender, data)
  --ipc receiver function for all alerts with string name ipc_alert_name
end)

ipc.reply(alert_name, data_string)
  --reply to alert sender. works only when in an alert receiver handler function
  
ipc.send(destination_worker_pid, ipc_alert_name, data_string)

ipc.broadcast(ipc_alert_name, data_string)


```



usage:

```nginx

http {

  init_worker_by_lua_block {
    local ipc = require "ngx.ipc"
    ipc.receive("hello", function(sender, data)
      ngx.log(ngx.ALERT, ("%d says %s"):format(sender, data))
      
      ipc.reply("hello-reply", "hello to you too")
      
    end)
    
    ipc.receive("reply", function(sender, data) 
      ngx.log(ngx.ALERT, ("%d replied %s"):format(sender, data))
    end) 
  }
  
  server {
    listen       80;
    
    location ~ /send/(\d+)/(.*)$ {
      set $dst_pid $1;
      set $data $2;
      content_by_lua_block {
        local ipc = require "ngx.ipc"
        ipc.send(ngx.var.dst_pid, "hello", ngx.var.data)
      }
    }
    
    location ~ /broadcast/(.*)$ {
      set $data $1;
      content_by_lua_block { 
        local ipc = require "ngx.ipc"
        ipc.broadcast("hello", ngx.var.data)
      }
    }
    
  }
}

```

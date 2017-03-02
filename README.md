Interprocess communication for lua_nginx_module and openresty. Send named alerts with string data between Nginx worker processes.

Asynchronous, nonblocking, non-locking.

I wrote this as a quick hack to separate the [interprocess code](https://github.com/slact/nchan/tree/master/src/store/memory) out of [Nchan](https://github.com/slact/nchan) mostly on a flight back from Nginx Conf 2016. The completion of this module was generously sponsored by [ring.com](https://ring.com). Thanks guys!

API:
```lua
local ipc = require "ngx.ipc"
```

## `ipc.receive`

Register one or several alert handlers. 
Note that `receive` cannot be used in the `init_by_lua*` context. During startup, use `init_worker_by_lua*`.

Register an alert handler:
```lua
ipc.receive(ipc_alert_name, function(data)
  --ipc receiver function for all alerts with string name ipc_alert_name
end)
```

Several alert names can be registered at once by passing a table:
```lua
ipc.receive({
  hello = function(data) 
    --got a hello
  end,
  goodbye = function(data)
    --got a goodbye
  end
})
```

delete alert handler:
```lua
ipc.receive(ipc_alert_name, nil)
```

Alerts received without a handler are discarded.

## `ipc.reply`

reply to alert sender. works only when in an alert receiver handler function

```lua
  ipc.receive("hello", function(data)
    ipc.reply("hello-response", "hi, you said "..data)
  end)
```

## `ipc.send`

send alert tp another worker
```lua
ipc.send(destination_worker_pid, ipc_alert_name, data_string)
```

## `ipc.broadcast`
broadcast alert to all workers (including sender)
```lua
ipc.broadcast(alert_name, data_string)
```

## `ipc.sender`
when receiving an alert, ipc.sender contains sender process id.
all other times, it is nil
```lua
ipc.receive("hello", function(data)
  if ipc.sender ==  ngx.worker.pid() then
    --just said hello to myself
  end
end)
```

# Example

```nginx

http {

  init_worker_by_lua_block {
    local ipc = require "ngx.ipc"
    ipc.receive("hello", function(data)
      ngx.log(ngx.ALERT, "sender" .. ipc.sender .. " says " .. data)
      
      ipc.reply("reply", "hello to you too. you said " .. data)
      
    end)
    
    ipc.receive("reply", function(data) 
      ngx.log(ngx.ALERT, tostring(ipc.sender) .. " replied " .. data)
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

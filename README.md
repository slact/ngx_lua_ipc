Interprocess communication for lua_nginx_module and openresty. Send named alerts with string data between Nginx worker processes.

Asynchronous, nonblocking, non-locking, and [*fast*](#speed)!

### History 

I wrote this as a quick hack to separate the [interprocess code](https://github.com/slact/nchan/tree/master/src/store/memory) out of [Nchan](https://github.com/slact/nchan) mostly on a flight back from [Nginx Conf 2016](https://www.nginx.com/nginxconf/2016). The completion of this module was generously sponsored by [ring.com](https://ring.com). Thanks guys!

# API

```lua
local ipc = require "ngx.ipc"
```

### `ipc.send`
Send alert to a worker process.
```lua
ipc.send(destination_worker_pid, ipc_alert_name, data_string)
```

Return:
 - `true` on success
 - `nil, error_msg` if `ipc_alert_name` length is > 254, `data_string` length is > 4G, `destination_worker_pid` is not a valid worker process.


### `ipc.broadcast`
Broadcast alert to all workers (including sender).
```lua
ipc.broadcast(alert_name, data_string)
```

Return:
 - `true` on success
 - `nil, error_msg` if `ipc_alert_name` length is > 254 or `data_string` length is > 4G

### `ipc.receive`
Register one or several alert handlers. 
Note that `receive` cannot be used in the `init_by_lua*` context. During startup, use `init_worker_by_lua*`.

Register an alert handler:
```lua
ipc.receive(ipc_alert_name, function(data)
  --ipc receiver function for all alerts with string name ipc_alert_name
end)
```
Return:
 - `true`

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

Deleting an alert handler:
```lua
ipc.receive(ipc_alert_name, nil)
```

*Alerts received without a handler are discarded.*

### `ipc.reply`
Reply to worker that sent an alert. Works only when in an alert receiver handler function.

```lua
  ipc.receive("hello", function(data)
    ipc.reply("hello-response", "hi, you said "..data)
  end)
```

Return:
 - `true`
 
Raises error if used outside of a `ipc.receive` handler.


### `ipc.sender`
When receiving an alert, `ipc.sender` contains the sending worker"s process id.
all other times, it is nil
```lua
ipc.receive("hello", function(data)
  if ipc.sender ==  ngx.worker.pid() then
    --just said hello to myself
  end
end)
```

# Example

`nginx.conf`
```lua
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
        local ok, err = ipc.send(ngx.var.dst_pid, "hello", ngx.var.data)
        if ok then
          ngx.say("Sent alert to pid " .. ngx.var.dst_pid);
        else
          ngx.status = 500
          ngx.say(err)
        end
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

# How it works

IPC alerts are split into 4K packets and delivered to workers via Unix pipes. On the receiving end, a persistent timer started with `ntx.timer.at` hangs around waiting to be manually triggered by the reading IPC event handler. A simple hack in concept, but a bit convoluted in implementation.

# Speed

It's pretty fast. On an i5-2500K (2 core, 4 thread) running Nginx with the Lua module built with Luajit, here are the results of my benchmarks:
 - 5 workers, 10b alerts: 220K alerts/sec
 - 5 workers, 10Kb alerts: 110K alerts/sec
 - 20 workers, 10b alerts: 220K alerts/sec
 - 20 workers, 10Kb alerts: 33K alerts/sec


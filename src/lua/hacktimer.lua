-- crazy timer hack
local ipc = require "ngx.ipc"
local handlers = ipc.handlers
local timerhack = nil
local pending_alerts = nil
local timer_handler
timer_handler = function(premature)
  
  local sender = {} -- for "ngx.ipc.reply" convenience function
  ipc.sender = sender
  
  for src_slot, src_pid, name, data in pending_alerts do
    
    sender.slot = src_slot
    sender.process = src_pid
    
    local handler = handlers[name]
    if handler then
      handler(src_pid, data)
    elseif ipc.default_handler then
      ipc.default_handler(src_pid, name, data)
    end
  end
  
  ipc.sender = nil
  --add timer again and hack it
  timerhack(timer_handler)
end

return function(pending_alerts_func, timerhack_func)
  pending_alerts = pending_alerts_func
  timerhack = timerhack_func
  timerhack(timer_handler)
end

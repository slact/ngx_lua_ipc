return function(ipc)
  return function(name, data)
    if not ipc.sender then
      error("Can't reply, function called outside of IPC alert handler.")
    end
    return ipc.send(ipc.sender.process, name, data)
  end  
end

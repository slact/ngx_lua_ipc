return function(ipc)
  return function(name, data)
    if not ipc.sender then
      error("Can't reply, ngx.ipc.reply called outside of IPC alert handler.")
    end
    return ipc.send(ipc.sender, name, data)
  end  
end

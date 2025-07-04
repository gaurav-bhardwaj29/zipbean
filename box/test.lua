return "you visited path = " .. request.path ..
       "\nwith user-agent = " .. (request.headers["User-Agent"] or "n/a") ..
       "\nquery param: id = " .. (request.query.id or "none")

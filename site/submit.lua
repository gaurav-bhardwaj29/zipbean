-- expects: POST /submit.lua with body: name=Gaurav&email=g@me.com
local form = parse_query_string(request.body or "")
local sql = string.format(
  "INSERT INTO users (name, email) VALUES ('%s', '%s')",
  form.name or "", form.email or ""
)
sqlite_query("site/data.db", sql)
return "Inserted: " .. (form.name or "unknown")

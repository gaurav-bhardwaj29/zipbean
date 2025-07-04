local rows = sqlite_query("site/data.db", "SELECT id, name, email FROM users LIMIT 10")
local out = {}
for i, row in ipairs(rows) do
    table.insert(out, string.format("%s. %s (%s)", row.id, row.name, row.email))
end
return table.concat(out, "\n")

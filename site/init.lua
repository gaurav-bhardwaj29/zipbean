function parse_query_string(qs)
    local t = {}
    for kv in string.gmatch(qs or "", "([^&]+)") do
        local k, v = string.match(kv, "([^=]+)=([^=]*)")
        if k and v then
            t[k] = v
        end
    end
    return t
end

print("[init.lua loaded]")

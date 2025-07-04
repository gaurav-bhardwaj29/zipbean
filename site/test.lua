return string.format("Method: %s\nPath: %s\nQuery: %s",
    request.method or "?", request.path or "?", request.query.id or "none")

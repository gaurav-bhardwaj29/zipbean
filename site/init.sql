CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT,
    email TEXT
);

-- Optional seed data
INSERT INTO users (name, email) VALUES ('Ravi', 'r@me.com');

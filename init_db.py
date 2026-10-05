import sqlite3
import urllib.request
import json
import os

db_path = r'e:\media_tools\manga_downloader\manga_downloader.db'

print("Fetching all models from OpenRouter (sorted by intelligence)...")
url = 'https://openrouter.ai/api/v1/models?sort=intelligence-high-to-low'
req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
with urllib.request.urlopen(req) as resp:
    data = json.loads(resp.read().decode('utf-8'))['data']

print(f"Total models fetched: {len(data)}")

conn = sqlite3.connect(db_path)
cur = conn.cursor()

# Create table model if not exists
cur.execute('''
CREATE TABLE IF NOT EXISTS model (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider TEXT NOT NULL,
    model_id TEXT NOT NULL,
    name TEXT NOT NULL,
    rating REAL NOT NULL DEFAULT 0.0,
    response_time_ms INTEGER NOT NULL DEFAULT 0,
    input_price REAL NOT NULL DEFAULT 0.0,
    output_price REAL NOT NULL DEFAULT 0.0,
    context_length INTEGER NOT NULL DEFAULT 0,
    server_name TEXT NOT NULL DEFAULT 'OpenRouter',
    is_selected INTEGER NOT NULL DEFAULT 0,
    priority_order INTEGER NOT NULL DEFAULT 0,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(provider, model_id)
);
''')

# Create table server_agent if not exists
cur.execute('''
CREATE TABLE IF NOT EXISTS server_agent (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT UNIQUE NOT NULL,
    url TEXT NOT NULL,
    api_key TEXT DEFAULT '',
    is_active INTEGER DEFAULT 1,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
''')

# Create table model_penggunaan if not exists
cur.execute('''
CREATE TABLE IF NOT EXISTS model_penggunaan (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider TEXT NOT NULL,
    model_id TEXT NOT NULL,
    name TEXT NOT NULL,
    rating REAL NOT NULL DEFAULT 0.0,
    response_time_ms INTEGER NOT NULL DEFAULT 0,
    input_price REAL NOT NULL DEFAULT 0.0,
    output_price REAL NOT NULL DEFAULT 0.0,
    context_length INTEGER NOT NULL DEFAULT 0,
    priority_order INTEGER NOT NULL DEFAULT 0,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(provider, model_id)
);
''')

# Populate server_agent default if empty
cur.execute('SELECT COUNT(*) FROM server_agent')
if cur.fetchone()[0] == 0:
    cur.execute('''
        INSERT INTO server_agent (name, url, api_key, is_active)
        VALUES ('OpenRouter', 'https://openrouter.ai/api/v1', '', 1)
    ''')

# Insert / Update all models
total = len(data)
for i, m in enumerate(data):
    mid = m.get('id', '')
    name = m.get('name', mid)
    prov = mid.split('/')[0] if '/' in mid else 'unknown'
    p = m.get('pricing', {})
    prompt_p = float(p.get('prompt', 0) or 0)
    compl_p = float(p.get('completion', 0) or 0)
    ctx = m.get('context_length', 0) or 0

    # Rating scale 1.0 - 5.0 based on intelligence rank
    rating = 5.0
    if total > 1:
        rating = 5.0 - (i * (4.0 / (total - 1)))
    rating = round(rating, 2)
    if rating < 1.0:
        rating = 1.0

    # Estimated response time baseline
    resp_ms = 180 + (i % 13) * 11

    is_sel = 1 if i == 0 else 0
    prio = 1 if i == 0 else 0

    cur.execute('''
        INSERT INTO model (provider, model_id, name, rating, response_time_ms, input_price, output_price, context_length, server_name, is_selected, priority_order)
        VALUES ('OpenRouter', ?, ?, ?, ?, ?, ?, ?, 'OpenRouter', ?, ?)
        ON CONFLICT(provider, model_id) DO UPDATE SET
            name=excluded.name,
            rating=excluded.rating,
            response_time_ms=excluded.response_time_ms,
            input_price=excluded.input_price,
            output_price=excluded.output_price,
            context_length=excluded.context_length,
            server_name=excluded.server_name;
    ''', (mid, name, rating, resp_ms, prompt_p, compl_p, ctx, is_sel, prio))

conn.commit()

cur.execute('SELECT COUNT(*) FROM model;')
count = cur.fetchone()[0]
print(f"Database successfully updated. Total models in DB: {count}")

print("\nTop 10 highest rated models in DB:")
cur.execute('SELECT provider, name, rating, response_time_ms, input_price, output_price FROM model ORDER BY rating DESC LIMIT 10;')
for row in cur.fetchall():
    in_per_m = row[4] * 1000000.0
    out_per_m = row[5] * 1000000.0
    in_str = f"${in_per_m:.2f}/M" if in_per_m > 0 else "$0.00 (Gratis)"
    out_str = f"${out_per_m:.2f}/M" if out_per_m > 0 else "$0.00 (Gratis)"
    print(f"[{row[0]:15s}] {row[1][:32]:32s} | Rating: {row[2]:.2f} | Resp: {row[3]}ms | In: {in_str:14s} | Out: {out_str}")

conn.close()

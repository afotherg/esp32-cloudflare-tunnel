#!/usr/bin/env python3
"""Verify live telemetry, request errors, freshness, and concurrent requests."""
import argparse
import concurrent.futures
import json
import urllib.error
import urllib.request

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('url')
p.add_argument('--requests', type=int, default=12)
args = p.parse_args()
base = args.url.rstrip('/')

def fetch(path='/api/telemetry', method='GET'):
    req = urllib.request.Request(base+path, method=method,
                                 headers={'User-Agent': 'ESP32-Telemetry-Check/1.0'})
    with urllib.request.urlopen(req, timeout=20) as res:
        assert res.status == 200
        assert ('text/html' if path == '/' else 'application/json') in res.headers['Content-Type']
        assert res.headers.get('Cache-Control') == 'no-store'
        if method == 'HEAD' or path == '/':
            return res.read()
        return json.load(res)

page = fetch('/')
assert b'Inside the chip.' in page and b"fetch('/api/telemetry'" in page
assert fetch('/', method='HEAD') == b''
assert fetch('/healthz')['chip'] == 'ESP32-S3'
first = fetch()
assert first['chip'] == 'ESP32-S3'
assert first['tunnel_connected'] is True
assert first['heap_free_bytes'] > 0
assert first['heap_used_bytes'] > 0
assert first['chip_temperature_c'] is None or -10 <= first['chip_temperature_c'] <= 80
assert fetch(method='HEAD') == b''
for path, method, expected in [('/does-not-exist','GET',404),('/','POST',405)]:
    try:
        fetch(path, method)
        raise AssertionError('Expected HTTP error')
    except urllib.error.HTTPError as e:
        assert e.code == expected, (e.code, expected)
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    rows = list(pool.map(lambda _: fetch(), range(args.requests)))
assert min(r['uptime_seconds'] for r in rows) >= first['uptime_seconds']
assert max(r['requests_served'] for r in rows) > first['requests_served']
assert all(r['reconnections'] == first['reconnections'] for r in rows)
print(json.dumps(rows[-1], indent=2))
print(f'PASS: HTML dashboard, both JSON endpoints, HEAD, 404, 405, and {args.requests} concurrent requests')

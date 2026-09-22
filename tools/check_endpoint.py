#!/usr/bin/env python3
"""Verify live telemetry, request errors, freshness, and concurrent requests."""
import argparse
import concurrent.futures
import json
import gzip
import urllib.error
import urllib.request

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('url')
p.add_argument('--requests', type=int, default=12)
p.add_argument('--connections', type=int, default=4)
p.add_argument('--workers', type=int, default=4)
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
for encoding, compressed in [('gzip', True), ('identity', False)]:
    req = urllib.request.Request(base+'/', headers={'Accept-Encoding': encoding,
                                                   'User-Agent': 'ESP32-Telemetry-Check/1.0'})
    with urllib.request.urlopen(req, timeout=20) as res:
        data = res.read()
        assert (res.headers.get('Content-Encoding') == 'gzip') == compressed
        html = gzip.decompress(data) if compressed else data
        # Cloudflare may inject per-request scripts into HTML responses.
        assert b'Inside the chip.' in html and b"fetch('/api/telemetry'" in html
        assert 'accept-encoding' in res.headers.get('Vary', '').lower()
assert fetch('/healthz')['chip'] == 'ESP32-S3'
first = fetch()
assert first['chip'] == 'ESP32-S3'
assert first['tunnel_connected'] is True
assert first['tunnel_connections'] == args.connections
assert first['tunnel_healthy'] == (args.connections == first['tunnel_connections_desired'])
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
with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
    rows = list(pool.map(lambda _: fetch(), range(args.requests)))
assert min(r['uptime_seconds'] for r in rows) >= first['uptime_seconds']
assert max(r['requests_served'] for r in rows) > first['requests_served']
assert all(r['reconnections'] == first['reconnections'] for r in rows)
assert all(r['tunnel_connections'] == args.connections for r in rows)
assert all(0 <= r['served_by_connection'] < 4 for r in rows)
print(json.dumps(rows[-1], indent=2))
print(f'PASS: HTML dashboard, both JSON endpoints, HEAD, 404, 405, and {args.requests} requests ({args.workers} concurrent workers)')

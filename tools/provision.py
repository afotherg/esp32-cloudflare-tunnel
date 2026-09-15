#!/usr/bin/env python3
"""Generate the private NVS image; no credentials are compiled into firmware."""
import argparse
import base64
import csv
import getpass
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--credentials', type=Path, help='Private JSON with ssid, password, token; otherwise prompt')
p.add_argument('--output', type=Path, default=Path('secrets/nvs.bin'))
p.add_argument('--idf', type=Path, default=Path.home()/'.platformio/packages/framework-espidf')
args = p.parse_args()
os.umask(0o077)
if args.credentials:
    c = json.loads(args.credentials.read_text())
else:
    c = dict(ssid=input('Wi-Fi SSID: '), password=getpass.getpass('Wi-Fi password: '), token=getpass.getpass('Tunnel token: '))
assert 0 < len(c['ssid'].encode()) <= 32, 'SSID must be 1–32 bytes'
assert len(c['password'].encode()) <= 63, 'Wi-Fi password exceeds 63 bytes'
t = json.loads(base64.b64decode(c['token'], validate=True))
assert len(t['a']) == 32
uuid.UUID(t['t'])
assert 0 < len(base64.b64decode(t['s'], validate=True)) <= 128
args.output.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp)/'nvs.csv'
    with source.open('w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['key', 'type', 'encoding', 'value'])
        w.writerow(['tunnel', 'namespace', '', ''])
        for key in ('ssid', 'password', 'token'):
            w.writerow([key, 'data', 'string', c[key]])
    generator = args.idf/'components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py'
    subprocess.run([sys.executable, str(generator), 'generate', str(source), str(args.output.resolve()), '0x6000'], check=True)
args.output.chmod(0o600)
print('Private provisioning image created. Flash it at 0x9000; do not commit it.')

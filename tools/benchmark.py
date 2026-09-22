#!/usr/bin/env python3
"""Compare HTML and telemetry load, saving status, latency and device counters."""
import argparse
import asyncio
from collections import Counter
import json
from pathlib import Path
import ssl
import statistics
import time
import aiohttp


def percentile(values, fraction):
    return sorted(values)[int((len(values) - 1) * fraction)] if values else None


async def benchmark(base, path, count, concurrency, timeout):
    # Explicit TLS verification; SSL_CERT_FILE can select a system CA bundle.
    connector = aiohttp.TCPConnector(limit=concurrency, ssl=ssl.create_default_context())
    async with aiohttp.ClientSession(connector=connector,
                                    timeout=aiohttp.ClientTimeout(total=timeout),
                                    headers={'User-Agent': 'ESP32-Benchmark/1.0'}) as session:
        async def snapshot():
            try:
                async with session.get(base + '/api/telemetry') as response:
                    response.raise_for_status()
                    return await response.json()
            except Exception as error:
                return {'error': str(error)}

        before = await snapshot()
        semaphore = asyncio.Semaphore(concurrency)

        async def request():
            async with semaphore:
                started = time.perf_counter()
                try:
                    async with session.get(base + path) as response:
                        body = await response.read()
                        return response.status, time.perf_counter() - started, len(body), None
                except Exception as error:
                    return None, time.perf_counter() - started, 0, type(error).__name__ + ': ' + str(error)

        start = time.perf_counter()
        rows = await asyncio.gather(*(request() for _ in range(count)))
        elapsed = time.perf_counter() - start
        after = await snapshot()
    times = [row[1] * 1000 for row in rows if row[0] is not None and 200 <= row[0] < 300]
    return {
        'path': path, 'requests': count, 'concurrency': concurrency, 'timeout_seconds': timeout,
        'elapsed_seconds': elapsed, 'successful': len(times),
        'successful_requests_per_second': len(times) / elapsed,
        'status_counts': dict(Counter(str(row[0]) for row in rows if row[0] is not None)),
        'transport_errors': dict(Counter(row[3] for row in rows if row[3])),
        'decoded_bytes': sum(row[2] for row in rows),
        'latency_ms': {'mean': statistics.mean(times) if times else None,
                       'median': statistics.median(times) if times else None,
                       'p95': percentile(times, .95), 'p99': percentile(times, .99)},
        'before': before, 'after': after,
    }


async def main(args):
    results = []
    for path in args.paths:
        for concurrency in args.concurrency:
            result = await benchmark(args.url.rstrip('/'), path, args.requests, concurrency, args.timeout)
            results.append(result)
            print(json.dumps(result), flush=True)
            if args.output:
                Path(args.output).write_text(json.dumps(results, indent=2) + '\n')
            await asyncio.sleep(2)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('url')
    parser.add_argument('--paths', nargs='+', default=['/', '/api/telemetry'])
    parser.add_argument('--concurrency', nargs='+', type=int, default=[1, 5, 10, 20])
    parser.add_argument('--requests', type=int, default=1000)
    parser.add_argument('--timeout', type=float, default=10)
    parser.add_argument('--output', help='Optional JSON report path')
    args = parser.parse_args()
    if args.requests < 1 or min(args.concurrency) < 1 or args.timeout <= 0:
        parser.error('requests, concurrency, and timeout must be positive')
    asyncio.run(main(args))

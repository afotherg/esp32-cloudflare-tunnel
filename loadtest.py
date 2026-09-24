#!/usr/bin/env python3

import asyncio
import aiohttp
import argparse
import math
import random
import time
import statistics
from collections import Counter


async def fetch(session, url, semaphore, results, jitter):
    async with semaphore:
        # Hold the slot while waiting so queued tasks cannot accumulate and
        # bypass the pacing. Include initial requests, but exclude this delay
        # from request latency and the HTTP timeout.
        if jitter > 0:
            await asyncio.sleep(random.uniform(0, jitter))
        start = time.perf_counter()

        try:
            async with session.get(url) as response:
                body = await response.read()
                elapsed = time.perf_counter() - start

                results.append({
                    "status": response.status,
                    "time": elapsed,
                    "bytes": len(body),
                    "error": None,
                    "cf_error_type": response.headers.get("cf-error-type"),
                    "cf_error_origin": response.headers.get("cf-error-origin"),
                    "cf_ray": response.headers.get("cf-ray"),
                    "body": (
                        body[:2000].decode("utf-8", errors="replace")
                        if response.status >= 400
                        else None
                    ),
                })

        except Exception as e:
            elapsed = time.perf_counter() - start

            results.append({
                "status": None,
                "time": elapsed,
                "bytes": 0,
                "error": f"{type(e).__name__}: {e!r}",
                "cf_error_type": None,
                "cf_error_origin": None,
                "cf_ray": None,
                "body": None,
            })


async def run_test(url, requests, concurrency, timeout, jitter=0.25):
    semaphore = asyncio.Semaphore(concurrency)
    results = []

    client_timeout = aiohttp.ClientTimeout(total=timeout)

    # ssl=False disables TLS certificate verification.
    connector = aiohttp.TCPConnector(
        limit=concurrency,
        ssl=False,
    )

    async with aiohttp.ClientSession(
        timeout=client_timeout,
        connector=connector,
    ) as session:

        print(f"URL:         {url}")
        print(f"Requests:    {requests}")
        print(f"Concurrency: {concurrency}")
        print(f"Timeout:     {timeout} seconds")
        print(f"Jitter:      0–{jitter:g} seconds before each request")
        print()

        start = time.perf_counter()

        tasks = [
            asyncio.create_task(
                fetch(session, url, semaphore, results, jitter)
            )
            for _ in range(requests)
        ]

        await asyncio.gather(*tasks)

        total_time = time.perf_counter() - start

    return results, total_time


def percentile(values, p):
    if not values:
        return 0

    values = sorted(values)
    index = int((len(values) - 1) * p)
    return values[index]


def print_latency(title, values):
    if not values:
        return

    print()
    print(title)
    print(f"  min:             {min(values) * 1000:.1f} ms")
    print(f"  mean:            {statistics.mean(values) * 1000:.1f} ms")
    print(f"  median:          {statistics.median(values) * 1000:.1f} ms")
    print(f"  p90:             {percentile(values, 0.90) * 1000:.1f} ms")
    print(f"  p95:             {percentile(values, 0.95) * 1000:.1f} ms")
    print(f"  p99:             {percentile(values, 0.99) * 1000:.1f} ms")
    print(f"  max:             {max(values) * 1000:.1f} ms")


def print_results(results, total_time):
    successful = [
        r for r in results
        if r["status"] is not None
        and 200 <= r["status"] < 300
    ]

    http_errors = [
        r for r in results
        if r["status"] is not None
        and r["status"] >= 400
    ]

    other_http = [
        r for r in results
        if r["status"] is not None
        and not (200 <= r["status"] < 300)
        and r["status"] < 400
    ]

    transport_errors = [
        r for r in results
        if r["error"] is not None
    ]

    success_latencies = [
        r["time"] for r in successful
    ]

    all_http_latencies = [
        r["time"] for r in results
        if r["status"] is not None
    ]

    status_counts = Counter(
        r["status"]
        for r in results
        if r["status"] is not None
    )

    total_bytes = sum(
        r["bytes"] for r in results
    )

    print()
    print("Results")
    print("-------")

    print(f"Total time:        {total_time:.3f} seconds")
    print(f"Requests:          {len(results)}")
    print(f"Requests/sec:      {len(results) / total_time:.2f}")
    print(f"Successful (2xx):  {len(successful)}")
    print(f"HTTP errors:       {len(http_errors)}")
    print(f"Other HTTP:        {len(other_http)}")
    print(f"Transport errors:  {len(transport_errors)}")
    print(f"Bytes received:    {total_bytes}")

    print_latency(
        "Successful request latency:",
        success_latencies,
    )

    print_latency(
        "All HTTP response latency:",
        all_http_latencies,
    )

    print()
    print("HTTP status:")

    if status_counts:
        for status, count in sorted(status_counts.items()):
            percentage = count / len(results) * 100

            print(
                f"  {status}: {count} "
                f"({percentage:.1f}%)"
            )
    else:
        print("  No HTTP responses")

    if http_errors:
        print()
        print("HTTP error details:")

        error_status_counts = Counter(
            r["status"] for r in http_errors
        )

        for status, count in sorted(error_status_counts.items()):
            print(f"  {status}: {count}")

        print()
        print("First HTTP error:")
        r = http_errors[0]

        print(f"  status:          {r['status']}")
        print(f"  cf-error-type:   {r['cf_error_type']}")
        print(f"  cf-error-origin: {r['cf_error_origin']}")
        print(f"  cf-ray:          {r['cf_ray']}")

        if r["body"]:
            print()
            print("Response body:")
            print("----------------")
            print(r["body"])
            print("----------------")

    if transport_errors:
        print()
        print("Transport errors:")

        error_counts = Counter(
            r["error"]
            for r in transport_errors
        )

        for error, count in error_counts.most_common(20):
            print(f"  {count:5}  {error}")


def main():
    parser = argparse.ArgumentParser(
        description="Asynchronous HTTP load tester"
    )

    parser.add_argument(
        "url",
        nargs="?",
        default="https://esp32.fothergill.com/",
        help="URL to test",
    )

    parser.add_argument(
        "-n",
        "--requests",
        type=int,
        default=1000,
        help="Total number of requests",
    )

    parser.add_argument(
        "-c",
        "--concurrency",
        type=int,
        default=20,
        help="Maximum concurrent request slots (including jitter waits)",
    )

    parser.add_argument(
        "--timeout",
        type=float,
        default=10,
        help="Request timeout in seconds",
    )

    parser.add_argument(
        "--jitter",
        type=float,
        default=0.25,
        metavar="SECONDS",
        help="Random delay from 0 to SECONDS before each request (default: 0.25; 0 disables)",
    )

    args = parser.parse_args()

    if args.requests < 1:
        parser.error("requests must be at least 1")

    if args.concurrency < 1:
        parser.error("concurrency must be at least 1")

    if args.timeout <= 0:
        parser.error("timeout must be greater than 0")

    if not math.isfinite(args.jitter) or args.jitter < 0:
        parser.error("jitter must be a finite, nonnegative number")

    results, total_time = asyncio.run(
        run_test(
            args.url,
            args.requests,
            args.concurrency,
            args.timeout,
            args.jitter,
        )
    )

    print_results(
        results,
        total_time,
    )


if __name__ == "__main__":
    main()

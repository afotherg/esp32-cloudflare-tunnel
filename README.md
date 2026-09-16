# Native Cloudflare Tunnel on ESP32-S3

This firmware connects the Heltec WiFi LoRa 32 V3 directly to Cloudflare and
serves a live dashboard and device telemetry as JSON. The computer is used to flash and inspect
the board; it does not run `cloudflared`, a proxy, or a telemetry relay.

```
HTTPS request → Cloudflare → outbound TLS/HTTP2 connection → ESP32 telemetry
```

The tunnel's configured `http://localhost` origin is implemented on the ESP32.
The tunnel handler and a local port 80 HTTP server use the same telemetry
function. Tunnel requests are dispatched directly, avoiding an extra loopback
TCP connection on the microcontroller.

## Endpoints

`GET /` serves the self-contained dashboard, with live metric cards, temperature
and allocated-memory charts, memory allocation, and device details. It polls
every five seconds, pauses when hidden, backs off on failures, and labels stale
readings. Chart history is held only in the browser for a five-minute window.
The page uses no external fonts, scripts, or images.

`GET /api/telemetry` and `GET /healthz` return JSON with:

- Internal chip temperature in Celsius (not ambient temperature).
- Allocated, free, minimum free, and largest contiguous internal heap sizes.
- Uptime, CPU frequency and cores, chip revision, flash size, and SDK version.
- Wi-Fi RSSI, local IP, tunnel connection status, request count, and reconnects.

Tunnel endpoints also support HEAD. Unknown paths return 404 and other methods
return 405. Responses use `Cache-Control: no-store`. `/healthz` returns the same
telemetry, including tunnel state. A missing/failed temperature reading is `null`.
Heap metrics refer to allocatable internal 8-bit heap, not all physical RAM.

Edit `web/dashboard.html` to change the dashboard. The PlatformIO pre-build step
embeds it into a generated `src/dashboard_asset.hpp`; this generated file is
ignored. The tunnel streams the page directly from flash rather than allocating
a full HTML copy per request. Credentials remain entirely separate from the page.

## OLED request display

The onboard 128×64 OLED displays the latest request date, time in the
America/Los_Angeles time zone (PST/PDT), and client IPv4 or IPv6 address. All
tunnel HTTP requests update it, including the dashboard's five-second telemetry
polls. Internal tunnel control/configuration traffic does not update it.
The display preserves the request timestamp rather than advancing like a clock.

Tunnel requests use Cloudflare's `CF-Connecting-IP`, including its serialized
header representation; local requests use the TCP peer address. Missing IPs and
unsynchronized time are labeled explicitly. Client IPs are shown only on the
physical OLED, not added to the public telemetry response. `oled_ready` and
`oled_updates` provide non-identifying display diagnostics in telemetry.

The SSD1306 uses SDA 17, SCL 18, reset 21, and active-low Vext 36 on the Heltec V3.
A separate task coalesces bursts to the latest request and bounds display I/O,
so network handling does not wait for OLED transfers.

## Build

The tested toolchain is PlatformIO Espressif32 5.4.0 with ESP-IDF 4.4.5. It is
pinned in `platformio.ini` and uses the locally available ESP32-S3 toolchain.

```sh
cd esp32-cloudflare-tunnel
pio run
```

## Provision credentials

Credentials are stored in the NVS partition, separately from the firmware.
There are no Wi-Fi credentials or tunnel tokens in the source code. The
`secrets/` folder and build output are ignored by Git. NVS and flash backups
contain credentials and should remain private. This development firmware does
not enable flash encryption or secure boot.

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements-dev.txt
.venv/bin/python tools/provision.py
```

The provisioning tool prompts for Wi-Fi SSID, password, and tunnel token.
Alternatively, pass `--credentials /private/path/credentials.json` containing
`ssid`, `password`, and `token`. It creates `secrets/nvs.bin` with mode 0600.
Copy `credentials.json.example` to `credentials.json` and replace the placeholders
to use this format. Files named `credentials.json` are ignored at any directory
depth; the example is safe to commit.
Updating the application alone preserves existing NVS. Flash `nvs.bin` only
when provisioning or intentionally replacing credentials.

## Flash the board on server

SSH access is `user@server`; the board is `/dev/cu.usbserial-0001`. Use
115200 baud on this connection: a 460800-baud full flash read encountered serial
corruption. Close serial monitors before flashing.

Back up the current flash before replacing it:

```sh
ssh user@server 'mkdir -p ~/esp32-cloudflare/backups; chmod 700 ~/esp32-cloudflare'
ssh user@server 'python3 -m esptool --chip esp32s3 --port /dev/cu.usbserial-0001 --baud 115200 read_flash 0 ALL ~/esp32-cloudflare/backups/before-cloudflare.bin'
```

Do not overwrite an existing original backup when performing later updates.
Copy the build and provisioning artifacts:

```sh
scp .pio/build/heltec_wifi_lora_32_v3/{bootloader,partitions,firmware}.bin \
    secrets/nvs.bin tools/monitor.py user@server:esp32-cloudflare/
ssh user@server 'chmod 600 ~/esp32-cloudflare/nvs.bin'
ssh user@server 'cd ~/esp32-cloudflare && python3 -m esptool --chip esp32s3 --port /dev/cu.usbserial-0001 --baud 115200 write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB 0x0 bootloader.bin 0x8000 partitions.bin 0x9000 nvs.bin 0x10000 firmware.bin'
```

For firmware-only updates, write only `0x10000 firmware.bin`. Read logs with:

```sh
ssh user@server 'python3 ~/esp32-cloudflare/monitor.py --seconds 60'
```

Successful startup logs show Wi-Fi connection, verified TLS, `TUNNEL REGISTERED`,
and the hostname received through remote configuration. An NTP time sync is
required before TLS certificate verification. Outbound TCP 7844 must be allowed.

To restore the original board state, write the saved full image at address 0:

```sh
ssh user@server 'python3 -m esptool --chip esp32s3 --port /dev/cu.usbserial-0001 --baud 115200 write_flash 0 ~/esp32-cloudflare/backups/before-cloudflare.bin'
```

## Verification

Compile the portable RPC codec with sanitizers and test against the actual
Cap'n Proto schemas, including multiple segments and malformed pointers:

```sh
mkdir -p tests/build
c++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -I src \
    src/rpc.cpp tests/rpc_cli.cpp -o tests/build/rpc_cli
.venv/bin/python -m unittest discover -s tests -v
.venv/bin/python tools/check_endpoint.py https://esp32.fothergill.com
```

## Protocol and scope

The ESP32 opens TLS to `region1.v2.argotunnel.com:7844` or region 2, verifying the
Cloudflare Origin CA chain and the `h2.cftunnel.com` certificate identity. Cloudflare
then acts as the HTTP/2 client on that connection. ESP-IDF's nghttp2 server handles
framing, HPACK, flow control, and multiplexing. A small bounded Cap'n Proto codec
implements bootstrap, named-tunnel registration, registration replies, and finish.

The client advertises `allow_remote_config` and `serialized_headers`, accepts
remote ingress for `http://localhost`, `http://localhost:80`, and a 404 fallback,
and checks request hostnames against those routes. Exact hostnames and leading
`*.` wildcards are supported. Path-regex ingress is rejected. HTTP/2 PINGs detect
stalled connections, with bounded exponential backoff and jitter on reconnect.
Wi-Fi reconnects automatically.

This is a focused telemetry connector, not a complete replacement for
`cloudflared`. It runs one edge connection, with no QUIC, TCP forwarding,
WebSockets, remote management logs, arbitrary origin proxying, or OTA updater.
It exposes the telemetry through the access policy already configured for the
tunnel; it does not add application authentication.

Protocol references:

- [Cloudflare HTTP/2 connection implementation](https://github.com/cloudflare/cloudflared/blob/master/connection/http2.go)
- [Tunnel registration schema](https://github.com/cloudflare/cloudflared/blob/master/tunnelrpc/proto/tunnelrpc.capnp)
- [Cloudflare feature negotiation](https://github.com/cloudflare/cloudflared/blob/master/features/features.go)
- [Cloudflare CA certificates](https://github.com/cloudflare/cloudflared/blob/master/tlsconfig/cloudflare_ca.go)

The vendored Cloudflare schema and CA certificates retain their upstream license
in `certs/LICENSE.cloudflare`. `tests/schema/tunnelrpc.capnp` removes only Go
annotations and adds a test result wrapper; the protocol fields are unchanged.

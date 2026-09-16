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
pinned in `platformio.ini`; PlatformIO installs the required ESP32-S3 toolchain.
Install PlatformIO Core, clone this repository, and run the commands below from
the repository directory on the computer connected to the board.

```sh
git clone https://github.com/afotherg/esp32-cloudflare-tunnel.git
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
source .venv/bin/activate
python -m pip install -r requirements-dev.txt "esptool>=4.8,<5"
python tools/provision.py
```

These examples use a macOS/Linux shell with the virtual environment activated.
On Windows, activate `.venv\Scripts\Activate.ps1` in PowerShell and adapt shell
variables and line continuations accordingly.

The provisioning tool prompts for Wi-Fi SSID, password, and tunnel token.
Alternatively, pass `--credentials /private/path/credentials.json` containing
`ssid`, `password`, and `token`. It creates `secrets/nvs.bin` with mode 0600.
Copy `credentials.json.example` to `credentials.json` and replace the placeholders
to use this format. Files named `credentials.json` are ignored at any directory
depth; the example is safe to commit.
Updating the application alone preserves existing NVS. Flash `nvs.bin` only
when provisioning or intentionally replacing credentials.

## Flash a locally connected board

Connect the Heltec V3 directly to your computer with a USB data cable. All
commands below run on that computer; no SSH connection or second machine is
needed. Close serial monitors before backing up, flashing, or restoring flash.

Find the board's serial port:

```sh
pio device list
```

Typical port names are `/dev/cu.usbserial-0001` or `/dev/cu.usbmodem…` on macOS,
`/dev/ttyUSB0` or `/dev/ttyACM0` on Linux, and `COM3` on Windows. Use the port
reported for **your board**, rather than copying an example unchanged. For the
macOS/Linux commands below, set:

```sh
ESP32_PORT=/dev/ttyUSB0
```

In PowerShell, assign `$ESP32_PORT = "COM3"` instead. The examples use 115200 baud
for reliable transfers. Keep the virtual environment from provisioning active
so `python -m esptool` uses the installed 4.x version.

Back up the current flash before replacing it. Store backups under the ignored
`secrets/` directory because they may contain credentials:

```sh
mkdir -p secrets/backups
umask 077
ESP32_BACKUP="secrets/backups/before-cloudflare-$(date +%Y%m%d-%H%M%S).bin"
python -m esptool --chip esp32s3 --port "$ESP32_PORT" --baud 115200 \
    read_flash 0 ALL "$ESP32_BACKUP"
```

Keep that filename for recovery. Do not overwrite the original backup during
later updates. On Windows, choose a unique filename under `secrets/backups`
and restrict access to that directory using your operating system's permissions.

Flash the bootloader, partition table, private provisioning image, and application
directly from the local build directory:

```sh
python -m esptool --chip esp32s3 --port "$ESP32_PORT" --baud 115200 \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x0 .pio/build/heltec_wifi_lora_32_v3/bootloader.bin \
    0x8000 .pio/build/heltec_wifi_lora_32_v3/partitions.bin \
    0x9000 secrets/nvs.bin \
    0x10000 .pio/build/heltec_wifi_lora_32_v3/firmware.bin
```

For subsequent firmware-only updates, rebuild and write only the application
partition, preserving the stored Wi-Fi and tunnel credentials:

```sh
pio run
python -m esptool --chip esp32s3 --port "$ESP32_PORT" --baud 115200 \
    write_flash 0x10000 .pio/build/heltec_wifi_lora_32_v3/firmware.bin
```

Read startup logs from the same local serial port:

```sh
python tools/monitor.py --port "$ESP32_PORT" --seconds 60
```

Successful startup logs show Wi-Fi connection, verified TLS, `TUNNEL REGISTERED`,
and the hostname received through remote configuration. An NTP time sync is
required before TLS certificate verification. Outbound TCP 7844 must be allowed.
Once provisioned, the board needs power and Wi-Fi; the computer is not needed
for serving the dashboard or maintaining the tunnel.

To restore the original board state, write the saved full image at address 0.
If using a new terminal session, set `ESP32_PORT` and `ESP32_BACKUP` to your
board's port and the existing backup filename first:

```sh
python -m esptool --chip esp32s3 --port "$ESP32_PORT" --baud 115200 \
    write_flash 0 "$ESP32_BACKUP"
```

## Verification

With the virtual environment active, compile the portable RPC codec with
sanitizers and test against the actual Cap'n Proto schemas, including multiple
segments and malformed pointers. Replace `https://esp32.example.com` with the
public hostname configured for your tunnel:

```sh
mkdir -p tests/build
c++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -I src \
    src/rpc.cpp tests/rpc_cli.cpp -o tests/build/rpc_cli
python -m unittest discover -s tests -v
python tools/check_endpoint.py https://esp32.example.com
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

### Cloudflare dashboard status: Degraded

Expect this client to show **Degraded** in the Cloudflare dashboard even while
the website and telemetry endpoints are working. Standard `cloudflared` maintains
four redundant connections to Cloudflare; this ESP32 implementation maintains
only **one** to limit memory usage. It therefore lacks the connection redundancy
associated with Cloudflare's **Healthy** status.

This status alone does not mean requests are failing. However, if the single
connection drops, requests can fail until the ESP32 reconnects. The firmware
reconnects automatically, but it has no second active connection to carry traffic
during recovery. See [Cloudflare's tunnel status definitions](https://developers.cloudflare.com/tunnel/troubleshooting/).

Protocol references:

- [Cloudflare HTTP/2 connection implementation](https://github.com/cloudflare/cloudflared/blob/master/connection/http2.go)
- [Tunnel registration schema](https://github.com/cloudflare/cloudflared/blob/master/tunnelrpc/proto/tunnelrpc.capnp)
- [Cloudflare feature negotiation](https://github.com/cloudflare/cloudflared/blob/master/features/features.go)
- [Cloudflare CA certificates](https://github.com/cloudflare/cloudflared/blob/master/tlsconfig/cloudflare_ca.go)

The vendored Cloudflare schema and CA certificates retain their upstream license
in `certs/LICENSE.cloudflare`. `tests/schema/tunnelrpc.capnp` removes only Go
annotations and adds a test result wrapper; the protocol fields are unchanged.

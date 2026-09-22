#include "../certs/cloudflare_origin_ca.hpp"
#include "cJSON.h"
#include "connection_state.hpp"
#include "dashboard_asset.hpp"
#include "driver/temp_sensor.h"
#include "edge_dns.hpp"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spi_flash.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/apps/sntp.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mbedtls/ssl.h"
#include "nghttp2/nghttp2.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "request_display.hpp"
#include "rpc.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>

static constexpr char TAG[] = "tunnel";
static EventGroupHandle_t events;
static esp_netif_t *netif;
static ConnectionState connections;
static std::atomic<unsigned> wifiGeneration{0};
static std::mutex handshakeMutex, routesMutex, temperatureMutex;
struct Route {
    std::string host, service;
};
static std::vector<Route> routes;
static int appliedVersion = -1;
static std::array<std::string, ConnectionState::desired> edgeAddresses;
static std::atomic<unsigned> connectionRequests[ConnectionState::desired]{};
static std::atomic<unsigned> connectionRetries[ConnectionState::desired]{};
static std::atomic<unsigned> stackFree[ConnectionState::desired]{};
static std::atomic<unsigned> requests{0}, reconnects{0};
static bool temperatureReady = false;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
static Json json(const char *s) { return Json(cJSON_Parse(s), cJSON_Delete); }
static std::string stringify(cJSON *j) {
    char *s = cJSON_PrintUnformatted(j);
    if (!s)
        throw std::bad_alloc();
    std::string out(s);
    cJSON_free(s);
    return out;
}
static std::string field(cJSON *j, const char *key) {
    auto v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static rpc::Bytes decode64(const std::string &v) {
    rpc::Bytes b(v.size() + 1);
    size_t n = 0;
    if (mbedtls_base64_decode(b.data(), b.size(), &n, reinterpret_cast<const uint8_t *>(v.data()),
                              v.size()) != 0)
        throw std::runtime_error("invalid base64 credential");
    b.resize(n);
    return b;
}
static std::string nvsString(nvs_handle_t n, const char *key) {
    size_t len = 0;
    ESP_ERROR_CHECK(nvs_get_str(n, key, nullptr, &len));
    if (len > 2048)
        throw std::runtime_error("credential too long");
    std::string out(len, '\0');
    ESP_ERROR_CHECK(nvs_get_str(n, key, &out[0], &len));
    out.resize(len - 1);
    return out;
}
static rpc::Credentials credentials;
static bool validIp(const std::string &ip) {
    uint8_t address[16];
    return inet_pton(AF_INET, ip.c_str(), address) == 1 ||
           inet_pton(AF_INET6, ip.c_str(), address) == 1;
}
static std::string requestIp(const std::string &serialized) {
    // Cloudflare's negotiated serialized_headers feature encodes each name/value
    // using unpadded base64. CF-Connecting-IP is set by the trusted edge.
    size_t start = 0;
    while (start < serialized.size()) {
        size_t end = serialized.find(';', start);
        if (end == std::string::npos)
            end = serialized.size();
        size_t colon = serialized.find(':', start);
        if (colon < end) {
            auto decode = [](std::string value) {
                while (value.size() % 4)
                    value += '=';
                auto raw = decode64(value);
                return std::string(raw.begin(), raw.end());
            };
            try {
                std::string name = decode(serialized.substr(start, colon - start));
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (name == "cf-connecting-ip") {
                    auto ip = decode(serialized.substr(colon + 1, end - colon - 1));
                    if (validIp(ip))
                        return ip;
                }
            } catch (...) {
                return {};
            }
        }
        start = end + 1;
    }
    return {};
}
static std::string telemetry(int connectionIndex = -1) {
    Json j(cJSON_CreateObject(), cJSON_Delete);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    multi_heap_info_t heap{};
    heap_caps_get_info(&heap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    wifi_ap_record_t ap{};
    esp_wifi_sta_get_ap_info(&ap);
    esp_netif_ip_info_t ip{};
    esp_netif_get_ip_info(netif, &ip);
    char ipstr[16];
    snprintf(ipstr, sizeof(ipstr), IPSTR, IP2STR(&ip.ip));
    cJSON_AddStringToObject(j.get(), "device", "Heltec WiFi LoRa 32 V3");
    cJSON_AddStringToObject(j.get(), "chip", "ESP32-S3");
    cJSON_AddStringToObject(j.get(), "firmware", "esp32-native-0.2.0");
    cJSON_AddNumberToObject(j.get(), "chip_revision", chip.revision);
    cJSON_AddNumberToObject(j.get(), "cpu_cores", chip.cores);
    cJSON_AddNumberToObject(j.get(), "cpu_frequency_mhz", CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ);
    cJSON_AddNumberToObject(j.get(), "uptime_seconds", esp_timer_get_time() / 1000000.0);
    std::lock_guard<std::mutex> temperatureLock(temperatureMutex);
    float temp = 0;
    if (temperatureReady && temp_sensor_read_celsius(&temp) == ESP_OK && std::isfinite(temp))
        cJSON_AddNumberToObject(j.get(), "chip_temperature_c", temp);
    else
        cJSON_AddNullToObject(j.get(), "chip_temperature_c");
    cJSON_AddStringToObject(j.get(), "temperature_note",
                            "Internal chip temperature; not ambient air temperature");
    cJSON_AddNumberToObject(j.get(), "heap_used_bytes", heap.total_allocated_bytes);
    cJSON_AddNumberToObject(j.get(), "heap_free_bytes", heap.total_free_bytes);
    cJSON_AddNumberToObject(j.get(), "heap_minimum_free_bytes", heap.minimum_free_bytes);
    cJSON_AddNumberToObject(j.get(), "heap_largest_free_block_bytes", heap.largest_free_block);
    cJSON_AddNumberToObject(j.get(), "flash_bytes", spi_flash_get_chip_size());
    cJSON_AddNumberToObject(j.get(), "wifi_rssi_dbm", ap.rssi);
    cJSON_AddStringToObject(j.get(), "local_ip", ipstr);
    const unsigned count = ConnectionState::count(connections.snapshot());
    cJSON_AddBoolToObject(j.get(), "tunnel_connected", count > 0);
    cJSON_AddBoolToObject(j.get(), "tunnel_healthy", count == ConnectionState::desired);
    cJSON_AddNumberToObject(j.get(), "tunnel_connections", count);
    cJSON_AddNumberToObject(j.get(), "tunnel_connections_desired", ConnectionState::desired);
    cJSON_AddNumberToObject(j.get(), "served_by_connection", connectionIndex);
    auto details = cJSON_AddArrayToObject(j.get(), "connections");
    for (unsigned i = 0; i < ConnectionState::desired; ++i) {
        auto item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", i);
        cJSON_AddNumberToObject(item, "requests", connectionRequests[i]);
        cJSON_AddNumberToObject(item, "reconnections", connectionRetries[i]);
        cJSON_AddNumberToObject(item, "stack_free_bytes", stackFree[i]);
        cJSON_AddItemToArray(details, item);
    }
    cJSON_AddStringToObject(j.get(), "tunnel_transport", "native TLS/HTTP2");
    cJSON_AddNumberToObject(j.get(), "requests_served", requests);
    cJSON_AddNumberToObject(j.get(), "reconnections", reconnects);
    cJSON_AddNumberToObject(j.get(), "reset_reason", esp_reset_reason());
    cJSON_AddBoolToObject(j.get(), "oled_ready", requestDisplayHealthy());
    cJSON_AddNumberToObject(j.get(), "oled_updates", requestDisplayUpdates());
    cJSON_AddStringToObject(j.get(), "sdk_version", esp_get_idf_version());
    return stringify(j.get());
}
static esp_err_t localHandler(httpd_req_t *r) {
    try {
        sockaddr_storage peer{};
        socklen_t length = sizeof(peer);
        char address[46]{};
        if (getpeername(httpd_req_to_sockfd(r), reinterpret_cast<sockaddr *>(&peer), &length) ==
            0) {
            if (peer.ss_family == AF_INET)
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(&peer)->sin_addr, address,
                          sizeof(address));
            else if (peer.ss_family == AF_INET6)
                inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6 *>(&peer)->sin6_addr, address,
                          sizeof(address));
        }
        requestDisplayRecord(address);
        ++requests;
        if (std::string(r->uri).substr(0, std::string(r->uri).find('?')) == "/") {
            httpd_resp_set_type(r, "text/html; charset=utf-8");
            httpd_resp_set_hdr(r, "Cache-Control", "no-store");
            return httpd_resp_send(r, DASHBOARD_HTML, sizeof(DASHBOARD_HTML) - 1);
        }
        auto body = telemetry();
        httpd_resp_set_type(r, "application/json");
        httpd_resp_set_hdr(r, "Cache-Control", "no-store");
        return httpd_resp_send(r, body.data(), body.size());
    } catch (...) {
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "telemetry unavailable");
    }
}
struct Stream {
    std::string method, path, host, upgrade, input, output, clientIp;
    const char *staticBody = nullptr;
    size_t staticLength = 0;
    size_t offset = 0, headerBytes = 0;
    bool control = false, responded = false;
};
static std::string chooseEdge(const char *region, unsigned index, unsigned attempt) {
    esp_netif_dns_info_t dns{};
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK ||
        dns.ip.type != ESP_IPADDR_TYPE_V4)
        throw std::runtime_error("IPv4 DNS unavailable");
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
        throw std::runtime_error("DNS socket unavailable");
    struct SocketCloser {
        int fd;
        ~SocketCloser() { close(fd); }
    } closer{fd};
    sockaddr_in resolver{};
    resolver.sin_family = AF_INET;
    resolver.sin_port = htons(53);
    resolver.sin_addr.s_addr = dns.ip.u_addr.ip4.addr;
    timeval timeout{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, reinterpret_cast<sockaddr *>(&resolver), sizeof(resolver)) != 0)
        throw std::runtime_error("DNS connect failed");
    auto query = edge_dns::query(region, uint16_t(esp_random()));
    if (send(fd, query.data(), query.size(), 0) != int(query.size()))
        throw std::runtime_error("DNS send failed");
    std::vector<uint8_t> response(2048);
    int received = recv(fd, response.data(), response.size(), 0);
    if (received <= 0)
        throw std::runtime_error("DNS timeout");
    response.resize(received);
    auto addresses = edge_dns::parse(response, query);
    for (unsigned n = 0; n < addresses.size(); ++n) {
        const auto &address = addresses[(index / 2 + attempt + n) % addresses.size()];
        char text[16];
        inet_ntop(AF_INET, address.data(), text, sizeof(text));
        if (std::find(edgeAddresses.begin(), edgeAddresses.end(), text) == edgeAddresses.end())
            return text;
    }
    throw std::runtime_error("No unused edge address available");
}
class Tunnel {
    esp_tls_t *tls = nullptr;
    nghttp2_session *h2 = nullptr;
    std::map<int32_t, Stream> streams;
    rpc::Bytes rpcInput;
    int32_t controlId = 0;
    bool failed = false;
    const unsigned index;
    bool registered = false;
    static ssize_t frameLength(nghttp2_session *, uint8_t, int32_t, int32_t connectionWindow,
                               int32_t streamWindow, uint32_t maxFrame, void *) {
        return std::min({uint32_t(connectionWindow), uint32_t(streamWindow), maxFrame, 2048u});
    }
    int64_t began = 0, lastRx = 0, lastPing = 0;
    static Tunnel &self(void *p) { return *static_cast<Tunnel *>(p); }
    static nghttp2_nv nv(const char *name, const char *value) {
        return {reinterpret_cast<uint8_t *>(const_cast<char *>(name)),
                reinterpret_cast<uint8_t *>(const_cast<char *>(value)), strlen(name), strlen(value),
                NGHTTP2_NV_FLAG_NONE};
    }
    static ssize_t readData(nghttp2_session *, int32_t id, uint8_t *buf, size_t len,
                            uint32_t *flags, nghttp2_data_source *, void *p) {
        auto &t = self(p);
        auto it = t.streams.find(id);
        if (it == t.streams.end())
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        auto &s = it->second;
        const size_t size = s.staticBody ? s.staticLength : s.output.size();
        const char *body = s.staticBody ? s.staticBody : s.output.data();
        size_t n = std::min(len, size - s.offset);
        if (n)
            memcpy(buf, body + s.offset, n);
        s.offset += n;
        if (s.offset == size) {
            if (!s.control)
                *flags |= NGHTTP2_DATA_FLAG_EOF;
            else if (n == 0)
                return NGHTTP2_ERR_DEFERRED;
        }
        return n;
    }
    void respond(int32_t id, const std::string &body, const char *status = "200", bool head = false,
                 bool html = false) {
        auto &s = streams.at(id);
        s.responded = true;
        s.output = head ? "" : body;
        s.staticBody = html && !head ? DASHBOARD_HTML : nullptr;
        s.staticLength = html && !head ? sizeof(DASHBOARD_HTML) - 1 : 0;
        s.offset = 0;
        auto headers = std::vector<nghttp2_nv>{
            nv(":status", status), nv("cf-cloudflared-response-meta", "{\"src\":\"origin\"}"),
            nv("cf-cloudflared-response-headers",
               html ? "Q29udGVudC1UeXBl:dGV4dC9odG1sOyBjaGFyc2V0PXV0Zi04;Q2FjaGUtQ29udHJvbA:"
                      "bm8tc3RvcmU"
                    : "Q29udGVudC1UeXBl:YXBwbGljYXRpb24vanNvbg;Q2FjaGUtQ29udHJvbA:bm8tc3RvcmU")};
        nghttp2_data_provider provider{};
        provider.read_callback = readData;
        if (nghttp2_submit_response(h2, id, headers.data(), headers.size(), &provider) != 0)
            failed = true;
    }
    void queue(const rpc::Bytes &bytes) {
        auto &s = streams.at(controlId);
        if (s.offset) {
            s.output.erase(0, s.offset);
            s.offset = 0;
        }
        s.output.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        nghttp2_session_resume_data(h2, controlId);
    }
    static int beginHeaders(nghttp2_session *, const nghttp2_frame *f, void *p) {
        auto &t = self(p);
        if (f->hd.type == NGHTTP2_HEADERS && f->headers.cat == NGHTTP2_HCAT_REQUEST) {
            if (t.streams.size() >= 20)
                return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
            t.streams.emplace(f->hd.stream_id, Stream{});
        }
        return 0;
    }
    static int header(nghttp2_session *, const nghttp2_frame *f, const uint8_t *name, size_t nl,
                      const uint8_t *value, size_t vl, uint8_t, void *p) {
        auto &t = self(p);
        auto it = t.streams.find(f->hd.stream_id);
        if (it == t.streams.end())
            return 0;
        auto &s = it->second;
        s.headerBytes += nl + vl;
        if (s.headerBytes > 8192)
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        std::string key(reinterpret_cast<const char *>(name), nl),
            v(reinterpret_cast<const char *>(value), vl);
        if (key == ":method")
            s.method = v;
        else if (key == ":path")
            s.path = v;
        else if (key == ":authority")
            s.host = v;
        else if (key == "cf-cloudflared-proxy-connection-upgrade")
            s.upgrade = v;
        else if (key == "cf-connecting-ip" && validIp(v))
            s.clientIp = v;
        else if (key == "cf-cloudflared-request-headers") {
            auto ip = requestIp(v);
            if (!ip.empty())
                s.clientIp = ip;
        }
        return 0;
    }
    void configuration(int32_t id) {
        auto &s = streams.at(id);
        auto j = json(s.input.c_str());
        auto v = cJSON_GetObjectItemCaseSensitive(j.get(), "version");
        auto config = cJSON_GetObjectItemCaseSensitive(j.get(), "config");
        auto ingress = cJSON_GetObjectItemCaseSensitive(config, "ingress");
        bool valid = cJSON_IsNumber(v) && cJSON_IsArray(ingress) && cJSON_GetArraySize(ingress) > 0;
        std::vector<Route> next;
        cJSON *rule = nullptr;
        cJSON_ArrayForEach(rule, ingress) {
            auto service = field(rule, "service");
            if (service != "http://localhost" && service != "http://localhost:80" &&
                service != "http_status:404")
                valid = false;
            if (!field(rule, "path").empty())
                valid = false;
            next.push_back({field(rule, "hostname"), service});
        }
        std::lock_guard<std::mutex> lock(routesMutex);
        if (valid && v->valueint > appliedVersion) {
            routes = std::move(next);
            appliedVersion = v->valueint;
        }
        Json reply(cJSON_CreateObject(), cJSON_Delete);
        cJSON_AddNumberToObject(reply.get(), "lastAppliedVersion", appliedVersion);
        if (!valid)
            cJSON_AddStringToObject(reply.get(), "err",
                                    "This telemetry client supports only http://localhost[:80] and "
                                    "http_status:404 ingress");
        else {
            cJSON_ArrayForEach(rule, ingress) {
                auto hostname = field(rule, "hostname");
                if (!hostname.empty())
                    ESP_LOGI(TAG, "Public endpoint: https://%s", hostname.c_str());
            }
        }
        respond(id, stringify(reply.get()));
    }
    std::string serviceFor(std::string host) const {
        host = host.substr(0, host.find(':'));
        std::transform(host.begin(), host.end(), host.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        std::lock_guard<std::mutex> lock(routesMutex);
        if (appliedVersion < 0)
            return "pending";
        for (const auto &r : routes) {
            if (r.host.empty() || host == r.host)
                return r.service;
            if (r.host.compare(0, 2, "*.") == 0) {
                auto suffix = r.host.substr(1);
                if (host.size() > suffix.size() &&
                    host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0)
                    return r.service;
            }
        }
        return "http_status:404";
    }
    static int frame(nghttp2_session *, const nghttp2_frame *f, void *p) {
        auto &t = self(p);
        t.lastRx = esp_timer_get_time();
        if (f->hd.type == NGHTTP2_GOAWAY) {
            ESP_LOGW(TAG, "Connection %u received GOAWAY code=%lu", t.index,
                     static_cast<unsigned long>(f->goaway.error_code));
            t.failed = true;
            return 0;
        }
        auto it = t.streams.find(f->hd.stream_id);
        if (it == t.streams.end())
            return 0;
        auto &s = it->second;
        try {
            if (f->hd.type == NGHTTP2_HEADERS && !s.responded) {
                if (s.upgrade.empty())
                    requestDisplayRecord(s.clientIp.c_str());
                if (s.upgrade == "control-stream") {
                    if (t.controlId)
                        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
                    t.controlId = f->hd.stream_id;
                    s.control = true;
                    t.respond(t.controlId, "");
                    t.queue(rpc::bootstrap());
                    auto registrationCredentials = credentials;
                    esp_netif_ip_info_t ip{};
                    esp_netif_get_ip_info(netif, &ip);
                    auto bytes = reinterpret_cast<uint8_t *>(&ip.ip.addr);
                    registrationCredentials.ip.assign(bytes, bytes + 4);
                    t.queue(rpc::registration(registrationCredentials, t.index));
                    ESP_LOGI(TAG, "Control stream opened; registering connector");
                } else if (s.upgrade == "update-configuration") {
                    // Configuration JSON arrives as DATA and is acknowledged at END_STREAM.
                } else if (!s.upgrade.empty())
                    t.respond(f->hd.stream_id, "{\"error\":\"unsupported protocol\"}", "501");
                else if (s.method != "GET" && s.method != "HEAD")
                    t.respond(f->hd.stream_id, "{\"error\":\"use GET or HEAD\"}", "405");
                else {
                    auto path = s.path.substr(0, s.path.find('?'));
                    const auto service = t.serviceFor(s.host);
                    if (service == "pending")
                        t.respond(f->hd.stream_id, "{\"error\":\"configuration pending\"}", "503",
                                  s.method == "HEAD");
                    else if (service == "http_status:404" ||
                             (path != "/" && path != "/api/telemetry" && path != "/healthz"))
                        t.respond(f->hd.stream_id, "{\"error\":\"not found\"}", "404",
                                  s.method == "HEAD");
                    else {
                        ++requests;
                        ++connectionRequests[t.index];
                        t.respond(f->hd.stream_id, path == "/" ? "" : telemetry(t.index), "200",
                                  s.method == "HEAD", path == "/");
                    }
                }
            }
            if ((f->hd.type == NGHTTP2_DATA || f->hd.type == NGHTTP2_HEADERS) &&
                (f->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
                if (s.control)
                    t.failed = true;
                else if (s.upgrade == "update-configuration" && !s.responded)
                    t.configuration(f->hd.stream_id);
            }
        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "Frame processing failed: %s", e.what());
            t.failed = true;
        }
        return 0;
    }
    static int data(nghttp2_session *, uint8_t, int32_t id, const uint8_t *data, size_t len,
                    void *p) {
        auto &t = self(p);
        auto it = t.streams.find(id);
        if (it == t.streams.end())
            return 0;
        try {
            if (it->second.control) {
                if (t.rpcInput.size() + len > 16384)
                    throw std::runtime_error("RPC input limit");
                t.rpcInput.insert(t.rpcInput.end(), data, data + len);
                while (size_t size = rpc::frameSize(t.rpcInput)) {
                    rpc::Bytes frame(t.rpcInput.begin(), t.rpcInput.begin() + size);
                    t.rpcInput.erase(t.rpcInput.begin(), t.rpcInput.begin() + size);
                    auto reply = rpc::parse(frame);
                    if (reply.kind == rpc::Reply::Registered) {
                        t.registered = true;
                        connections.set(t.index, true);
                        ESP_LOGI(TAG, "TUNNEL REGISTERED index=%u at %s (%u/4)", t.index,
                                 reply.detail.c_str(),
                                 ConnectionState::count(connections.snapshot()));
                        t.queue(rpc::finish(1));
                    } else if (reply.kind == rpc::Reply::Rejected) {
                        // Do not log server-provided credential-related error text.
                        ESP_LOGE(TAG, "Tunnel registration rejected by edge");
                        t.failed = true;
                    }
                }
            } else if (it->second.upgrade == "update-configuration") {
                if (it->second.input.size() + len > 16384)
                    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
                it->second.input.append(reinterpret_cast<const char *>(data), len);
            }
        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "Control processing failed: %s", e.what());
            t.failed = true;
        }
        return 0;
    }
    static int invalidFrame(nghttp2_session *, const nghttp2_frame *, int error, void *p) {
        ESP_LOGW(TAG, "Connection %u invalid HTTP/2 frame: %s", self(p).index,
                 nghttp2_strerror(error));
        return 0;
    }
    static int closeStream(nghttp2_session *, int32_t id, uint32_t, void *p) {
        auto &t = self(p);
        if (id == t.controlId)
            t.failed = true;
        t.streams.erase(id);
        return 0;
    }

  public:
    explicit Tunnel(unsigned connectionIndex) : index(connectionIndex) {}
    ~Tunnel() {
        connections.set(index, false);
        if (h2)
            nghttp2_session_del(h2);
        if (tls)
            esp_tls_conn_destroy(tls);
        std::lock_guard<std::mutex> lock(handshakeMutex);
        edgeAddresses[index].clear();
    }
    void run(unsigned attempt) {
        std::unique_lock<std::mutex> handshake(handshakeMutex);
        const char *region = index % 2 ? "region2.v2.argotunnel.com" : "region1.v2.argotunnel.com";
        const std::string host = chooseEdge(region, index, attempt);
        edgeAddresses[index] = host;
        esp_tls_cfg_t cfg{};
        cfg.common_name = "h2.cftunnel.com";
        cfg.cacert_buf = reinterpret_cast<const uint8_t *>(CLOUDFLARE_CA);
        cfg.cacert_bytes = sizeof(CLOUDFLARE_CA);
        cfg.timeout_ms = 15000;
        tls = esp_tls_init();
        if (!tls)
            throw std::bad_alloc();
        ESP_LOGI(TAG, "Connecting index=%u to %s:7844 with verified TLS", index, host.c_str());
        if (esp_tls_conn_new_sync(host.c_str(), host.size(), 7844, &cfg, tls) != 1)
            throw std::runtime_error("TLS connect failed");
        const unsigned generation = wifiGeneration.load();
        handshake.unlock();
        int fd = -1;
        ESP_ERROR_CHECK(esp_tls_get_conn_sockfd(tls, &fd));
        timeval tv{1, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        nghttp2_session_callbacks *cb = nullptr;
        if (nghttp2_session_callbacks_new(&cb))
            throw std::bad_alloc();
        nghttp2_session_callbacks_set_data_source_read_length_callback(cb, frameLength);
        nghttp2_session_callbacks_set_on_begin_headers_callback(cb, beginHeaders);
        nghttp2_session_callbacks_set_on_header_callback(cb, header);
        nghttp2_session_callbacks_set_on_frame_recv_callback(cb, frame);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, data);
        nghttp2_session_callbacks_set_on_stream_close_callback(cb, closeStream);
        nghttp2_session_callbacks_set_on_invalid_frame_recv_callback(cb, invalidFrame);
        int ret = nghttp2_session_server_new(&h2, cb, this);
        nghttp2_session_callbacks_del(cb);
        if (ret)
            throw std::runtime_error("HTTP/2 initialization failed");
        nghttp2_settings_entry settings[] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 16},
                                             {NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, 4096},
                                             {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, 8192}};
        nghttp2_submit_settings(h2, NGHTTP2_FLAG_NONE, settings, 3);
        began = lastRx = lastPing = esp_timer_get_time();
        while (!failed && (xEventGroupGetBits(events) & 1) && wifiGeneration == generation) {
            const uint8_t *out = nullptr;
            ssize_t size;
            while ((size = nghttp2_session_mem_send(h2, &out)) > 0) {
                size_t pos = 0;
                while (pos < size_t(size)) {
                    ssize_t n = esp_tls_conn_write(tls, out + pos, size - pos);
                    if (n <= 0)
                        throw std::runtime_error("TLS write failed");
                    pos += n;
                }
            }
            if (size < 0)
                throw std::runtime_error("HTTP/2 write failed");
            uint8_t in[2048];
            ssize_t n = esp_tls_conn_read(tls, in, sizeof(in));
            if (n > 0) {
                ssize_t used = nghttp2_session_mem_recv(h2, in, n);
                if (used < 0 || used != n)
                    throw std::runtime_error("HTTP/2 receive failed");
            } else if (n == 0)
                throw std::runtime_error("edge closed TLS");
            else if (n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE &&
                     n != MBEDTLS_ERR_SSL_TIMEOUT)
                throw std::runtime_error("TLS read failed");
            int64_t now = esp_timer_get_time();
            if (!registered && now - began > 45000000)
                throw std::runtime_error("registration timeout");
            if (now - lastRx > 90000000)
                throw std::runtime_error("edge heartbeat timeout");
            if (now - lastPing > 25000000) {
                uint8_t ping[8]{};
                memcpy(ping, &now, 8);
                nghttp2_submit_ping(h2, 0, ping);
                lastPing = now;
            }
            stackFree[index] = uxTaskGetStackHighWaterMark(nullptr);
            vTaskDelay(1);
        }
    }
};
static void wifiEvent(void *, esp_event_base_t base, int32_t id, void *arg) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
        esp_wifi_connect();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ++wifiGeneration;
        xEventGroupClearBits(events, 1);
        connections.clear();
        esp_wifi_connect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto e = static_cast<ip_event_got_ip_t *>(arg);
        ESP_LOGI(TAG, "Wi-Fi connected: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(events, 1);
    }
}
static void tunnelWorker(void *arg) {
    const unsigned index = reinterpret_cast<uintptr_t>(arg);
    vTaskDelay(pdMS_TO_TICKS(index * 1500));
    unsigned failures = 0;
    for (unsigned attempt = 0;; ++attempt) {
        xEventGroupWaitBits(events, 1, pdFALSE, pdTRUE, portMAX_DELAY);
        while (time(nullptr) < 1700000000) {
            ESP_LOGI(TAG, "Waiting for time sync for certificate verification");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        int64_t started = esp_timer_get_time();
        try {
            Tunnel t(index);
            t.run(attempt);
        } catch (const std::exception &e) {
            ESP_LOGW(TAG, "Connection %u ended: %s", index, e.what());
        }
        connections.set(index, false);
        ++connectionRetries[index];
        ++reconnects;
        if (esp_timer_get_time() - started > 120000000)
            failures = 0;
        unsigned delay = std::min(60u, 2u << std::min(failures++, 5u));
        vTaskDelay(pdMS_TO_TICKS(delay * 1000 + esp_random() % 1000));
    }
}
extern "C" void app_main() {
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();
    requestDisplayInit();
    // Never erase NVS automatically: it contains the provisioned tunnel credentials.
    ESP_ERROR_CHECK(nvs_flash_init());
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open("tunnel", NVS_READONLY, &nvs));
    std::string ssid = nvsString(nvs, "ssid"), password = nvsString(nvs, "password"),
                token = nvsString(nvs, "token");
    nvs_close(nvs);
    auto raw = decode64(token);
    raw.push_back(0);
    auto secret = json(reinterpret_cast<const char *>(raw.data()));
    credentials.account = field(secret.get(), "a");
    credentials.secret = decode64(field(secret.get(), "s"));
    std::string uuid = field(secret.get(), "t");
    uuid.erase(std::remove(uuid.begin(), uuid.end(), '-'), uuid.end());
    if (uuid.size() != 32) {
        ESP_LOGE(TAG, "Invalid tunnel ID");
        return;
    }
    for (size_t i = 0; i < 32; i += 2) {
        char *end = nullptr;
        auto pair = uuid.substr(i, 2);
        unsigned long v = strtoul(pair.c_str(), &end, 16);
        if (*end)
            return;
        credentials.tunnel.push_back(v);
    }
    credentials.client.resize(16);
    esp_fill_random(credentials.client.data(), 16);
    credentials.client[6] = (credentials.client[6] & 15) | 0x40;
    credentials.client[8] = (credentials.client[8] & 63) | 0x80;
    // Keep token material out of telemetry and log output.
    std::fill(token.begin(), token.end(), 0);
    std::fill(raw.begin(), raw.end(), 0);
    secret.reset();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    events = xEventGroupCreate();
    netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, "esp32-cloudflare");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEvent, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifiEvent, nullptr));
    wifi_config_t wifi{};
    if (ssid.size() > 32 || password.size() > 63)
        return;
    memcpy(wifi.sta.ssid, ssid.data(), ssid.size());
    memcpy(wifi.sta.password, password.data(), password.size());
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    temp_sensor_config_t ts = TSENS_CONFIG_DEFAULT();
    temperatureReady = temp_sensor_set_config(ts) == ESP_OK && temp_sensor_start() == ESP_OK;
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.stack_size = 6144;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &http) == ESP_OK)
        for (const char *uri : {"/", "/api/telemetry", "/healthz"}) {
            httpd_uri_t route{};
            route.uri = uri;
            route.method = HTTP_GET;
            route.handler = localHandler;
            httpd_register_uri_handler(server, &route);
        }
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, const_cast<char *>("pool.ntp.org"));
    sntp_setservername(1, const_cast<char *>("time.nist.gov"));
    sntp_init();
    for (unsigned i = 1; i < ConnectionState::desired; ++i) {
        if (xTaskCreate(tunnelWorker, "tunnel", 12288, reinterpret_cast<void *>(i), 5, nullptr) !=
            pdPASS)
            ESP_LOGE(TAG, "Unable to allocate worker %u", i);
    }
    tunnelWorker(nullptr);
}

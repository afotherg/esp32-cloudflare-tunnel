#include "request_display.hpp"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
constexpr auto BUS = I2C_NUM_0;
constexpr uint8_t ADDRESS = 0x3c;
QueueHandle_t queue = nullptr;
std::atomic<bool> healthy{false};
std::atomic<uint32_t> updates{0};
struct Request {
    time_t time;
    char ip[46];
};
uint8_t pixels[1024];
// Five-column, seven-row glyphs. Lowercase IPv6 letters use the uppercase glyphs.
constexpr uint8_t digits[][5] = {{62, 81, 73, 69, 62}, {0, 66, 127, 64, 0},   {66, 97, 81, 73, 70},
                                 {33, 65, 69, 75, 49}, {24, 20, 18, 127, 16}, {39, 69, 69, 69, 57},
                                 {60, 74, 73, 73, 48}, {1, 113, 9, 5, 3},     {54, 73, 73, 73, 54},
                                 {6, 73, 73, 41, 30}};
constexpr uint8_t letters[][5] = {
    {126, 17, 17, 17, 126}, {127, 73, 73, 73, 54}, {62, 65, 65, 65, 34},  {127, 65, 65, 34, 28},
    {127, 73, 73, 73, 65},  {127, 9, 9, 9, 1},     {62, 65, 73, 73, 122}, {127, 8, 8, 8, 127},
    {0, 65, 127, 65, 0},    {32, 64, 65, 63, 1},   {127, 8, 20, 34, 65},  {127, 64, 64, 64, 64},
    {127, 2, 12, 2, 127},   {127, 4, 8, 16, 127},  {62, 65, 65, 65, 62},  {127, 9, 9, 9, 6},
    {62, 65, 81, 33, 94},   {127, 9, 25, 41, 70},  {70, 73, 73, 73, 49},  {1, 1, 127, 1, 1},
    {63, 64, 64, 64, 63},   {31, 32, 64, 32, 31},  {63, 64, 56, 64, 63},  {99, 20, 8, 20, 99},
    {7, 8, 112, 8, 7},      {97, 81, 73, 69, 67}};
void text(int y, const char *value) {
    int x = 1;
    while (*value && x + 5 < 128) {
        char ch = *value++;
        if (ch >= 'a' && ch <= 'z')
            ch -= 32;
        uint8_t glyph[5]{};
        if (ch >= '0' && ch <= '9')
            memcpy(glyph, digits[ch - '0'], 5);
        else if (ch >= 'A' && ch <= 'Z')
            memcpy(glyph, letters[ch - 'A'], 5);
        else if (ch == '.')
            glyph[2] = 64;
        else if (ch == ':')
            glyph[2] = 36;
        else if (ch == '-')
            memset(glyph, 8, 5);
        else if (ch == '/') {
            glyph[0] = 32;
            glyph[1] = 16;
            glyph[2] = 8;
            glyph[3] = 4;
            glyph[4] = 2;
        }
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if ((glyph[col] & (1 << row)) && y + row < 64)
                    pixels[(y + row) / 8 * 128 + x + col] |= 1 << ((y + row) % 8);
        x += 6;
    }
}
bool displayWrite(uint8_t control, const uint8_t *data, size_t size) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd)
        return false;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, ADDRESS << 1, true);
    i2c_master_write_byte(cmd, control, true);
    i2c_master_write(cmd, data, size, true);
    i2c_master_stop(cmd);
    bool ok = i2c_master_cmd_begin(BUS, cmd, pdMS_TO_TICKS(100)) == ESP_OK;
    i2c_cmd_link_delete(cmd);
    return ok;
}
bool flush() {
    const uint8_t range[] = {0x21, 0, 127, 0x22, 0, 7};
    bool ok = displayWrite(0, range, sizeof(range)) && displayWrite(0x40, pixels, sizeof(pixels));
    healthy = ok;
    return ok;
}
void worker(void *) {
    Request latest{};
    for (;;) {
        if (xQueueReceive(queue, &latest, portMAX_DELAY) != pdTRUE)
            continue;
        while (xQueueReceive(queue, &latest, 0) == pdTRUE) {
        }
        memset(pixels, 0, sizeof(pixels));
        text(0, "LATEST REQUEST");
        char date[22], clock[22];
        if (latest.time >= 1700000000) {
            tm local{};
            localtime_r(&latest.time, &local);
            strftime(date, sizeof(date), "%Y-%m-%d", &local);
            strftime(clock, sizeof(clock), "%H:%M:%S %Z", &local);
            text(12, date);
            text(24, clock);
        } else
            text(18, "TIME NOT SYNCED");
        text(36, "CLIENT IP");
        char first[22]{};
        strncpy(first, latest.ip, 21);
        text(46, first);
        if (strlen(latest.ip) > 21)
            text(55, latest.ip + 21);
        if (flush()) {
            ++updates;
            ESP_LOGD(
                "oled", "Request displayed; I2C acknowledged; time %s; client IP %s (update %u)",
                latest.time >= 1700000000 ? "synced" : "pending",
                strcmp(latest.ip, "UNAVAILABLE") ? "present" : "missing", unsigned(updates.load()));
        } else
            ESP_LOGW("oled", "Display transfer failed");
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
} // namespace
void requestDisplayInit() {
    gpio_set_direction(GPIO_NUM_36, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_36, 0);
    gpio_set_direction(GPIO_NUM_21, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_21, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(GPIO_NUM_21, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    i2c_config_t cfg{};
    cfg.mode = I2C_MODE_MASTER;
    cfg.sda_io_num = 17;
    cfg.scl_io_num = 18;
    cfg.sda_pullup_en = true;
    cfg.scl_pullup_en = true;
    cfg.master.clk_speed = 400000;
    if (i2c_param_config(BUS, &cfg) != ESP_OK ||
        i2c_driver_install(BUS, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK)
        return;
    const uint8_t init[] = {0xae, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0,    0x40, 0x8d,
                            0x14, 0x20, 0,    0xa1, 0xc8, 0xda, 0x12, 0x81, 0x7f,
                            0xd9, 0xf1, 0xdb, 0x40, 0xa4, 0xa6, 0xaf};
    if (!displayWrite(0, init, sizeof(init))) {
        ESP_LOGW("oled", "Display not detected");
        return;
    }
    text(0, "LATEST REQUEST");
    text(24, "WAITING FOR REQUEST");
    flush();
    queue = xQueueCreate(1, sizeof(Request));
    if (!queue) {
        healthy = false;
        return;
    }
    if (xTaskCreate(worker, "request_oled", 3072, nullptr, 2, nullptr) != pdPASS) {
        vQueueDelete(queue);
        queue = nullptr;
        healthy = false;
    }
}
void requestDisplayRecord(const char *ip) {
    if (!queue)
        return;
    Request request{};
    request.time = time(nullptr);
    snprintf(request.ip, sizeof(request.ip), "%s", ip && *ip ? ip : "UNAVAILABLE");
    xQueueOverwrite(queue, &request);
}
bool requestDisplayHealthy() { return healthy; }
uint32_t requestDisplayUpdates() { return updates; }

#include "stream.h"

#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#include <atomic>
#include <memory>
#include <vector>

#include "core/codec.h"
#include "web_page.h"

namespace stream {

namespace {

constexpr uint32_t FRAME_MS = 33;  // 30 fps: every other Game Boy frame
constexpr int MAX_VIEWERS = 2;
constexpr uint32_t NEEDS_HEAP = 60 * 1024;  // Wi-Fi, TCP and a few messages
constexpr size_t CHUNK = 4096;
constexpr uint32_t MAX_WAIT_MS = 25;        // for the phone to take a frame, then the rest waits

AsyncWebServer *server = nullptr;
AsyncWebSocket *ws = nullptr;
codec::Encoder encoder;
std::atomic<bool> newViewer{false};
std::atomic<uint8_t> buttons{0};
uint32_t lastSent = 0, frames = 0, messages = 0;
bool on = false;

void onEvent(AsyncWebSocket *, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        client->setCloseClientOnQueueFull(false);
        newViewer = true;
    } else if (type == WS_EVT_DISCONNECT) {
        buttons = 0;
    } else if (type == WS_EVT_DATA) {
        auto *info = static_cast<AwsFrameInfo *>(arg);
        // [2, buttons] from the on-screen pad
        if (info->final && info->index == 0 && info->len == 2 && len == 2 && data[0] == 2) buttons = data[1];
    }
}

}  // namespace

bool start() {
    if (on) return true;
    // Wi-Fi allocates many small buffers: the total counts, not the largest block.
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < NEEDS_HEAP) return false;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(SSID, PASSWORD);
    WiFi.setSleep(false);  // power save adds tens of ms to every frame
    server = new AsyncWebServer(80);
    ws = new AsyncWebSocket("/ws");
    ws->onEvent(onEvent);
    server->addHandler(ws);
    server->on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send(200, "text/html; charset=utf-8", reinterpret_cast<const uint8_t *>(WEB_PAGE), sizeof(WEB_PAGE) - 1);
    });
    server->begin();
    on = true;
    return true;
}

void stop() {
    if (!on) return;
    on = false;
    ws->closeAll();
    server->end();
    delete server;  // also frees the handlers added to it
    server = nullptr;
    ws = nullptr;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    buttons = 0;
}

bool running() { return on; }
int viewers() { return on ? ws->count() : 0; }

void offer(const uint8_t frame[144][160], const uint16_t *palette, int paletteSize) {
    if (!on || ws->count() == 0) return;
    uint32_t now = millis();
    if (now - lastSent < FRAME_MS) return;
    lastSent = now;
    ws->cleanupClients(MAX_VIEWERS);
    if (newViewer.exchange(false)) encoder.forceFull();
    encoder.begin(frame, palette, paletteSize);
    // Messages of at most CHUNK bytes, at most two queued per viewer (WS_MAX_QUEUED_MESSAGES):
    // a slow phone makes us stop early, and the lines not sent go with the next frame.
    uint32_t start = millis();
    while (millis() - start < MAX_WAIT_MS) {
        if (!ws->availableForWriteAll()) {
            vTaskDelay(1);
            continue;
        }
        // No exceptions here: a failed allocation would abort, so check the heap first.
        if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < CHUNK + 4096) return;
        auto message = std::make_shared<std::vector<uint8_t>>(CHUNK);
        size_t n = encoder.next(message->data(), CHUNK);
        if (!n) {
            frames++;
            return;
        }
        message->resize(n);
        ws->binaryAll(message);
        messages++;
    }
}

uint32_t sentFrames() { return frames; }
uint32_t sentMessages() { return messages; }

uint8_t phoneButtons() { return buttons; }

}  // namespace stream

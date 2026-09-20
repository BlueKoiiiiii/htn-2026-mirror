/* ESP32-S3 / Arduino-ESP32 2.x / LVGL 8 / ArduinoJson 6 bidirectional client.
 * Network I/O has its own FreeRTOS task. Only watchLinkPoll touches LVGL.
 * The laptop LAN/hotspot connection is NOT TLS encrypted. Trusted private networks only.
 */
#include "WatchLink.h"
#include "WatchSecrets.h"
#include <Arduino.h>
#include <esp_system.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <atomic>
#include <cstring>
#include <cerrno>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#if LVGL_VERSION_MAJOR != 8
#error "This add-on targets your existing LVGL 8 APIs. Do not upgrade LVGL for this change."
#endif
#if ARDUINOJSON_VERSION_MAJOR != 6
#error "Use ArduinoJson 6.21.5 for this add-on."
#endif

namespace {
constexpr size_t MAX_FRAME = 4096;
constexpr size_t MAX_TEXT_BYTES = 2000; // Up to 500 Unicode code points from the laptop relay.
constexpr uint32_t PREVIEW_MS = 10000;
constexpr uint32_t MAX_UI_QUEUE_AGE_MS = 5000;

struct Incoming {
    uint32_t session;
    uint32_t receivedAt;
    char id[101];
    char text[MAX_TEXT_BYTES + 1];
};
struct Ack {
    uint32_t session;
    char id[101];
};
struct Outgoing {
    uint32_t session;
    uint32_t queuedAt;
    char id[101];
};
struct Result {
    uint32_t session;
    char id[101];
    char status[24];
};

QueueHandle_t inbox = nullptr;
QueueHandle_t acks = nullptr;
QueueHandle_t outbox = nullptr;
QueueHandle_t results = nullptr;
std::atomic<bool> stopRequested{false};
std::atomic<bool> taskExited{true};
std::atomic<bool> online{false};
std::atomic<uint32_t> currentSession{0};
bool initialized = false;

// Only the networking task uses these buffers/client/JSON document.
WiFiClient client;
StaticJsonDocument<4096> rxJson;
char rxLine[MAX_FRAME + 1];
size_t rxUsed = 0;
bool authenticated = false;
uint32_t lastRx = 0;
uint32_t connectedAt = 0;
uint32_t lastPing = 0;

// Only the Arduino main loop uses these LVGL objects and duplicate IDs.
lv_obj_t* panel = nullptr;
lv_obj_t* body = nullptr;
uint32_t previewAt = 0;
bool previewVisible = false;
char recent[16][101] = {};
size_t recentIndex = 0;
bool waitingResult = false;
uint32_t sentAt = 0;
uint32_t sendSession = 0;
char pendingRequest[101] = {};

bool validId(const char* id) {
    if (!id) return false;
    const size_t n = strlen(id);
    if (n < 1 || n > 100) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

// Bounded nonblocking writes avoid WiFiClient's potentially long internal retries.
// Only this task owns client and its file descriptor.
bool writeBytes(const char* bytes, size_t length) {
    size_t sent = 0;
    const uint32_t started = millis();
    while (sent < length && !stopRequested.load() && millis() - started < 1500) {
        const int fd = client.fd();
        if (fd < 0) return false;
        const int n = ::send(fd, bytes + sent, length - sent, MSG_DONTWAIT);
        if (n > 0) sent += static_cast<size_t>(n);
        else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return false;
        else vTaskDelay(pdMS_TO_TICKS(10));
    }
    return sent == length;
}

bool sendJson(const JsonDocument& doc) {
    char data[512]; // Hello/heartbeat/ACK and short button-press requests only.
    const size_t needed = measureJson(doc);
    if (needed + 1 >= sizeof(data)) return false;
    const size_t n = serializeJson(doc, data, sizeof(data));
    data[n] = '\n';
    return writeBytes(data, n + 1);
}

void disconnectTcp() {
    if (online.exchange(false)) Serial.println("[WatchLink] Laptop disconnected.");
    authenticated = false;
    rxUsed = 0;
    client.stop();
}

bool parseFrame() {
    rxLine[rxUsed] = '\0';
    rxJson.clear();
    const DeserializationError error = deserializeJson(rxJson, rxLine, rxUsed,
        DeserializationOption::NestingLimit(4));
    if (error || !rxJson.is<JsonObject>() || !rxJson["v"].is<int>() || rxJson["v"].as<int>() != 1) return false;
    const char* type = rxJson["type"] | "";
    if (!authenticated) {
        if (strcmp(type, "welcome") != 0 || strcmp(rxJson["device_id"] | "", WATCH_DEVICE_ID) != 0) {
            Serial.println("[WatchLink] Authentication/handshake failed. Check the watch-only token.");
            return false;
        }
        authenticated = true;
        online.store(true);
        Serial.println("[WatchLink] Connected to laptop; ready for bidirectional messages.");
    } else if (strcmp(type, "pong") == 0) {
        // Incoming heartbeat reply.
    } else if (strcmp(type, "notify") == 0) {
        const char* id = rxJson["id"] | "";
        if (!validId(id) || (strcmp(rxJson["from"] | "", "mirror-01") != 0 &&
                            strcmp(rxJson["from"] | "", "laptop") != 0) ||
            strcmp(rxJson["to"] | "", WATCH_DEVICE_ID) != 0 || !rxJson["text"].is<const char*>()) return false;
        const JsonString text = rxJson["text"].as<JsonString>();
        if (!text.c_str() || text.size() == 0 || text.size() > MAX_TEXT_BYTES ||
            memchr(text.c_str(), '\0', text.size()) != nullptr) return false;
        // Count code points; the trusted relay validates complete Unicode strings.
        size_t characters = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            if ((static_cast<uint8_t>(text.c_str()[i]) & 0xC0) != 0x80) ++characters;
        }
        if (characters > 500) return false;
        static Incoming incoming; // Not shared: FreeRTOS copies the whole struct.
        incoming.session = currentSession.load();
        incoming.receivedAt = millis();
        strcpy(incoming.id, id);
        memcpy(incoming.text, text.c_str(), text.size());
        incoming.text[text.size()] = '\0';
        if (xQueueSend(inbox, &incoming, 0) != pdTRUE) {
            Serial.println("[WatchLink] UI queue full: notification dropped, not acknowledged.");
        }
    } else if (strcmp(type, "result") == 0) {
        const char* id = rxJson["request_id"] | "";
        const char* status = rxJson["status"] | "";
        if (!validId(id) || strcmp(rxJson["to"] | "", "mirror-01") != 0 ||
            (strcmp(status, "delivered") && strcmp(status, "offline") &&
             strcmp(status, "unconfirmed") && strcmp(status, "busy"))) return false;
        Result result;
        result.session = currentSession.load();
        strcpy(result.id, id);
        strcpy(result.status, status);
        if (xQueueSend(results, &result, 0) != pdTRUE) Serial.println("[WatchLink] Result queue full.");
    } else return false;
    lastRx = millis();
    return true;
}

void networkTask(void*) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(true); // Modem power saving, NOT processor deep sleep.
    WiFi.begin(WATCH_WIFI_SSID, WATCH_WIFI_PASSWORD);
    uint32_t lastWiFiAttempt = millis();
    uint32_t lastTcpAttempt = millis() - 30000U;
    uint32_t retryMs = 2000;
    bool wifiWasConnected = false;
    IPAddress host;
    host.fromString(WATCH_LAPTOP_HOST); // Already validated in watchLinkBegin().

    while (!stopRequested.load()) {
        if (WiFi.status() != WL_CONNECTED) {
            if (wifiWasConnected || client.connected()) disconnectTcp();
            wifiWasConnected = false;
            if (millis() - lastWiFiAttempt >= 15000) {
                lastWiFiAttempt = millis();
                Serial.println("[WatchLink] Waiting for Wi-Fi; retrying without blocking the UI.");
                WiFi.reconnect();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!wifiWasConnected) {
            wifiWasConnected = true;
            Serial.print("[WatchLink] Wi-Fi connected, watch LAN IP: ");
            Serial.println(WiFi.localIP());
            lastTcpAttempt = millis() - 30000U;
        }
        if (!client.connected()) {
            disconnectTcp();
            if (millis() - lastTcpAttempt < retryMs) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            lastTcpAttempt = millis();
            Serial.printf("[WatchLink] Connecting to laptop %s:%u\n", WATCH_LAPTOP_HOST, static_cast<unsigned>(WATCH_LAPTOP_PORT));
            if (!client.connect(host, WATCH_LAPTOP_PORT, 1000)) {
                retryMs = retryMs < 15000 ? retryMs * 2 : 30000;
                continue;
            }
            client.setNoDelay(true);
            currentSession.fetch_add(1);
            rxUsed = 0;
            authenticated = false;
            connectedAt = lastRx = lastPing = millis();
            StaticJsonDocument<256> hello;
            hello["v"] = 1;
            hello["type"] = "hello";
            hello["device_id"] = WATCH_DEVICE_ID;
            hello["token"] = WATCH_TOKEN;
            if (hello.overflowed() || !sendJson(hello)) { disconnectTcp(); continue; }
        }

        bool valid = true;
        size_t budget = MAX_FRAME * 2;
        while (budget-- && client.available() && !stopRequested.load()) {
            const int c = client.read();
            if (c < 0) break;
            if (c == '\n') {
                if (rxUsed + 1 > MAX_FRAME || !parseFrame()) { valid = false; break; }
                rxUsed = 0;
            } else {
                if (rxUsed >= MAX_FRAME - 1) { valid = false; break; }
                rxLine[rxUsed++] = static_cast<char>(c);
            }
        }
        if (!valid) {
            Serial.println("[WatchLink] Invalid/oversized frame or rejected handshake; reconnecting.");
            disconnectTcp();
            continue;
        }
        const uint32_t now = millis();
        if ((!authenticated && now - connectedAt > 5000) || now - lastRx > 35000) {
            disconnectTcp();
            continue;
        }
        if (authenticated) {
            retryMs = 2000;
            Ack ack;
            while (xQueueReceive(acks, &ack, 0) == pdTRUE) {
                if (ack.session != currentSession.load()) continue;
                StaticJsonDocument<256> document;
                document["v"] = 1;
                document["type"] = "ack";
                document["ack_for"] = ack.id;
                document["status"] = "ui_received";
                if (document.overflowed() || !sendJson(document)) { valid = false; break; }
            }
            if (!valid) { disconnectTcp(); continue; }
            Outgoing outgoing;
            if (xQueueReceive(outbox, &outgoing, 0) == pdTRUE) {
                if (outgoing.session == currentSession.load() && millis() - outgoing.queuedAt < 2500) {
                    StaticJsonDocument<512> document;
                    document["v"] = 1;
                    document["type"] = "send";
                    document["id"] = outgoing.id;
                    document["to"] = "mirror-01";
                    document["text"] = "Daniel is thinking of you!";
                    if (document.overflowed() || !sendJson(document)) { disconnectTcp(); continue; }
                    Serial.printf("[WatchLink] Sending to mirror: %.24s\n", outgoing.id);
                } else {
                    Serial.println("[WatchLink] Old send request discarded; not replayed after reconnect.");
                }
            }
            if (now - lastPing >= 10000) {
                lastPing = now;
                const char ping[] = "{\"v\":1,\"type\":\"ping\"}\n";
                if (!writeBytes(ping, sizeof(ping) - 1)) { disconnectTcp(); continue; }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    disconnectTcp();
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    taskExited.store(true);
    vTaskDelete(nullptr);
}

void createPanel() {
    if (panel) return;
    // The top layer persists across your existing Main/Menu/Gyro/Laser screens.
    panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(panel, 224, 190);
    lv_obj_center(panel);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_style_text_color(panel, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "MESSAGE");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    body = lv_label_create(panel);
    lv_obj_set_size(body, 200, 114);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 25);
    lv_label_set_long_mode(body, LV_LABEL_LONG_DOT); // Preview: long text gets ellipsis.
    lv_obj_t* hint = lv_label_create(panel);
    lv_label_set_text(hint, "Auto-closes in 10s");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
}
void showPreview(const char* text) {
    createPanel();
    lv_label_set_text(body, text);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(panel);
    previewAt = millis();
    previewVisible = true;
}

void deleteQueues() {
    if (inbox) vQueueDelete(inbox);
    if (acks) vQueueDelete(acks);
    if (outbox) vQueueDelete(outbox);
    if (results) vQueueDelete(results);
    inbox = acks = outbox = results = nullptr;
}
} // namespace

bool watchLinkBegin() {
    if (initialized) return true;
    IPAddress host;
    if (!host.fromString(WATCH_LAPTOP_HOST) || strcmp(WATCH_DEVICE_ID, "watch-01") != 0 ||
        strlen(WATCH_TOKEN) != 64 || strcmp(WATCH_WIFI_SSID, "YOUR_WIFI_NAME") == 0 ||
        strlen(WATCH_WIFI_SSID) == 0) {
        Serial.println("[WatchLink] Setup error: fill in include/WatchSecrets.h first.");
        return false;
    }
    for (size_t i = 0; i < 64; ++i) {
        const char c = WATCH_TOKEN[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            Serial.println("[WatchLink] Setup error: invalid watch token.");
            return false;
        }
    }
    inbox = xQueueCreate(2, sizeof(Incoming));
    acks = xQueueCreate(4, sizeof(Ack));
    outbox = xQueueCreate(1, sizeof(Outgoing));
    results = xQueueCreate(4, sizeof(Result));
    if (!inbox || !acks || !outbox || !results) {
        deleteQueues();
        Serial.println("[WatchLink] Not enough memory for notification queues.");
        return false;
    }
    stopRequested.store(false);
    taskExited.store(false);
    if (xTaskCreate(networkTask, "watch-net", 8192, nullptr, 1, nullptr) != pdPASS) {
        taskExited.store(true);
        deleteQueues();
        Serial.println("[WatchLink] Could not create networking task.");
        return false;
    }
    initialized = true;
    return true;
}

bool watchLinkPoll() {
    if (previewVisible && millis() - previewAt >= PREVIEW_MS) {
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        previewVisible = false;
    }
    if (!initialized || stopRequested.load()) return false;
    bool receivedNew = false;
    Result result;
    while (xQueueReceive(results, &result, 0) == pdTRUE) {
        if (!waitingResult || result.session != sendSession || strcmp(result.id, pendingRequest)) continue;
        waitingResult = false;
        const char* label = "Not confirmed by mirror";
        if (!strcmp(result.status, "delivered")) label = "Mirror received your message";
        else if (!strcmp(result.status, "offline")) label = "Not sent: mirror offline";
        else if (!strcmp(result.status, "busy")) label = "Not sent: relay busy";
        showPreview(label);
        Serial.printf("[WatchLink] Send result: %s\n", result.status);
    }
    if (waitingResult && (!online.load() || currentSession.load() != sendSession || millis() - sentAt > 15000)) {
        waitingResult = false;
        showPreview("Delivery not confirmed");
    }
    static Incoming incoming;
    while (xQueueReceive(inbox, &incoming, 0) == pdTRUE) {
        if (!online.load() || incoming.session != currentSession.load() ||
            millis() - incoming.receivedAt > MAX_UI_QUEUE_AGE_MS) {
            Serial.println("[WatchLink] Dropped stale/disconnected UI notification.");
            continue;
        }
        bool duplicate = false;
        for (const auto& id : recent) if (strcmp(id, incoming.id) == 0) duplicate = true;
        if (!duplicate) {
            showPreview(incoming.text);
            strcpy(recent[recentIndex], incoming.id);
            recentIndex = (recentIndex + 1) % 16;
            receivedNew = true;
            Serial.printf("[WatchLink] UI accepted notification %.8s\n", incoming.id);
        }
        Ack ack;
        ack.session = incoming.session;
        strcpy(ack.id, incoming.id);
        if (xQueueSend(acks, &ack, 0) != pdTRUE) Serial.println("[WatchLink] ACK queue full.");
    }
    if (previewVisible && millis() - previewAt >= PREVIEW_MS) {
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        previewVisible = false;
    }
    return receivedNew;
}

// Main Arduino loop only; never called from a network callback/ISR.
// Success means locally queued, NOT delivered. The result arrives asynchronously.
bool watchLinkSendPing() {
    if (!initialized || stopRequested.load() || !online.load()) {
        showPreview("Not sent: laptop offline");
        return false;
    }
    if (waitingResult) return false;
    Outgoing outgoing;
    outgoing.session = currentSession.load();
    outgoing.queuedAt = millis();
    snprintf(outgoing.id, sizeof(outgoing.id), "watch-%08lx-%08lx-%lu",
        static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()),
        static_cast<unsigned long>(outgoing.queuedAt));
    if (xQueueSend(outbox, &outgoing, 0) != pdTRUE) {
        showPreview("Not sent: networking busy");
        return false;
    }
    waitingResult = true;
    sentAt = millis();
    sendSession = outgoing.session;
    strcpy(pendingRequest, outgoing.id);
    showPreview("Sending to mirror...");
    return true;
}

bool watchLinkConnected() { return online.load(); }

void watchLinkPrepareSleep() {
    if (!initialized) return;
    stopRequested.store(true);
    const uint32_t started = millis();
    while (!taskExited.load() && millis() - started < 3000) delay(10);
    if (!taskExited.load()) Serial.println("[WatchLink] Network shutdown still pending; deep sleep will terminate it.");
}

// main.cpp — ESP32-S2, AirWireCom (master)
// PlatformIO: см. platformio.ini

#include <Arduino.h>
#include "USB.h"
#include "USBCDC.h"
#include "tusb.h"                 // TinyUSB core
#include "class/cdc/cdc_device.h" // cdc_line_coding_t, tud_cdc_n_get_line_coding
#include <esp_now.h>
#include <WiFi.h>
#include <Preferences.h>
#include "button.h"
#include "labels.h"
#include "display_ui.h"

constexpr uint8_t PIN_BUTTON = 0;
constexpr uint8_t PIN_LED = 15;

constexpr uint8_t PKT_DATA = 0x01;
constexpr uint8_t PKT_PAIR = 0x02;
constexpr uint8_t PKT_ACK = 0x03; // [ACK, slot, b0,b1,b2,b3]
constexpr uint8_t PKT_UNPAIR = 0x04;
constexpr uint8_t PKT_BAUD = 0x05; // [BAUD, b0,b1,b2,b3]
constexpr uint8_t PKT_CLAIM = 0x06;
constexpr uint8_t PKT_PING = 0x07;
constexpr uint8_t PKT_PONG = 0x08;

constexpr uint32_t PING_INTERVAL = 30000;
constexpr uint32_t OFFLINE_AFTER = 35000;
constexpr uint32_t BAUD_DEFAULT = 115200;

USBCDC CDC0(0);
USBCDC CDC1(1);
USBCDC CDC2(2);
USBCDC *cdc[3] = {&CDC0, &CDC1, &CDC2};

struct Slave
{
    uint8_t mac[6] = {};
    bool active = false;
    uint32_t lastSeen = 0;
};
Slave slaves[3];
Preferences prefs;

Button btn(PIN_BUTTON);
DisplayUI ui;

const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ─────────────────────────────────────────────────────────────

void packU32(uint8_t *buf, uint32_t val)
{
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
    buf[2] = (val >> 16) & 0xFF;
    buf[3] = (val >> 24) & 0xFF;
}

uint32_t unpackU32(const uint8_t *buf)
{
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

int findSlave(const uint8_t *mac)
{
    for (int i = 0; i < 3; i++)
        if (slaves[i].active && memcmp(slaves[i].mac, mac, 6) == 0)
            return i;
    return -1;
}

int freeSlot()
{
    for (int i = 0; i < 3; i++)
        if (!slaves[i].active)
            return i;
    return -1;
}

void espnowAddPeer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac))
        return;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;
    p.encrypt = false;
    esp_now_add_peer(&p);
}

void espnowRemovePeer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac))
        esp_now_del_peer(mac);
}

void savePairs()
{
    prefs.begin("pairs", false);
    for (int i = 0; i < 3; i++)
    {
        char key[8];
        snprintf(key, sizeof(key), "mac%d", i);
        if (slaves[i].active)
            prefs.putBytes(key, slaves[i].mac, 6);
        else
            prefs.remove(key);
    }
    prefs.end();
}

void loadPairs()
{
    prefs.begin("pairs", true);
    for (int i = 0; i < 3; i++)
    {
        char key[8];
        snprintf(key, sizeof(key), "mac%d", i);
        if (prefs.isKey(key))
        {
            prefs.getBytes(key, slaves[i].mac, 6);
            slaves[i].active = true;
            espnowAddPeer(slaves[i].mac);
        }
    }
    prefs.end();
}

void syncToUI()
{
    for (int i = 0; i < 3; i++)
    {
        ui.slaves[i].active = slaves[i].active;
        ui.slaves[i].online = slaves[i].active &&
                              (millis() - slaves[i].lastSeen < OFFLINE_AFTER);
        ui.slaves[i].lastSeen = slaves[i].lastSeen;
        if (slaves[i].active)
            memcpy(ui.slaves[i].mac, slaves[i].mac, 6);
    }
}

// ─────────────────────────────────────────────────────────────
//  CDC Line Coding — читаем скорость выставленную хостом
// ─────────────────────────────────────────────────────────────

// Получить текущий baud rate для CDC интерфейса (0..2)
uint32_t getHostBaud(uint8_t itf)
{
    cdc_line_coding_t lc;
    tud_cdc_n_get_line_coding(itf, &lc);
    return (lc.bit_rate > 0) ? lc.bit_rate : BAUD_DEFAULT;
}

// Отправить новый baud rate слейву
void sendBaudToSlave(uint8_t slot, uint32_t baud)
{
    if (slot >= 3 || !slaves[slot].active)
        return;
    uint8_t pkt[5];
    pkt[0] = PKT_BAUD;
    packU32(pkt + 1, baud);
    esp_now_send(slaves[slot].mac, pkt, 5);
    ui.slotBaud[slot] = baud;
#ifdef DEBUG
    Serial.printf("[Master] Slot %d baud → %lu (from host)\n",
                  slot, (unsigned long)baud);
#endif
}

// Проверять изменение line coding каждый тик
void checkLineCoding()
{
    static uint32_t lastBaud[3] = {0, 0, 0};

    for (int i = 0; i < 3; i++)
    {
        if (!slaves[i].active)
            continue;
        uint32_t baud = getHostBaud(i);

        ui.slotBaud[i] = baud;
        if (baud != lastBaud[i])
        {
#ifdef DEBUG
            Serial.printf("[MASTER] Slot %d baud: %lu -> %lu\n", i, (unsigned long)lastBaud[i], (unsigned long)baud);
#endif
            lastBaud[i] = baud;
            sendBaudToSlave(i, baud);
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  Heartbeat
// ─────────────────────────────────────────────────────────────

void sendPings()
{
    static uint32_t lastPing = 0;
    if (millis() - lastPing < PING_INTERVAL)
        return;
    lastPing = millis();
    uint8_t pkt[1] = {PKT_PING};
    for (int i = 0; i < 3; i++)
    {
        if (slaves[i].active)
            esp_now_send(slaves[i].mac, pkt, 1);
    }
}

// ─────────────────────────────────────────────────────────────
//  Колбэки UI
// ─────────────────────────────────────────────────────────────

void cbClaimSent()
{
    uint8_t pkt[7];
    pkt[0] = PKT_CLAIM;
    memcpy(pkt + 1, ui.pendingMac, 6);
    espnowAddPeer(BROADCAST);
    esp_now_send(BROADCAST, pkt, 7);
}

void cbConfirmPair(uint8_t slot, uint8_t labelIdx)
{
    const uint8_t *mac = ui.pendingMac;
    if (findSlave(mac) >= 0)
        return;
    if (slot >= 3 || slaves[slot].active)
    {
        int s = freeSlot();
        if (s < 0)
            return;
        slot = s;
    }

    espnowAddPeer(mac);
    memcpy(slaves[slot].mac, mac, 6);
    slaves[slot].active = true;
    slaves[slot].lastSeen = millis();
    savePairs();
    ui.labels.saveLabel(slot, labelIdx);

    // Берём текущий baud с хоста (или дефолт если порт закрыт)
    uint32_t baud = getHostBaud(slot);
    ui.slotBaud[slot] = baud;

    uint8_t ack[7];
    ack[0] = PKT_ACK;
    ack[1] = slot;
    packU32(ack + 2, baud);
    esp_now_send(mac, ack, 7);

    syncToUI();
    ui.showSuccess(slot);

    if (ui.hasDisplay())
        digitalWrite(PIN_LED, LOW);

#ifdef DEBUG
    Serial.printf("[Master] Paired with %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.printf("[Master] Paired slot %d  label=%s  baud=%lu\n",
                  slot, PRESET_LABELS[labelIdx], (unsigned long)baud);
#endif
}

void cbUnpair(uint8_t slot)
{
    if (slot >= 3 || !slaves[slot].active)
        return;
    uint8_t pkt[1] = {PKT_UNPAIR};
    esp_now_send(slaves[slot].mac, pkt, 1);
    delay(80);
    espnowRemovePeer(slaves[slot].mac);
    slaves[slot] = {};
    savePairs();
    syncToUI();
    if (ui.hasDisplay())
        menuMain.refresh();
}

void cbResetAll()
{
    for (int i = 0; i < 3; i++)
    {
        if (!slaves[i].active)
            continue;
        uint8_t pkt[1] = {PKT_UNPAIR};
        esp_now_send(slaves[i].mac, pkt, 1);
    }
    delay(100);
    for (int i = 0; i < 3; i++)
    {
        espnowRemovePeer(slaves[i].mac);
        slaves[i] = {};
    }
    savePairs();
    syncToUI();
}

void cbLabelChanged(uint8_t slot, uint8_t idx)
{
    ui.labels.saveLabel(slot, idx);
#ifdef DEBUG
    Serial.printf("[Master] Slot %d label → \"%s\" (reboot for Win name)\n",
                  slot, PRESET_LABELS[idx]);
    Serial.printf("[Master] Slot %d baud → %lu\n",
                  slot, (unsigned long)ui.slotBaud[slot]);
#endif
}

// ─────────────────────────────────────────────────────────────
//  ESP-NOW receive
// ─────────────────────────────────────────────────────────────

void onReceive(const uint8_t *mac_addr, const uint8_t *data, int len)
{
    if (len < 1)
        return;
    const uint8_t *mac = mac_addr;

    int knownIdx = findSlave(mac);
    if (knownIdx >= 0)
        slaves[knownIdx].lastSeen = millis();

    switch (data[0])
    {

    case PKT_PAIR:
    {
        int existing = findSlave(mac);
        if (existing >= 0)
        {
            uint32_t baud = getHostBaud(existing);
            uint8_t ack[7];
            ack[0] = PKT_ACK;
            ack[1] = (uint8_t)existing;
            packU32(ack + 2, baud);
            espnowAddPeer(mac);
            esp_now_send(mac, ack, 7);
            syncToUI();
            return;
        }
        int slot = freeSlot();
        if (slot < 0)
        {
            Serial.println("[Master] No free slots!");
            return;
        }
        espnowAddPeer(mac);
        if (ui.hasDisplay())
            digitalWrite(PIN_LED, HIGH);
        ui.showPairRequest(mac, (uint8_t)slot);
        break;
    }

    case PKT_PONG:
        break; // lastSeen обновлён выше

    case PKT_CLAIM:
    {
        if (len >= 7 && memcmp(data + 1, ui.pendingMac, 6) == 0)
        {
            ui.onRemoteClaim();
            if (ui.hasDisplay())
                digitalWrite(PIN_LED, LOW);
        }
        break;
    }

    case PKT_DATA:
    {
        if (knownIdx >= 0 && len > 1)
            cdc[knownIdx]->write(data + 1, len - 1);
        break;
    }

    case PKT_UNPAIR:
    {
        if (knownIdx >= 0)
        {
            espnowRemovePeer(mac);
            slaves[knownIdx] = {};
            savePairs();
            syncToUI();
            if (ui.hasDisplay())
                menuMain.refresh();
        }
        break;
    }
    }
}

// ─────────────────────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────────────────────

void setup()
{
    Serial.begin(115200);

    ui.labels.load();

    char d[3][16];
    for (int i = 0; i < 3; i++)
        ui.labels.descriptor(i, d[i], sizeof(d[i]));
    // CDC0.setStringDescriptor(d[0]);
    // CDC1.setStringDescriptor(d[1]);
    // CDC2.setStringDescriptor(d[2]);

    CDC0.begin(115200);
    CDC0.setTimeout(10);
    CDC1.begin(115200);
    CDC1.setTimeout(10);
    CDC2.begin(115200);
    CDC2.setTimeout(10);

    USB.productName("AirWireCom");
    USB.manufacturerName("ESP32-S2");
    USB.begin();

    Serial.printf("[Master] CDC: \"%s\" \"%s\" \"%s\"\n", d[0], d[1], d[2]);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    espnowAddPeer(BROADCAST);
    if (esp_now_init() != ESP_OK)
    {
        Serial.println("[Master] ESP-NOW init FAILED");
        return;
    }
    esp_now_register_recv_cb(onReceive);

    loadPairs();

    btn.begin();
    ui.onConfirmPair = cbConfirmPair;
    ui.onUnpair = cbUnpair;
    ui.onResetAll = cbResetAll;
    ui.onLabelChanged = cbLabelChanged;
    ui.onClaimSent = cbClaimSent;
    syncToUI();
    ui.begin(&btn, PIN_LED);

    Serial.printf("[Master] MAC: %s\n", WiFi.macAddress().c_str());
    Serial.printf("[Master] Display: %s\n", ui.hasDisplay() ? "YES" : "NO (headless mode)");
    Serial.println("[Master] Ready.");
}

// ─────────────────────────────────────────────────────────────
//  Loop
// ─────────────────────────────────────────────────────────────

void loop()
{
    ui.update();
    sendPings();
    checkLineCoding(); // следим за изменением baud rate на хосте

    static uint32_t lastSync = 0;
    if (millis() - lastSync > 1000)
    {
        lastSync = millis();
        syncToUI();
        if (ui.hasDisplay())
            menuMain.refresh();
    }

    for (int i = 0; i < 3; i++)
    {
        if (!slaves[i].active)
            continue;
        int avail = cdc[i]->available();
        if (avail <= 0)
            continue;
        uint8_t buf[250];
        buf[0] = PKT_DATA;
        int n = cdc[i]->readBytes(buf + 1, min(avail, 249));
        if (n > 0)
            esp_now_send(slaves[i].mac, buf, n + 1);
    }
}

/*
 * AirWireCom — единая прошивка для мастера и слейва
 * ESP32-S2 / ESP32 / ESP8266
 *
 * Роль определяется при паринге:
 *   Нажал кнопку первым (инициировал) → SLAVE
 *   Принял запрос (подтвердил)         → MASTER
 *
 * Кнопка (EncButton):
 *   Старт без пары           — сразу отправляем PKT_PAIR
 *   click() в ST_CONFIRM     — подтвердить, стать мастером
 *   hold()                   — сброс пары / отклонить запрос
 *
 * LED:
 *   SEEKING  — быстрое мигание
 *   CONFIRM  — двойное мигание
 *   Онлайн   — горит постоянно
 *   Офлайн   — редкое моргание
 */

#include <Arduino.h>
#include <EncButton.h>

// ── Платформозависимые включения ──────────────────────────────
#ifdef ESP8266
    #include <ESP8266WiFi.h>
    extern "C" {
        #include <espnow.h>
    }
    #include <EEPROM.h>
    #define HAS_NATIVE_USB   0
    #define PLATFORM_ESP8266 1
#else
    #include <WiFi.h>
    #include <esp_now.h>
    #include <Preferences.h>
    #include <HardwareSerial.h>
    #if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3)
        #include "tusb_config.h"    
        #include "USB.h"
        #include "USBCDC.h"
        #include "tusb.h" // TinyUSB core
        #include "class/cdc/cdc_device.h" // cdc_line_coding_t, tud_cdc_n_get_line_coding
        #define HAS_NATIVE_USB 1
        // Заглушки DFU — libarduino_tinyusb.a требует эти символы
        extern "C" {
            uint32_t tud_dfu_get_timeout_cb(uint8_t, uint8_t)      { return 0; }
            void     tud_dfu_download_cb(uint8_t, uint32_t,
                                         uint8_t const*, uint16_t) {}
            void     tud_dfu_manifest_cb(uint8_t)                  {}
            bool     tud_dfu_firmware_valid_check_cb(void)         { return true; }
            void     tud_dfu_abort_cb(uint8_t)                     {}
            void     tud_dfu_runtime_reboot_to_dfu_cb(void)        {}
            uint8_t  const *tud_hid_descriptor_report_cb(uint8_t)  { return nullptr; }
            void     tud_hid_set_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t const *, uint16_t) {}
            uint16_t tud_hid_get_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t *, uint16_t) { return 0; }
        }
    #else
        #define HAS_NATIVE_USB 0
    #endif
#endif

// ═════════════════════════════════════════════════════════════
//  НАСТРОЙКИ — все параметры собраны здесь
// ═════════════════════════════════════════════════════════════

// ── Пины ──────────────────────────────────────────────────────
#ifdef PLATFORM_ESP8266
    constexpr uint8_t PIN_BUTTON  = 0;    // GPIO0 = FLASH кнопка
    constexpr uint8_t PIN_LED     = 2;    // встроенный LED (инвертирован)
    constexpr uint8_t PIN_RX_MON  = 14;   // мониторинг RX для автодетекта baud
#else
    constexpr uint8_t PIN_BUTTON  = 0;    // BOOT кнопка
    constexpr uint8_t PIN_LED     = 15;   // поправь под плату
    constexpr uint8_t PIN_RX_MON  = 4;    // мониторинг RX (не нужен на S2/S3)
    // LEDC (PWM) для плавного управления LED
    constexpr uint8_t  LEDC_CH    = 0;
    constexpr uint32_t LEDC_FREQ  = 5000; // Гц
    constexpr uint8_t  LEDC_RES   = 8;    // бит → диапазон 0..255
    // UART пины для отладки на S2/S3 (SerialUSB занят USB CDC)
    constexpr uint8_t  PIN_DEBUG_TX = 17;
    constexpr uint8_t  PIN_DEBUG_RX = 18;
#endif

// ── Протокол ──────────────────────────────────────────────────
constexpr uint8_t PKT_DATA   = 0x01;
constexpr uint8_t PKT_PAIR   = 0x02;  // broadcast: ищу пару
constexpr uint8_t PKT_CLAIM  = 0x03;  // [CLAIM, myMac×6] — стал мастером
constexpr uint8_t PKT_ACK    = 0x04;  // [ACK, baud×4] — подтверждение + baud
constexpr uint8_t PKT_UNPAIR = 0x05;
constexpr uint8_t PKT_BAUD   = 0x06;  // [BAUD, baud×4]
constexpr uint8_t PKT_PING   = 0x07;
constexpr uint8_t PKT_PONG   = 0x08;

// Размеры пакетов (для читаемости условий len >= N)
constexpr uint8_t PKT_CLAIM_LEN = 7;  // тип + MAC(6)
constexpr uint8_t PKT_ACK_LEN   = 5;  // тип + baud(4)
constexpr uint8_t PKT_BAUD_LEN  = 5;  // тип + baud(4)

// ── Таймауты и интервалы (мс) ─────────────────────────────────
constexpr uint32_t SEEK_INTERVAL   = 2000;   // повтор PKT_PAIR каждые N мс
constexpr uint32_t CONFIRM_TTL     = 15000;  // ждать подтверждения N мс
constexpr uint32_t PING_INTERVAL   = 30000;  // heartbeat каждые N мс
constexpr uint32_t OFFLINE_AFTER   = 35000;  // нет пакетов N мс → офлайн
constexpr uint32_t UNPAIR_DELAY_MS = 80;     // пауза после отправки PKT_UNPAIR

// ── Кнопка ────────────────────────────────────────────────────
constexpr uint16_t BTN_HOLD_MS = 3000;  // порог долгого нажатия

// ── LED — тайминги паттернов (мс) ─────────────────────────────
constexpr uint32_t LED_SEEK_PERIOD    = 150;   // период быстрого мигания
constexpr uint32_t LED_CONFIRM_PERIOD = 1200;  // период двойного мигания
constexpr uint32_t LED_CONFIRM_PULSE1 = 120;   // длина первого импульса
constexpr uint32_t LED_CONFIRM_GAP    = 250;   // пауза между импульсами
constexpr uint32_t LED_CONFIRM_PULSE2 = 370;   // конец второго импульса
constexpr uint32_t LED_OFFLINE_PERIOD = 3000;  // период редкого моргания
constexpr uint32_t LED_OFFLINE_PULSE  = 100;   // длина вспышки

// ── LED — яркость ─────────────────────────────────────────────
constexpr uint8_t  LED_FULL           = 255;   // максимальная яркость (8 бит)
constexpr uint8_t  LED_OFF            = 0;
#ifdef PLATFORM_ESP8266
constexpr uint16_t LED_PWM_MAX        = 1023;  // ESP8266 analogWrite 0..1023
constexpr uint8_t  LED_PWM_SCALE      = 4;     // перевод 8→10 бит (* 4)
#endif

// ── Baud rate ─────────────────────────────────────────────────
constexpr uint32_t BAUD_DEFAULT    = 115200;
constexpr uint32_t BAUD_DETECT_MIN = 5;        // минимальный импульс мкс (защита от шума)
constexpr uint32_t MICROS_PER_SEC  = 1000000UL;

const uint32_t STD_BAUDS[] = {
    9600, 19200, 38400, 57600, 115200, 230400, 250000, 500000, 1000000
};

// ── Flash ─────────────────────────────────────────────────────
#ifdef PLATFORM_ESP8266
constexpr uint8_t  EEPROM_SIZE      = 16;
constexpr uint8_t  EEPROM_MAGIC     = 0xAB;   // маркер валидных данных
constexpr uint8_t  EEPROM_MAGIC_OFF = 0;      // смещение маркера
constexpr uint8_t  EEPROM_ROLE_OFF  = 1;      // смещение роли ('M'/'S')
constexpr uint8_t  EEPROM_MAC_OFF   = 2;      // смещение MAC (6 байт)
constexpr uint8_t  EEPROM_BAUD_OFF  = 8;      // смещение baud (4 байта)
constexpr uint8_t  EEPROM_EMPTY     = 0x00;   // признак сброса
#endif

// ── ESP-NOW ───────────────────────────────────────────────────
constexpr uint8_t  ESPNOW_CHANNEL   = 0;
constexpr uint8_t  MAC_LEN          = 6;
constexpr uint8_t  DATA_MAX         = 249;    // макс. данных в одном пакете
constexpr uint8_t  DATA_BUF_SIZE    = DATA_MAX + 1; // +1 для байта типа пакета

// ═════════════════════════════════════════════════════════════

// ── Состояния ─────────────────────────────────────────────────
enum State { ST_SEEKING, ST_CONFIRM, ST_PAIRED_MASTER, ST_PAIRED_SLAVE };

State    state        = ST_SEEKING;
uint8_t  peerMac[MAC_LEN]      = {};
uint32_t currentBaud           = BAUD_DEFAULT;
uint32_t lastContact           = 0;
uint8_t  candidateMac[MAC_LEN] = {};
bool     hasCandidate          = false;
uint32_t confirmStart          = 0;
uint32_t lastSeek              = 0;
bool     claimSent             = false;

const uint8_t BROADCAST[MAC_LEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

Button btn(PIN_BUTTON);

#if HAS_NATIVE_USB
    USBCDC SerialUSB(0);
#endif

#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3)
    // Keep Serial on UART0 for debug logs.
    HardwareSerial Serial(0);
#endif

// ─────────────────────────────────────────────────────────────
//  Flash
// ─────────────────────────────────────────────────────────────

#ifdef PLATFORM_ESP8266

void saveState() {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.write(EEPROM_MAGIC_OFF, EEPROM_MAGIC);
    EEPROM.write(EEPROM_ROLE_OFF, (state == ST_PAIRED_MASTER) ? 'M' : 'S');
    for (int i = 0; i < MAC_LEN; i++) EEPROM.write(EEPROM_MAC_OFF + i, peerMac[i]);
    uint32_t b = currentBaud;
    for (int i = 0; i < 4; i++) { EEPROM.write(EEPROM_BAUD_OFF + i, b & 0xFF); b >>= 8; }
    EEPROM.commit(); EEPROM.end();
}

bool loadState() {
    EEPROM.begin(EEPROM_SIZE);
    bool ok = (EEPROM.read(EEPROM_MAGIC_OFF) == EEPROM_MAGIC);
    if (ok) {
        state = (EEPROM.read(EEPROM_ROLE_OFF) == 'M') ? ST_PAIRED_MASTER : ST_PAIRED_SLAVE;
        for (int i = 0; i < MAC_LEN; i++) peerMac[i] = EEPROM.read(EEPROM_MAC_OFF + i);
        uint32_t b = 0;
        for (int i = 3; i >= 0; i--) { b <<= 8; b |= EEPROM.read(EEPROM_BAUD_OFF + i); }
        if (b > 0) currentBaud = b;
    }
    EEPROM.end();
    return ok;
}

void clearState() {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.write(EEPROM_MAGIC_OFF, EEPROM_EMPTY);
    EEPROM.commit(); EEPROM.end();
}

#else

Preferences prefs;

void saveState() {
    prefs.begin("awc", false);
    prefs.putChar("role", (state == ST_PAIRED_MASTER) ? 'M' : 'S');
    prefs.putBytes("peer", peerMac, MAC_LEN);
    prefs.putUInt("baud", currentBaud);
    prefs.end();
}

bool loadState() {
    prefs.begin("awc", true);
    bool ok = prefs.isKey("peer");
    if (ok) {
        state = (prefs.getChar("role", 'S') == 'M') ? ST_PAIRED_MASTER : ST_PAIRED_SLAVE;
        prefs.getBytes("peer", peerMac, MAC_LEN);
        currentBaud = prefs.getUInt("baud", BAUD_DEFAULT);
    }
    prefs.end();
    return ok;
}

void clearState() {
    prefs.begin("awc", false); prefs.clear(); prefs.end();
}

#endif

// ─────────────────────────────────────────────────────────────
//  LED
// ─────────────────────────────────────────────────────────────

void ledSet(uint8_t brightness) {
#ifdef PLATFORM_ESP8266
    analogWrite(PIN_LED, LED_PWM_MAX - (brightness * LED_PWM_SCALE));
#else
    ledcWrite(LEDC_CH, brightness);
#endif
}

void updateLed() {
    uint32_t now = millis();

    switch (state) {

    case ST_SEEKING:
        ledSet((now / LED_SEEK_PERIOD) % 2 ? LED_FULL : LED_OFF);
        break;

    case ST_CONFIRM: {
        uint32_t t  = now % LED_CONFIRM_PERIOD;
        bool     on = (t < LED_CONFIRM_PULSE1) ||
                      (t >= LED_CONFIRM_GAP && t < LED_CONFIRM_PULSE2);
        ledSet(on ? LED_FULL : LED_OFF);
        break;
    }

    case ST_PAIRED_MASTER:
    case ST_PAIRED_SLAVE:
        if (millis() - lastContact < OFFLINE_AFTER) {
            ledSet(LED_FULL);
        } else {
            ledSet((now % LED_OFFLINE_PERIOD) < LED_OFFLINE_PULSE ? LED_FULL : LED_OFF);
        }
        break;
    }
}

// ─────────────────────────────────────────────────────────────
//  Baud rate
// ─────────────────────────────────────────────────────────────

void packU32(uint8_t* buf, uint32_t val) {
    buf[0] =  val        & 0xFF;
    buf[1] = (val >>  8) & 0xFF;
    buf[2] = (val >> 16) & 0xFF;
    buf[3] = (val >> 24) & 0xFF;
}

uint32_t unpackU32(const uint8_t* buf) {
    return  (uint32_t)buf[0]
         | ((uint32_t)buf[1] <<  8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

uint32_t snapToBaud(uint32_t raw) {
    uint32_t best     = STD_BAUDS[0];
    uint32_t bestDiff = abs((long)raw - (long)best);
    for (auto b : STD_BAUDS) {
        uint32_t diff = abs((long)raw - (long)b);
        if (diff < bestDiff) { bestDiff = diff; best = b; }
    }
    return best;
}

#if HAS_NATIVE_USB
uint32_t getHostBaud() {
    cdc_line_coding_t lc;
    tud_cdc_n_get_line_coding(0, &lc);
    return (lc.bit_rate > 0) ? lc.bit_rate : BAUD_DEFAULT;
}
#else
volatile uint32_t _minPulseUs  = UINT32_MAX;
volatile uint32_t _lastEdgeUs  = 0;
volatile bool     _baudDetected = false;

void IRAM_ATTR onRxEdge() {
    uint32_t now   = micros();
    uint32_t pulse = now - _lastEdgeUs;
    _lastEdgeUs = now;
    if (pulse >= BAUD_DETECT_MIN && pulse < _minPulseUs) {
        _minPulseUs  = pulse;
        _baudDetected = true;
    }
}

void startBaudDetect() {
    _minPulseUs   = UINT32_MAX;
    _baudDetected = false;
    pinMode(PIN_RX_MON, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_RX_MON), onRxEdge, CHANGE);
}

uint32_t getHostBaud() {
    if (!_baudDetected || _minPulseUs == UINT32_MAX) return currentBaud;
    return snapToBaud(MICROS_PER_SEC / _minPulseUs);
}
#endif

void applyBaud(uint32_t baud) {
    if (baud == 0 || baud == currentBaud) return;
    currentBaud = baud;
    Serial.flush();
    Serial.begin(baud);
    Serial.setTimeout(10);
    Serial.printf("[AWC] Baud → %lu\n", (unsigned long)baud);
}

// ─────────────────────────────────────────────────────────────
//  ESP-NOW
// ─────────────────────────────────────────────────────────────

void espnowAddPeer(const uint8_t* mac) {
#ifdef PLATFORM_ESP8266
    esp_now_add_peer((uint8_t*)mac, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, nullptr, 0);
#else
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, MAC_LEN);
    p.channel = ESPNOW_CHANNEL; p.encrypt = false;
    esp_now_add_peer(&p);
#endif
}

void espnowRemovePeer(const uint8_t* mac) {
#ifndef PLATFORM_ESP8266
    if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
#endif
}

void sendPair() {
    espnowAddPeer(BROADCAST);
    uint8_t pkt[1] = { PKT_PAIR };
    esp_now_send((uint8_t*)BROADCAST, pkt, sizeof(pkt));
}

void becomeMaster() {
    memcpy(peerMac, candidateMac, MAC_LEN);
    state       = ST_PAIRED_MASTER;
    lastContact = millis();
    claimSent   = false;
    espnowAddPeer(peerMac);

    uint32_t baud = getHostBaud();
    currentBaud   = baud;

    // PKT_CLAIM: уведомить всех что мы взяли этого слейва
    uint8_t claim[PKT_CLAIM_LEN];
    claim[0] = PKT_CLAIM;
    WiFi.macAddress(claim + 1);
    esp_now_send(candidateMac, claim, PKT_CLAIM_LEN);

    // PKT_ACK → слейву: подтверждение + baud
    uint8_t ack[PKT_ACK_LEN];
    ack[0] = PKT_ACK;
    packU32(ack + 1, baud);
    esp_now_send(peerMac, ack, PKT_ACK_LEN);

    saveState();
    Serial.printf("[AWC] MASTER  peer=%02X:%02X:%02X:%02X:%02X:%02X  baud=%lu\n",
        peerMac[0], peerMac[1], peerMac[2],
        peerMac[3], peerMac[4], peerMac[5],
        (unsigned long)baud);
}

void becomeSlave(const uint8_t* masterMac, uint32_t baud) {
    memcpy(peerMac, masterMac, MAC_LEN);
    state       = ST_PAIRED_SLAVE;
    lastContact = millis();
    claimSent   = false;
    espnowAddPeer(peerMac);
    applyBaud(baud);
    saveState();
    Serial.printf("[AWC] SLAVE  master=%02X:%02X:%02X:%02X:%02X:%02X  baud=%lu\n",
        peerMac[0], peerMac[1], peerMac[2],
        peerMac[3], peerMac[4], peerMac[5],
        (unsigned long)baud);
}

void resetPair() {
    if (state == ST_PAIRED_MASTER || state == ST_PAIRED_SLAVE) {
        uint8_t pkt[1] = { PKT_UNPAIR };
        esp_now_send(peerMac, pkt, sizeof(pkt));
        delay(UNPAIR_DELAY_MS);
        espnowRemovePeer(peerMac);
    }
    clearState();
    memset(peerMac, 0, MAC_LEN);
    memset(candidateMac, 0, MAC_LEN);
    hasCandidate = false;
    claimSent    = false;
    state        = ST_SEEKING;
    lastSeek     = 0;
    Serial.println("[AWC] Reset → Seeking...");
}

// ─────────────────────────────────────────────────────────────
//  ESP-NOW receive
// ─────────────────────────────────────────────────────────────

#ifdef PLATFORM_ESP8266
void onReceive(uint8_t* mac, uint8_t* data, uint8_t len) {
#else
void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
#endif
    if (len < 1) return;

    // Любой пакет от партнёра обновляет lastContact
    if ((state == ST_PAIRED_MASTER || state == ST_PAIRED_SLAVE)
        && memcmp(mac, peerMac, MAC_LEN) == 0)
        lastContact = millis();

    switch (data[0]) {

    case PKT_PAIR:
        if (state == ST_SEEKING) {
            memcpy(candidateMac, mac, MAC_LEN);
            hasCandidate = true;
            confirmStart = millis();
            state        = ST_CONFIRM;
            Serial.printf("[AWC] Pair request from %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        }
        break;

    case PKT_CLAIM: {
        if (len < PKT_CLAIM_LEN) break;
        const uint8_t* claimerMac = data + 1;
        if (state == ST_CONFIRM || state == ST_SEEKING) {
            if (!claimSent) {
                // Они успели раньше — ждём PKT_ACK от них
                Serial.println("[AWC] Claim received, waiting ACK...");
            } else {
                // Оба отправили CLAIM — арбитраж: больший MAC = мастер
                uint8_t myMac[MAC_LEN];
                WiFi.macAddress(myMac);
                if (memcmp(myMac, claimerMac, MAC_LEN) > 0) {
                    memcpy(candidateMac, claimerMac, MAC_LEN);
                    becomeMaster();
                }
                // Их MAC больше — ждём PKT_ACK
            }
        }
        break;
    }

    case PKT_ACK:
        if (len >= PKT_ACK_LEN && (state == ST_CONFIRM || state == ST_SEEKING)) {
            uint32_t baud = unpackU32(data + 1);
            becomeSlave(mac, baud > 0 ? baud : BAUD_DEFAULT);
        }
        break;

    case PKT_BAUD:
        if (len >= PKT_BAUD_LEN && state == ST_PAIRED_SLAVE) {
            applyBaud(unpackU32(data + 1));
            saveState();
        }
        break;

    case PKT_PING: {
        if (state == ST_PAIRED_MASTER || state == ST_PAIRED_SLAVE) {
            uint8_t pong[1] = { PKT_PONG };
            esp_now_send(peerMac, pong, sizeof(pong));
        }
        break;
    }

    case PKT_PONG:
        break;  // lastContact обновлён выше

    case PKT_DATA:
        if (len > 1) {
#if HAS_NATIVE_USB
            if (state == ST_PAIRED_MASTER)
                SerialUSB.write(data + 1, len - 1);
            else if (state == ST_PAIRED_SLAVE)
                Serial.write(data + 1, len - 1);
#else
            Serial.write(data + 1, len - 1);
#endif
        }
        break;

    case PKT_UNPAIR:
        espnowRemovePeer(peerMac);
        clearState();
        memset(peerMac, 0, MAC_LEN);
        state    = ST_SEEKING;
        lastSeek = 0;
        Serial.println("[AWC] Unpaired by peer → Seeking...");
        break;
    }
}

// ─────────────────────────────────────────────────────────────
//  Периодические задачи
// ─────────────────────────────────────────────────────────────

void seekLoop() {
    if (state != ST_SEEKING) return;
    if (millis() - lastSeek >= SEEK_INTERVAL) {
        lastSeek = millis();
        sendPair();
    }
}

void confirmLoop() {
    if (state != ST_CONFIRM) return;
    if (millis() - confirmStart >= CONFIRM_TTL) {
        state        = ST_SEEKING;
        hasCandidate = false;
        claimSent    = false;
        Serial.println("[AWC] Confirm timeout → Seeking...");
    }
}

void pingLoop() {
    if (state != ST_PAIRED_MASTER) return;
    static uint32_t lastPing = 0;
    if (millis() - lastPing < PING_INTERVAL) return;
    lastPing = millis();
    uint8_t pkt[1] = { PKT_PING };
    esp_now_send(peerMac, pkt, sizeof(pkt));
}

void baudLoop() {
    if (state != ST_PAIRED_MASTER) return;
    static uint32_t lastBaud = 0;
    uint32_t baud = getHostBaud();
    if (baud == lastBaud) return;
    lastBaud    = baud;
    currentBaud = baud;
    saveState();
    uint8_t pkt[PKT_BAUD_LEN];
    pkt[0] = PKT_BAUD;
    packU32(pkt + 1, baud);
    esp_now_send(peerMac, pkt, PKT_BAUD_LEN);
    Serial.printf("[AWC] Host baud → %lu\n", (unsigned long)baud);
}

void dataLoop() {
    if (state != ST_PAIRED_MASTER && state != ST_PAIRED_SLAVE) return;

    if (state == ST_PAIRED_MASTER) {
#if HAS_NATIVE_USB
        int avail = SerialUSB.available();
        if (avail > 0) {
            uint8_t buf[DATA_BUF_SIZE]; buf[0] = PKT_DATA;
            int n = SerialUSB.readBytes(buf + 1, min(avail, (int)DATA_MAX));
            if (n > 0) esp_now_send(peerMac, buf, n + 1);
        }
#else
        int avail = Serial.available();
        if (avail > 0) {
            uint8_t buf[DATA_BUF_SIZE]; buf[0] = PKT_DATA;
            int n = Serial.readBytes(buf + 1, min(avail, (int)DATA_MAX));
            if (n > 0) esp_now_send(peerMac, buf, n + 1);
        }
#endif
    } else {
        int avail = Serial.available();
        if (avail > 0) {
            uint8_t buf[DATA_BUF_SIZE]; buf[0] = PKT_DATA;
            int n = Serial.readBytes(buf + 1, min(avail, (int)DATA_MAX));
            if (n > 0) esp_now_send(peerMac, buf, n + 1);
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  Кнопка
// ─────────────────────────────────────────────────────────────

void handleButton() {
    btn.tick();

    if (btn.hold()) {
        if (state == ST_CONFIRM) {
            state        = ST_SEEKING;
            hasCandidate = false;
            claimSent    = false;
            lastSeek     = 0;
            Serial.println("[AWC] Pair request rejected.");
        } else {
            resetPair();
        }
        return;
    }

    if (btn.click()) {
        if (state == ST_CONFIRM && hasCandidate) {
            espnowAddPeer(candidateMac);
            claimSent = true;
            becomeMaster();
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────────────────────

void setup() {
#ifdef PLATFORM_ESP8266
    pinMode(PIN_LED, OUTPUT);
    analogWrite(PIN_LED, LED_PWM_MAX);  // выключен (инверт.)
#else
    ledcSetup(LEDC_CH, LEDC_FREQ, LEDC_RES);
    ledcAttachPin(PIN_LED, LEDC_CH);
    ledcWrite(LEDC_CH, LED_OFF);
#endif

    btn.setHoldTimeout(BTN_HOLD_MS);

#if HAS_NATIVE_USB
    SerialUSB.begin(BAUD_DEFAULT);
    SerialUSB.setTimeout(10);
    USB.productName("AirWireCom");
    USB.begin();
    Serial.begin(BAUD_DEFAULT, SERIAL_8N1, PIN_DEBUG_RX, PIN_DEBUG_TX);
#else
    Serial.begin(BAUD_DEFAULT);
    Serial.setTimeout(10);
    startBaudDetect();
#endif

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    espnowAddPeer(BROADCAST);

#ifdef PLATFORM_ESP8266
    if (esp_now_init() != 0) {
        Serial.println("[AWC] ESP-NOW FAILED"); return;
    }
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_register_recv_cb(onReceive);
#else
    if (esp_now_init() != ESP_OK) {
        Serial.println("[AWC] ESP-NOW FAILED"); return;
    }
    esp_now_register_recv_cb(onReceive);
#endif

    if (loadState()) {
        espnowAddPeer(peerMac);
        lastContact = millis();
        Serial.printf("[AWC] Restored as %s  peer=%02X:%02X:%02X:%02X:%02X:%02X\n",
            state == ST_PAIRED_MASTER ? "MASTER" : "SLAVE",
            peerMac[0],peerMac[1],peerMac[2],
            peerMac[3],peerMac[4],peerMac[5]);
    } else {
        state    = ST_SEEKING;
        lastSeek = 0;
        sendPair();
        Serial.printf("[AWC] No pair → Seeking  MAC=%s\n",
            WiFi.macAddress().c_str());
    }
}

// ─────────────────────────────────────────────────────────────
//  Loop
// ─────────────────────────────────────────────────────────────

void loop() {
    handleButton();
    updateLed();
    seekLoop();
    confirmLoop();
    pingLoop();
    baudLoop();
    dataLoop();
}
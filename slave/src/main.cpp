// main.cpp — ESP8266 / ESP32 (slave)

#include <Arduino.h>

#ifdef ESP8266
    #include <ESP8266WiFi.h>
    extern "C" {
        #include <espnow.h>
    }
    #include <EEPROM.h>
#else
    #include <WiFi.h>
    #include <esp_now.h>
    #include <Preferences.h>
#endif

// ── Pins ──────────────────────────────────────────────────────
#ifdef ESP8266
    constexpr uint8_t PIN_BUTTON  = 0;
    constexpr uint8_t PIN_LED     = 2;    // PWM-capable pin
    // ESP8266: analogWrite 0..1023, HIGH=off (inverted)
    #define LED_BRIGHTNESS(b) analogWrite(PIN_LED, 1023 - (b))
    #define LED_ON_FULL()     digitalWrite(PIN_LED, LOW)
    #define LED_OFF_FULL()    digitalWrite(PIN_LED, HIGH)
#else
    constexpr uint8_t PIN_BUTTON  = 0;
    constexpr uint8_t PIN_LED     = 2;    // PWM-capable pin
    constexpr uint8_t LEDC_CH     = 0;    // LEDC channel
    constexpr uint32_t LEDC_FREQ  = 5000;
    constexpr uint8_t  LEDC_RES   = 8;    // 8 bits -> 0..255
    #define LED_BRIGHTNESS(b) ledcWrite(LEDC_CH, (b))
    #define LED_ON_FULL()     ledcWrite(LEDC_CH, 255)
    #define LED_OFF_FULL()    ledcWrite(LEDC_CH, 0)
#endif

constexpr uint32_t UART_BAUD_DEFAULT = 115200;
constexpr uint32_t OFFLINE_AFTER     = 35000;
constexpr uint32_t BLINK_FAST_MS     = 150;   // fast-blink period

// ── Protocol ──────────────────────────────────────────────────
constexpr uint8_t PKT_DATA   = 0x01;
constexpr uint8_t PKT_PAIR   = 0x02;
constexpr uint8_t PKT_ACK    = 0x03;
constexpr uint8_t PKT_UNPAIR = 0x04;
constexpr uint8_t PKT_BAUD   = 0x05;
constexpr uint8_t PKT_PING   = 0x07;
constexpr uint8_t PKT_PONG   = 0x08;

uint32_t unpackU32(const uint8_t* buf) {
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

// ── State ─────────────────────────────────────────────────
uint8_t  masterMac[6] = {};
bool     paired       = false;
uint32_t currentBaud  = UART_BAUD_DEFAULT;
uint32_t lastContact  = 0;

// ─────────────────────────────────────────────────────────────
//  Flash
// ─────────────────────────────────────────────────────────────

#ifdef ESP8266

void saveState() {
    EEPROM.begin(16);
    EEPROM.write(0, 0xAB);
    for (int i = 0; i < 6; i++) EEPROM.write(1 + i, masterMac[i]);
    uint32_t b = currentBaud;
    for (int i = 0; i < 4; i++) { EEPROM.write(7 + i, b & 0xFF); b >>= 8; }
    EEPROM.commit(); EEPROM.end();
}

bool loadState() {
    EEPROM.begin(16);
    bool ok = (EEPROM.read(0) == 0xAB);
    if (ok) {
        for (int i = 0; i < 6; i++) masterMac[i] = EEPROM.read(1 + i);
        uint8_t tmp[4];
        for (int i = 0; i < 4; i++) tmp[i] = EEPROM.read(7 + i);
        uint32_t b = unpackU32(tmp);
        if (b > 0) currentBaud = b;
    }
    EEPROM.end();
    return ok;
}

void clearState() {
    EEPROM.begin(16); EEPROM.write(0, 0x00); EEPROM.commit(); EEPROM.end();
}

#else

Preferences prefs;

void saveState() {
    prefs.begin("slave", false);
    prefs.putBytes("master", masterMac, 6);
    prefs.putUInt("baud", currentBaud);
    prefs.end();
}

bool loadState() {
    prefs.begin("slave", true);
    bool ok = prefs.isKey("master");
    if (ok) {
        prefs.getBytes("master", masterMac, 6);
        currentBaud = prefs.getUInt("baud", UART_BAUD_DEFAULT);
    }
    prefs.end();
    return ok;
}

void clearState() {
    prefs.begin("slave", false);
    prefs.remove("master");
    prefs.remove("baud");
    prefs.end();
}

#endif

// ─────────────────────────────────────────────────────────────
//  LED indication
//
//  State               Behavior
//  ──────────────────  ───────────────────────────────────────
//  Not paired          Smooth breathing (fade up -> fade down)
//  Paired + online     Solid on
//  Paired + offline    Fast blink 150 ms
// ─────────────────────────────────────────────────────────────

void updateLed() {
    if (!paired) {
        // Breathing: sine wave with ~2 s period
        // sin returns -1..1, normalize to 0..255
        float phase = (float)(millis() % 2000) / 2000.0f;  // 0..1
        float s = (sin(phase * 2.0f * PI) + 1.0f) / 2.0f;  // 0..1

        // Gamma correction — linear PWM looks non-uniform to the eye
        // x^2.2 приближённо, x^2 проще и достаточно
        s = s * s;

#ifdef ESP8266
        LED_BRIGHTNESS((uint16_t)(s * 1023));
#else
        LED_BRIGHTNESS((uint8_t)(s * 255));
#endif
        return;
    }

    bool online = (millis() - lastContact < OFFLINE_AFTER);

    if (online) {
        LED_ON_FULL();
    } else {
        // Fast blinking
        bool blink = (millis() / BLINK_FAST_MS) % 2;
        blink ? LED_ON_FULL() : LED_OFF_FULL();
    }
}

// ─────────────────────────────────────────────────────────────
//  Baud rate
// ─────────────────────────────────────────────────────────────

void applyBaud(uint32_t baud) {
    if (baud == 0 || baud == currentBaud) return;
    currentBaud = baud;
    Serial.flush();
    Serial.begin(baud);
    Serial.setTimeout(10);
    Serial.printf("[Slave] Baud → %lu\n", (unsigned long)baud);
}

// ─────────────────────────────────────────────────────────────
//  ESP-NOW helpers
// ─────────────────────────────────────────────────────────────

void registerPeer(const uint8_t* mac) {
#ifdef ESP8266
    esp_now_add_peer((uint8_t*)mac, ESP_NOW_ROLE_COMBO, 1, nullptr, 0);
#else
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0; p.encrypt = false;
    esp_now_add_peer(&p);
#endif
}

void sendPairRequest() {
    static const uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    registerPeer(broadcast);
    uint8_t pkt[1] = { PKT_PAIR };
    esp_now_send((uint8_t*)broadcast, pkt, 1);
    Serial.println("[Slave] Pair request sent...");
}

void sendUnpair() {
    if (!paired) return;
    uint8_t pkt[1] = { PKT_UNPAIR };
    esp_now_send(masterMac, pkt, 1);
    delay(80);
}

// ─────────────────────────────────────────────────────────────
//  ESP-NOW callbacks
// ─────────────────────────────────────────────────────────────

#ifdef ESP8266
void onReceive(uint8_t* mac, uint8_t* data, uint8_t len) {
#else
void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    const uint8_t* mac = info->src_addr;
#endif
    if (len < 1) return;

    if (paired && memcmp(mac, masterMac, 6) == 0)
        lastContact = millis();

    switch (data[0]) {

    case PKT_PING:
        if (paired) {
            uint8_t pong[1] = { PKT_PONG };
            esp_now_send(masterMac, pong, 1);
        }
        break;

    case PKT_ACK:
        if (len >= 7) {
            memcpy(masterMac, mac, 6);
            uint32_t baud = unpackU32(data + 2);
            currentBaud = (baud > 0) ? baud : UART_BAUD_DEFAULT;
            saveState();
            registerPeer(masterMac);
            paired      = true;
            lastContact = millis();
            Serial.flush();
            delay(10);
            Serial.begin(currentBaud);
            Serial.setTimeout(10);
            Serial.printf("[Slave] Paired! Slot %d, baud %lu\n",
                data[1], (unsigned long)currentBaud);
        }
        break;

    case PKT_BAUD:
        if (len >= 5 && paired) {
            applyBaud(unpackU32(data + 1));
            saveState();
        }
        break;

    case PKT_DATA:
        if (paired && len > 1)
            Serial.write(data + 1, len - 1);
        break;

    case PKT_UNPAIR:
        paired      = false;
        lastContact = 0;
        clearState();
        applyBaud(UART_BAUD_DEFAULT);
        Serial.println("[Slave] Unpaired.");
        break;
    }
}

// ─────────────────────────────────────────────────────────────
//  Button
// ─────────────────────────────────────────────────────────────

void handleButton() {
    static bool     last      = HIGH;
    static uint32_t pressT    = 0;
    static bool     longFired = false;

    bool b = digitalRead(PIN_BUTTON);

    if (b == LOW && last == HIGH)  { pressT = millis(); longFired = false; }

    if (b == LOW && !longFired && millis() - pressT >= 5000) {
        longFired = true;
        sendUnpair();
        paired      = false;
        lastContact = 0;
        clearState();
        applyBaud(UART_BAUD_DEFAULT);
        Serial.println("[Slave] Reset!");
    }

    if (b == HIGH && last == LOW) {
        uint32_t held = millis() - pressT;
        if (!longFired && held >= 40 && held < 5000)
            sendPairRequest();
    }

    last = b;
}

// ─────────────────────────────────────────────────────────────
//  Setup / Loop
// ─────────────────────────────────────────────────────────────

void setup() {
    pinMode(PIN_BUTTON, INPUT_PULLUP);

#ifdef ESP8266
    pinMode(PIN_LED, OUTPUT);
#else
    ledcSetup(LEDC_CH, LEDC_FREQ, LEDC_RES);
    ledcAttachPin(PIN_LED, LEDC_CH);
#endif

    if (loadState()) {
        Serial.begin(currentBaud);
        Serial.setTimeout(10);
        registerPeer(masterMac);
        paired      = true;
        lastContact = millis();
        Serial.printf("[Slave] Ready. Baud: %lu  Master: %02X:%02X:%02X:%02X:%02X:%02X\n",
            (unsigned long)currentBaud,
            masterMac[0], masterMac[1], masterMac[2],
            masterMac[3], masterMac[4], masterMac[5]);
    } else {
        Serial.begin(UART_BAUD_DEFAULT);
        Serial.setTimeout(10);
        Serial.println("[Slave] Not paired. Press button to pair.");
    }

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

#ifdef ESP8266
    if (esp_now_init() != 0) {
        Serial.println("[Slave] ESP-NOW init FAILED"); return;
    }
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_register_recv_cb(onReceive);
#else
    if (esp_now_init() != ESP_OK) {
        Serial.println("[Slave] ESP-NOW init FAILED"); return;
    }
    esp_now_register_recv_cb(onReceive);
#endif

    // If the device has no physical button, initiate pairing automatically on first boot.
    // This mirrors a short button press right after startup.
    if (!paired) {
        sendPairRequest();
    }
}

void loop() {
    handleButton();
    updateLed();

    if (!paired) return;

    int avail = Serial.available();
    if (avail <= 0) return;

    uint8_t buf[250];
    buf[0] = PKT_DATA;
    int n = Serial.readBytes(buf + 1, min(avail, 249));
    if (n > 0) esp_now_send(masterMac, buf, n + 1);
}


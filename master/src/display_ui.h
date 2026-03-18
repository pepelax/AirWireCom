#pragma once
// display_ui.h — UI на двух GyverMenu + GyverOLED
//
// Паринг одинаковый в обоих режимах (с дисплеем и без):
//   Запрос пришёл → Accept / Ignore → готово
//   Метку выбирать при паринге не нужно — меняется в меню слота когда удобно
//
// Дисплей: OLED 0.96" 128×64 (SSD1306, I2C)
//   SDA → GPIO 8,  SCL → GPIO 9
//
// ── МАППИНГ КНОПОК ───────────────────────────────────────────
#define BTN_MAP_VARIANT_A   // двойное=ОК, долгое=Назад
// #define BTN_MAP_VARIANT_B

#ifdef BTN_MAP_VARIANT_A
    #define ACT_NEXT  BTN_SHORT
    #define ACT_OK    BTN_DOUBLE
    #define ACT_BACK  BTN_LONG
    #define ACT_RESET BTN_VLONG
#else
    #define ACT_NEXT  BTN_SHORT
    #define ACT_OK    BTN_LONG
    #define ACT_BACK  BTN_DOUBLE
    #define ACT_RESET BTN_VLONG
#endif

#include <Arduino.h>
#include <Wire.h>
#include <GyverOLED.h>
#include <GyverMenu.h>
#include "button.h"
#include "labels.h"

constexpr uint8_t  OLED_SDA       = 8;
constexpr uint8_t  OLED_SCL       = 9;
constexpr uint8_t  OLED_ADDR      = 0x3C;
constexpr uint8_t  MENU_COLS      = 21;
constexpr uint8_t  MENU_ROWS      = 8;
constexpr uint32_t ONLINE_TIMEOUT = 35000;
constexpr uint32_t PAIR_POPUP_TTL = 10000;
constexpr uint32_t SUCCESS_MS     = 3000;
constexpr uint32_t BLINK_FAST_MS  = 150;
constexpr uint32_t BLINK_SLOW_MS  = 3000;

// Page IDs
constexpr uint8_t PG_SLOT_0  = 0;
constexpr uint8_t PG_SLOT_1  = 1;
constexpr uint8_t PG_SLOT_2  = 2;
constexpr uint8_t PG_REQUEST = 10;  // Accept / Ignore
constexpr uint8_t PG_SUCCESS = 11;  // экран успеха

GyverOLED<SSD1306_128x64, OLED_BUFFER> oled;
GyverMenu menuMain(MENU_COLS, MENU_ROWS);
GyverMenu menuPair(MENU_COLS, MENU_ROWS);

// ─────────────────────────────────────────────────────────────

class DisplayUI {
public:
    struct SlaveInfo {
        bool     active   = false;
        bool     online   = false;
        uint8_t  mac[6]   = {};
        uint32_t lastSeen = 0;
    };

    SlaveInfo  slaves[3];
    LabelStore labels;

    uint8_t  pendingMac[6] = {};
    uint8_t  pendingSlot   = 0;
    bool     pairClaimed   = false;

    // Текущий baud каждого слота (из CDC line coding)
    uint32_t slotBaud[3] = {115200, 115200, 115200};

    // Колбэки
    void (*onConfirmPair)(uint8_t slot) = nullptr;  // без labelIdx — метка отдельно
    void (*onUnpair)(uint8_t slot)      = nullptr;
    void (*onResetAll)()                = nullptr;
    void (*onLabelChanged)(uint8_t slot, uint8_t idx) = nullptr;
    void (*onClaimSent)()               = nullptr;

    // ─────────────────────────────────────────────────────────
    void begin(Button* btn, uint8_t ledPin) {
        _btn    = btn;
        _ledPin = ledPin;
        pinMode(_ledPin, OUTPUT);

        Wire.begin(OLED_SDA, OLED_SCL);
        Wire.setClock(400000L);
        Wire.beginTransmission(OLED_ADDR);
        _hasDisplay = (Wire.endTransmission() == 0);

        if (_hasDisplay) {
            oled.init(OLED_ADDR);
#ifdef FLIP_DISPLAY
            oled.flipV(true);
#endif
            oled.clear();
            _splash();
            _setupMenuMain();
            _setupMenuPair();
            _active = &menuMain;
            menuMain.refresh();
        }

        _instance = this;
    }

    void begin(Button* btn, uint8_t ledPin) {
        _btn    = btn;
        _ledPin = ledPin;
        pinMode(_ledPin, OUTPUT);

        Wire.begin(OLED_SDA, OLED_SCL);
        Wire.setClock(400000L);
        Wire.beginTransmission(OLED_ADDR);
        _hasDisplay = (Wire.endTransmission() == 0);

        if (_hasDisplay) {
            oled.init(OLED_ADDR);
            oled.flipV(true);  // поворот на 180° — убрать если не нужен
            oled.clear();
            _splash();
            _setupMenuMain();
            _setupMenuPair();
            _active = &menuMain;
            menuMain.refresh();
        }

        _instance = this;
    }

    void update() {
        if (!_btn) return;
        ButtonEvent e = _btn->update();

        // Сброс из любого места — одинаково с дисплеем и без
        if (e == ACT_RESET) {
            if (onResetAll) onResetAll();
            _reset();
            return;
        }

        // Автотаймаут попапа — одинаково
        if (_pairPopupT > 0 && !pairClaimed &&
            millis() - _pairPopupT >= PAIR_POPUP_TTL) {
            _reset();
            return;
        }

        // Автозакрытие успеха — одинаково
        if (_showingSuccess && millis() - _successT >= SUCCESS_MS) {
            _showingSuccess = false;
            if (_hasDisplay) _switchTo(&menuMain);
            return;
        }

        if (_hasDisplay) _handleWithDisplay(e);
        else             _handleNoDisplay(e);

        if (_hasDisplay && _showingSuccess) menuPair.refresh();
    }

    // Вызывается из ESP-NOW при получении PKT_PAIR
    void showPairRequest(const uint8_t* mac, uint8_t slot) {
        memcpy(pendingMac, mac, 6);
        pendingSlot  = slot;
        pairClaimed  = false;
        _pairPopupT  = millis();

        if (_hasDisplay) {
            _switchTo(&menuPair);
        }
        // Без дисплея — LED начнёт мигать через _updateLed()
    }

    void onRemoteClaim() {
        if (_hasDisplay && _active == &menuPair && !pairClaimed)
            _reset();
        _pairPopupT = 0;
    }

    void showSuccess(uint8_t slot) {
        _successSlot    = slot;
        _successT       = millis();
        _showingSuccess = true;
        _pairPopupT     = 0;
        if (_hasDisplay) {
            menuPair.home();
            menuPair.refresh();
        }
    }

    bool hasDisplay()     const { return _hasDisplay; }
    bool hasPairRequest() const {
        return _pairPopupT > 0 && millis() - _pairPopupT < PAIR_POPUP_TTL;
    }

private:
    Button*    _btn            = nullptr;
    uint8_t    _ledPin         = 15;
    bool       _hasDisplay     = false;
    GyverMenu* _active         = nullptr;
    bool       _showingSuccess = false;
    uint32_t   _successT       = 0;
    uint8_t    _successSlot    = 0;
    uint32_t   _pairPopupT     = 0;
    uint8_t    _activeSlot     = 0;

    static DisplayUI* _instance;

    void _reset() {
        pairClaimed     = false;
        _showingSuccess = false;
        _pairPopupT     = 0;
        if (_hasDisplay) _switchTo(&menuMain);
    }

    // ── Навигация с дисплеем ──────────────────────────────────
    void _handleWithDisplay(ButtonEvent e) {
        if      (e == ACT_NEXT) _active->down();
        else if (e == ACT_OK)   _active->set();
        else if (e == ACT_BACK) {
            if (_active == &menuPair) _reset();
            else _active->back();
        }
    }

    // ── Навигация без дисплея ─────────────────────────────────
    //  Входящий запрос:
    //    Короткое → Accept
    //    Долгое   → Ignore
    //  Нормальная работа:
    //    Кнопка не используется (только сброс >10с)
    void _handleNoDisplay(ButtonEvent e) {
        if (!hasPairRequest()) return;

        if (e == ACT_NEXT) {
            // Accept
            pairClaimed = true;
            if (onClaimSent)   onClaimSent();
            if (onConfirmPair) onConfirmPair(pendingSlot);
            _pairPopupT = 0;
        } else if (e == ACT_BACK) {
            // Ignore
            _pairPopupT = 0;
        }
    }

    // ── LED мастера (только без дисплея) ─────────────────────
    //  Входящий запрос   → быстрое мигание
    //  Нет слейвов       → дыхание
    //  Все онлайн        → горит постоянно
    //  Есть офлайн       → редкое моргание раз в 3 сек
    void _updateLed() {
        if (_hasDisplay) return;

        uint32_t now = millis();

        if (hasPairRequest()) {
            bool b = (now / BLINK_FAST_MS) % 2;
            digitalWrite(_ledPin, b ? HIGH : LOW);
            return;
        }

        int active = 0, online = 0;
        for (int i = 0; i < 3; i++) {
            if (slaves[i].active) {
                active++;
                if (slaves[i].online) online++;
            }
        }

        if (active == 0) {
            float phase = (float)(now % 2000) / 2000.0f;
            float s = (sinf(phase * 2.0f * PI) + 1.0f) / 2.0f;
            s = s * s;
            analogWrite(_ledPin, (uint8_t)(s * 255));
            return;
        }

        if (online == active) {
            digitalWrite(_ledPin, HIGH);
            return;
        }

        uint32_t phase = now % BLINK_SLOW_MS;
        digitalWrite(_ledPin, phase < 100 ? HIGH : LOW);
    }

    // ─────────────────────────────────────────────────────────
    //  GyverMenu
    // ─────────────────────────────────────────────────────────
    void _setupHandlers(GyverMenu& m) {
        m.onCursor([](uint8_t row, bool chosen, bool) -> uint8_t {
            oled.setCursor(0, row);
            oled.invertText(chosen);
            return 0;
        });
        m.setFastCursor(true);
        m.onPrint([](const char* str, size_t len) {
            if (str) { for (size_t i = 0; i < len; i++) oled.print(str[i]); }
            else     { oled.invertText(false); oled.update(); }
        });
    }

    void _setupMenuMain() {
        _setupHandlers(menuMain);
        menuMain.setBackSign("< Back");
        menuMain.onBuild([](gm::Builder& b) { _instance->_buildMain(b); });
    }

    void _setupMenuPair() {
        _setupHandlers(menuPair);
        menuPair.setBackSign("< Cancel");
        menuPair.onBuild([](gm::Builder& b) { _instance->_buildPair(b); });
    }

    void _switchTo(GyverMenu* m) {
        _active = m;
        m->home();
        m->refresh();
    }

    // ── Главное меню ─────────────────────────────────────────
    void _buildMain(gm::Builder& b) {
        for (uint8_t i = 0; i < 3; i++) {
            char title[22];
            if (slaves[i].active) {
                snprintf(title, sizeof(title), "%s-%d %s",
                    labels.label(i), i + 1,
                    slaves[i].online ? "[ON]" : "[off]");
            } else {
                snprintf(title, sizeof(title), "%s-%d  ---",
                    labels.label(i), i + 1);
            }
            if (b.PageBegin(i, title)) {
                _activeSlot = i;
                _buildSlot(b, i);
                b.PageEnd();
            }
        }
    }

    void _buildSlot(gm::Builder& b, uint8_t i) {
        char macStr[10];
        if (slaves[i].active)
            snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X",
                slaves[i].mac[3], slaves[i].mac[4], slaves[i].mac[5]);
        else
            strncpy(macStr, "--:--:--", sizeof(macStr));
        b.ValueStr("MAC", macStr);

        char baudStr[14];
        snprintf(baudStr, sizeof(baudStr), "%lu *auto",
            (unsigned long)slotBaud[i]);
        b.ValueStr("Baud", baudStr);

        if (slaves[i].active) {
            if (b.Button("Unpair")) {
                if (onUnpair) onUnpair(_activeSlot);
            }
        } else {
            b.Label("Press btn on slave");
            b.Label("to pair.");
        }

        // Метка — меняется в любой момент, не при паринге
        if (b.Select("Label", &labels.labelIdxRef(i), LABEL_OPTS)) {
            if (onLabelChanged) onLabelChanged(i, labels.labelIdx(i));
        }
    }

    // ── Меню паринга — только Accept / Ignore ────────────────
    void _buildPair(gm::Builder& b) {
        if (_showingSuccess) { _buildSuccess(b); return; }

        if (b.PageBegin(PG_REQUEST, "! New slave")) {
            // Таймер обратного отсчёта
            uint32_t elapsed = millis() - _pairPopupT;
            uint32_t left = elapsed < PAIR_POPUP_TTL
                          ? (PAIR_POPUP_TTL - elapsed) / 1000 : 0;
            char timer[8]; snprintf(timer, sizeof(timer), "%lus", (unsigned long)left);
            b.ValueStr("Timeout", timer);

            char mac[18];
            snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                pendingMac[0], pendingMac[1], pendingMac[2],
                pendingMac[3], pendingMac[4], pendingMac[5]);
            b.ValueStr("MAC", mac);

            char portStr[8];
            snprintf(portStr, sizeof(portStr), "COM%d", pendingSlot + 3);
            b.ValueStr("Port", portStr);

            char baudStr[12];
            snprintf(baudStr, sizeof(baudStr), "%lu",
                (unsigned long)slotBaud[pendingSlot]);
            b.ValueStr("Baud", baudStr);

            b.Label("─────────────────────");

            if (b.Button("Accept")) {
                pairClaimed = true;
                if (onClaimSent)   onClaimSent();
                if (onConfirmPair) onConfirmPair(pendingSlot);
                // Экран успеха покажет showSuccess() из main.cpp
            }
            if (b.Button("Ignore")) {
                _reset();
            }

            b.PageEnd(false);
        }
    }

    // ── Экран успеха ─────────────────────────────────────────
    void _buildSuccess(gm::Builder& b) {
        uint32_t elapsed = millis() - _successT;
        uint8_t  pct = (uint8_t)(100UL * min(elapsed, (uint32_t)SUCCESS_MS) / SUCCESS_MS);
        char pctStr[6]; snprintf(pctStr, sizeof(pctStr), "%d%%", pct);

        char slotStr[10];
        snprintf(slotStr, sizeof(slotStr), "%s-%d",
            labels.label(_successSlot), _successSlot + 1);

        char baudStr[12];
        snprintf(baudStr, sizeof(baudStr), "%lu",
            (unsigned long)slotBaud[_successSlot]);

        b.Label("Paired!");
        b.ValueStr("Slot",  slotStr);
        b.ValueStr("Baud",  baudStr);
        b.Label("Label: menu -> slot");
        b.ValueStr("Close", pctStr);

        static uint32_t lastRefresh = 0;
        if (millis() - lastRefresh > 500) {
            lastRefresh = millis();
            menuPair.refresh();
        }
    }

    // ── Заставка ─────────────────────────────────────────────
    void _splash() {
        oled.home(); oled.setScale(1);
        oled.setCursor(22, 3); oled.print("AirWireCom");
        oled.setCursor(20, 5); oled.print("Starting...");
        oled.update();
        delay(1200);
        oled.clear(); oled.update();
    }
};

DisplayUI* DisplayUI::_instance = nullptr;
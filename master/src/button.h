#pragma once
// button.h — детектор нажатий одной кнопки
// Типы: короткое, двойное, долгое (>2с), очень долгое (>10с)

#include <Arduino.h>

enum ButtonEvent {
    BTN_NONE = 0,
    BTN_SHORT,   // одиночное короткое нажатие
    BTN_DOUBLE,  // двойное нажатие
    BTN_LONG,    // удержание >LONG_MS  (срабатывает не отпуская)
    BTN_VLONG,   // удержание >VLONG_MS (срабатывает не отпуская)
};

namespace BtnTiming {
    constexpr uint32_t DEBOUNCE  =   40;
    constexpr uint32_t DOUBLE_GAP = 350;  // макс. пауза между двумя нажатиями
    constexpr uint32_t LONG      = 2000;
    constexpr uint32_t VLONG     = 10000;
}

class Button {
public:
    explicit Button(uint8_t pin) : _pin(pin) {}

    void begin() { pinMode(_pin, INPUT_PULLUP); }

    // Вызывать каждый тик из loop()
    ButtonEvent update() {
        bool raw     = (digitalRead(_pin) == LOW);
        uint32_t now = millis();

        if (raw != _rawLast) { _debounceT = now; _rawLast = raw; }
        bool pressed = (now - _debounceT >= BtnTiming::DEBOUNCE) ? raw : _stable;

        ButtonEvent result = BTN_NONE;

        if (pressed && !_stable) {
            _pressT     = now;
            _longFired  = false;
            _vlongFired = false;
        }

        if (pressed && _stable) {
            uint32_t held = now - _pressT;
            if (!_vlongFired && held >= BtnTiming::VLONG) {
                _vlongFired = _longFired = true;
                result = BTN_VLONG;
            } else if (!_longFired && held >= BtnTiming::LONG) {
                _longFired = true;
                result = BTN_LONG;
            }
        }

        if (!pressed && _stable) {
            uint32_t held = now - _pressT;
            if (!_longFired && held >= BtnTiming::DEBOUNCE) {
                if (_waitDouble) {
                    result      = BTN_DOUBLE;
                    _waitDouble = false;
                } else {
                    _waitDouble = true;
                    _doubleT    = now;
                }
            }
        }

        if (_waitDouble && !pressed && (now - _doubleT >= BtnTiming::DOUBLE_GAP)) {
            _waitDouble = false;
            result = BTN_SHORT;
        }

        _stable = pressed;
        return result;
    }

private:
    uint8_t  _pin;
    bool     _rawLast    = false;
    bool     _stable     = false;
    uint32_t _debounceT  = 0;
    uint32_t _pressT     = 0;
    bool     _longFired  = false;
    bool     _vlongFired = false;
    bool     _waitDouble = false;
    uint32_t _doubleT    = 0;
};

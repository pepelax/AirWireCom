#pragma once
// labels.h — метки слотов
// Baud rate больше не хранится — берётся автоматически из CDC line coding

#include <Arduino.h>
#include <Preferences.h>

// ── Предустановленные метки ───────────────────────────────────
const char LABEL_OPTS[] = "Slot;CNC;Arduino;3D;Laser;Plasma;Lathe;Robot;Sensor;Custom";

const char* const PRESET_LABELS[] = {
    "Slot", "CNC", "Arduino", "3D", "Laser",
    "Plasma", "Lathe", "Robot", "Sensor", "Custom",
};
constexpr uint8_t PRESET_COUNT = 10;

// ─────────────────────────────────────────────────────────────

class LabelStore {
public:
    void load() {
        Preferences p;
        p.begin("labels", true);
        for (int i = 0; i < 3; i++) {
            char k[8]; snprintf(k, sizeof(k), "lbl%d", i);
            _lbl[i] = p.getUChar(k, 0);
            if (_lbl[i] >= PRESET_COUNT) _lbl[i] = 0;
        }
        p.end();
    }

    void saveLabel(uint8_t slot, uint8_t idx) {
        if (slot >= 3 || idx >= PRESET_COUNT) return;
        _lbl[slot] = idx;
        Preferences p; p.begin("labels", false);
        char k[8]; snprintf(k, sizeof(k), "lbl%d", slot);
        p.putUChar(k, idx); p.end();
    }

    void resetSlot(uint8_t slot) { saveLabel(slot, 0); }

    uint8_t     labelIdx(uint8_t s) const { return s < 3 ? _lbl[s] : 0; }
    const char* label   (uint8_t s) const {
        uint8_t i = labelIdx(s);
        return i < PRESET_COUNT ? PRESET_LABELS[i] : "Slot";
    }

    // Ссылка для виджета Select GyverMenu
    uint8_t& labelIdxRef(uint8_t s) { return _lbl[s < 3 ? s : 0]; }

    // Строка для CDC дескриптора Windows: "CNC-1"
    void descriptor(uint8_t s, char* buf, size_t len) const {
        if (s >= 3) { strncpy(buf, "COM", len); return; }
        snprintf(buf, len, "%s-%d", label(s), s + 1);
    }

private:
    uint8_t _lbl[3] = {0, 0, 0};
};

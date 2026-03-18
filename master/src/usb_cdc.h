#pragma once

#include <Arduino.h>
#include "tusb.h"

// Number of CDC interfaces we expose.
constexpr uint8_t CDC_PORT_COUNT = 2;

class CdcPort {
public:
    explicit CdcPort(uint8_t n) : itf(n) {}

    bool connected() const { return tud_cdc_n_connected(itf); }
    int  available() const { return (int)tud_cdc_n_available(itf); }

    int readBytes(uint8_t *buf, size_t len) {
        return (int)tud_cdc_n_read(itf, buf, (uint32_t)len);
    }

    size_t write(const uint8_t *buf, size_t len) {
        if (!connected()) return 0;
        size_t sent = tud_cdc_n_write(itf, buf, (uint32_t)len);
        tud_cdc_n_write_flush(itf);
        return sent;
    }

    void flush() { tud_cdc_n_write_flush(itf); }

private:
    uint8_t itf;
};

// Global CDC port instances.
extern CdcPort cdcPorts[CDC_PORT_COUNT];

// Initialize USB device and start TinyUSB task.
void usb_init();

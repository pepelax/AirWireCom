#include "tusb.h"

// Provide minimal callbacks to satisfy TinyUSB linkage when classes are disabled.
extern "C" uint8_t const *tud_hid_descriptor_report_cb(uint8_t) { return nullptr; }
extern "C" uint16_t tud_hid_get_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t *, uint16_t) { return 0; }
extern "C" void tud_hid_set_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t const *, uint16_t)
{
    // No HID support; ignore.
}
extern "C" void tud_dfu_runtime_reboot_to_dfu_cb(void)
{
    // DFU runtime disabled; nothing to do.
}

extern "C"
{
    uint32_t tud_dfu_get_timeout_cb(uint8_t, uint8_t) { return 0; }
    void tud_dfu_download_cb(uint8_t, uint32_t,
                             uint8_t const *, uint16_t) {}
    void tud_dfu_manifest_cb(uint8_t) {}
    bool tud_dfu_firmware_valid_check_cb(void) { return true; }
    void tud_dfu_abort_cb(uint8_t) {}
}
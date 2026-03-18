#include "usb_cdc.h"
#include "tusb.h"
#include "esp_system.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// tinyusb_driver_install is defined in esp32-hal-tinyusb.c but not exposed.
// Redeclare minimal config struct and prototype.
extern "C" {
typedef struct {
    bool external_phy;
} tinyusb_config_t;
esp_err_t tinyusb_driver_install(const tinyusb_config_t *config);
}

CdcPort cdcPorts[CDC_PORT_COUNT] = { CdcPort(0), CdcPort(1) };

static void usb_task(void *arg)
{
    (void)arg;
    while (true) {
        tud_task();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

extern "C" void tud_mount_cb(void)
{
    printf("[USB] mounted\n");
}

extern "C" void tud_umount_cb(void)
{
    printf("[USB] unmounted\n");
}

void usb_init()
{
    static bool started = false;
    if (started) return;
    tinyusb_config_t cfg = {
        .external_phy = false
    };
    // Configure pins/phy and start TinyUSB stack.
    tinyusb_driver_install(&cfg);
    xTaskCreatePinnedToCore(usb_task, "usb_task", 4096, nullptr, 5, nullptr, 0);
    started = true;
}

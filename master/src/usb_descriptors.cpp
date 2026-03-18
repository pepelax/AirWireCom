#include "tusb.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include <cstring>

// Interface numbers
enum
{
    ITF_NUM_CDC0 = 0,
    ITF_NUM_CDC0_DATA,
    ITF_NUM_CDC1,
    ITF_NUM_CDC1_DATA,
    ITF_NUM_MSC,
    ITF_NUM_TOTAL
};

// Endpoint assignment
#define EPNUM_CDC0_NOTIF  0x81
#define EPNUM_CDC0_OUT    0x02
#define EPNUM_CDC0_IN     0x82

#define EPNUM_CDC1_NOTIF  0x83
#define EPNUM_CDC1_OUT    0x04
#define EPNUM_CDC1_IN     0x84

// MSC endpoints
#define EPNUM_MSC_OUT     0x05
#define EPNUM_MSC_IN      0x85

// String descriptor indexes
enum
{
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC0,
    STRID_CDC1,
    STRID_MSC,
};

// Device descriptor
static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = 0x303A,      // Espressif VID
    .idProduct          = 0x4001,      // Custom PID for AirWairCom dual CDC
    .bcdDevice          = 0x0100,

    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,

    .bNumConfigurations = 1
};

extern "C" uint8_t const * tud_descriptor_device_cb(void)
{
    return reinterpret_cast<uint8_t const *>(&desc_device);
}

// Configuration descriptor
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + 2*TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)
static uint8_t const desc_configuration[] =
{
    // Config number, interface count, string index, total length, attribute, power (mA)
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED, 250),

    // CDC 0
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC0, STRID_CDC0, EPNUM_CDC0_NOTIF, 64, EPNUM_CDC0_OUT, EPNUM_CDC0_IN, 64),
    // CDC 1
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC1, STRID_CDC1, EPNUM_CDC1_NOTIF, 64, EPNUM_CDC1_OUT, EPNUM_CDC1_IN, 64),
    // MSC
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

extern "C" uint8_t const * tud_descriptor_configuration_cb(uint8_t)
{
    return desc_configuration;
}

// String descriptors
static char serial_str[32] = "000000000000";

static void make_serial()
{
    if (serial_str[0] != '0') return;
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    snprintf(serial_str, sizeof(serial_str), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static char const* string_desc_arr[] =
{
    (const char[]){ 0x09, 0x04 }, // 0: English (0x0409)
    "AirWire",                    // 1: Manufacturer
    "AirWairCom",                 // 2: Product
    serial_str,                   // 3: Serial (filled at runtime)
    "Air CDC 0",                  // 4: CDC0 interface string
    "Air CDC 1",                  // 5: CDC1 interface string
    "Air MSC",                    // 6: MSC interface
};

extern "C" uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void) langid;
    static uint16_t desc_str[32];
    uint8_t chr_count;

    make_serial();

    if (index == 0) {
        desc_str[1] = (uint16_t)((string_desc_arr[0][1] << 8) | string_desc_arr[0][0]);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr)/sizeof(string_desc_arr[0])) return NULL;
        const char* str = string_desc_arr[index];
        chr_count = (uint8_t) strnlen(str, 31);
        for (uint8_t i = 0; i < chr_count; i++) {
            desc_str[1 + i] = str[i];
        }
    }

    desc_str[0] = (TUSB_DESC_STRING << 8) | (2*chr_count + 2);
    return desc_str;
}

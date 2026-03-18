#pragma once

// TinyUSB configuration for ESP32-S2 (Arduino core).
// We provide our own file via -DCFG_TUSB_CONFIG_FILE="tusb_config.h".

#include "tusb_option.h"

// ---------------------------------------------------------------------------
// Basic board setup
// ---------------------------------------------------------------------------
// Disable Arduino's automatic USB init so we can manage TinyUSB ourselves.
#ifdef ARDUINO_USB_MODE
#undef ARDUINO_USB_MODE
#endif
#define ARDUINO_USB_MODE        0

#ifdef ARDUINO_USB_CDC_ON_BOOT
#undef ARDUINO_USB_CDC_ON_BOOT
#endif
#define ARDUINO_USB_CDC_ON_BOOT 0

// Identify MCU/RTOS and enable the device root port.
#ifdef CFG_TUSB_MCU
#undef CFG_TUSB_MCU
#endif
#define CFG_TUSB_MCU            OPT_MCU_ESP32S2

#ifdef CFG_TUSB_RHPORT0_MODE
#undef CFG_TUSB_RHPORT0_MODE
#endif
#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE

#ifdef CFG_TUSB_OS
#undef CFG_TUSB_OS
#endif
#define CFG_TUSB_OS             OPT_OS_FREERTOS


// ---------------------------------------------------------------------------
// Device class enablement
// ---------------------------------------------------------------------------
#ifdef CFG_TUD_CDC
#undef CFG_TUD_CDC
#endif
#define CFG_TUD_CDC             1   // expose three CDC interfaces

#ifdef CFG_TUD_MSC
#undef CFG_TUD_MSC
#endif
#define CFG_TUD_MSC             1   // keep MSC enabled (USBMSC.cpp is always built)

#ifdef CFG_TUD_HID
#undef CFG_TUD_HID
#endif
#define CFG_TUD_HID             0

#ifdef CFG_TUD_MIDI
#undef CFG_TUD_MIDI
#endif
#define CFG_TUD_MIDI            0

#ifdef CFG_TUD_VENDOR
#undef CFG_TUD_VENDOR
#endif
#define CFG_TUD_VENDOR          0

#ifdef CFG_TUD_AUDIO
#undef CFG_TUD_AUDIO
#endif
#define CFG_TUD_AUDIO           0

#ifdef CFG_TUD_VIDEO
#undef CFG_TUD_VIDEO
#endif
#define CFG_TUD_VIDEO           0

#ifdef CFG_TUD_CUSTOM_CLASS
#undef CFG_TUD_CUSTOM_CLASS
#endif
#define CFG_TUD_CUSTOM_CLASS    0

#ifdef CFG_TUD_DFU_RUNTIME
#undef CFG_TUD_DFU_RUNTIME
#endif
#define CFG_TUD_DFU_RUNTIME     0

#ifdef CFG_TUD_DFU
#undef CFG_TUD_DFU
#endif
#define CFG_TUD_DFU             0

// ---------------------------------------------------------------------------
// Buffer sizes
// ---------------------------------------------------------------------------
#ifdef CFG_TUD_CDC_RX_BUFSIZE
#undef CFG_TUD_CDC_RX_BUFSIZE
#endif
#define CFG_TUD_CDC_RX_BUFSIZE  256

#ifdef CFG_TUD_CDC_TX_BUFSIZE
#undef CFG_TUD_CDC_TX_BUFSIZE
#endif
#define CFG_TUD_CDC_TX_BUFSIZE  256

#ifdef CFG_TUD_MSC_BUFSIZE
#undef CFG_TUD_MSC_BUFSIZE
#endif
#define CFG_TUD_MSC_BUFSIZE     4096

// The core code has a typo: it looks for CFG_TUD_ENDOINT0_SIZE. Provide alias.
#ifndef CFG_TUD_ENDOINT0_SIZE
#define CFG_TUD_ENDOINT0_SIZE   CFG_TUD_ENDPOINT0_SIZE
#endif

// Keep other defaults from tusb_option.h (endpoint0 size, task queue, etc.).

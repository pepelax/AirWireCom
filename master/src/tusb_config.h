// tusb_config.h — должен лежать в папке src рядом с main.cpp
// Переопределяет настройки TinyUSB из Arduino ESP32 core
// чтобы получить 3 виртуальных COM-порта вместо одного.
//
// ВАЖНО: после изменения этого файла сделай
//   PlatformIO: Clean (Ctrl+Alt+T → "Clean")
//   перед следующей компиляцией

#pragma once

#ifdef ARDUINO_USB_MODE
    #undef ARDUINO_USB_MODE
#endif

#ifdef ARDUINO_USB_CDC_ON_BOOT
    #undef ARDUINO_USB_CDC_ON_BOOT
#endif

#ifdef CFG_TUD_CDC
    #undef CFG_TUD_CDC
#endif

#ifdef CFG_TUD_CDC_RX_BUFSIZE
    #undef CFG_TUD_CDC_RX_BUFSIZE
#endif

#ifdef CFG_TUD_CDC_TX_BUFSIZE
    #undef CFG_TUD_CDC_TX_BUFSIZE
#endif

#ifdef CFG_TUD_HID
    #undef CFG_TUD_HID
#endif

#ifdef CFG_TUD_MIDI
    #undef CFG_TUD_MIDI
#endif

#ifdef CFG_TUD_MSC
    #undef CFG_TUD_MSC
#endif

#ifdef CFG_TUD_VENDOR
    #undef CFG_TUD_VENDOR
#endif

#define ARDUINO_USB_MODE        0
#define ARDUINO_USB_CDC_ON_BOOT 0
#define CFG_TUD_CDC             3    // три CDC интерфейса = три COM-порта
#define CFG_TUD_CDC_RX_BUFSIZE  256
#define CFG_TUD_CDC_TX_BUFSIZE  256

#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_MSC             0
#define CFG_TUD_VENDOR          0

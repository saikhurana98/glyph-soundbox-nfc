# Glyph Soundbox NFC POC

A proof of concept that turns the [PCB Cupid G-Kit Soundbox](https://learn.pcbcupid.com/documentation/modules/g-kit/soundbox) into an NFC-controlled, Yoto-inspired music player.

## What works

- PN532 detection on the Glyph C6 I2C rail (`SDA=GPIO4`, `SCL=GPIO5`)
- MP3 playback from the Soundbox microSD module
- One playlist per NFC UID, including reordering from the PWA
- Playback while the card is present and pause when it is removed
- Per-card playlist position and elapsed time stored in ESP32 NVS
- BLE configuration from an installable Web Bluetooth PWA
- First-tap convenience: an unmapped card initially maps to every MP3 on the SD card

This is intentionally a POC. Resume seeking is a time-to-byte approximation and is most accurate for constant-bitrate MP3 files.

## Hardware pins

The firmware follows the pin ordering in PCB Cupid's upstream SoundPod sketch:

| Function | GPIO |
|---|---:|
| I2C SDA (OLED + PN532 + codec control) | 4 |
| I2C SCL | 5 |
| SD CS / SCK / MOSI / MISO | 17 / 21 / 22 / 23 |
| I2S MCLK / BCLK / WS / DOUT | 16 / 14 / 18 / 15 |

Set the PN532 module to I2C mode. Its expected 7-bit address is `0x24`.

## Build and upload

```sh
pio run
pio run -t upload
pio device monitor
```

## PWA

The app lives in [`web/`](web/) and is published at
[saikhurana98.github.io/glyph-soundbox-nfc](https://saikhurana98.github.io/glyph-soundbox-nfc/).
Open it in Chrome or Edge, connect, tap a card, choose tracks, reorder them, and save.

Web Bluetooth is not supported by Safari on iPhone/iPad. An iOS-native wrapper is outside this POC.

## BLE protocol

Service `7e400001-b5a3-f393-e0a9-e50e24dcca9e` has a write command characteristic and notify event characteristic. Messages are pipe-delimited UTF-8 strings such as `TRACKS`, `MAP|UID|0,2,1`, `CARD|UID`, and `PLAYING|UID|position|track|seconds`.

## Credits

Built on PCB Cupid's MIT-licensed Soundbox example and `PCBCUPID_PLAYERS` library. Inspired by the interaction model of the Yoto Player; this project is unaffiliated with Yoto.

# YN038 XiaoZhi speaker

This target adapts the YN038 ESP32-S3-WROOM-1-N16R8 board as a headless
XiaoZhi voice speaker.

## Wiring

### INMP441 microphone

| INMP441 | YN038 |
| --- | --- |
| VDD | 3V3 |
| GND | GND |
| SCK / BCLK | GPIO5 |
| WS / LRCL | GPIO4 |
| SD | GPIO6 |
| L/R | GND |

### MAX98357A amplifier

| MAX98357A | YN038 |
| --- | --- |
| VIN | 5V |
| GND | GND |
| BCLK | GPIO15 |
| LRC / LRCLK | GPIO16 |
| DIN | GPIO7 |
| SPK+ / SPK- | Speaker terminals |

Do not connect `SPK-` to ground. All modules must share ground. The physical
BOOT button on GPIO0 toggles listening after startup and enters Wi-Fi
provisioning during startup. This target intentionally has no display.

Flashing XiaoZhi replaces the existing application firmware; keep a verified
backup before writing it.

## Optional environment display variant

Build `yn038-xiaozhi-tft` with scripts/build.py. The original variant remains headless.
1.8-inch 128x160 SPI panel: GND=GND, VDD=3V3, SCL=GPIO12, SDA=GPIO11,
RST=GPIO14, DC=GPIO13, CS=GPIO17. BLK is external backlight power, not a GPIO.
Verify module backlight voltage/current before powering it. Sensor bus stays GPIO8/9/10.
Driver follows the existing ST7735-compatible ST7789 implementation; actual panel
controller, color order and offsets require visual confirmation. No MISO/readback.
Dashboard shows missing or stale (>15s) sensor values as --.

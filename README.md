# PrintSphere

Round ESP32-S3 printer companion for Bambu Lab: live status, progress ring, camera snapshots, cloud + LAN sync, and touch setup on a circular display.

<img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/820c2e9b-10a7-4430-949c-e8b0adc1357d" /><img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/5923dc59-0123-4df1-b54d-673c6dbad23b" />


## Native ESP-IDF Project

PrintSphere is the native ESP-IDF rebuild and the better successor to
[`big-printsphere.yaml`](../big-printsphere.yaml), not just a subproject of it.

## Goals

- ESP-IDF `v5.5.x`
- LVGL `v9.4.0`
- ESP32-S3 Touch AMOLED 1.75
- no Home Assistant required
- direct Bambu LAN status client in LAN Mode
- only LAN Mode is not needed on the printer

## Already Implemented

- official Waveshare BSP for display, touch, and LVGL
- AXP2101 PMU integration via `XPowersLib`
- NVS-based configuration storage
- `AP+STA` Wi-Fi manager
- local setup portal on `esp_http_server`
- local Bambu MQTT status client via `device/{serial}/report`

## Setup Flow

1. The ESP always starts a setup AP with SSID `PrintSphere-Setup`
   and password `printsphere`
2. In setup AP mode, the portal only asks for:
   - Wi-Fi SSID
   - Wi-Fi password
3. Save Wi-Fi and let the ESP reboot
4. After the ESP joins your home network, reopen the portal on its new IP
5. Connect Bambu Cloud with:
   - Bambu email
   - Bambu password
   - optional email code or 2FA code if Bambu requests verification
6. It is recommended to connect the local printer path for LAN MQTT and camera snapshots with:
   - printer IP or hostname
   - printer serial number
   - LAN-Mode access code

Cloud and local printer credentials are applied live from the web portal without another reboot.

## UI Status

The current screen already shows:

- print progress
- lifecycle status
- job name
- temperatures
- layer information
- remaining time
- Wi-Fi status
- battery and USB status

## Current Limitations
- camera and MJPEG decoding are not fully finished yet
- MQTT TLS is working, but is not yet pinned to dedicated Bambu certificate handling

## Flash

/release/firmware.bin can be flashed on emtpy devices via known methods

via webflasher like the one from ESPHome 
https://web.esphome.io/
connect USB - choose COM Port 
dont prepare fore first use, just
Install with firmware.bin

or espboards.dev
https://www.espboards.dev/tools/program/

from VSCode:
```bash
idf.py -p PORT flash
```

Alternatively, the merged initial-flash image can be written directly with esptool:
```bash
esptool.exe --chip esp32s3 --port PORT write_flash 0x0 release/firmware.bin
```

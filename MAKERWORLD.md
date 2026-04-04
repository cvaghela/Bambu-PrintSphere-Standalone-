# PrintSphere: MakerWorld Guide

PrintSphere is a standalone status display for Bambu Lab printers based on the `Waveshare ESP32-S3 AMOLED 1.75`. This guide is for users who print the case from MakerWorld and want to get the hardware running with the current feature set.

## What You Need

- `Waveshare ESP32-S3 AMOLED 1.75`
- the matching printed case from the MakerWorld project
- a USB-C cable and 5 V power source
- a 2.4 GHz Wi-Fi network
- a Bambu Cloud account
- optional local printer access in addition to the cloud setup (model dependant):
  - printer IP or hostname
  - printer serial number
  - access code

## Flashing The Firmware

1. Download latest [`release/firmware.bin`](release/firmware.bin) version.
2. Connect the display to your computer via USB.
3. Flash the firmware with one of these tools:
   - `https://web.esphome.io/`
   - `https://www.espboards.dev/tools/program/`
   - `https://espressif.github.io/esptool-js/`
4. On `web.esphome.io`:
   - connect the device
   - choose the COM port
   - do not use "Prepare for first use"
   - install `firmware.bin` directly
5. On `espboards.dev` or `esptool-js`:
   - write `firmware.bin` to address `0x0`

The bootloader is already included in the file.

## First Start

1. After flashing, PrintSphere starts a Wi-Fi access point:
   - SSID: `PrintSphere-Setup`
   - password: `printsphere`
2. Connect to that Wi-Fi network.
3. Open `http://192.168.4.1` in your browser.
4. Enter your home Wi-Fi credentials and save them.
5. The device reboots and connects to your home network.
6. Open the device IP in your local network.

## Unlocking Web Config

The full Web Config is protected after the initial Wi-Fi setup.

1. Touch and hold the display for about one second.
2. A six-digit PIN appears on the screen.
3. Enter that PIN in the browser.
4. The PIN stays valid for about 2 minutes.
5. The unlocked browser session stays active for about 10 minutes.

## Setup In Web Config

### 1. Wi-Fi

This stores the home network. If your Wi-Fi changes later, update it here again.

### 2. Bambu Cloud

At the moment, the practical end-user setup path expects Bambu Cloud.

Enter:

- Bambu email
- Bambu password
- cloud region

Then press `Connect Cloud`.

Notes:

- If Bambu requests an email code or 2FA code, you can enter it directly in Web Config.
- The printer serial number is often filled in automatically after a successful cloud connection.
- The cloud path now also appears to provide progress, remaining time, layer information, and temperatures on many models.
- That broader cloud behavior still needs more real-world validation on newer printer families.

### 3. Connection Mode

Available modes:

- `Hybrid`:
  Current default recommendation. PrintSphere tries to combine cloud and local data, picks the better active path, and still uses the local camera when available.
- `Cloud only`:
  Good starting point for many newer models. The local MQTT path and local camera page are disabled.
- `Local only`:
  Mainly technical or experimental right now. The current real-world setup still assumes a working cloud login first, and the cloud cover page is not used in this mode.

Important:

- Changing the connection mode restarts the device.

### 4. Local Printer Access

Local printer access is optional on top of the cloud setup.

Enter:

- printer IP or hostname
- printer serial number
- access code

This may still help with:

- local fallback data in `Hybrid`
- local MQTT status on supported models
- the local camera page

Current guidance from the code:

- `P1`, `A1`, and `X1` families are the strongest candidates for adding the local path.
- `P2S` does not currently support local status in the code.
- `H2` local status may require Developer Mode.
- On many newer models, cloud may already be enough for everyday use.
- If cloud data still looks incomplete on your printer, adding the local path could still help.

### 5. Arc Colors

You can customize the ring colors directly in Web Config.

- changes should preview live immediately
- saving them does not require a reboot

## What You See On The Device

- Main page:
  can show progress ring, lifecycle, temperatures, layers, remaining time, battery, and Wi-Fi state
- Page 2:
  can show cloud cover preview and title
- Page 3:
  can show local camera snapshots when that path is working

Extra interactions:

- long-press the display to request a Web Config PIN
- tap the camera page to request a refresh of the current image
- the camera page also tries to auto-refresh while it is open
- tap the center logo to request a chamber light toggle on supported models

## Model Guidance

Use these as practical starting points based on the current code and limited real-world testing:

- `P1`, `A1`, and often `X1` family:
  start with `Hybrid` and usually also configure the local printer path
- `P2` and `H2` family:
  start cloud-first

Camera notes:

- local JPEG camera support currently fits best for `A1`, `A1 Mini`, `P1P`, and `P1S`
- `X1`, `P2`, and `H2` families use an RTSP-style camera path in code, but that path is not fully finished yet

## Troubleshooting

- If Web Config is locked, hold the display again to request a new PIN.
- If the device does not join your home Wi-Fi, reconnect to `PrintSphere-Setup` and check the Wi-Fi credentials.
- If Bambu asks for a verification code during login, enter it directly in Web Config.
- If cloud data already shows temperatures and layers on your printer, local setup may simply be unnecessary.
- If cloud data still looks incomplete on your printer, adding the local printer path could improve the result.
- If the camera page matters to you, the best-supported local camera path is currently on `A1` and `P1` family printers.

## Current Notes

- Camera and MJPEG/RTSP work are not fully finished yet.
- Cloud support has been expanded and should now cover temperatures and layer information on many models, but this is not yet fully validated across all newer printers.
- Most hands-on testing so far has been on `P1S` and `P1P`.
- [`release/firmware.bin`](release/firmware.bin) is the ready-to-use initial flash image for end users.

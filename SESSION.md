# Session

## Project

C6 Wi-Fi + Zigbee Gateway

## Hardware

- Seeed Studio XIAO ESP32-C6
- No additional hardware planned

## Objective

Create a small experimental gateway using the ESP32-C6 SoftAP on Wi-Fi and Zigbee End Device connectivity to the existing Zigbee network used by Home Assistant/Zigbee2MQTT.

## Current state

### Phase 0 — ESP-IDF baseline

- [x] Repository created
- [x] ESP-IDF project skeleton added
- [x] ESP32-C6 target configured
- [x] Minimal `app_main()` added
- [ ] Cloud build verified
- [ ] Firmware flashed to hardware

## Rules for development

- One step at a time.
- Do not add Wi-Fi and Zigbee together before each part is independently proven.
- Do not add sensors or external hardware.
- Keep the XIAO ESP32-C6 as the complete hardware platform.
- Keep Home Assistant/Zigbee2MQTT infrastructure unchanged.

## Next step

Verify the GitHub Actions cloud build succeeds for the minimal ESP-IDF firmware.

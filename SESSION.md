# Session

## Project

C6 Wi-Fi + Zigbee Gateway

## Hardware

- Seeed Studio XIAO ESP32-C6
- No additional hardware planned

## Objective

Create a small experimental gateway using the ESP32-C6 SoftAP on Wi-Fi and Zigbee Router connectivity to the existing Zigbee network used by Home Assistant/Zigbee2MQTT.

## Current state

### Phase 0 — ESP-IDF baseline
- [x] Repository created
- [x] ESP-IDF project skeleton added
- [x] ESP32-C6 target configured
- [x] Minimal firmware flashed and verified

### Phase 1 — Wi-Fi SoftAP
- [x] SoftAP started
- [x] iPhone/client connection verified
- [x] Multiple clients verified

### Phase 2 — HTTP status
- [x] HTTP server started
- [x] JSON status endpoint verified at http://192.168.4.1/

### Phase 3 — Zigbee Router coexistence
- [ ] Cloud build verified
- [ ] Firmware flashed
- [ ] Zigbee stack initialized alongside Wi-Fi
- [ ] Zigbee Router joins the existing HA/Zigbee2MQTT network

## Rules for development

- One step at a time.
- Do not add gateway protocol logic before Wi-Fi + Zigbee coexistence is proven.
- Do not add sensors or external hardware.
- Keep the XIAO ESP32-C6 as the complete hardware platform.
- Keep Home Assistant/Zigbee2MQTT infrastructure unchanged.

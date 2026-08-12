# C6 Wi-Fi + Zigbee Gateway

Experimental project for the Seeed Studio XIAO ESP32-C6.

## Goal

Explore a small gateway where the ESP32-C6 provides a Wi-Fi SoftAP for local clients and communicates with the existing Home Assistant Zigbee network as a Zigbee End Device.

The project is deliberately incremental. We will prove each capability before adding the next one.

## Current phase

**Phase 0 — ESP-IDF baseline**

- ESP32-C6 target
- Minimal firmware
- Cloud build through GitHub Actions
- No Wi-Fi or Zigbee functionality yet

## Planned progression

1. Confirm cloud build
2. Flash the minimal firmware once over USB
3. Add Wi-Fi SoftAP
4. Verify a client can connect
5. Add Zigbee End Device
6. Verify Wi-Fi + Zigbee coexistence
7. Only then explore the gateway protocol

## Development approach

ESP-IDF is used instead of ESPHome because the required SoftAP + Zigbee combination is outside the ESPHome Zigbee component's supported model.

The project is intentionally kept small and test-driven: one working step at a time.

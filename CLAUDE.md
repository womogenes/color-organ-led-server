# Color Organ ESP32 WebSocket RGB Controller

## Overview
ESP32-based RGB LED controller with WebSocket interface and automatic sine wave animations.

## Hardware
- ESP32 (pins 21, 22, 23 for RGB)
- 2N7000 MOSFETs switching 24V LED current
- PWM control with gamma 2.2 correction

## Features

### WebSocket Control
- Endpoint: `ws://[ESP_IP]/ws`
- Send 3-byte RGB packets (0-255 values)
- Sub-1ms latency
- Ping mode: send `[255,255,255]` to get echo response

### Sine Wave Animation Mode
- Auto-activates when no WebSocket connection
- Red: 0.5s period sine wave
- Green: 1.0s period sine wave  
- Blue: 2.0s period sine wave
- All phases aligned for color mixing effects

## Configuration
WiFi setup via `idf.py menuconfig` → Example Connection Configuration

## Build & Flash
```bash
idf.py build
idf.py flash monitor
```

## Technical Details
- Gamma 2.2 lookup table for perceptual brightness
- Thread-safe LED control with mutex
- Background animation task (20ms updates)
- WebSocket connection state tracking
- Default 50% brightness on startup

## Dependencies
- esp_driver_ledc, esp_driver_gpio
- esp_http_server, esp_wifi, nvs_flash
- protocol_examples_common, esp_timer
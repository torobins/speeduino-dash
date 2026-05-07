# Speeduino Dash

Web-based realtime dashboard for a Speeduino ECU on a marine 3-cylinder 2-stroke engine.

An ESP32 reads engine data from the Speeduino over secondary serial and serves a browser-based gauge dashboard over WiFi.

## Hardware

- **MCU**: Adafruit QT Py ESP32 Pico
- **ECU**: Speeduino (secondary serial interface)
- **Logic level converter** between ESP32 (3.3V) and Speeduino (5V)

### Wiring

| Signal | ESP32 Pin | Direction |
|--------|-----------|-----------|
| Speeduino TX → ESP RX | GPIO27 (A2) | ECU → ESP |
| ESP TX → Speeduino RX | GPIO32 | ESP → ECU |

## Protocol

Uses the Speeduino **'A' command** on the secondary serial port:
- Send `0x41` ('A'), receive 1 confirm byte + 75 realtime data bytes
- Single round-trip for all engine parameters
- 115200 baud, 8N1

## Dashboard Gauges

| Parameter | Offset | Size | Range |
|-----------|--------|------|-------|
| RPM | 14 | 2 bytes | 0-7000 |
| MAP (kPa) | 4 | 2 bytes | 0-120 |
| TPS (%) | 24 | 1 byte | 0-100 |
| Timing Advance | 23 | 1 byte (signed) | -10 to +45 |
| Battery (V) | 9 | 1 byte (x10) | 10-16V bar |

## Building & Flashing

Requires [PlatformIO](https://platformio.org/).

```
pio run -t upload
```

Serial monitor (close before flashing):
```
pio device monitor
```

## Architecture

- `src/main.cpp` — all firmware code (single file)
- Dashboard HTML/CSS/JS embedded as PROGMEM
- ESPAsyncWebServer + WebSocket for real-time browser updates
- Polls ECU at 150ms, broadcasts to browser at ~4 Hz (250ms)
- Adaptive backoff (2s) when ECU is unresponsive

## WiFi

Currently hardcoded in firmware:
- SSID: `Penny`
- Password: `foxglove2017`

## Future Plans

See [SENSOR_PLAN.md](SENSOR_PLAN.md) for the full sensor and hardware roadmap:
- GPS module for vessel speed and location
- Per-cylinder EGT (x3) for combustion monitoring
- CHT for cooling system safety
- Full EFI sensor suite
- Migration to full ESP32 dev board for more GPIO

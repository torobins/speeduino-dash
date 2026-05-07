# Sensor & Hardware Roadmap

## Engine
Marine 3-cylinder 2-stroke, wet exhaust. Currently carbureted, planning full EFI via Speeduino.
Alpha-N (TPS-based) fueling — MAP not useful on 2-strokes.

## Current Sensors (via Speeduino 'A' command)
- RPM, MAP, TPS, battery voltage, timing advance
- CLT offset defined, not yet displayed

## Planned Additions

### GPS Module
- NEO-6M or BN-220 (u-blox, 3.3V UART, 9600 baud)
- Serial1 on GPIO33, RX only
- TinyGPSPlus library
- Dashboard: speed gauge + satellite count

### Per-Cylinder EGT (x3) — Primary Tuning Sensor
- One K-type thermocouple per cylinder
- Custom spacer/flange between exhaust port and manifold (1/8" NPT)
- Mount before water injection point
- MAX31855 amplifier per thermocouple, shared SPI bus + individual CS pins
- Why not wideband O2: 2-stroke scavenging causes false rich readings,
  and water in marine exhaust destroys the zirconia sensor element

### CHT (per head) — Secondary Safety
- Cylinder head temperature for cooling system health monitoring
- Too slow to catch lean spikes (EGT handles that)
- Spark plug washer type or threaded sensor
- K-type + MAX31855, same SPI bus

### Full EFI Sensor Priority
1. TPS — primary load input (already reading)
2. EGT x3 — per-cylinder combustion monitoring
3. Crank position — timing reference
4. CHT — cooling system / cold enrichment
5. IAT — intake air density compensation
6. Barometric pressure — altitude compensation
7. Coolant temp — warm-up enrichment
8. GPS — vessel speed and location

## Hardware Notes
- Current board: Adafruit QT Py ESP32 Pico (limited pins)
- Will need to switch to full ESP32 dev board when adding EGT + GPS
- PlatformIO board swap: change platformio.ini + remap pin defines

# RoostOS Communicator

Firmware for the LILYGO T-Deck Plus handheld device.

## Building

### For Device (ESP32-S3)
```bash
pio run -e esp32s3
```

### For Testing (Native)
```bash
pio test -e native
```

## Project Structure

- `src/` - Main application source code
- `test/` - Test suites
  - `native/` - Host-based unit tests (runs on build machine)
- `platformio.ini` - PlatformIO configuration for both device and test environments

## Environments

- **esp32s3** - LILYGO T-Deck Plus device target
- **native** - Native test environment (Unity framework)

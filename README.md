# AThermostat

Thermostat controller for Goodman furnace + heatpump, running on ESP32-S3-DevKitC-1.

## Board

- **MCU:** ESP32-S3 (QFN56, 8MB Flash, 8MB PSRAM)
- **Board:** ESP32-S3-DevKitC-1-N8
- **Framework:** Arduino (PlatformIO)

## Pin Map (ESP32-S3-DevKitC-1)

### Relay Outputs

| GPIO | Pin Name | Function |
|------|----------|----------|
| 4 | PIN_FAN1 | Fan stage 1 relay |
| 5 | PIN_REV | Reversing valve relay |
| 6 | PIN_FURN_COOL_LOW | Furnace cool low relay |
| 7 | PIN_FURN_COOL_HIGH | Furnace cool high relay |
| 15 | PIN_W1 | Heat stage 1 (W1) relay |
| 16 | PIN_W2 | Heat stage 2 (W2) relay |
| 17 | PIN_COMP1 | Compressor stage 1 relay |
| 18 | PIN_COMP2 | Compressor stage 2 relay |

### Digital Inputs

| GPIO | Pin Name | Function |
|------|----------|----------|
| 45 | PIN_OUT_TEMP_OK | Outdoor temperature OK signal (strapping pin, pulldown) |
| 47 | PIN_DEFROST_MODE | Defrost mode signal (pulldown) |

### Pressure Sensors (HX710)

| GPIO | Pin Name | Function |
|------|----------|----------|
| 19 | PIN_HX710_1_DOUT | HX710 sensor 1 data out (shared with USB D-) |
| 20 | PIN_HX710_1_CLK | HX710 sensor 1 clock (shared with USB D+) |
| 10 | PIN_HX710_2_DOUT | HX710 sensor 2 data out |
| 11 | PIN_HX710_2_CLK | HX710 sensor 2 clock |

### I2C Bus (Reserved)

| GPIO | Pin Name | Function |
|------|----------|----------|
| 8 | PIN_SDA | I2C data |
| 9 | PIN_SCL | I2C clock |

### CAN Bus (TWAI)

| GPIO | Pin Name | Function |
|------|----------|----------|
| 13 | PIN_CAN_TX | CAN transmit via SN65HVD230 transceiver |
| 14 | PIN_CAN_RX | CAN receive via SN65HVD230 transceiver |

### RS-485 Modbus (Planned)

| GPIO | Pin Name | Function |
|------|----------|----------|
| 1 | RS-485 A TX | UART1 TX - A2L leak detector (via auto-direction module) |
| 2 | RS-485 A RX | UART1 RX - A2L leak detector |
| 21 | RS-485 B TX | UART2 TX - Furnace Modbus (via auto-direction module) |
| 38 | RS-485 B RX | UART2 RX - Furnace Modbus |

### System / Debug

| GPIO | Function |
|------|----------|
| 43 | UART0 TX (serial debug) |
| 44 | UART0 RX (serial debug) |

### Free GPIOs

| GPIO | Notes |
|------|-------|
| 12 | General purpose, reserved |
| 48 | General purpose, reserved (Neopixel on some DevKitC boards) |

### Unavailable GPIOs

| GPIO | Reason |
|------|--------|
| 0 | Strapping pin (boot mode select) |
| 3 | Strapping pin (JTAG signal source) |
| 26-32 | Connected to SPI flash |
| 33-37 | Connected to octal PSRAM |
| 39-42 | JTAG pads (freeable via DIS_PAD_JTAG eFuse) |
| 46 | Strapping pin (boot mode / ROM log) |

## API Endpoints

All API endpoints use the `/api/` prefix to avoid ESPAsyncWebServer route prefix matching conflicts.

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/status` | GET | Thermostat state, temps, I/O, uptime, CPU temp |
| `/api/mode` | POST | Set thermostat mode (off/heat/cool/heat_cool/fan_only) |
| `/api/setpoint` | POST | Set heat/cool setpoints |
| `/api/fan_idle` | POST | Set fan idle behavior |
| `/api/force_no_hp` | POST | Toggle force-no-heatpump flag |
| `/api/force_furnace` | POST | Toggle force-furnace flag |
| `/api/pins` | GET | Pin states and eFuse info |
| `/api/config/load` | GET | Load device configuration |
| `/api/config/save` | POST | Save device configuration |
| `/api/login` | POST | Authentication |
| `/heap` | GET | Memory and CPU load stats |
| `/update` | POST | OTA firmware upload (writes to LittleFS, applies from main loop) |
| `/update/info` | GET | Current build and backup firmware info |
| `/update/revert` | POST | Revert to backup firmware |
| `/reboot` | POST | Reboot device |

## Web Pages

| Path | Description |
|------|-------------|
| `/dashboard` | Main dashboard - temp, mode, setpoints, I/O, system info |
| `/pins` | GPIO pin states, eFuse info, protected pins |
| `/config` | Device configuration editor |
| `/update` | OTA firmware update page |
| `/log/view` | Log viewer |
| `/heap/view` | Heap/PSRAM/CPU load monitor |

## Build & Deploy

```bash
# Build
~/.platformio/penv/bin/pio run -e esp32s3devkitc1

# Upload firmware (kill serial lock first)
fuser -k /dev/ttyUSB0; ~/.platformio/penv/bin/pio run -e esp32s3devkitc1 -t upload

# Upload HTML files via FTP (do NOT use uploadfs - it wipes config.txt)
curl -T data/www/dashboard.html ftp://admin:admin@<DEVICE_IP>/www/dashboard.html
```

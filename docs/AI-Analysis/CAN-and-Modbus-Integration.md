# CAN Bus and Modbus Integration Analysis

## Overview

Analysis of integrating CAN bus as the internal communication backbone between HVAC
control devices, with Modbus RTU gateways for external sensor/equipment integration.

## Network Topology

```
                            CAN Bus (18/3 thermostat wire, 50 ft max)
══════════════════╤═══════════════════╤═════════════════╤══════════════════
                  │                   │                 │
           ┌──────┴───────┐    ┌──────┴──────┐   ┌─────┴──────┐
           │  AThermostat  │    │   Display   │   │GoodmanHPCtrl│
           │  (ESP32-S3)   │    │   (ESP32)   │   │   (ESP32)   │
           │    [120R]     │    │  CAN only   │   │   [120R]    │
           └──┬─────────┬──┘    └─────────────┘   └─────────────┘
              │         │                           CAN only
         RS-485 A   RS-485 B
        (UART1)    (UART2)
        GPIO 1/2   GPIO 21/38
              │         │
        ┌─────┴────┐ ┌──┴──────────┐
        │ A2L Leak │ │  Furnace    │
        │ Detector │ │  Modbus     │
        │ (Modbus) │ │  (Modbus)   │
        └──────────┘ └─────────────┘
```

## Why CAN Bus Over Modbus for Internal Communication

### CAN Bus Advantages (chosen)
- **Multi-master**: Any device can initiate communication without polling
- **Hardware arbitration**: Collisions resolved automatically at the bit level
- **Built-in error handling**: Hardware CRC, ACK, automatic retransmit, bus-off recovery
- **Event-driven**: No polling delay -- setpoint changes and alarms propagate immediately
- **ESP32 native TWAI peripheral**: No UART consumed, dedicated hardware controller
- **Noise immunity**: Designed for automotive environments (relays, compressors, motors)
- **Easy expansion**: Adding a 3rd device (display) is just a wire tap -- no protocol changes

### Modbus RTU Disadvantages for Internal Use
- Master-slave only -- one side must poll, adding latency
- Adding a display requires either a second bus or relay logic through master
- Consumes a UART per bus
- Software CRC, no hardware error recovery
- Half-duplex direction switching adds complexity (unless using auto-direction module)

### Modbus RTU Advantages for External Devices (used for sensors/furnace)
- Industry-standard HVAC/BMS protocol
- A2L refrigerant leak detectors (Bacharach MGS-400 etc.) speak Modbus
- Furnace communicating controllers speak Modbus
- Well-defined register model, extensive tooling for debugging

## CAN Bus Specifications

| Parameter | Value |
|-----------|-------|
| Speed | 250 kbps |
| Wire | 18 AWG thermostat wire (unshielded, not twisted pair) |
| Max distance | 50 ft (~15m) -- well within CAN limits |
| Termination | 120R at each endpoint (AThermostat + GoodmanHPCtrl) |
| Transceiver | SN65HVD230DR (3.3V, SOIC-8) |
| ESP32 peripheral | TWAI (Two-Wire Automotive Interface) |

## CAN Message ID Scheme

| ID | Source | Content | Byte Layout |
|----|--------|---------|-------------|
| 0x100 | AThermostat | Mode, setpoints, flags | [0] mode, [1-2] heatSP x10, [3-4] coolSP x10, [5] flags |
| 0x101 | AThermostat | Indoor temp, pressures | [0-1] temp x10, [2-3] press1 x100, [4-5] press2 x100 |
| 0x102 | AThermostat | A2L leak sensor data | [0-1] PPM, [2] alarm state, [3] fault |
| 0x103 | AThermostat | Furnace Modbus data | [0] flame, [1-2] exhaust temp x10, [3] fault code |
| 0x110 | Display | User setpoint change | [0-1] heatSP x10, [2-3] coolSP x10 |
| 0x111 | Display | User mode change | [0] mode |
| 0x200 | GoodmanHPCtrl | Compressor state, faults | TBD |
| 0x201 | GoodmanHPCtrl | Outdoor temp, pressures | TBD |
| 0x3FF | Any | Heartbeat | [0] nodeID, [1-4] uptime_sec (BE) |

### Node IDs (used in heartbeat)
- 0x01 = AThermostat
- 0x02 = Display
- 0x03 = GoodmanHPCtrl

### Data Encoding
- Temperatures: int16_t x10 (e.g., 725 = 72.5 F), -9999 = invalid
- Pressures: int16_t x100 (e.g., 125 = 1.25 inWC), -9999 = invalid
- Setpoints: int16_t x10
- Flags byte (0x100): bit0=forceFurnace, bit1=forceNoHP, bit2=defrost

## Pin Assignments

### Common Pins (all devices)
| Function | GPIO | Notes |
|----------|------|-------|
| CAN TX | 13 | TWAI TX -> SN65HVD230 TXD |
| CAN RX | 14 | TWAI RX <- SN65HVD230 RXD |

### AThermostat Additional (RS-485 Modbus gateway)
| Function | GPIO | Notes |
|----------|------|-------|
| RS-485 A TX (UART1) | 1 | -> Auto-direction module -> A2L leak sensor |
| RS-485 A RX (UART1) | 2 | <- Auto-direction module |
| RS-485 B TX (UART2) | 21 | -> Auto-direction module -> Furnace Modbus |
| RS-485 B RX (UART2) | 38 | <- Auto-direction module |

### AThermostat Full Pin Map
| GPIO | Function | Status |
|------|----------|--------|
| 0 | Strapping (boot) | Unavailable |
| 1 | RS-485 A TX | **NEW** |
| 2 | RS-485 A RX | **NEW** |
| 3 | Strapping (JTAG sel) | Unavailable |
| 4 | Relay: fan1 | Existing |
| 5 | Relay: rev | Existing |
| 6 | Relay: furn_cool_low | Existing |
| 7 | Relay: furn_cool_high | Existing |
| 8 | I2C SDA | Existing |
| 9 | I2C SCL | Existing |
| 10 | HX710 #2 DOUT | Existing |
| 11 | HX710 #2 CLK | Existing |
| 12 | **Free** | Reserved |
| 13 | CAN TX | **NEW** |
| 14 | CAN RX | **NEW** |
| 15 | Relay: w1 | Existing |
| 16 | Relay: w2 | Existing |
| 17 | Relay: comp1 | Existing |
| 18 | Relay: comp2 | Existing |
| 19 | HX710 #1 DOUT (USB D-) | Existing |
| 20 | HX710 #1 CLK (USB D+) | Existing |
| 21 | RS-485 B TX | **NEW** |
| 26-32 | SPI Flash | Unavailable |
| 33-37 | Octal PSRAM | Unavailable |
| 38 | RS-485 B RX | **NEW** |
| 39-42 | JTAG | Unavailable |
| 43 | UART0 TX (debug) | Existing |
| 44 | UART0 RX (debug) | Existing |
| 45 | Input: out_temp_ok | Existing |
| 46 | Strapping | Unavailable |
| 47 | Input: defrost_mode | Existing |
| 48 | **Free** | Reserved |

## Hardware

### CAN Transceiver Circuit (per node)

```
ESP32                  SN65HVD230DR              Bus
                    +----------------+
GPIO 13 (TX) ------| 1 TXD    VCC 8 |---- 3.3V
                    |                |       |
GND ---------------| 2 GND   CANH 7 |------+---- CAN_H
                    |                |      |
              3.3V -| 3 VCC   CANL 6 |------+---- CAN_L
                    |                |   [120R]*
GPIO 14 (RX) ------| 4 RXD     Rs 5 |---- GND    (* endpoints only)
                    +----------------+
                         |
                     100nF to GND (Vcc bypass)

Rs (pin 5) = GND -> high-speed mode (1 Mbps capable)
```

### RS-485 Auto-Direction Module (Sharvi Technology RS485-TTL Module_pro)

Uses 74HC04D hex inverter + RC circuit (56K + 33pF + 1N4148W) to automatically
control the SP3485EEN DE/RE pins based on TX activity. Eliminates the need for a
dedicated GPIO direction control pin.

**Features:**
- SP3485EEN 3.3V half-duplex RS-485 transceiver
- 74HC04D auto-direction circuit (no DE/RE GPIO needed)
- TPS61089RNRR 5V to 12V boost (bus power output)
- TVS diodes (SMBj5.5CA) on A/B lines
- Resettable PTC fuses on A/B lines
- TX/RX activity LEDs
- DIP switch for termination resistor (120R)

**ESP32 connection:** TX, RX, VIN (5V), GND -- only 2 GPIOs per bus.

### Parts List (LCSC)

#### CAN Transceiver (every node)
| Part | LCSC # | Package | Vcc | Price | Notes |
|------|--------|---------|-----|-------|-------|
| **SN65HVD230DR** (TI) | C12084 | SOIC-8 | 3.3V | ~$0.75 | Recommended: proven ESP32 combo, 16kV ESD |
| TJA1051T/3/1J (NXP) | C38695 | SOIC-8 | 3.3V | ~$0.28 | Budget alternative, CAN FD capable |

#### RS-485 Transceiver (on auto-direction module, AThermostat only)
| Part | LCSC # | Package | Vcc | Price | Notes |
|------|--------|---------|-----|-------|-------|
| **SP3485EN-L/TR** (MaxLinear) | C8963 | SOIC-8 | 3.3V | ~$0.39 | Used on Sharvi module |
| MAX3485ESA+T (ADI) | C18148 | SOIC-8 | 3.3V | ~$1.16 | Pin-compatible alternative |

#### Passives (per node)
| Part | Qty | Notes |
|------|-----|-------|
| 120R resistor (0805) | 1 | CAN termination -- endpoints only |
| 100nF cap (0805) | 1 per transceiver | Vcc bypass |
| 3-pin screw terminal | 1 | CAN bus (CAN_H, CAN_L, GND) |

### BOM Summary Per Device

| | AThermostat | GoodmanHPCtrl | Display |
|---|---|---|---|
| SN65HVD230DR | x1 | x1 | x1 |
| RS485-TTL Module_pro | x2 | -- | -- |
| 120R (0805) | x1 (endpoint) | x1 (endpoint) | -- |
| 100nF (0805) | x1 | x1 | x1 |
| **IC cost** | **~$0.75** | **~$0.75** | **~$0.75** |

## Software Architecture

### CANBus Driver (implemented)
- `include/CANBus.h` / `src/CANBus.cpp`
- Uses ESP-IDF TWAI driver directly
- Poll task (10ms) reads RX queue on main loop -- no thread contention
- Heartbeat task (5s) sends node alive status
- Auto bus-off recovery
- Callback-based RX dispatch

### CAN Publish Task (in main.cpp)
- Runs every 2 seconds on TaskScheduler
- Packs thermostat state (mode, setpoints, flags) into 0x100
- Packs sensor data (indoor temp, pressures) into 0x101
- Future: 0x102 (A2L leak) and 0x103 (furnace) when Modbus masters added

### CAN RX Handler (in main.cpp)
- Dispatches by message ID
- 0x110: Display setpoint change -> thermostat.setHeatSetpoint/setCoolSetpoint
- 0x111: Display mode change -> thermostat.setMode
- 0x200: HP state -> log (future: integrate into dashboard)
- 0x3FF: Heartbeat -> log

### Future: Modbus Masters
- UART1 (GPIO 1/2): A2L leak detector polling (1s interval)
- UART2 (GPIO 21/38): Furnace Modbus polling (2s interval)
- Both publish results to CAN bus as 0x102 and 0x103
- AThermostat acts as Modbus-to-CAN gateway for both devices
- Local leak alarm response (immediate system shutdown) -- no CAN round-trip

## Physical Wiring

### CAN Bus Daisy Chain
```
AThermostat ---- ~25 ft ---- Display ---- ~25 ft ---- GoodmanHPCtrl
  [120R]                    (no term)                     [120R]
```

- 18/3 thermostat wire: CAN_H, CAN_L, GND
- Daisy chain (NOT star topology)
- 120R termination at endpoints only
- Display taps in anywhere along the run

### Modbus RS-485 (short runs from AThermostat)
- A2L leak sensor: 2-wire (A, B) + GND to nearby evaporator coil area
- Furnace Modbus: 2-wire (A, B) + GND to furnace controller
- Both through auto-direction RS485-TTL modules (no DE/RE GPIO)

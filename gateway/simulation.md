# Renode Simulation for SAMR21 Xpro

## Current State

The project currently uses a **modified SAMD21 platform** (`atsamd21j17d-aft.repl`) to simulate the SAMR21 Xpro board. This works for basic CPU, UART, GPIO, and timer functionality, but **does not model the IEEE 802.15.4 radio**.

### Multi-Application Support

The workspace supports multiple Zephyr applications in the same repository:

```
ufsm292-zephyr-app-2026/
├── app/                  # Main application
├── gateway/              # Gateway firmware (new)
├── lib/                  # Shared libraries
├── drivers/              # Custom drivers
├── include/              # Shared headers
└── build-app/            # Build output for app
    build-gateway/        # Build output for gateway
```

#### Building Applications

```bash
export GNUARMEMB_TOOLCHAIN_PATH=/usr

# Build app
west build -b samr21_xpro -s app -d build-app

# Build gateway
west build -b samr21_xpro -s gateway -d build-gateway
```

#### Running in Renode

```bash
# Run app
renode -e "include @renode/samr21_xpro.resc" -e "set bin @build-app/zephyr/zephyr.elf" -e "start"

# Run gateway
renode -e "include @renode/samr21_xpro.resc" -e "set bin @build-gateway/zephyr/zephyr.elf" -e "start"
```

### Why SAMD21 Works as a Base

The SAMR21 is essentially a SAMD21 with an integrated AT86RF233 802.15.4 radio transceiver. The core peripherals are identical:

- Cortex-M0+ CPU @ 48MHz
- Same SRAM/Flash layout
- Same SERCOM, GPIO, Timer, RTC peripherals
- Same peripheral register addresses

### What's Missing

The SAMR21 Xpro board includes an **external AT86RF233 radio chip** connected via SPI:

| Signal | Pin | Function |
|--------|-----|----------|
| SPI MOSI | SERCOM4/PB30 | SPI Data Out |
| SPI MISO | SERCOM4/PC19 | SPI Data In |
| SPI SCK | SERCOM4/PC18 | SPI Clock |
| SPI CS | SERCOM4/PB31 | Chip Select |
| IRQ | PB0 | Radio interrupt (RX_START, TRX_END, etc.) |
| RESET | PB15 | Radio reset |
| SLP_TR | PA20 | Sleep/Transmit trigger |
| DIG2 | PB17 | Digital output 2 |

## Hardware Architecture

```
┌─────────────────────────────────────────────────────┐
│                   SAMR21G18A                        │
│                                                     │
│  ┌─────────┐    ┌─────────┐    ┌─────────────────┐ │
│  │  CPU    │    │ SERCOM4 │    │    GPIO         │ │
│  │ Cortex- │◄──►│  (SPI)  │    │  PB0  (IRQ)    │ │
│  │  M0+    │    │         │    │  PB15 (RESET)  │ │
│  └─────────┘    └────┬────┘    │  PA20 (SLP_TR) │ │
│                      │         │  PB17 (DIG2)   │ │
└──────────────────────┼─────────┴─────────────────┘
                       │ SPI Bus
                       ▼
              ┌────────────────┐
              │   AT86RF233    │
              │  802.15.4 RF   │
              │  Transceiver   │
              └────────────────┘
```

## Radio Register Interface

The AT86RF233 communicates via SPI using a command-based protocol:

### SPI Commands (first byte)
```
Bit 7-6: Command Type
  00 = SRAM Read/Write
  01 = Frame Buffer Read/Write
  10 = Register Read/Write

Bit 5: Read (0) / Write (1)
Bit 4-0: Address (for register commands)
```

### Key Registers
| Address | Name | Function |
|---------|------|----------|
| 0x01 | TRX_STATUS | Radio status (TRX_ON, RX_ON, etc.) |
| 0x02 | TRX_STATE | State control commands |
| 0x03 | TRX_CTRL_0 | PHY configuration |
| 0x04 | TRX_CTRL_1 | TX/RX control |
| 0x05 | PHY_TX_PWR | Transmit power |
| 0x06 | PHY_RSSI | RSSI/CRC status |
| 0x08 | PHY_CC_CCA | Channel/CCA |
| 0x0E | IRQ_MASK | Interrupt mask |
| 0x0F | IRQ_STATUS | Interrupt status |
| 0x1C | PART_NUM | Device ID (0x01 for AT86RF233) |
| 0x1D | VERSION_NUM | Version (0x02) |
| 0x20-0x2B | SHORT_ADDR/IEEE_ADDR | Address registers |

### State Machine
```
┌─────────────┐
│   RESET     │
└──────┬──────┘
       ▼
┌─────────────┐
│  PLL_ON     │◄──────┐
└──────┬──────┘       │
       │              │
       ▼              │
┌─────────────┐       │
│   TRX_OFF   │───────┘ (CMD_FORCE_TRX_OFF)
└──────┬──────┘
       │ (CMD_TRX_OFF)
       ▼
┌─────────────┐
│  RX_ON      │──────┐
└──────┬──────┘      │
       │             │ (auto after RX)
       ▼             │
┌─────────────┐      │
│  RX_AACK_ON │      │
└──────┬──────┘      │
       │             │
       ▼             │
┌─────────────┐      │
│  BUSY RX    │──────┘
└─────────────┘
```

## Proposed Solution: Python Peripheral Simulation

Renode supports **Python peripherals** that can intercept SPI transactions. We can create a custom peripheral that simulates the AT86RF233's register interface.

### Approach

1. **Keep the SAMD21 base platform** - It already has the correct CPU, memory, and GPIO
2. **Add SERCOM4 as SPI** - Model the SPI controller for radio communication
3. **Create Python peripheral for AT86RF233** - Intercept SPI transactions and respond with radio register values
4. **Wire GPIO interrupts** - Connect radio IRQ to NVIC

### Implementation Plan

#### Step 1: Create AT86RF233 Python Peripheral

```python
# renode/pydev/at86rf233.py
# Simulates AT86RF233 802.15.4 radio transceiver

class AT86RF233:
    def __init__(self):
        self.regs = {
            0x01: 0x08,  # TRX_STATUS: TRX_OFF
            0x02: 0x08,  # TRX_STATE: TRX_OFF
            0x03: 0x00,  # TRX_CTRL_0
            0x04: 0x00,  # TRX_CTRL_1
            0x05: 0x00,  # PHY_TX_PWR
            0x06: 0x00,  # PHY_RSSI
            0x08: 0x00,  # PHY_CC_CCA
            0x0E: 0x00,  # IRQ_MASK
            0x0F: 0x00,  # IRQ_STATUS
            0x1C: 0x01,  # PART_NUM: AT86RF233
            0x1D: 0x02,  # VERSION_NUM
            0x20: 0x00,  # SHORT_ADDR_0
            0x21: 0x00,  # SHORT_ADDR_1
            0x22: 0x00,  # PAN_ID_0
            0x23: 0x00,  # PAN_ID_1
        }
        self.frame_buffer = bytearray(128)
        self.state = 'TRX_OFF'
        
    def handle_spi(self, mosi_data):
        """Process SPI transaction and return MISO data"""
        cmd = (mosi_data >> 6) & 0x03
        rw = (mosi_data >> 5) & 0x01
        addr = mosi_data & 0x1F
        
        if cmd == 0x02:  # Register access
            if rw == 0:  # Read
                return self.regs.get(addr, 0x00)
            else:  # Write
                self.regs[addr] = mosi_data
                self.handle_state_change(addr, mosi_data)
                return 0x00
        return 0x00
    
    def handle_state_change(self, addr, value):
        if addr == 0x02:  # TRX_STATE
            if value == 0x08:
                self.state = 'TRX_OFF'
            elif value == 0x06:
                self.state = 'RX_ON'
            elif value == 0x09:
                self.state = 'RX_AACK_ON'
```

#### Step 2: Update Platform Description

```repl
# renode/samr21_xpro.repl

using "platforms/cpus/atsamd21j17d-aft.repl"

# Add SERCOM4 for radio SPI
sercom4: UART.SAMD5_UART @ sysbus 0x4200_1800
    -> nvic0@13

# Add AT86RF233 radio peripheral
radio: Python.PythonPeripheral @ sysbus 0x5000_0000
    size: 0x1000
    initable: true
    filename: "renode/pydev/at86rf233.py"

# Wire radio IRQ to NVIC
radio -> nvic0@0  # External interrupt 0 (PB0)
```

#### Step 3: Update Rescue Script

```resc
# renode/samr21_xpro.resc

using sysbus

$name?="samr21_xpro"
$bin?=@build/zephyr/zephyr.elf

mach create $name
machine LoadPlatformDescription @renode/samr21_xpro.repl

# Load firmware
sysbus LoadELF $bin

# Show UART output
showAnalyzer sysbus.usart0

# Add radio traffic logging
sysbus.radio LogIRQChanges true

macro reset
"""
    sysbus LoadELF $bin
    sysbus.radio reset
"""

runMacro $reset
```

## Alternative: Frame Injection via Monitor

For simpler testing, we can inject frames directly via Renode Monitor commands:

```resc
# After boot, inject a test frame
macro inject_frame
"""
    # Write frame to radio's frame buffer
    sysbus.rawWrite 0x5000_0100 [0x08 0x41 0x80 0x00 0x00 0xFF 0xFF 0x01 0x02 0x03]
    # Set RX_START interrupt
    sysbus.radio WriteDoubleWord 0x0F 0x04
"""
```

## Testing Strategy

1. **Boot Test** - Verify Zephyr boots and outputs to UART
2. **Register Read** - Read PART_NUM (0x1C) should return 0x01
3. **State Machine** - Test TRX_STATE transitions
4. **Frame RX** - Inject frame and verify IRQ fires
5. **Frame TX** - Send frame and verify TRX_END interrupt

## References

- [AT86RF233 Datasheet](https://ww1.microchip.com/downloads/en/DeviceDoc/AT86RF233-Data-Sheet-DS40002182A.pdf)
- [Zephyr RF2XX Driver](../../zephyr/drivers/ieee802154/ieee86rf233.c)
- [Renode Python Peripherals](https://renode.readthedocs.io/en/latest/advanced/python.html)
- [Renode Platform Description](https://renode.readthedocs.io/en/latest/advanced/platform_description_format.html)

## Open Questions

1. Should we model the full radio state machine or just register reads/writes?
2. Do we need to simulate actual RF behavior (CSMA/CA, ACK handling)?
3. How to handle frame buffer - memory-mapped or SPI-based?
4. Should we add a second machine for radio-to-radio communication?

---

*Last updated: 2026*

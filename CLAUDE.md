# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

RemoteKeyboard is an AVR-based firmware that monitors and controls matrix-scanned keyboards and keypads. It runs on ATMega168 microcontrollers (Arduino Pro Mini compatible) and communicates over serial (38400 baud default) to report key presses/releases and accept commands to simulate key operations.

## Hardware Architecture

The firmware is designed for ATMega48/88/168 microcontrollers with configurable pin mappings:
- **Row outputs** (8): PB1:0 (2 pins) + PD7:2 (6 pins) - Drive matrix rows
- **Column inputs** (6): PC5:0 (Arduino A5-A0, PCINT13:8) - Read matrix columns
- **Auxiliary outputs** (1): PB2 (Arduino D10) - Non-matrix switches
- Clock: 8 MHz internal oscillator
- Serial: UART on PD1:0 (TX/RX)

The hardware uses vertical debouncing (3-sample validation) and pin-change interrupts for real-time matrix scanning.

## Build Commands

`avr-gcc` is not on the PATH on this machine; use the Arduino IDE's toolchain:

```bash
export PATH="/Users/ned/Library/Arduino15/packages/arduino/tools/avr-gcc/7.3.0-atmel3.6.1-arduino7/bin:$PATH"
```

All build operations use the AVR toolchain configured in the Makefile:

```bash
# Build firmware (creates .elf and .hex files)
make

# Clean build artifacts
make clean

# Program the microcontroller via avrdude (uses first /dev/cu.usbserial* port)
make program

# Open serial monitor with minicom
make serial

# Run demo mode (Ruby script simulates keyboard input)
make demo
```

The Makefile is configured for:
- Target: `remoteKeyboard.c` + `uartlibrary/uart.c`
- MCU: ATMega168, 8 MHz (F_CPU=8000000)
- Baud: 38400 (configurable via BAUD variable)
- Programmer: avrisp with optiboot bootloader at 115200 baud

## Serial Protocol

The device communicates using simple ASCII commands over serial (38400 8N1):

**From device (key events):**
- `pRC\r\n` - Row R, Column C pressed (e.g., `p23` = row 2, col 3 pressed)
- `rRC\r\n` - Row R, Column C released

**To device (commands):**
- `pRC\r` - Simulate pressing row R, column C
- `rRC\r` - Simulate releasing row R, column C
- `R\r` - Reset microcontroller (hardware reset via watchdog)
- `\r` - Dump debug state (shows column/row observations, forced/active/reported switches, scan counts, event queue overflows)

Row/column indices are ASCII digits: rows 0-7, columns 0-6 (column 6 = aux switches; aux rows must be < N_AUX_OUTPUTS).

Forced (simulated) keys are deliberately NOT echoed back as `pRC`/`rRC` events; only real key transitions are reported.

## Code Architecture

### Core Components

1. **Interrupt-driven scanning** (`PCINT1_vect` ISR):
   - Triggered on any column input change
   - Auto-detects quiescent state polarity (active-high vs active-low matrices)
   - Reads row states only when exactly one column is active
   - Implements vertical debouncing with 3-state validation
   - Drives forced switches (simulated key presses)

2. **Timer interrupt** (`TIMER0_OVF_vect` ISR at 30.5 Hz):
   - Handles auxiliary (non-matrix) switch scanning
   - Updates auxiliary outputs

3. **Main loop**:
   - Transmits queued key events over serial
   - Processes serial commands asynchronously
   - Enters idle sleep only when the event queue and RX buffer are empty (cli/sleep_enable/sei/sleep_cpu sequence to avoid the check-then-sleep race)

### Key Data Structures

- `forcedSwitches[N_COLUMNS+1]` - Bitmap of simulated key presses
- `activeSwitches[N_COLUMNS+1]` - Current switch states (1st sample)
- `priorActiveSwitches[N_COLUMNS+1]` - Previous switch states (2nd sample)
- `reportedSwitches[N_COLUMNS+1]` - Last state reported to the host; events are emitted only when the debounced state differs from it (prevents unmatched events after one-sample glitches)
- `eventQueue[]` - Lock-free SPSC ring buffer (head written only by ISRs, tail only by main); one byte per event: press flag bit 6, row bits 5:3, column bits 2:0
- Debouncing logic: `valid = (prior ^ active) & ~(active ^ input)`, then gated by `(reported ^ input) & ~forced`

### Pin Configuration Macros

The firmware uses extensive preprocessor macros in `remoteKeyboard.h` for pin mapping:
- `PB_ROW_MASK`, `PC_ROW_MASK`, `PD_ROW_MASK` - Define which port pins are row outputs
- `PB_COL_MASK`, `PC_COL_MASK`, `PD_COL_MASK` - Define which port pins are column inputs
- `PB_TO_ROW()`, `PB_FROM_ROW()` etc. - Convert between logical row/column bits and port bits

Changing pin assignments requires modifying these macros and corresponding `N_ROWS`, `N_COLUMNS` constants.

## Ruby Control Scripts

The `ruby/` directory contains host-side control software:

- **terminal.rb** - Full interactive terminal with TTY reader for Brother P-touch label printer
  - Maps PC keyboard to device keycodes
  - Handles shift, caps lock, num lock states
  - Bidirectional translation between host keys and device keys
  - Run: `ruby ruby/terminal.rb`

- **test.rb** - Demo mode script that continuously types a message
  - Uses same keyboard abstraction but runs autonomous demo
  - Run: `make demo` or `ruby ruby/test.rb`

Both scripts require the `serialport` gem. The terminal script also requires `tty-reader`.

The two scripts are near-duplicates of the same `RemoteKeyboard` module (SerialInterface/Keyboard/BrotherPTouchHomeAndHobby classes); bug fixes must be applied to both. Serial writes are paced at 10 bits per byte (8N1) plus `@write_delay`; the serial read loops block for 20 ms per iteration to avoid spinning.

## Configuration Changes

To modify the matrix dimensions or pin assignments:

1. Update `N_ROWS` and `N_COLUMNS` in `remoteKeyboard.h`
2. Adjust `PB_ROW_MASK`, `PC_ROW_MASK`, `PD_ROW_MASK` for new row pins
3. Adjust `PB_COL_MASK`, `PC_COL_MASK`, `PD_COL_MASK` for new column pins
4. Update the `*_TO_ROW()` and `*_TO_COL()` bit-shifting macros to map port bits to logical positions
5. Rebuild with `make clean && make`

The current configuration is optimized for Arduino Pro Mini form factor.

## Important Implementation Details

- The firmware dynamically detects whether the matrix is active-high or active-low by counting set bits
- Row pins are tristated (DDR=0) when not being driven to avoid conflicts
- The `countBits()` function uses a PROGMEM lookup table to efficiently count active columns and find the active column number
- **ISRs must never call `uart_putc()`** - it busy-waits when the TX buffer fills, and with interrupts off inside an ISR the buffer can never drain (permanent hang). ISRs queue events via `queueKeyEvent()`; all UART transmission happens in the main loop, which also keeps `uart_putc()` single-context (it is not reentrant)
- `readRowStates()` leaves all row pins tristated with pull-ups off (it must NOT restore the saved DDR - that re-drives rows forced for the previous column onto the newly strobed column); the PCINT ISR re-asserts forced rows afterwards
- Only the `PCINT1_vect` (port C) column ISR exists; a `#error` guard in remoteKeyboard.c fires if `PB_COL_MASK`/`PD_COL_MASK` become nonzero without adding PCINT0/PCINT2 ISRs
- `uartlibrary/` is a vendored Fleury UART library with local modifications (added `uart_available()`, modernized ISR vector names to `USART_RX_vect`/`USART_UDRE_vect`); do not re-sync it with upstream without preserving these
- I want to re-implement this code in MicroPython and also fix any bugs.
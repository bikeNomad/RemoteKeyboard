// $Id: remoteKeyboard.h 12 2009-05-20 12:46:11Z ned $
#ifndef INCLUDED_REMOTE_KEYBOARD_H
#define INCLUDED_REMOTE_KEYBOARD_H

// if DECODE_KEYS is defined, keys will be decoded instead of rxx and pxx
// codes being sent. 
//
// #define DECODE_KEYS

#include <avr/io.h>
#include <stdint.h>
#include <avr/interrupt.h>
#ifdef DECODE_KEYS
#include <avr/eeprom.h>
#endif
#include <avr/pgmspace.h>
#include <util/delay.h>
#include <avr/sleep.h>
#include "uartlibrary/uart.h"

#if !defined(BAUD)
#   define BAUD 9600
#endif

#if defined(__AVR_ATmega48__) ||\
	defined(__AVR_ATmega88__) ||\
	defined(__AVR_ATmega168__) ||\
	defined(__AVR_ATmega48P__) ||\
	defined(__AVR_ATmega88P__) ||\
	defined(__AVR_ATmega168P__)
#   define MEGA_X8

// Processor: ATMega48/88/168 (also Arduino)
// 28 pins DIP (or 32 pins in QFP)
// PB7:0
// 	PCINT7:0
// 	PB5:2 SPI, ISP
// 	PB5 (SCK) is LED on Arduino
// 	PB3:1 PWM
// PC5:0
//  also analog inputs
// 	PCINT15:8
// 	PC5:4 I2C
// PC6/RESET (also debugWIRE)
// PD7:0
// 	PD1:0 serial
// 	PCINT23:16
//
// PD7:2 (6)
// PC5:4 (2) (also I2C)
// PC3:0 (4) (also analog inputs)
// PB7:6 (2)
// PB5:3 (3) (also SPI) (PB5 has LED)
// PB2:0 (3)
//
// PCINT  22221111 111110000000000
//        32109876 432109876543210
//        ========================
// PORT   D       C       B
//        765432ss-r54321076*43210
//        ========================
//        DDDDDDDD  AAAAAA  DDDDDD
//        000000000000000000111100
// ARD.   76543210  543210  321098
//        ========================
//
// Arduino notes:
// Digital I/O pins D2-D12 (11), A0-A5 (6) available
// D13 is SCK/LED
// D0, D1 are serial
#   define PB_OTHER_OUTPUTS     (_BV(PB5) | _BV(PB4) | _BV(PB3))
#   define PB_OUTPUT_INIT       0
#   define PC_OTHER_OUTPUTS     0
#   define PC_OUTPUT_INIT       0
#   define PD_OTHER_OUTPUTS     0
#   define PD_OUTPUT_INIT       _BV(PD1)
//
// Row (output) pins (8):
// PB1:0 (2), PD7:2 (6)
#   define N_ROWS 8
#   define LOG2_N_ROWS 3
#	define UNUSED_ROWS_MASK		((0xFF<<N_ROWS)&0xFF)

#   define PB_ROW_MASK          0x03
#   define PB_TO_ROW(val)       (((val)&PB_ROW_MASK)<<6)
#   define PB_FROM_ROW(val)     ((val)>>6)

#   define PC_ROW_MASK          0x00
#   define PC_TO_ROW(val)
#   define PC_FROM_ROW(val)

#   define PD_ROW_MASK          0xFC
#   define PD_TO_ROW(val)       ((val)>>2)
#   define PD_FROM_ROW(val)     ((val)<<2)

#define concat(a,b) a##b
#define PORTx(letter)       concat(PORT,letter)
#define PINx(letter)        concat(PIN,letter)
#define DDRx(letter)        concat(DDR,letter)

// Aux output pins (1): PB2
// (arduino D10)
#   define N_AUX_OUTPUTS        1

#   define AUX0_PORT            B
#   define AUX0_BIT             2
#   define AUX0_ON_STATE        0

#ifdef AUX0_PORT
#   define AUX0_MASK            _BV(AUX0_BIT)
#   define AUX0_PORTREG         PORTx(AUX0_PORT)
#   define AUX0_PINREG          PINx(AUX0_PORT)
#   define AUX0_DDREG           DDRx(AUX0_PORT)
#endif

// Column (input) pins (6):
// PC5:0
// Arduino A5-A0
// 	PCINT13:8
#   define N_COLUMNS 6
//  actually LOG2(N_COLUMNS+1)
#   define LOG2_N_COLUMNS 3

#	define UNUSED_COLUMNS_MASK	((0xFF<<N_COLUMNS)&0xFF)

#   define PB_COL_MASK          0x00
#   define PB_TO_COL(val)
#   define PB_FROM_COL(val)

#   define PC_COL_MASK          0x3F
#   define PC_TO_COL(val)       ((val)&PC_COL_MASK)
#   define PC_FROM_COL(val)     ((val)&PC_COL_MASK)

#   define PD_COL_MASK          0x00
#   define PD_TO_COL(val)
#   define PD_FROM_COL(val)

#endif  /* atmegax8, atmegax8p */

#if N_ROWS > 8
    typedef uint16_t row_mask_t;
#else
    typedef uint8_t row_mask_t;
#endif

#if N_COLUMNS+1 > 8
    typedef uint16_t column_mask_t;
#else
    typedef uint8_t column_mask_t;
#endif

#if N_ROWS > 8 || N_COLUMNS+1 > 8
    typedef uint16_t mask_t;
#else
    typedef uint8_t mask_t;
#endif

#if defined(DECODE_KEYS)
#   define EEPROM_VAR __attribute__ ((section (".eeprom")))
#   if LOG2_N_ROWS + LOG2_N_COLUMNS > 8
        typedef uint16_t index_t;
#   else
        typedef uint8_t index_t;
#   endif

typedef enum
{
    // 00 regular key (6 bits holds row/column)
    RegularKey = 0,
    // 01 shifted key (only one shift key, though)
    ShiftedKey = 1,
    // 10 sticky key
    StickyKey = 2,
    // 11 key macro
    MacroKey = 3
} KeyType;

typedef struct
{
    index_t rowIndex: LOG2_N_COLUMNS;
    index_t columnIndex: LOG2_N_ROWS;
} KeyLocation;

typedef struct
{
    KeyType keyType: 2; // type of following 6 bit record:
    union
    {
        // for RegularKey, ShiftedKey, StickyKey
        KeyLocation keyLocation;

        // for MacroKey
        uint8_t macroIndex: 6;
    };

} KeyDefinition;

#   define N_SPECIALS 4

// EEPROM storage:
// row major, column minor storage of (non-special) codes
// to be sent over serial port.
// NOTE that 0x00 is never sent (a 00 entry may be a special)
extern const uint8_t EEPROM_VAR codes[N_ROWS * (N_COLUMNS+1)];

// key pressed =>
//    code = codes[ row*N_COLUMNS + column ]
//      if (code is 0)
//          look up code in specials
//              if found
//                  send code or do appropriate action
//      else (code non-0)
//          send code over UART
//              after modifying shift state
//      endif
//
// key released => 
//    code = codes[ row*N_COLUMNS + column ]
//      if (code is 0)
//          look up code in specials
//              if found and is shift key
//                  release shift state
//      endif
//
// character received => (momentary press)
//   see if shift state has changed.
//      if so, reset shift state
//   search for code in codes[]

// indexed storage of special keys
extern const KeyDefinition EEPROM_VAR specialKeys[N_SPECIALS];

#endif  /* defined(DECODE_KEYS) */

// RAM storage:
// bitmap for which switches are forced
// last column is for aux (non-matrix) switches
extern volatile row_mask_t forcedSwitches[N_COLUMNS+1];   // bitmap for which switches are forced
extern volatile row_mask_t activeSwitches[N_COLUMNS+1];   // user pressed keys
extern volatile row_mask_t changedSwitches[N_COLUMNS+1];  // user pressed keys (edge detect)

// number of Timer0 interrupt periods that each bit in ledPattern is worth
#define LED_BIT_TICKS 4

// Arduino Mini Pro: LED on PB5 pin
#define LED_ON      PORTB |= _BV(PB5)
#define LED_OFF     PORTB &= ~_BV(PB5)

typedef enum
{
    SERIAL_CMD_OK,
    SERIAL_CMD_INCOMPLETE,
    SERIAL_CMD_ERROR
} SerialCommandState;

extern void setLEDPattern(uint8_t pattern);
// extern void pressSwitch(uint8_t row, uint8_t column);
// extern void releaseSwitch(uint8_t row, uint8_t column);

#endif  /* INCLUDED_REMOTE_KEYBOARD_H */

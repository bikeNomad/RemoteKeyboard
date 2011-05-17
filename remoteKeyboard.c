// $Id: remoteKeyboard.c 13 2009-05-20 13:32:39Z ned $

#include "remoteKeyboard.h"
#include <avr/io.h>

#if 0
FUSES =
{
    // 
    .low = LFUSE_DEFAULT,
    // enable SPI (default) as well as saving EEPROM over chip erase and 4.5V BOD level
    .high = (/*FUSE_DWEN & */ FUSE_SPIEN & FUSE_EESAVE & FUSE_BODLEVEL1 & FUSE_BODLEVEL0),
    // no reset vector
    .extended = EFUSE_DEFAULT
};
#endif

#ifdef DECODE_KEYS

// EEPROM storage:
// codes 
const uint8_t EEPROM_VAR codes[(N_COLUMNS+1) * N_ROWS];

// extra keys
const KeyDefinition EEPROM_VAR specialKeys[N_SPECIALS];

#endif

// RAM storage:
// bitmap for which switches are forced
         row_mask_t forcedSwitches[N_COLUMNS+1];
volatile row_mask_t activeSwitches[N_COLUMNS+1];          // first time we notice a switch change
volatile row_mask_t priorActiveSwitches[N_COLUMNS+1];     // second time

// DEBUG
column_mask_t seenColumnsHigh;
column_mask_t seenColumnsLow = 0xFF;
row_mask_t seenRowsHigh;
row_mask_t seenRowsLow = 0xFF;

// LED flashing: bits shifted out from LS bit; 1 = LED ON 
// each bit is worth  LED_BIT_TICKS
volatile uint8_t ledPattern;

// read column inputs, return raw value
// CALLED FROM ISR
static column_mask_t readColumnInputs(void)
{
    uint8_t retval = 0;

#if PB_COL_MASK != 0
    retval |= PB_TO_COL(PINB);
#endif
#if PC_COL_MASK != 0
    retval |= PC_TO_COL(PINC);
#endif
#if PD_COL_MASK != 0
    retval |= PD_TO_COL(PIND);
#endif

    seenColumnsHigh |= retval;      // DEBUG
    seenColumnsLow &= retval;

    return retval;
}

// read row inputs, return raw value
// CALLED FROM ISR
static row_mask_t readRowStates(void)
{
    uint8_t oldDDR;
    uint8_t retval = 0;

#if PB_ROW_MASK != 0
    oldDDR = DDRB;              // save direction
    DDRB = oldDDR & ~PB_ROW_MASK;   // reset DDR for inputs
    retval |= PB_TO_ROW(PINB);  // and read current values
    DDRB = oldDDR;              // restore direction
#endif
#if PC_ROW_MASK != 0
    oldDDR = DDRC;              // save direction
    DDRC = oldDDR & ~PC_ROW_MASK;   // reset DDR for inputs
    retval |= PC_TO_ROW(PINC);  // and read current values
    DDRC = oldDDR;              // restore direction
#endif
#if PD_ROW_MASK != 0
    oldDDR = DDRD;              // save direction
    DDRD = oldDDR & ~PD_ROW_MASK;   // reset DDR for inputs
    retval |= PD_TO_ROW(PIND);  // and read current values
    DDRD = oldDDR;              // restore direction
#endif

    seenRowsHigh |= retval;     // DEBUG
    seenRowsLow &= retval;

    return retval;
}

// CALLED FROM ISR
static void processRowInputs(row_mask_t rowInputs, uint8_t activeColumn)
{
    // priorActive active rowInputs ch ch2 valid
    // 0 0 x x 0 0 
    // 0 1 0 1 1 0 (glitch)
    // 0 1 1 0 1 1 pressed
    // 1 0 0 0 1 1 released
    // 1 0 1 1 1 0 (glitch)
    // 1 1 x x 0 0 
    // 
    // changed = active ^ rowInputs
    // changed2 = priorActive ^ active
    // valid = changed2 & ~changed
    // then:
    // priorActive = active
    // active = rowInputs
    // 
    // compare with last samples for this column to find changed ones (1 == changed)
    row_mask_t changed = activeSwitches[activeColumn] ^ rowInputs;
    // compare last with prior to detect glitches (1 == changed last time)
    row_mask_t changed2 =
        activeSwitches[activeColumn] ^ priorActiveSwitches[activeColumn];
    // valid ones are ones that have just changed but were stable before that
    // for at
    // least one sample.
    row_mask_t valid = changed2 & ~changed;

    row_mask_t mask = 1;
    for (uint8_t rowBitNum = 0; rowBitNum < N_ROWS; rowBitNum++, mask <<= 1)
    {
        // for each row input with a valid change
        if (valid & mask)
        {
            uint8_t state = (rowInputs & mask) ? 'p' : 'r'; // pressed/released
#ifndef DECODE_KEYS
            // send out p00 or r00 type codes
            uart_putc(state);
            uart_putc(rowBitNum + '0');
            uart_putc(activeColumn + '0');
#else
            // decode keys from EEPROM settings
            index_t index = activeColumn * N_ROWS + rowBitNum;
            uint8_t keyCode = eeprom_read_byte(&codes[index]);
            uart_putc(keyCode); // send keyCode over serial link
#endif
        }
    }
    priorActiveSwitches[activeColumn] = activeSwitches[activeColumn];
    activeSwitches[activeColumn] = rowInputs;
}

// read individual AUX switch states as if they were row inputs
// CALLED FROM ISR
static row_mask_t readAuxSwitchStates(void)
{
    uint8_t oldDDR, bit;
    row_mask_t retval = 0;

#if N_AUX_OUTPUTS >= 1
    oldDDR = AUX0_DDREG;
    AUX0_DDREG &= ~AUX0_MASK;    // set as input
    bit = AUX0_PINREG & AUX0_MASK;          // read input
    if (!AUX0_ON_STATE)         // invert if active low
        bit ^= AUX0_MASK;
    retval |= bit;
    AUX0_DDREG = oldDDR;
#endif

    return retval;
}

// set row outputs corresponding to bits in mask as outputs
// polarity=1: set PORTx bits in mask
// polarity=0: reset PORTx bits in mask
// to set all to inputs pass a 0
// leaves DDRx set
// CALLED FROM ISR
static void assertRowOutputs(row_mask_t mask, row_mask_t polarity)
{
    uint8_t bits;

#if PB_ROW_MASK != 0
    bits = PB_FROM_ROW(mask);
    if (bits)
    {
        DDRB |= bits;
        if (polarity)
            PORTB |= bits;
        else
            PORTB &= ~bits;
    }
#endif
#if PC_ROW_MASK != 0
    bits = PC_FROM_ROW(mask);
    if (bits)
    {
        DDRC |= bits;
        if (polarity)
            PORTC |= bits;
        else
            PORTC &= ~bits;
    }
#endif
#if PD_ROW_MASK != 0
    bits = PD_FROM_ROW(mask);
    if (bits)
    {
        DDRD |= bits;
        if (polarity)
            PORTD |= bits;
        else
            PORTD &= ~bits;
    }
#endif
}

// set row outputs corresponding to bits in mask as outputs
// and set those outputs to their ON state
// to set all to inputs pass a 0
// leaves DDRx bits set
// CALLED FROM ISR
static void assertAuxOutputs(row_mask_t mask)
{
#if N_AUX_OUTPUTS >= 1
    if (mask & 1)
    {
        if (AUX0_ON_STATE)
            AUX0_PORTREG |= AUX0_MASK;
        else
            AUX0_PORTREG &= ~AUX0_MASK;
        AUX0_DDREG |= AUX0_MASK;    // set as output
    }
    else
    {
        AUX0_PORTREG &= ~AUX0_MASK; // ensure pullups off
        AUX0_DDREG &= ~AUX0_MASK;   // set as input
    }
#endif
}

// CALLED FROM ISR
static uint8_t countBits(uint8_t number, uint8_t *lastBitnumSet)
{
    uint8_t numberSet = 0;
    uint8_t bitNum = 0;

    for (uint8_t mask = 1; mask; mask <<= 1, bitNum++)
    {
        if (number & mask)
        {
            numberSet++;
            *lastBitnumSet = bitNum;
        }
    }

    return numberSet;
}

// 30.5 Hz periodic interrupt
ISR(TIMER0_OVF_vect)
{
    // read aux switches
    processRowInputs(readAuxSwitchStates(), N_COLUMNS);
    // handle forced aux switches
    assertAuxOutputs(forcedSwitches[N_COLUMNS]);

    static uint8_t ledTicksRemaining;
    static uint8_t lastLedPattern;

    // new pattern?
    if (lastLedPattern != ledPattern)
        ledTicksRemaining = 1;

    if (!ledTicksRemaining)
        return;

    if (--ledTicksRemaining)
        return;

    // 1 => 0 transition: time to do something
    if (ledPattern & 1)
        LED_ON;
    else
        LED_OFF;

    if (ledPattern != 0)
    {
        ledTicksRemaining = LED_BIT_TICKS;
        ledPattern >>= 1;
    }

    lastLedPattern = ledPattern;
}

// pin change interrupt vector PCINT1
// triggered by any logic change on enabled PCINTxx pins (inputs from host column strobe pins)
// TODO debounce over entire period of strobe (or more).
ISR(PCINT1_vect)
{
    // wait for things to settle
    _delay_us(10);

    // get the column inputs (at least one of which has just changed)
    column_mask_t columnInputs = readColumnInputs();

    // set all row outputs as inputs
    assertRowOutputs(0, 0);

    static column_mask_t lastColumnInputs;
    static mask_t quiescentState;

  again:
    columnInputs ^= quiescentState; // invert if necessary

    uint8_t activeColumn = 0;
    uint8_t nSetBits = countBits(columnInputs, &activeColumn);

    switch (nSetBits)
    {
        case 1:
            {
                // read the row inputs and convert to logical levels (1 == active)
                row_mask_t rowInputs = readRowStates() ^ quiescentState;
                // handle transitions and report on changes
                processRowInputs(rowInputs, activeColumn);
                // now force any switches that we're forcing
                assertRowOutputs(forcedSwitches[activeColumn], quiescentState);
            }
            break;

        case 7:                // quiescent state wrong; one active
            columnInputs ^= quiescentState; // restore flipped bits
            quiescentState = ~quiescentState;
            goto again;
            break;

        case 8:                // quiescent state wrong; nothing active
            quiescentState = ~quiescentState;
            break;

        case 0:                // no active column lines
            break;
    }

    // remember last column scan
    lastColumnInputs = columnInputs;
}

// print byte as 2 hex chars
static void printHexByte(uint8_t val)
{
    uint8_t v = (val & 0xF0) >> 4;
    v = (v > 0x09) ? ('A' - 0x0a + v) : ('0' + v);
    uart_putc(v);
    v = val & 0x0F;
    v = (v > 0x09) ? ('A' - 0x0a + v) : ('0' + v);
    uart_putc(v);
}

// debug: dump state of interrupt handler observations
static void dumpState(void)
{
    uart_puts_P("\r\nchi: ");
    printHexByte(seenColumnsHigh);
    uart_puts_P(" clo: ");
    printHexByte(seenColumnsLow);
    uart_puts_P("\r\nrhi: ");
    printHexByte(seenRowsHigh);
    uart_puts_P(" rlo: ");
    printHexByte(seenRowsLow);
    uart_puts_P("\r\nCo Fo Ac Pr\r\n");
    for (uint8_t i = 0; i <= N_COLUMNS; i++)
    {
        printHexByte(i);
        uart_putc(' ');
        printHexByte(forcedSwitches[i]);
        uart_putc(' ');
        printHexByte(activeSwitches[i]);
        uart_putc(' ');
        printHexByte(priorActiveSwitches[i]);
        uart_puts_P("\r\n");
    }
}

void setLEDPattern(uint8_t pattern)
{
    ledPattern = pattern;
}

static void pressSwitch(uint8_t row, uint8_t column)
{
    forcedSwitches[column] |= (1 << row);
}

static void releaseSwitch(uint8_t row, uint8_t column)
{
    forcedSwitches[column] &= ~(1 << row);
}

// Set or clear bit in forcedSwitches[] in response to command string
// Returns 0 if OK, else error
// Handles 3-character strings (p00 or r00 style) from serial port
// 1st character: 'p' == press; 'r' == release
// 2nd character: row, 0 .. (N_ROWS-1)
//      aux. switch I/O starts at row 0
// 3rd character: column
//      0 .. (N_COLUMNS-1) are matrix switches
//      aux. switch I/O is at column (N_COLUMNS)
// so (if N_COLUMNS==6):
//      p00     press row 0, column 0
//      p05     press row 0, column 5
//      p06     press aux 0
static uint8_t doPressOrReleaseRC(char *command)
{
    uint8_t row = command[1];
    if (row >= '0' && row < '0' + N_ROWS)
        row -= '0';
    else
        goto error;

    uint8_t column = command[2];
    if (column >= '0' && column <= '0' + N_COLUMNS)
        column -= '0';
    else
        goto error;

    if (command[0] == 'p')
        pressSwitch(row, column);
    else if (command[0] == 'r')
        releaseSwitch(row, column);
    else
        goto error;

    return 0;

  error:
    return 1;
}

// check for serial command
// and process it if it's complete
// Returns SERIAL_CMD_OK for complete and recognized command
// SERIAL_CMD_INCOMPLETE for incomplete (<4 chars) or
// SERIAL_CMD_ERROR for anything else
// Serial commands:
// p00 or r00 type codes
// pRC\r press row R, column C
// rRC\r release row R, column C
static SerialCommandState processSerialCommand(void)
{
    static char command[8];
    static uint8_t bytesReceived;

    // while there's still data to read
    // and no errors
    while ((bytesReceived < sizeof (command) - 1))
    {
        unsigned int c = uart_getc();
        if ((c & 0xFF00) == 0)
        {
            command[bytesReceived++] = (uint8_t)c;
        }
        else
        {
            // serial error or no data?
            if ((c & 0xFF00) == UART_NO_DATA)
                return SERIAL_CMD_INCOMPLETE;
            else
                return SERIAL_CMD_ERROR;
        }
#ifndef DECODE_KEYS
        if (c == '\r')
        {
            SerialCommandState retval = SERIAL_CMD_ERROR;
            if (bytesReceived == 2 && command[0] == 'R')
            {
                goto *0;    // reset
            }
            if (bytesReceived == 4)
            {
                command[3] = '\0';  // mark end of string
                if (doPressOrReleaseRC(command) == 0)
                    retval = SERIAL_CMD_OK; // on no error
                else
                {
                    uart_puts(command);
                    uart_puts_P("?\r\n");
                }
            }
            bytesReceived = 0;  // reset counter
            return retval;
        }
#else /* DECODE_KEYS */
        // search for code in specialKeys[]
        for (KeyDefinition * special = specialKeys;
             special < specialKeys + N_SPECIALS; special++)
        {
            // special->keyLocation.rowIndex
            // special->keyLocation.columnIndex
        }

        // search for code in codes[]
        for (index_t i = codes; i < sizeof (codes); i++)
        {
            if (c == codes[i])
            {
                // found it
            }
        }
#endif
    }
    // here when buffer is full
    bytesReceived = 0;
    return SERIAL_CMD_ERROR;
}

static void initIO(void)
{
    // init IO
    // set unused pins as outputs pulled low
    PORTB = PB_OUTPUT_INIT;
    DDRB = PB_OTHER_OUTPUTS;

    PORTC = PC_OUTPUT_INIT;
    DDRC = PC_OTHER_OUTPUTS;

    PORTD = PD_OUTPUT_INIT;
    DDRD = PD_OTHER_OUTPUTS;
}

static void initTimers(void)
{
    // init 8-bit Timer0 for periodic interrupts
    // max period = 30.5 Hz with 8 MHz clock and clk/1024 prescaler
    // min period = 31250 Hz with 8 MHz clock and clk/1 prescaler
    TCCR0A = 0;
    TCCR0B = 5 << CS00; // clk/1024 = 30.5 Hz
    TIMSK0 = _BV(TOIE0);
}

static void initPCInterrupts(void)
{
    // init pin-change interrupts
#if PB_COL_MASK != 0
    PCMSK0 = PB_COL_MASK;
    PCICR |= _BV(PCIE0);
#endif
#if PC_COL_MASK != 0
    PCMSK1 = PC_COL_MASK;
    PCICR |= _BV(PCIE1);
#endif
#if PD_COL_MASK != 0
    PCMSK2 = PD_COL_MASK;
    PCICR |= _BV(PCIE2);
#endif
}

int main(void)
{
    initIO();
    initTimers();
    initPCInterrupts();
    uart_init(UART_BAUD_SELECT(BAUD, F_CPU));

    sei();                      // enable IRQ globally

    ledPattern = 0x55;

    // debug: print wakeup message
    uart_puts_P("Hi there $Rev: 27 $\r\n");

    // main loop: process serial commands and go to sleep
    for (;;)
    {
        SerialCommandState err = processSerialCommand();
        if (err == SERIAL_CMD_ERROR)
        {
            dumpState();
            _delay_ms(1000);
        }
        // nothing else to do: go to sleep
        // set_sleep_mode(SLEEP_MODE_IDLE);
        // sleep_mode();
    }
}

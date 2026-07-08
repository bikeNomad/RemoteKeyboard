// $Id: remoteKeyboard.c 13 2009-05-20 13:32:39Z ned $

#include "remoteKeyboard.h"
#include <avr/io.h>

#if 0
FUSES =
{
    //
    .low      = LFUSE_DEFAULT,
    // enable SPI (default) as well as saving EEPROM over chip erase and 4.5V BOD level
    .high     = (/*FUSE_DWEN & */ FUSE_SPIEN & FUSE_EESAVE & FUSE_BODLEVEL1 & FUSE_BODLEVEL0),
    // no reset vector
    .extended = EFUSE_DEFAULT
};
#endif

// forward declarations
static column_mask_t readColumnInputs(void);
static row_mask_t readRowStates(void);
static void queueKeyEvent(uint8_t pressed, uint8_t row, uint8_t column);
static void sendQueuedEvents(void);
static void processRowInputs(row_mask_t rowInputs, uint8_t activeColumn);
static row_mask_t readAuxSwitchStates(void);
static void assertRowOutputs(row_mask_t mask, row_mask_t quiescent);
static void assertAuxOutputs(row_mask_t mask);
static uint8_t countBits(uint8_t number, uint8_t *lastBitnumSet);
static void printHexByte(uint8_t );
static void dumpState(void);
static uint8_t doPressOrReleaseRC(char *command);
static SerialCommandState processSerialCommand(void);
static void initIO(void);
static void initTimers(void);
static void initPCInterrupts(void);

// RAM storage:
// bitmap for which switches are forced
volatile row_mask_t forcedSwitches[N_COLUMNS+1];
volatile row_mask_t activeSwitches[N_COLUMNS+1]; // first time we notice a switch change
volatile row_mask_t priorActiveSwitches[N_COLUMNS+1]; // second time
volatile row_mask_t reportedSwitches[N_COLUMNS+1]; // last state reported to host

// Key events queued by the ISRs and transmitted from the main loop.
// uart_putc() busy-waits when the TX buffer fills; with interrupts off
// inside an ISR that wait can never end, so ISRs must not transmit.
#define EVENT_QUEUE_SIZE 64            // power of 2
#define EVENT_QUEUE_MASK (EVENT_QUEUE_SIZE - 1)
#define EVENT_PRESS      0x40          // bit 6; row in bits 5:3, column in bits 2:0

static volatile uint8_t eventQueue[EVENT_QUEUE_SIZE];
static volatile uint8_t eventHead;     // written only by ISRs
static volatile uint8_t eventTail;     // written only by main loop
static volatile uint8_t eventOverflows; // events dropped because queue was full

// DEBUG
volatile column_mask_t seenColumnsHigh = 0;
volatile column_mask_t seenColumnsLow = 0xFF;
volatile row_mask_t seenRowsHigh = 0;
volatile row_mask_t seenRowsLow       = 0xFF;

volatile uint16_t columnStrobes[N_COLUMNS+1];

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

    seenColumnsHigh |= retval;         // DEBUG
    seenColumnsHigh |= UNUSED_COLUMNS_MASK;
    seenColumnsLow  &= retval;
    seenColumnsLow  &= ~UNUSED_COLUMNS_MASK;

    return retval;
}

// read row inputs, return raw value
// Leaves all row pins tristated with pull-ups off; the caller re-asserts
// any forced rows for the currently active column afterwards. Restoring
// the previous DDR here would re-drive rows that were forced for the
// previously active column onto the column now being strobed, and a
// leftover PORT bit would enable a pull-up that biases the read.
// CALLED FROM ISR
static row_mask_t readRowStates(void)
{
    uint8_t retval = 0;

#if PB_ROW_MASK != 0
    DDRB  &= ~PB_ROW_MASK;             // tristate
    PORTB &= ~PB_ROW_MASK;             // pull-ups off
#endif
#if PC_ROW_MASK != 0
    DDRC  &= ~PC_ROW_MASK;             // tristate
    PORTC &= ~PC_ROW_MASK;             // pull-ups off
#endif
#if PD_ROW_MASK != 0
    DDRD  &= ~PD_ROW_MASK;             // tristate
    PORTD &= ~PD_ROW_MASK;             // pull-ups off
#endif

    _delay_us(10);

#if PB_ROW_MASK != 0
    retval |= PB_TO_ROW(PINB);         // read current values
#endif
#if PC_ROW_MASK != 0
    retval |= PC_TO_ROW(PINC);         // read current values
#endif
#if PD_ROW_MASK != 0
    retval |= PD_TO_ROW(PIND);         // read current values
#endif

    seenRowsHigh |= retval;            // DEBUG
    seenRowsHigh |= UNUSED_ROWS_MASK;
    seenRowsLow  &= retval;
    seenRowsLow  &= ~UNUSED_ROWS_MASK;

    return retval;
}

// append a key event for the main loop to transmit
// CALLED FROM ISR
static void queueKeyEvent(uint8_t pressed, uint8_t row, uint8_t column)
{
    uint8_t next = (eventHead + 1) & EVENT_QUEUE_MASK;
    if (next == eventTail)
    {
        eventOverflows++;              // full; drop event
        return;
    }
    eventQueue[eventHead] = (pressed ? EVENT_PRESS : 0) | (row << 3) | column;
    eventHead = next;
}

// transmit queued key events as p00 or r00 type codes
// CALLED FROM MAIN LOOP ONLY
static void sendQueuedEvents(void)
{
    while (eventTail != eventHead)
    {
        uint8_t ev = eventQueue[eventTail];
        eventTail  = (eventTail + 1) & EVENT_QUEUE_MASK;
        uart_putc((ev & EVENT_PRESS) ? 'p' : 'r');
        uart_putc(((ev >> 3) & 0x07) + '0');
        uart_putc((ev & 0x07) + '0');
        uart_putc('\r');
        uart_putc('\n');
    }
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
    row_mask_t changed2
        =activeSwitches[activeColumn] ^ priorActiveSwitches[activeColumn];
    // valid ones are ones that have just changed but were stable before that
    // for at least one sample.
    row_mask_t valid = changed2 & ~changed;

    // Of those, only report switches whose debounced state differs from the
    // last state reported to the host; without this, a one-sample glitch is
    // suppressed on entry but emits an unmatched event when it returns to
    // the old state. Never report switches we are forcing ourselves: their
    // inputs can read back our own drive.
    row_mask_t report = valid
        & (reportedSwitches[activeColumn] ^ rowInputs)
        & ~forcedSwitches[activeColumn];

    row_mask_t mask  = 1;
    for (uint8_t rowBitNum = 0; rowBitNum < N_ROWS; rowBitNum++, mask <<= 1)
    {
        if (report & mask)
        {
            queueKeyEvent(rowInputs & mask, rowBitNum, activeColumn);
        }
    }
    reportedSwitches[activeColumn]
        = (reportedSwitches[activeColumn] & ~report) | (rowInputs & report);
    priorActiveSwitches[activeColumn] = activeSwitches[activeColumn];
    activeSwitches[activeColumn]      = rowInputs;
}

// read individual AUX switch states as if they were row inputs
// CALLED FROM ISR
static row_mask_t readAuxSwitchStates(void)
{
    uint8_t bit;
    row_mask_t retval = 0;

#if N_AUX_OUTPUTS >= 1
    uint8_t oldDDR0      = AUX0_DDREG;
    AUX0_DDREG &= ~AUX0_MASK;          // set as input
#endif
#if N_AUX_OUTPUTS >= 2
    uint8_t oldDDR1      = AUX1_DDREG;
    AUX1_DDREG &= ~AUX1_MASK;          // set as input
#endif

    _delay_us(10);

#if N_AUX_OUTPUTS >= 1
    bit         = AUX0_PINREG & AUX0_MASK; // read input
    if (!AUX0_ON_STATE)                // invert if active low
    {
        bit ^= AUX0_MASK;
    }
    AUX0_DDREG = oldDDR0;
    if (bit)
    {
        retval = 0x01;
    }
#endif
#if N_AUX_OUTPUTS >= 2
    bit         = AUX1_PINREG & AUX1_MASK; // read input
    if (!AUX1_ON_STATE)                // invert if active low
    {
        bit ^= AUX1_MASK;
    }
    AUX1_DDREG = oldDDR1;
    if (bit)
    {
        retval |= 0x02;
    }
#endif

    return retval;
}

// set row outputs corresponding to bits in mask as outputs
// quiescent=0: set PORTx bits in mask
// quiescent=1: reset PORTx bits in mask
// to set all to inputs pass a 0
// leaves DDRx set
// CALLED FROM ISR
static void assertRowOutputs(row_mask_t mask, row_mask_t quiescent)
{
    uint8_t bits;

#if PB_ROW_MASK != 0
    bits = PB_FROM_ROW(mask);
    DDRB  &= ~PB_ROW_MASK;         // reset to inputs
    PORTB &= ~PB_ROW_MASK;         // and don't pull up
    if (bits)
    {
        DDRB |= bits;                  // turn into outputs
        if (quiescent)
        {
            PORTB &= ~bits;
        }
        else
        {
            PORTB |= bits;
        }
    }
#endif
#if PC_ROW_MASK != 0
    bits = PC_FROM_ROW(mask);
    DDRC  &= ~PC_ROW_MASK;         // reset to inputs
    PORTC &= ~PC_ROW_MASK;         // and don't pull up
    if (bits)
    {
        DDRC |= bits;
        if (quiescent)
        {
            PORTC &= ~bits;
        }
        else
        {
            PORTC |= bits;
        }
    }
#endif
#if PD_ROW_MASK != 0
    bits = PD_FROM_ROW(mask);
    DDRD  &= ~PD_ROW_MASK;         // reset to inputs
    PORTD &= ~PD_ROW_MASK;         // and don't pull up
    if (bits)
    {
        DDRD |= bits;
        if (quiescent)
        {
            PORTD &= ~bits;
        }
        else
        {
            PORTD |= bits;
        }
    }
#endif
}

// set row outputs corresponding to bits in mask as outputs
// and set those outputs to their ON state
// to set all to inputs pass a 0
// leaves DDRx bits set
// CALLED FROM timer tick ISR
static void assertAuxOutputs(row_mask_t mask)
{
#if N_AUX_OUTPUTS >= 1
    if (mask & 1)
    {
        if (AUX0_ON_STATE)
        {
            AUX0_PORTREG |= AUX0_MASK;
        }
        else
        {
            AUX0_PORTREG &= ~AUX0_MASK;
        }
        AUX0_DDREG |= AUX0_MASK;       // set as output
    }
    else
    {
        AUX0_PORTREG &= ~AUX0_MASK; // ensure pullups off
        AUX0_DDREG   &= ~AUX0_MASK;    // set as input
    }
#endif
#if N_AUX_OUTPUTS >= 2
    if (mask & 2)
    {
        if (AUX1_ON_STATE)
        {
            AUX1_PORTREG |= AUX1_MASK;
        }
        else
        {
            AUX1_PORTREG &= ~AUX1_MASK;
        }
        AUX1_DDREG |= AUX1_MASK;       // set as output
    }
    else
    {
        AUX1_PORTREG &= ~AUX1_MASK; // ensure pullups off
        AUX1_DDREG   &= ~AUX1_MASK;    // set as input
    }
#endif
}

// CALLED FROM ISR
static uint8_t countBits(uint8_t number, uint8_t *lastBitnumSet)
{
    // high nybble: number of bits set; low nybble: highest bit set
    static const uint8_t usedBits[16] PROGMEM = {
        0x00, 0x10, 0x11, 0x21,            // 0 1 2 3
        0x12, 0x22, 0x22, 0x32,            // 4 5 6 7
        0x13, 0x23, 0x23, 0x33,            // 8 9 A B
        0x23, 0x33, 0x33, 0x43,            // C D E F
    };

    uint8_t lo = pgm_read_byte(&usedBits[number & 0x0F]);
    uint8_t hi = pgm_read_byte(&usedBits[number >> 4]);

    uint8_t numberSet  = lo >> 4;
    uint8_t highestBit = lo & 0x0F;

    if (hi >> 4)
    {
        numberSet += hi >> 4;
        highestBit = 4 + (hi & 0x0F);
    }

    if (numberSet)
    {
        *lastBitnumSet = highestBit;
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
}

// pin change interrupt vector PCINT1
// triggered by any logic change on enabled PCINTxx pins (inputs from host column strobe pins)
#if PB_COL_MASK != 0 || PD_COL_MASK != 0
#   error "column inputs on ports B or D need PCINT0/PCINT2 ISRs; only PCINT1 is implemented"
#endif
ISR(PCINT1_vect)
{
    // get the column inputs (at least one of which has just changed)
    column_mask_t columnInputs = readColumnInputs();

    static mask_t quiescentState = 0xFF;    // debug

again:
    columnInputs ^= quiescentState; // invert if necessary
    columnInputs &= ~UNUSED_COLUMNS_MASK; // ignore unused columns

    uint8_t activeColumn = 0;
    uint8_t nSetBits     = countBits(columnInputs, &activeColumn);

    switch (nSetBits)
    {
        case 1:		// single active column line
        {
            // read the row inputs and convert to logical levels (1 == active)
            row_mask_t rowInputs = readRowStates() ^ quiescentState;
            // handle transitions and report on changes
            columnStrobes[activeColumn]++;  // DEBUG
            processRowInputs(rowInputs, activeColumn);
            // now force any switches that we're forcing
            assertRowOutputs(forcedSwitches[activeColumn], quiescentState);
        }
        break;

        case 0:                        // no active column lines
            // set all row outputs as inputs
            assertRowOutputs(0, 0);
            break;

        case N_COLUMNS - 1:            // quiescent state wrong; one active
            columnInputs  ^= quiescentState; // restore flipped bits
            quiescentState = ~quiescentState;
            goto again;
            break;

        case N_COLUMNS:                // quiescent state wrong; nothing active
            quiescentState = ~quiescentState;
            break;
    }
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
    uart_puts_P("\r\nCo Fo Ac Pr Re CSTR\r\n");
    for (uint8_t i = 0; i <= N_COLUMNS; i++)
    {
        // 16-bit counter shared with the ISR; copy and reset atomically
        uint16_t strobes;
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
        {
            strobes          = columnStrobes[i];
            columnStrobes[i] = 0;      // reset count
        }
        printHexByte(i);
        uart_putc(' ');
        printHexByte(forcedSwitches[i]);
        uart_putc(' ');
        printHexByte(activeSwitches[i]);
        uart_putc(' ');
        printHexByte(priorActiveSwitches[i]);
        uart_putc(' ');
        printHexByte(reportedSwitches[i]);
        uart_putc(' ');
        printHexByte(strobes >> 8);
        printHexByte(strobes & 0xFF);
        uart_puts_P("\r\n");
    }
    uart_puts_P("Ov: ");
    printHexByte(eventOverflows);
    uart_puts_P("\r\n");
    eventOverflows = 0;
}

void pressSwitch(uint8_t row, uint8_t column)
{
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
        forcedSwitches[column] |= (1 << row);
    }
}

void releaseSwitch(uint8_t row, uint8_t column)
{
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
        forcedSwitches[column] &= ~(1 << row);
    }
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
    if ((row >= '0') && (row < '0' + N_ROWS))
        row -= '0';
    else
        goto error;
    uint8_t column = command[2];
    if ((column >= '0') && (column <= '0' + N_COLUMNS))
        column -= '0';
    else
        goto error;
    // the aux column only has N_AUX_OUTPUTS switches
    if ((column == N_COLUMNS) && (row >= N_AUX_OUTPUTS))
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
//
// p00 or r00 type codes
// 	pRC\r press row R, column C
// 	rRC\r release row R, column C
//
// R for reset

static SerialCommandState processSerialCommand(void)
{
	static char command[8];
	static uint8_t bytesReceived;

	// while there's still data to read
	// and no errors
	while ((bytesReceived < sizeof(command) - 1))
	{
		unsigned int c = uart_getc();
		if ((c & 0xFF00) == 0)
		{
			command[bytesReceived++] = (uint8_t) c;
		}
		else
		{
			// serial error or no data?
			if ((c & 0xFF00) == UART_NO_DATA)
				return SERIAL_CMD_INCOMPLETE;
			else
				return SERIAL_CMD_ERROR;
		}
		if (c == '\r') // gotten a full command line?
		{
			SerialCommandState retval = SERIAL_CMD_ERROR;

			switch (command[0])
			{
			case 'p': // press
				// fall through
			case 'r': // release
				if (bytesReceived == 4)
				{
					command[3] = '\0'; // mark end of string
					if (doPressOrReleaseRC(command) == 0)
					{
						retval = SERIAL_CMD_OK; // on no error
					}
					else
					{
						uart_puts(command);
						uart_puts_P("?\r\n");
					}
				}
				break;
			case 'R': // reset
				if (bytesReceived == 2)
				{
					// hardware reset via watchdog; a jump to 0 would leave
					// peripherals configured and interrupts enabled
					cli();
					wdt_enable(WDTO_15MS);
					for (;;)
						;
				}
				break;
			case '\r': // empty line: dump state
	            dumpState();
				break;
			}

			bytesReceived = 0; // reset counter
			return retval;
		}
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
    DDRB  = PB_OTHER_OUTPUTS;

    PORTC = PC_OUTPUT_INIT;
    DDRC  = PC_OTHER_OUTPUTS;

    PORTD = PD_OUTPUT_INIT;
    DDRD  = PD_OTHER_OUTPUTS;
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
    // after a watchdog reset (from the 'R' command) the watchdog is still
    // running with a 15 ms timeout; stop it before it resets us again
    MCUSR = 0;
    wdt_disable();

    initIO();
    initTimers();
    initPCInterrupts();
    uart_init(UART_BAUD_SELECT(BAUD, F_CPU));

    sei();                             // enable IRQ globally

    // debug: print wakeup message
    uart_puts_P("RemoteKeyboard v1.0 by Ned Konz\r\n");

    // main loop: send key events, process serial commands, and go to sleep
    for (;; )
    {
        sendQueuedEvents();
        while (processSerialCommand() != SERIAL_CMD_INCOMPLETE)
            ;
        // Sleep only if nothing arrived since the checks above. Interrupts
        // stay off between the checks and sleep_cpu(); the sei() takes
        // effect after the following instruction, so an interrupt in that
        // window executes and then immediately wakes the sleep.
        cli();
        if ((eventHead == eventTail) && !uart_available())
        {
            set_sleep_mode(SLEEP_MODE_IDLE);
            sleep_enable();
            sei();
            sleep_cpu();
            sleep_disable();
        }
        else
        {
            sei();
        }
    }
    return 0;
}

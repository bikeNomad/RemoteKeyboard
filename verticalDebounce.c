// http://www.electro-tech-online.com/micro-controllers/87624-vertical-counters-keyboard-debouncing.html#post684083
//
// Scott Dattalo's debounce method for up to 8 switches uses a 2
// bit vertical counter as eight independent debounce timers.  A
// few instructions have been added for parallel switch state
// management.
// 
// a switch is "debounced" (both press and release states) when it
// has been sampled four times at the same state spanning a 24 msec
// period (using 8 msec sampling intervals).
// 
// This routine filters out the "new release" switch state and only passes on
// the "new press" switch state. Your Main program simply needs to test and
// clear a switch flag bit in the sflags variable for "momentary" type
// switches, or simply test a switch flag bit for momentary switches that you
// want to emulate a "toggle" switch (press the switch to toggle the flag bit
// from on-to-off or from off-to-on).
// 
// I should also mention that since the slatch variable contains the real-time
// debounced switch state (plus debounce delay), it's relatively easy to
// implement "repeat" key functionality on-the-fly as needed in Main using this 
// variable.
// 
vcbit1 ^= vcbit0;               // inc counters (unconditionally)
vcbit0 = ~vcbit0;               // 
vcmask = ~portb ^ slatch;       // changes (press or release)
vcbit0 &= vcmask;               // clear inactive counters
vcbit1 &= vcmask;               // 
vcmask = ~vcmask;               // check for timed-out counters
vcmask |= vcbit0;               // 
vcmask |= vcbit1;               // 
vcmask = ~vcmask;               // any 1's are timed-out counters
slatch ^= vcmask;               // update debounced switch state
vcmask &= slatch;               // filter out "new release" bits
sflags ^= vcmask;               // update switch flags for Main

// vcbit1 vcbit0 portb vcmask
// 0     0       0

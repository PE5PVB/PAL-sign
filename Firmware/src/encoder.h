// A rotary encoder with a push switch, for choosing a card without a
// terminal and without the PC tool. Turning walks a candidate through
// the list without touching the picture; pushing switches to it.
//
// The two phases are read from a hardware interrupt (CHANGE on each
// pin), not a poll, which can collapse two close edges into one sample.
// The push switch is polled from the producer loop instead, once per
// row.
#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>

// Sets the pins up and attaches the phase interrupts. No-op when
// CFG_ENCODER_ON is false.
void encoderBegin();

// Reads the switch; called once per row from the producer loop.
void encoderPoll();

// Detents since the last call (positive clockwise) and whether the
// switch was pressed. Both clear on read.
int encoderTakeSteps();
bool encoderTakePush();

// Fires once on the debounced release edge. Clears on read.
bool encoderTakeRelease();

// Fires once the moment a held switch crosses LONG_PRESS_US, without
// waiting for release; encoderTakePush() still fires at the press edge
// too. Clears on read.
bool encoderTakeLongPress();

// Discards drift not yet settled into a step and any step not yet
// taken. Called on a press for the on-device menu: a cheap encoder's
// push switch is not mechanically isolated from its shaft, so pressing
// it can register as a turn. See MENU_TURN_GUARD_MS in Firmware.ino.
void encoderFlush();

// Whether encoderBegin() has actually run, so a front panel unplugged
// at boot (see PIN_FRONT_PRESENT in board.h) reads the same as
// CFG_ENCODER_ON being off, without needing a second flag anywhere else.
bool encoderPresent();

#endif  // ENCODER_H

#pragma once

// Holding a button for a while as its own action (piccoletto: hold START for
// the quick menu, hold BACK for HOME; piccolo's HOLD START TO REMAP widget
// later). Plain timing on a held / not held signal, no devices: the frontend
// says each frame whether the button is down.

#include <cstdint>

struct InputHold
{
    uint64_t since_ms = 0;       // held since (0: not held)
    bool     fired    = false;   // it fired: nothing more until it's let go
};

// Every frame: true once, on the frame the button has been held `hold_ms`;
// then false until it's let go (and held again).
bool input_hold_update(InputHold* hold, bool held, uint64_t now_ms, uint64_t hold_ms);

// It fired and hasn't been let go since: a press seen now is still that hold
// (e.g. the BACK that just left a screen), not a new one.
bool input_hold_fired(const InputHold* hold);

// Forget it (the frontend stopped watching this button).
void input_hold_reset(InputHold* hold);

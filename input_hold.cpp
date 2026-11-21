#include "input_hold.h"

bool input_hold_update(InputHold* hold, bool held, uint64_t now_ms, uint64_t hold_ms)
{
    if (!held)
    {
        input_hold_reset(hold);
        return false;
    }
    if (hold->fired)
    {
        return false;
    }
    if (hold->since_ms == 0)
    {
        hold->since_ms = now_ms == 0 ? 1 : now_ms;   // (0 means "not held")
        return false;
    }
    if (now_ms >= hold->since_ms && now_ms - hold->since_ms >= hold_ms)
    {
        hold->fired = true;
        return true;
    }
    return false;
}

bool input_hold_fired(const InputHold* hold)
{
    return hold->fired;
}

void input_hold_reset(InputHold* hold)
{
    hold->since_ms = 0;
    hold->fired = false;
}

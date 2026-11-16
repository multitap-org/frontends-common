// GameMode only exists on Linux: here it's never available.

#include "game_mode.h"

bool game_mode_supported()
{
    return false;
}

void game_mode_set(GameMode* game_mode, bool wanted)
{
    game_mode->requested = false;
    game_mode->state = wanted ? GameModeState::not_supported : GameModeState::off;
}

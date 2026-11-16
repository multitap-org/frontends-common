#pragma once

// Controller layouts ("appearances"): how a *type* of controller looks, not
// one model (unlike RetroArch's autoconfig). A layout gives the names of its
// buttons (A at the bottom on Xbox, cross on PlayStation, A on the right on
// Nintendo...), which controls it has (a Genesis pad has no sticks), and the
// SDL types it is picked for. MAP CONTROLLER asks only for what the layout
// has, and every label in the UI comes from the controller's layout.
//
// One file per layout, layouts/<Maker>/<model>.cfg (Nintendo/snes.cfg,
// Microsoft/xbox360.cfg...), in the settings_file dialect, so anyone can add
// one; the folder is the maker the selector groups them by:
//
//   [layout]
//   name = SWITCH PRO CONTROLLER
//   order = 30                      the selector's order (lower first)
//   sdl_types = switchpro joyconpair   SDL_GetGamepadStringForType names it is preselected for
//   dpad = yes                      (no: it has none)
//   sticks = 2                      (0, 1 or 2)
//
//   [labels]                        by SDL's position names; a control left out isn't on the pad
//   a = B                           the bottom face button
//   b = A                           the right one
//   x = Y                           the left one
//   y = X                           the top one
//   leftshoulder = L
//   rightshoulder = R
//   lefttrigger = ZL
//   righttrigger = ZR
//   back = -
//   start = +
//   guide = HOME
//   leftstick = LS
//   rightstick = RS

#include <cstdint>
#include <string>
#include <vector>

#include "input_sdl_mapping.h"
#include "settings_file.h"

struct InputLayout
{
    std::string              id;          // "<maker>/<file name without .cfg>" ("Nintendo/snes")
    std::string              maker;       // its folder ("Nintendo")
    std::string              name;        // shown in the selector
    int32_t                  order = 100;
    std::vector<std::string> sdl_types;
    bool                     dpad = true;
    uint32_t                 sticks = 2;
    std::string              labels[INPUT_MAP_CONTROL_COUNT];   // "" = no label (d-pad, sticks) or not on the pad
};

struct InputLayouts
{
    std::vector<InputLayout> layouts;   // sorted by order, then name
};

// Reads one layout from its file's contents (replacing one with the same id).
bool input_layouts_add(InputLayouts* layouts, const char* maker, const char* model, const SettingsFile* file);

// Every <Maker>/<model>.cfg under `directory` (later directories win for the same id).
void input_layouts_load_directory(InputLayouts* layouts, const char* directory);

// The makers, in the layouts' order (each once).
void input_layouts_makers(const InputLayouts* layouts, std::vector<std::string>* makers);

// GENERIC, built in: when no file defines it, so there is always one.
void input_layouts_ensure_generic(InputLayouts* layouts);

const InputLayout* input_layouts_find(const InputLayouts* layouts, const char* id);

// The first layout listing this SDL type name ("ps5"...), or nullptr.
const InputLayout* input_layouts_for_type(const InputLayouts* layouts, const char* sdl_type);

// Does a pad of this layout have the control? (MAP CONTROLLER skips the rest.)
bool input_layout_has(const InputLayout* layout, InputMapControl control);

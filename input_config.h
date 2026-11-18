#pragma once

// settings/input.cfg: the controller profiles (input_bindings.h), in the
// settings_file INI dialect. (The input settings, InputSettings, are the
// frontend's own settings, with their per core / game layers.)
//
//   [controller/<id>]                     name and deadzone (percent)
//   name = SNES30
//   deadzone = 15
//   ignore = yes                          (or no; missing = automatic)
//   appearance = Sony/dualsense           (a layout id; none = declined)
//   hotkey_mode = stays_on                (hold, one_hotkey, stays_on; missing = hold)
//   label.misc1 = VOLUME UP               (a name given to a button; any button)
//   sdl_mapping = <guid>,SNES30,a:b1,...  (MAP CONTROLLER's SDL mapping, given to SDL
//                                          at start; missing = SDL's own, if it knows it)
//
//   [retropad/<id>]                       RetroPad bindings that differ from the defaults
//   pad.y = button:west key:A
//
//   [ui/<id>]                             interface and hotkey bindings that differ
//   hotkey.save_state = none
//
//   [core:<core>/<device>/<id>]           a controller for one core's emulated device
//   remap = custom                        (retropad: asked, and the RetroPad was kept)
//   pad.a = button:east                   RetroPad changes for it (custom)
//
// <id> is the profile id: a gamepad's GUID, "keyboard", "keyboard.2"...
// A binding lists its sources separated by spaces; "none" unbinds. Only what
// differs from the defaults is written, so improving the defaults reaches
// every controller nobody changed. Profiles of controllers not connected now
// are kept.

#include "input_bindings.h"
#include "settings_file.h"

// Reads the profiles (replacing them). Call before the
// devices are added, so their controllers come from the profiles.
void input_config_read(InputSystem* system, const SettingsFile* file);

// Takes the controllers into the profiles, then writes them.
void input_config_write(InputSystem* system, SettingsFile* file);

// With a file on disk (a missing file reads as empty).
bool input_config_load(InputSystem* system, const char* path);
bool input_config_save(InputSystem* system, const char* path);

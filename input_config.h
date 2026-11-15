#pragma once

// settings/input.cfg: the input settings and the controller profiles
// (input_bindings.h), in the settings_file INI dialect:
//
//   [input]
//   gamepad_takes_keyboard_port = true
//   hotkey_mode = hold                    (or double_tap)
//
//   [controller/<id>]                     name and deadzone (percent)
//   name = SNES30
//   deadzone = 15
//
//   [retropad/<id>]                       RetroPad bindings that differ from the defaults
//   pad.y = button:west key:A
//
//   [ui/<id>]                             interface and hotkey bindings that differ
//   hotkey.save_state = none
//
// <id> is the profile id: a gamepad's GUID, "keyboard", "keyboard.2"...
// A binding lists its sources separated by spaces; "none" unbinds. Only what
// differs from the defaults is written, so improving the defaults reaches
// every controller nobody changed. Profiles of controllers not connected now
// are kept.

#include "input_bindings.h"
#include "settings_file.h"

// Reads the settings and the profiles (replacing them). Call before the
// devices are added, so their controllers come from the profiles.
void input_config_read(InputSystem* system, const SettingsFile* file);

// Takes the controllers into the profiles, then writes settings and profiles.
void input_config_write(InputSystem* system, SettingsFile* file);

// With a file on disk (a missing file reads as empty).
bool input_config_load(InputSystem* system, const char* path);
bool input_config_save(InputSystem* system, const char* path);

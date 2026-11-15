#pragma once

// settings/input.cfg: the controller profiles (input_bindings.h), in the
// settings_file INI dialect. (The input settings, InputSettings, are the
// frontend's own settings, with their per core / game layers.)
//
//   [controller/<id>]                     name and deadzone (percent)
//   name = SNES30
//   deadzone = 15
//   ignore = yes                          (or no; missing = automatic)
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

// Reads the profiles (replacing them). Call before the
// devices are added, so their controllers come from the profiles.
void input_config_read(InputSystem* system, const SettingsFile* file);

// Takes the controllers into the profiles, then writes them.
void input_config_write(InputSystem* system, SettingsFile* file);

// With a file on disk (a missing file reads as empty).
bool input_config_load(InputSystem* system, const char* path);
bool input_config_save(InputSystem* system, const char* path);

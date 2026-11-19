#pragma once

// Settings and actions from a client (piccoletto): the host describes its
// settings (pages, labels in the client's language, the value in each layer)
// and the actions a device may trigger; the client changes settings and
// triggers actions. Nothing about a frontend's settings is known here: the
// host describes them from its own table (settings.h), so a client needs no
// copy of it.
//
// Messages (control channel, little-endian; text = u16 length + UTF-8; reals
// travel as text, as settings files keep them):
//
//   client -> host  settings_request  id u32 | start u32 | language
//   host -> client  settings_result   id u32 | start u32 | total u32 | layers u8 | core | game |
//                                     count u16 | count x setting
//   client -> host  setting_set       id u32 | layer u8 | reset u8 | key | value
//   host -> client  setting_result    id u32 | status u8 | values
//   host -> client  setting_changed   values             (key "" = all may have changed: ask again)
//   client -> host  actions_request   id u32 | language
//   host -> client  actions_result    id u32 | count u16 | count x action   (id 0 = sent unasked: the list changed)
//   client -> host  action            key | pressed u8
//
//   setting = type u8 | flags u8 | page | subpage | section | label | help |
//             min i32 | max i32 | step i32 | min_real | max_real | step_real |
//             choice_count u16 | choice_count x { key | label } | values
//   values  = key | set u8 | global | core | game | effective | source u8
//   action  = key | label | kind u8 | state u32
//
// `language` is a code ("es"); the host answers in it when it has it, else
// in English. Layers are SettingLayer's numbers (1 global, 2 core, 3 game);
// `layers` says which exist now (bit 1 core, bit 2 game: a core / game is
// running) and `core` / `game` name them. `set` has a bit per layer that
// defines the value (bit 0 global, 1 core, 2 game); `source` is the layer the
// effective value comes from (0 = the default).
//
// Settings need FULL access (types 136-140). Actions are asked for and
// triggered at GAMEPAD level (types 17-19); the host lists only the actions
// the device's level allows, and checks again when one arrives.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "localization.h"
#include "net_content.h"   // NetContentStatus
#include "settings.h"

enum class NetSettingsMessage : uint8_t
{
    actions_request = 17,
    actions_result = 18,
    action = 19,
    settings_request = 136,
    settings_result = 137,
    setting_set = 138,
    setting_result = 139,
    setting_changed = 140,
};

constexpr uint8_t NET_SETTING_OVERRIDABLE = 1;   // flags: may be set per core / per game
constexpr uint8_t NET_LAYER_CORE = 2;            // `layers` bits
constexpr uint8_t NET_LAYER_GAME = 4;
constexpr uint32_t NET_SETTING_TYPE_COUNT = 7;   // SettingType's values

struct NetSettingValues
{
    std::string key;
    uint8_t     set = 0;      // bit 0 global, bit 1 core, bit 2 game
    std::string global;       // what each layer defines ("" where it doesn't)
    std::string core;
    std::string game;
    std::string effective;
    uint8_t     source = 0;   // SettingLayer of `effective`
};

struct NetSettingChoice
{
    std::string key;     // the value as stored
    std::string label;   // as shown ("" = show the key)
};

struct NetSetting
{
    SettingType type = SettingType::boolean;
    uint8_t     flags = 0;
    std::string page;      // "VIDEO"
    std::string subpage;   // "PICTURE", or ""
    std::string section;   // "LOW LATENCY", or ""
    std::string label;
    std::string help;
    int32_t     min = 0;
    int32_t     max = 0;
    int32_t     step = 1;
    std::string min_real;
    std::string max_real;
    std::string step_real;
    std::vector<NetSettingChoice> choices;   // enumeration values; for text, suggestions (audio devices)
    NetSettingValues values;
};

enum class NetActionKind : uint8_t
{
    press,    // once per press
    hold,     // while held (pressed, then released)
    toggle,   // each press flips it; `state` says on (1) or off (0)
};
constexpr uint8_t NET_ACTION_KIND_COUNT = 3;

struct NetAction
{
    std::string   key;     // "save_state"
    std::string   label;
    NetActionKind kind = NetActionKind::press;
    uint32_t      state = 0;   // toggle: on / off; others: what the host shows next to it (the slot)
};

// --- messages ---------------------------------------------------------------
// Writers return the size (0 if it doesn't fit); readers are false unless
// the payload is exactly that message.

size_t net_settings_write_request(uint8_t* out, size_t capacity, uint32_t id, uint32_t start, const char* language);
bool   net_settings_read_request(const uint8_t* data, size_t size, uint32_t* id, uint32_t* start, std::string* language);

// The settings from `start` that fit.
size_t net_settings_write_result(uint8_t* out, size_t capacity, uint32_t id, const std::vector<NetSetting>* settings,
                                 uint32_t start, uint8_t layers, const char* core, const char* game);
bool   net_settings_read_result(const uint8_t* data, size_t size, uint32_t* id, uint32_t* start, uint32_t* total,
                                uint8_t* layers, std::string* core, std::string* game,
                                std::vector<NetSetting>* settings);

size_t net_settings_write_set(uint8_t* out, size_t capacity, uint32_t id, SettingLayer layer, bool reset,
                              const char* key, const char* value);
bool   net_settings_read_set(const uint8_t* data, size_t size, uint32_t* id, SettingLayer* layer, bool* reset,
                             std::string* key, std::string* value);

size_t net_settings_write_set_result(uint8_t* out, size_t capacity, uint32_t id, NetContentStatus status,
                                     const NetSettingValues* values);
bool   net_settings_read_set_result(const uint8_t* data, size_t size, uint32_t* id, NetContentStatus* status,
                                    NetSettingValues* values);

size_t net_settings_write_changed(uint8_t* out, size_t capacity, const NetSettingValues* values);
bool   net_settings_read_changed(const uint8_t* data, size_t size, NetSettingValues* values);

size_t net_settings_write_actions_request(uint8_t* out, size_t capacity, uint32_t id, const char* language);
bool   net_settings_read_actions_request(const uint8_t* data, size_t size, uint32_t* id, std::string* language);

size_t net_settings_write_actions(uint8_t* out, size_t capacity, uint32_t id, const std::vector<NetAction>* actions);
bool   net_settings_read_actions(const uint8_t* data, size_t size, uint32_t* id, std::vector<NetAction>* actions);

size_t net_settings_write_action(uint8_t* out, size_t capacity, const char* key, bool pressed);
bool   net_settings_read_action(const uint8_t* data, size_t size, std::string* key, bool* pressed);

// --- the host's side ----------------------------------------------------------

// A setting from `settings`' table, its labels through `strings` (the
// frontend's table, in the client's language). `page` / `subpage` /
// `section` are left for the caller. False if `key` isn't in the table.
bool net_settings_describe(const Settings* settings, const Localization* strings, const char* key,
                           NetSetting* setting);

// The value of `key` in each layer, as net_settings_describe puts it.
void net_settings_values(const Settings* settings, const char* key, NetSettingValues* values);

// Applies a setting_set to `settings` (the caller has checked the key may be
// changed from the network). not_found: no such setting; refused: the value
// isn't valid there, or the layer doesn't exist now.
NetContentStatus net_settings_apply(Settings* settings, SettingLayer layer, bool reset, const char* key,
                                    const char* value);

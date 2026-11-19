// Checks net_settings.h: every message written and read back, damaged ones
// refused, settings paged so each arrives once, and the host's side
// (describe, values per layer, apply) against a small table of its own.
//
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <string>
#include <vector>

#include "localization.h"
#include "net_link.h"
#include "net_settings.h"
#include "settings.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

static const LocalizedString STRINGS[] = {
    {"volume", "VOLUME"},
    {"volume_help", "How loud."},
    {"filter", "FILTER"},
    {"filter_nearest", "NEAREST"},
    {"filter_linear", "LINEAR"},
};
static const SettingEnumValue FILTERS[] = {{"nearest", 3}, {"linear", 4}};
static const SettingDescriptor TABLE[] = {
    {.key = "volume", .type = SettingType::integer, .default_value = "100", .overridable = true, .min = 0,
     .max = 100, .step = 5, .label = 0, .help = 1},
    {.key = "filter", .type = SettingType::enumeration, .default_value = "nearest", .overridable = false,
     .values = FILTERS, .value_count = 2, .label = 2},
    {.key = "speed", .type = SettingType::real, .default_value = "1.5", .min_real = 0.5, .max_real = 4.0,
     .step_real = 0.25},
};

static void test_messages()
{
    uint8_t out[NET_LINK_MAX_PAYLOAD];
    uint32_t id = 0;
    uint32_t start = 0;
    std::string language;
    size_t size = net_settings_write_request(out, sizeof(out), 4, 10, "es");
    check(net_settings_read_request(out, size, &id, &start, &language) && id == 4 && start == 10 && language == "es",
          "settings_request read");
    check(!net_settings_read_request(out, size - 1, &id, &start, &language), "settings_request cut refused");

    SettingLayer layer = SettingLayer::defaults;
    bool reset = true;
    std::string key;
    std::string value;
    size = net_settings_write_set(out, sizeof(out), 5, SettingLayer::core, false, "volume", "80");
    check(net_settings_read_set(out, size, &id, &layer, &reset, &key, &value) && id == 5 &&
              layer == SettingLayer::core && !reset && key == "volume" && value == "80",
          "setting_set read");
    out[5] = 0;   // the defaults layer
    check(!net_settings_read_set(out, size, &id, &layer, &reset, &key, &value), "setting_set to defaults refused");

    NetSettingValues values;
    values.key = "volume";
    values.set = 3;
    values.global = "90";
    values.core = "80";
    values.effective = "80";
    values.source = static_cast<uint8_t>(SettingLayer::core);
    NetContentStatus status = NetContentStatus::failed;
    NetSettingValues read;
    size = net_settings_write_set_result(out, sizeof(out), 5, NetContentStatus::ok, &values);
    check(net_settings_read_set_result(out, size, &id, &status, &read) && status == NetContentStatus::ok &&
              read.key == "volume" && read.set == 3 && read.global == "90" && read.core == "80" && read.game.empty() &&
              read.effective == "80" && read.source == 2,
          "setting_result read");
    size = net_settings_write_changed(out, sizeof(out), &values);
    check(net_settings_read_changed(out, size, &read) && read.core == "80", "setting_changed read");
    check(!net_settings_read_set_result(out, size, &id, &status, &read), "setting_changed isn't a setting_result");

    std::vector<NetAction> actions;
    NetAction save;
    save.key = "save_state";
    save.label = "SAVE STATE";
    save.state = 3;
    actions.push_back(save);
    NetAction pause;
    pause.key = "pause";
    pause.label = "PAUSE";
    pause.kind = NetActionKind::toggle;
    pause.state = 1;
    actions.push_back(pause);
    std::vector<NetAction> got;
    size = net_settings_write_actions(out, sizeof(out), 0, &actions);
    check(net_settings_read_actions(out, size, &id, &got) && id == 0 && got.size() == 2 && got[0].state == 3 &&
              got[1].kind == NetActionKind::toggle && got[1].label == "PAUSE",
          "actions_result read");
    size = net_settings_write_actions_request(out, sizeof(out), 8, "en");
    check(net_settings_read_actions_request(out, size, &id, &language) && id == 8 && language == "en",
          "actions_request read");
    NetControllers controllers;
    controllers.pad_ports[0] = 1;
    NetPort port;
    port.device = 1;
    NetPortDevice joypad;
    joypad.id = 1;
    joypad.name = "SNES Joypad";
    port.devices.push_back(joypad);
    NetPortDevice mouse;
    mouse.id = 2;
    mouse.name = "SNES Mouse";
    port.devices.push_back(mouse);
    NetPortDescriptor b_button;
    b_button.device = 1;
    b_button.id = 0;
    b_button.name = "B";
    port.descriptors.push_back(b_button);
    controllers.ports.push_back(port);
    controllers.ports.push_back(NetPort{});
    NetControllers controllers_read;
    size = net_settings_write_controllers(out, sizeof(out), 0, &controllers);
    check(net_settings_read_controllers(out, size, &id, &controllers_read) && id == 0 &&
              controllers_read.pad_ports[0] == 1 && controllers_read.pad_ports[1] == -1 &&
              controllers_read.ports.size() == 2 && controllers_read.ports[0].devices.size() == 2 &&
              controllers_read.ports[0].devices[1].name == "SNES Mouse" &&
              controllers_read.ports[0].descriptors.size() == 1 && controllers_read.ports[0].descriptors[0].name == "B" &&
              controllers_read.ports[1].devices.empty(),
          "controllers read");
    check(!net_settings_read_controllers(out, size - 1, &id, &controllers_read), "controllers cut refused");
    uint8_t which_pad = 9;
    uint32_t which_device = 0;
    size = net_settings_write_port_device(out, sizeof(out), 0, 2);
    check(net_settings_read_port_device(out, size, &which_pad, &which_device) && which_pad == 0 && which_device == 2,
          "port_device read");
    size = net_settings_write_port_device(out, sizeof(out), 7, 2);
    check(!net_settings_read_port_device(out, size, &which_pad, &which_device), "port_device for pad 7 refused");
    size = net_settings_write_controllers_request(out, sizeof(out), 3);
    check(net_settings_read_controllers_request(out, size, &id) && id == 3, "controllers_request read");

    bool pressed = false;
    size = net_settings_write_action(out, sizeof(out), "fast_forward", true);
    check(net_settings_read_action(out, size, &key, &pressed) && key == "fast_forward" && pressed, "action read");
    out[size - 1] = 2;
    check(!net_settings_read_action(out, size, &key, &pressed), "action with a bad flag refused");
}

static void test_host()
{
    Localization strings;
    check(localization_init(&strings, STRINGS, 5), "strings");
    Settings settings;
    check(settings_init(&settings, TABLE, 3), "table");
    settings_set(&settings, SettingLayer::global, "volume", "90");

    NetSetting volume;
    check(net_settings_describe(&settings, &strings, "volume", &volume) && volume.type == SettingType::integer &&
              volume.label == "VOLUME" && volume.help == "How loud." && volume.max == 100 && volume.step == 5 &&
              volume.flags == NET_SETTING_OVERRIDABLE && volume.values.global == "90" && volume.values.set == 1 &&
              volume.values.effective == "90" && volume.values.source == static_cast<uint8_t>(SettingLayer::global),
          "an integer described");
    NetSetting filter;
    check(net_settings_describe(&settings, &strings, "filter", &filter) && filter.choices.size() == 2 &&
              filter.choices[1].key == "linear" && filter.choices[1].label == "LINEAR" && filter.flags == 0 &&
              filter.values.effective == "nearest" && filter.values.source == 0,
          "an enumeration described, from its default");
    NetSetting speed;
    check(net_settings_describe(&settings, &strings, "speed", &speed) && speed.min_real == "0.5" &&
              speed.step_real == "0.25" && speed.label.empty(),
          "a real described");
    check(!net_settings_describe(&settings, &strings, "nope", &speed), "an unknown key isn't described");

    // Layers: core only exists while one runs.
    check(net_settings_apply(&settings, SettingLayer::core, false, "volume", "50") == NetContentStatus::refused,
          "no core running: core layer refused");
    settings_set_context(&settings, "snes9x", "Game");
    check(net_settings_apply(&settings, SettingLayer::core, false, "volume", "50") == NetContentStatus::ok,
          "core layer set");
    check(net_settings_apply(&settings, SettingLayer::core, false, "filter", "linear") == NetContentStatus::refused,
          "a global-only setting refused in the core layer");
    check(net_settings_apply(&settings, SettingLayer::global, false, "volume", "101") == NetContentStatus::refused,
          "out of range refused");
    check(net_settings_apply(&settings, SettingLayer::global, false, "nope", "1") == NetContentStatus::not_found,
          "unknown key not found");
    NetSettingValues values;
    net_settings_values(&settings, "volume", &values);
    check(values.set == 3 && values.core == "50" && values.effective == "50" &&
              values.source == static_cast<uint8_t>(SettingLayer::core),
          "values per layer");
    check(net_settings_apply(&settings, SettingLayer::core, true, "volume", "") == NetContentStatus::ok,
          "core layer reset");
    net_settings_values(&settings, "volume", &values);
    check(values.set == 1 && values.effective == "90", "back to the global value");

    // Pages: describe the same three settings many times, read them back
    // through small pages.
    std::vector<NetSetting> all;
    for (uint32_t i = 0; i < 60; i++)
    {
        NetSetting setting;
        net_settings_describe(&settings, &strings, TABLE[i % 3].key, &setting);
        setting.page = "PAGE " + std::to_string(i / 10);
        all.push_back(setting);
    }
    uint8_t out[700];
    std::vector<NetSetting> received;
    bool ok = true;
    uint32_t pages = 0;
    while (ok && received.size() < all.size() && pages < 100)
    {
        size_t size = net_settings_write_result(out, sizeof(out), 1, &all, static_cast<uint32_t>(received.size()),
                                                NET_LAYER_CORE | NET_LAYER_GAME, "snes9x", "Game");
        uint32_t id = 0;
        uint32_t start = 0;
        uint32_t total = 0;
        uint8_t layers = 0;
        std::string core;
        std::string game;
        std::vector<NetSetting> page;
        ok = size > 0 && net_settings_read_result(out, size, &id, &start, &total, &layers, &core, &game, &page) &&
             total == 60 && start == received.size() && layers == (NET_LAYER_CORE | NET_LAYER_GAME) &&
             core == "snes9x" && game == "Game" && !page.empty();
        for (size_t i = 0; i < page.size(); i++)
        {
            received.push_back(page[i]);
        }
        pages++;
    }
    check(ok && pages > 1, "settings pages read");
    bool same = received.size() == all.size();
    for (size_t i = 0; same && i < all.size(); i++)
    {
        same = received[i].page == all[i].page && received[i].label == all[i].label &&
               received[i].choices.size() == all[i].choices.size() &&
               received[i].values.effective == all[i].values.effective;
    }
    check(same, "every setting arrived once, in order");
}

int main()
{
    test_messages();
    test_host();
    if (g_failures == 0)
    {
        std::printf("net settings: all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}

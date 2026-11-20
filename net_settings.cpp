#include "net_settings.h"

#include <cstdio>
#include <cstring>

#include "net_wire.h"

// ---------------------------------------------------------------------------
// Fields
// ---------------------------------------------------------------------------

static NetWriter writer_begin(uint8_t* out, size_t capacity, NetSettingsMessage type)
{
    NetWriter writer = {out, capacity, 0, true};
    net_wire_put_u8(&writer, static_cast<uint8_t>(type));
    return writer;
}

static NetReader reader_begin(const uint8_t* data, size_t size, NetSettingsMessage type)
{
    NetReader reader;
    reader.data = data;
    reader.size = size;
    if (net_wire_get_u8(&reader) != static_cast<uint8_t>(type))
    {
        reader.ok = false;
    }
    return reader;
}

static size_t writer_end(const NetWriter* writer)
{
    return writer->ok ? writer->size : 0;
}

static void put_values(NetWriter* writer, const NetSettingValues* values)
{
    net_wire_put_string(writer, &values->key);
    net_wire_put_u8(writer, values->set);
    net_wire_put_string(writer, &values->global);
    net_wire_put_string(writer, &values->core);
    net_wire_put_string(writer, &values->game);
    net_wire_put_string(writer, &values->effective);
    net_wire_put_u8(writer, values->source);
}

static void get_values(NetReader* reader, NetSettingValues* values)
{
    net_wire_get_text(reader, &values->key);
    values->set = net_wire_get_u8(reader);
    net_wire_get_text(reader, &values->global);
    net_wire_get_text(reader, &values->core);
    net_wire_get_text(reader, &values->game);
    net_wire_get_text(reader, &values->effective);
    values->source = net_wire_get_u8(reader);
    if (values->set > 7 || values->source > static_cast<uint8_t>(SettingLayer::game))
    {
        reader->ok = false;
    }
}

static void put_setting(NetWriter* writer, const NetSetting* setting)
{
    net_wire_put_u8(writer, static_cast<uint8_t>(setting->type));
    net_wire_put_u8(writer, setting->flags);
    net_wire_put_string(writer, &setting->page);
    net_wire_put_string(writer, &setting->subpage);
    net_wire_put_string(writer, &setting->section);
    net_wire_put_string(writer, &setting->label);
    net_wire_put_string(writer, &setting->help);
    net_wire_put_i32(writer, setting->min);
    net_wire_put_i32(writer, setting->max);
    net_wire_put_i32(writer, setting->step);
    net_wire_put_string(writer, &setting->min_real);
    net_wire_put_string(writer, &setting->max_real);
    net_wire_put_string(writer, &setting->step_real);
    if (setting->choices.size() > UINT16_MAX)
    {
        writer->ok = false;
        return;
    }
    net_wire_put_u16(writer, static_cast<uint16_t>(setting->choices.size()));
    for (size_t i = 0; i < setting->choices.size(); i++)
    {
        net_wire_put_string(writer, &setting->choices[i].key);
        net_wire_put_string(writer, &setting->choices[i].label);
    }
    put_values(writer, &setting->values);
}

static void get_setting(NetReader* reader, NetSetting* setting)
{
    uint8_t type = net_wire_get_u8(reader);
    if (type >= NET_SETTING_TYPE_COUNT)
    {
        reader->ok = false;
        return;
    }
    setting->type = static_cast<SettingType>(type);
    setting->flags = net_wire_get_u8(reader);
    net_wire_get_text(reader, &setting->page);
    net_wire_get_text(reader, &setting->subpage);
    net_wire_get_text(reader, &setting->section);
    net_wire_get_text(reader, &setting->label);
    net_wire_get_text(reader, &setting->help);
    setting->min = net_wire_get_i32(reader);
    setting->max = net_wire_get_i32(reader);
    setting->step = net_wire_get_i32(reader);
    net_wire_get_text(reader, &setting->min_real);
    net_wire_get_text(reader, &setting->max_real);
    net_wire_get_text(reader, &setting->step_real);
    uint16_t count = net_wire_get_u16(reader);
    setting->choices.clear();
    for (uint16_t i = 0; i < count && reader->ok; i++)
    {
        NetSettingChoice choice;
        net_wire_get_text(reader, &choice.key);
        net_wire_get_text(reader, &choice.label);
        setting->choices.push_back(choice);
    }
    get_values(reader, &setting->values);
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

size_t net_settings_write_request(uint8_t* out, size_t capacity, uint32_t id, uint32_t start, const char* language)
{
    NetWriter writer = writer_begin(out, capacity, NetSettingsMessage::settings_request);
    net_wire_put_u32(&writer, id);
    net_wire_put_u32(&writer, start);
    net_wire_put_text(&writer, language, std::strlen(language));
    return writer_end(&writer);
}

bool net_settings_read_request(const uint8_t* data, size_t size, uint32_t* id, uint32_t* start, std::string* language)
{
    NetReader reader = reader_begin(data, size, NetSettingsMessage::settings_request);
    *id = net_wire_get_u32(&reader);
    *start = net_wire_get_u32(&reader);
    net_wire_get_text(&reader, language);
    return net_wire_done(&reader);
}

size_t net_settings_write_result(uint8_t* out, size_t capacity, uint32_t id, const std::vector<NetSetting>* settings,
                                 uint32_t start, uint8_t layers, const char* core, const char* game)
{
    uint32_t total = static_cast<uint32_t>(settings->size());
    NetWriter writer = writer_begin(out, capacity, NetSettingsMessage::settings_result);
    net_wire_put_u32(&writer, id);
    net_wire_put_u32(&writer, start);
    net_wire_put_u32(&writer, total);
    net_wire_put_u8(&writer, layers);
    net_wire_put_text(&writer, core, std::strlen(core));
    net_wire_put_text(&writer, game, std::strlen(game));
    size_t count_at = writer.size;
    net_wire_put_u16(&writer, 0);
    if (!writer.ok)
    {
        return 0;
    }
    // As many as fit, each whole.
    uint16_t count = 0;
    for (uint32_t i = start; i < total && count < UINT16_MAX; i++)
    {
        size_t before = writer.size;
        put_setting(&writer, &(*settings)[i]);
        if (!writer.ok)
        {
            writer.size = before;
            writer.ok = true;
            break;
        }
        count++;
    }
    out[count_at] = static_cast<uint8_t>(count);
    out[count_at + 1] = static_cast<uint8_t>(count >> 8);
    return writer.size;
}

bool net_settings_read_result(const uint8_t* data, size_t size, uint32_t* id, uint32_t* start, uint32_t* total,
                              uint8_t* layers, std::string* core, std::string* game, std::vector<NetSetting>* settings)
{
    settings->clear();
    NetReader reader = reader_begin(data, size, NetSettingsMessage::settings_result);
    *id = net_wire_get_u32(&reader);
    *start = net_wire_get_u32(&reader);
    *total = net_wire_get_u32(&reader);
    *layers = net_wire_get_u8(&reader);
    net_wire_get_text(&reader, core);
    net_wire_get_text(&reader, game);
    uint16_t count = net_wire_get_u16(&reader);
    for (uint16_t i = 0; i < count && reader.ok; i++)
    {
        NetSetting setting;
        get_setting(&reader, &setting);
        settings->push_back(setting);
    }
    return net_wire_done(&reader);
}

size_t net_settings_write_set(uint8_t* out, size_t capacity, uint32_t id, SettingLayer layer, bool reset,
                              const char* key, const char* value)
{
    NetWriter writer = writer_begin(out, capacity, NetSettingsMessage::setting_set);
    net_wire_put_u32(&writer, id);
    net_wire_put_u8(&writer, static_cast<uint8_t>(layer));
    net_wire_put_u8(&writer, reset ? 1 : 0);
    net_wire_put_text(&writer, key, std::strlen(key));
    net_wire_put_text(&writer, value, std::strlen(value));
    return writer_end(&writer);
}

bool net_settings_read_set(const uint8_t* data, size_t size, uint32_t* id, SettingLayer* layer, bool* reset,
                           std::string* key, std::string* value)
{
    NetReader reader = reader_begin(data, size, NetSettingsMessage::setting_set);
    *id = net_wire_get_u32(&reader);
    uint8_t layer_value = net_wire_get_u8(&reader);
    uint8_t reset_value = net_wire_get_u8(&reader);
    net_wire_get_text(&reader, key);
    net_wire_get_text(&reader, value);
    if (layer_value < static_cast<uint8_t>(SettingLayer::global) || layer_value > static_cast<uint8_t>(SettingLayer::game) ||
        reset_value > 1)
    {
        return false;
    }
    *layer = static_cast<SettingLayer>(layer_value);
    *reset = reset_value == 1;
    return net_wire_done(&reader);
}

size_t net_settings_write_set_result(uint8_t* out, size_t capacity, uint32_t id, NetContentStatus status,
                                     const NetSettingValues* values)
{
    NetWriter writer = writer_begin(out, capacity, NetSettingsMessage::setting_result);
    net_wire_put_u32(&writer, id);
    net_wire_put_u8(&writer, static_cast<uint8_t>(status));
    put_values(&writer, values);
    return writer_end(&writer);
}

bool net_settings_read_set_result(const uint8_t* data, size_t size, uint32_t* id, NetContentStatus* status,
                                  NetSettingValues* values)
{
    NetReader reader = reader_begin(data, size, NetSettingsMessage::setting_result);
    *id = net_wire_get_u32(&reader);
    uint8_t status_value = net_wire_get_u8(&reader);
    if (status_value >= NET_CONTENT_STATUS_COUNT)
    {
        return false;
    }
    *status = static_cast<NetContentStatus>(status_value);
    get_values(&reader, values);
    return net_wire_done(&reader);
}

size_t net_settings_write_changed(uint8_t* out, size_t capacity, const NetSettingValues* values)
{
    NetWriter writer = writer_begin(out, capacity, NetSettingsMessage::setting_changed);
    put_values(&writer, values);
    return writer_end(&writer);
}

bool net_settings_read_changed(const uint8_t* data, size_t size, NetSettingValues* values)
{
    NetReader reader = reader_begin(data, size, NetSettingsMessage::setting_changed);
    get_values(&reader, values);
    return net_wire_done(&reader);
}

size_t net_settings_write_actions_request(uint8_t* out, size_t capacity, uint32_t id, const char* language)
{
    NetWriter writer = writer_begin(out, capacity, NetSettingsMessage::actions_request);
    net_wire_put_u32(&writer, id);
    net_wire_put_text(&writer, language, std::strlen(language));
    return writer_end(&writer);
}

bool net_settings_read_actions_request(const uint8_t* data, size_t size, uint32_t* id, std::string* language)
{
    NetReader reader = reader_begin(data, size, NetSettingsMessage::actions_request);
    *id = net_wire_get_u32(&reader);
    net_wire_get_text(&reader, language);
    return net_wire_done(&reader);
}

size_t net_settings_write_actions(uint8_t* out, size_t capacity, uint32_t id, const std::vector<NetAction>* actions)
{
    if (actions->size() > UINT16_MAX)
    {
        return 0;
    }
    NetWriter writer = writer_begin(out, capacity, NetSettingsMessage::actions_result);
    net_wire_put_u32(&writer, id);
    net_wire_put_u16(&writer, static_cast<uint16_t>(actions->size()));
    for (size_t i = 0; i < actions->size(); i++)
    {
        const NetAction* action = &(*actions)[i];
        net_wire_put_string(&writer, &action->key);
        net_wire_put_string(&writer, &action->label);
        net_wire_put_u8(&writer, static_cast<uint8_t>(action->kind));
        net_wire_put_u32(&writer, action->state);
    }
    return writer_end(&writer);
}

bool net_settings_read_actions(const uint8_t* data, size_t size, uint32_t* id, std::vector<NetAction>* actions)
{
    actions->clear();
    NetReader reader = reader_begin(data, size, NetSettingsMessage::actions_result);
    *id = net_wire_get_u32(&reader);
    uint16_t count = net_wire_get_u16(&reader);
    for (uint16_t i = 0; i < count && reader.ok; i++)
    {
        NetAction action;
        net_wire_get_text(&reader, &action.key);
        net_wire_get_text(&reader, &action.label);
        uint8_t kind = net_wire_get_u8(&reader);
        if (kind >= NET_ACTION_KIND_COUNT)
        {
            return false;
        }
        action.kind = static_cast<NetActionKind>(kind);
        action.state = net_wire_get_u32(&reader);
        actions->push_back(action);
    }
    return net_wire_done(&reader);
}

size_t net_settings_write_action(uint8_t* out, size_t capacity, const char* key, bool pressed)
{
    NetWriter writer = writer_begin(out, capacity, NetSettingsMessage::action);
    net_wire_put_text(&writer, key, std::strlen(key));
    net_wire_put_u8(&writer, pressed ? 1 : 0);
    return writer_end(&writer);
}

bool net_settings_read_action(const uint8_t* data, size_t size, std::string* key, bool* pressed)
{
    NetReader reader = reader_begin(data, size, NetSettingsMessage::action);
    net_wire_get_text(&reader, key);
    uint8_t value = net_wire_get_u8(&reader);
    if (value > 1)
    {
        return false;
    }
    *pressed = value == 1;
    return net_wire_done(&reader);
}

size_t net_settings_write_controllers_request(uint8_t* out, size_t capacity, uint32_t id)
{
    NetWriter writer = writer_begin(out, capacity, NetSettingsMessage::controllers_request);
    net_wire_put_u32(&writer, id);
    return writer_end(&writer);
}

bool net_settings_read_controllers_request(const uint8_t* data, size_t size, uint32_t* id)
{
    NetReader reader = reader_begin(data, size, NetSettingsMessage::controllers_request);
    *id = net_wire_get_u32(&reader);
    return net_wire_done(&reader);
}

size_t net_settings_write_controllers(uint8_t* out, size_t capacity, uint32_t id, const NetControllers* controllers)
{
    if (controllers->ports.size() > UINT8_MAX)
    {
        return 0;
    }
    NetWriter writer = writer_begin(out, capacity, NetSettingsMessage::controllers);
    net_wire_put_u32(&writer, id);
    for (uint32_t pad = 0; pad < NET_CONTROLLERS_PADS; pad++)
    {
        net_wire_put_u8(&writer, static_cast<uint8_t>(controllers->pad_ports[pad]));
    }
    net_wire_put_u8(&writer, static_cast<uint8_t>(controllers->ports.size()));
    for (size_t p = 0; p < controllers->ports.size(); p++)
    {
        const NetPort* port = &controllers->ports[p];
        if (port->devices.size() > UINT16_MAX || port->descriptors.size() > UINT16_MAX)
        {
            return 0;
        }
        net_wire_put_u32(&writer, port->device);
        net_wire_put_u16(&writer, static_cast<uint16_t>(port->devices.size()));
        for (size_t d = 0; d < port->devices.size(); d++)
        {
            net_wire_put_u32(&writer, port->devices[d].id);
            net_wire_put_string(&writer, &port->devices[d].name);
        }
        net_wire_put_u16(&writer, static_cast<uint16_t>(port->descriptors.size()));
        for (size_t d = 0; d < port->descriptors.size(); d++)
        {
            const NetPortDescriptor* descriptor = &port->descriptors[d];
            net_wire_put_u8(&writer, descriptor->device);
            net_wire_put_u8(&writer, descriptor->index);
            net_wire_put_u8(&writer, descriptor->id);
            net_wire_put_string(&writer, &descriptor->name);
        }
    }
    return writer_end(&writer);
}

bool net_settings_read_controllers(const uint8_t* data, size_t size, uint32_t* id, NetControllers* controllers)
{
    controllers->ports.clear();
    NetReader reader = reader_begin(data, size, NetSettingsMessage::controllers);
    *id = net_wire_get_u32(&reader);
    for (uint32_t pad = 0; pad < NET_CONTROLLERS_PADS; pad++)
    {
        controllers->pad_ports[pad] = static_cast<int8_t>(net_wire_get_u8(&reader));
    }
    uint8_t port_count = net_wire_get_u8(&reader);
    for (uint8_t p = 0; p < port_count && reader.ok; p++)
    {
        NetPort port;
        port.device = net_wire_get_u32(&reader);
        uint16_t device_count = net_wire_get_u16(&reader);
        for (uint16_t d = 0; d < device_count && reader.ok; d++)
        {
            NetPortDevice device;
            device.id = net_wire_get_u32(&reader);
            net_wire_get_text(&reader, &device.name);
            port.devices.push_back(device);
        }
        uint16_t descriptor_count = net_wire_get_u16(&reader);
        for (uint16_t d = 0; d < descriptor_count && reader.ok; d++)
        {
            NetPortDescriptor descriptor;
            descriptor.device = net_wire_get_u8(&reader);
            descriptor.index = net_wire_get_u8(&reader);
            descriptor.id = net_wire_get_u8(&reader);
            net_wire_get_text(&reader, &descriptor.name);
            port.descriptors.push_back(descriptor);
        }
        controllers->ports.push_back(port);
    }
    return net_wire_done(&reader);
}

size_t net_settings_write_port_device(uint8_t* out, size_t capacity, uint8_t pad, uint32_t device)
{
    NetWriter writer = writer_begin(out, capacity, NetSettingsMessage::port_device);
    net_wire_put_u8(&writer, pad);
    net_wire_put_u32(&writer, device);
    return writer_end(&writer);
}

bool net_settings_read_port_device(const uint8_t* data, size_t size, uint8_t* pad, uint32_t* device)
{
    NetReader reader = reader_begin(data, size, NetSettingsMessage::port_device);
    *pad = net_wire_get_u8(&reader);
    *device = net_wire_get_u32(&reader);
    return net_wire_done(&reader) && *pad < NET_CONTROLLERS_PADS;
}

// ---------------------------------------------------------------------------
// The host's side
// ---------------------------------------------------------------------------

static std::string real_text(double value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%g", value);
    return text;
}

static std::string label_text(const Localization* strings, uint32_t id)
{
    return id == SETTING_NO_TEXT ? std::string() : std::string(localization_get(strings, id));
}

bool net_settings_describe(const Settings* settings, const Localization* strings, const char* key, NetSetting* setting)
{
    const SettingDescriptor* descriptor = settings_find(settings, key);
    if (descriptor == nullptr)
    {
        return false;
    }
    setting->type = descriptor->type;
    setting->flags = descriptor->overridable ? NET_SETTING_OVERRIDABLE : 0;
    setting->label = label_text(strings, descriptor->label);
    setting->help = label_text(strings, descriptor->help);
    setting->min = descriptor->min;
    setting->max = descriptor->max;
    setting->step = descriptor->step;
    if (descriptor->type == SettingType::real)
    {
        setting->min_real = real_text(descriptor->min_real);
        setting->max_real = real_text(descriptor->max_real);
        setting->step_real = real_text(descriptor->step_real);
    }
    setting->choices.clear();
    for (uint32_t i = 0; i < descriptor->value_count; i++)
    {
        NetSettingChoice choice;
        choice.key = descriptor->values[i].key;
        choice.label = label_text(strings, descriptor->values[i].label);
        setting->choices.push_back(choice);
    }
    net_settings_values(settings, key, &setting->values);
    return true;
}

static void layer_value(const Settings* settings, SettingLayer layer, const char* key, uint8_t bit, uint8_t* set,
                        std::string* out)
{
    const char* value = settings_get_layer(settings, layer, key);
    if (value != nullptr)
    {
        *set = static_cast<uint8_t>(*set | bit);
        *out = value;
    }
    else
    {
        out->clear();
    }
}

void net_settings_values(const Settings* settings, const char* key, NetSettingValues* values)
{
    values->key = key;
    values->set = 0;
    layer_value(settings, SettingLayer::global, key, 1, &values->set, &values->global);
    // A layer that doesn't exist now (no core / game running) has nothing.
    if (!settings->core_section.empty())
    {
        layer_value(settings, SettingLayer::core, key, 2, &values->set, &values->core);
    }
    else
    {
        values->core.clear();
    }
    if (!settings->game_section.empty())
    {
        layer_value(settings, SettingLayer::game, key, 4, &values->set, &values->game);
    }
    else
    {
        values->game.clear();
    }
    SettingLayer source = SettingLayer::defaults;
    const char* effective = settings_get(settings, key, &source);
    values->effective = effective != nullptr ? effective : "";
    values->source = static_cast<uint8_t>(source);
}

NetContentStatus net_settings_apply(Settings* settings, SettingLayer layer, bool reset, const char* key,
                                    const char* value)
{
    if (settings_find(settings, key) == nullptr)
    {
        return NetContentStatus::not_found;
    }
    if (reset)
    {
        if (layer == SettingLayer::defaults ||
            (layer == SettingLayer::core && settings->core_section.empty()) ||
            (layer == SettingLayer::game && settings->game_section.empty()))
        {
            return NetContentStatus::refused;
        }
        settings_reset(settings, layer, key);
        return NetContentStatus::ok;
    }
    // settings_set refuses (and logs) invalid values, global-only settings
    // outside the global layer and layers that don't exist now.
    return settings_set(settings, layer, key, value) ? NetContentStatus::ok : NetContentStatus::refused;
}

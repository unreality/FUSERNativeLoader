#pragma once
// The settings registry and the Settings screen's Mods tab (settings.cpp): FnlApi's add_setting / setting / set_setting
// / enable_setting, and the loader's own features' rows. Game thread, or the loader's start (fnl_start).
#include <cstdint>

#include "fnl_mod.h"

// A row under its heading: a mod's (mod: its name and version), or a group of the loader's own (mod nullptr, group
// its name). The value is read from FuserNative.ini and delivered to row.on_change with FNL_SETTING_INITIAL. False
// for a malformed row or a key already registered.
auto settings_add(FnlMod* mod, const wchar_t* group, const FnlSetting& row) -> bool;

// A registered key's value; 0 for an unknown key.
auto settings_value(const char* key) -> int32_t;

// A registered key's value set, saved and shown; its on_change isn't called.
auto settings_set(const char* key, int32_t value) -> void;

// A key's row greyed out or back; the key need not be registered yet.
auto settings_enable(const char* key, bool enabled) -> void;

// fnl_start: the Settings screen's hooks (the Mods tab and its rows' events).
auto settings_start() -> void;

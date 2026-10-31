// A template FUSER mod on the C tier: fnl_mod.h is all it needs. Copy this folder, rename the target, the DLL and the
// mod.ini's dll= / name=, and build it (../README.md). It logs, adds a toggle to the Settings screen's Mods tab, and
// says hello again at the first engine tick while the toggle is on.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <string>

#include "fnl_mod.h"

namespace
{
    const FnlApi* g_api = nullptr;
    FnlMod* g_self = nullptr;
    bool g_on = true;

    void log(const std::wstring& text) { g_api->log(g_self, FNL_LOG_NORMAL, text.c_str()); }

    // The toggle: its saved value once inside add_setting (FNL_SETTING_INITIAL), then every change on the Mods tab
    void on_toggle(void*, int32_t value, int flags)
    {
        g_on = value != 0;
        if (!(flags & FNL_SETTING_INITIAL)) log(std::wstring(L"toggle ") + (g_on ? L"on" : L"off"));
    }

    void on_first_tick(void*, const void*)
    {
        if (g_on) log(L"first engine tick: the game is running");
    }
} // namespace

extern "C" FNL_MOD_EXPORT int fnl_mod_init(const FnlApi* api, FnlMod* self)
{
    // a loader older than the API this mod uses
    if (!api || api->abi < FNL_MOD_ABI || !FNL_API_HAS(api, add_setting)) return FNL_FAILED;
    g_api = api;
    g_self = self;
    log(std::wstring(L"hello from the template mod on ") + (api->game->build ? api->game->build : L"an unknown game build"));

    FnlSetting toggle{};
    toggle.size = sizeof(toggle);
    toggle.key = "TemplateModToggle"; // its FuserNative.ini key: unique across mods
    toggle.label = L"Template Mod Toggle";
    toggle.kind = FNL_SETTING_TOGGLE;
    toggle.default_value = 1;
    toggle.on_change = &on_toggle;
    api->add_setting(self, &toggle);

    api->on(self, FNL_EV_FIRST_TICK, &on_first_tick, nullptr);
    return FNL_OK;
}

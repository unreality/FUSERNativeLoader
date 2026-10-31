// FUSERMultiplayer.dll's entry: a game-stage mod on the C++ tier (mod.ini beside it), and its rows on the Settings
// screen's Mods tab. The loader saves them in FuserNative.ini, where host_hook.cpp reads each when it needs it (a
// transfer, a request, a shared mix), so a change never touches work under way.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "fnl_mod.h"

auto host_hook_start(const FnlApi* api, FnlMod* self) -> void;

namespace
{
    const FnlApi* g_api = nullptr;
    FnlMod* g_self = nullptr;

    // Download Songs: No / Temporarily / Permanently (the behaviour before the setting existed)
    constexpr int32_t kDownloadNo = 0, kDownloadPermanently = 2;

    auto choice(const char* key, const wchar_t* label, int32_t def, const wchar_t* const* choices, uint32_t count, const int32_t* values = nullptr,
                void (*on_change)(void*, int32_t, int) = nullptr) -> FnlSetting
    {
        FnlSetting s{};
        s.size = sizeof(s);
        s.key = key;
        s.label = label;
        s.kind = FNL_SETTING_CHOICE;
        s.default_value = def;
        s.choices = choices;
        s.choice_count = count;
        s.choice_values = values;
        s.on_change = on_change;
        return s;
    }

    auto add(const FnlSetting& row) -> void { g_api->add_setting(g_self, &row); }

    auto register_settings() -> void
    {
        static const wchar_t* const download[] = {L"No", L"Temporarily", L"Permanently"};
        static const wchar_t* const p2p[] = {L"Off", L"From Followed", L"Always"};
        static const wchar_t* const kbps_names[] = {L"256 KB/s", L"512 KB/s", L"1 MB/s", L"2 MB/s", L"4 MB/s"};
        static const int32_t kbps[] = {256, 512, 1024, 2048, 4096};
        static const wchar_t* const max_names[] = {L"1", L"2", L"3", L"4"};
        static const int32_t max[] = {1, 2, 3, 4};

        // StoreAutoInstall=0, the key Download Songs replaced, reads as No while DownloadSongs isn't set
        wchar_t old[8]{};
        const bool store_auto_off = g_api->ini_get(g_self, L"StoreAutoInstall", old, 8) != FNL_INI_ABSENT && old[0] == L'0';
        // P2P Song Download only matters while Download Songs isn't No, the two upload rows while Send Songs is on
        add(choice("DownloadSongs", L"Download Songs", store_auto_off ? kDownloadNo : kDownloadPermanently, download, 3, nullptr,
                   [](void*, int32_t v, int) { g_api->enable_setting(g_self, "P2PSongDownload", v != kDownloadNo); }));
        add(choice("DownloadMixSongs", L"Download Mix Songs", 1, download, 3));
        add(choice("P2PSongDownload", L"P2P Song Download", 1, p2p, 3));
        FnlSetting send{};
        send.size = sizeof(send);
        send.key = "SendSongs";
        send.label = L"Send Songs to Other Players";
        send.kind = FNL_SETTING_TOGGLE;
        send.on_change = [](void*, int32_t v, int) {
            g_api->enable_setting(g_self, "SendSongsKBps", v != 0);
            g_api->enable_setting(g_self, "SendSongsMax", v != 0);
        };
        add(send);
        add(choice("SendSongsKBps", L"Upload Speed Limit", 2048, kbps_names, 5, kbps));
        add(choice("SendSongsMax", L"Simultaneous Uploads", 2, max_names, 4, max));
    }
} // namespace

extern "C" FNL_MOD_EXPORT int fnl_mod_init(const FnlApi* api, FnlMod* self)
{
    if (!api || api->abi < FNL_MOD_ABI || !FNL_API_HAS(api, patch_strict)) return FNL_FAILED;
    g_api = api;
    g_self = self;
    register_settings();
    host_hook_start(api, self);
    return FNL_OK;
}

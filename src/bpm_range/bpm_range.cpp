// FUSERBpmRange: the game's BPM range widened from 90-157 to 60-200. An early-stage mod on the C tier (mod.ini beside
// it): the bytes are patched before WinMain, before the game runs, as the loader itself did.
//
// GetMaxBpm / GetMinBpm return immediates (157 / 90), and the two functions that clamp the current and a song's BPM
// compare and load the same bounds a few bytes into their bodies. Each build's sites are checked byte for byte; on an
// exe without them nothing is patched.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>
#include <string>

#include "fnl_mod.h"

namespace
{
    const FnlApi* g_api = nullptr;
    FnlMod* g_self = nullptr;
    int g_patched = 0;

    struct Build
    {
        const wchar_t* name;
        uintptr_t max_bpm, min_bpm;    // GetMaxBpm / GetMinBpm: their immediate's byte
        uintptr_t clamp_current;       // the current-BPM clamp ...
        uintptr_t clamp_song;          // ... and the song-BPM clamp: their bounds are searched for from here
        size_t scan;                   // bytes to search
    };
    const Build kBuilds[] = {
        {L"EGS", 0x594F51, 0x594F81, 0x5A1EB0, 0x59ADB0, 0x120},
        {L"Steam", 0x5CC581, 0x5CC5B1, 0x5D9454, 0x5D23E4, 0x80},
    };

    auto patch(uintptr_t rva, uint8_t value) -> void
    {
        uint8_t* at = g_api->game->base + rva;
        if (rva < g_api->game->image_size && g_api->patch(g_self, at, &value, 1)) ++g_patched;
    }

    // The first `pattern` in [start, start + scan): its byte at `offset` set to `value`
    auto patch_near(uintptr_t start, size_t scan, const uint8_t* pattern, size_t length, size_t offset, uint8_t value) -> void
    {
        if (start + scan > g_api->game->image_size) return;
        const uint8_t* from = g_api->game->base + start;
        for (size_t i = 0; i + length <= scan; ++i)
        {
            if (std::memcmp(from + i, pattern, length) != 0) continue;
            patch(start + i + offset, value);
            return;
        }
    }

    auto apply(const Build& b) -> void
    {
        static const uint8_t cmp_current[] = {0x83, 0xFA, 0x5A}; // cmp edx, 90
        static const uint8_t cmp_song[] = {0x83, 0xFB, 0x5A};    // cmp ebx, 90
        static const uint8_t mov_lower[] = {0xB8, 0x5A, 0x00, 0x00, 0x00}; // mov eax, 90
        static const uint8_t mov_upper[] = {0xB8, 0x9D, 0x00, 0x00, 0x00}; // mov eax, 157
        patch(b.max_bpm, 0xC8); // 157 -> 200
        patch(b.min_bpm, 0x3C); // 90 -> 60
        patch_near(b.clamp_current, b.scan, cmp_current, sizeof cmp_current, 2, 0x3C);
        patch_near(b.clamp_current, b.scan, mov_lower, sizeof mov_lower, 1, 0x3C);
        patch_near(b.clamp_current, b.scan, mov_upper, sizeof mov_upper, 1, 0xC8);
        patch_near(b.clamp_song, b.scan, cmp_song, sizeof cmp_song, 2, 0x3C);
        patch_near(b.clamp_song, b.scan, mov_lower, sizeof mov_lower, 1, 0x3C);
        patch_near(b.clamp_song, b.scan, mov_upper, sizeof mov_upper, 1, 0xC8);
    }
} // namespace

extern "C" FNL_MOD_EXPORT int fnl_mod_init(const FnlApi* api, FnlMod* self)
{
    if (!api || api->abi < FNL_MOD_ABI || !FNL_API_HAS(api, patch)) return FNL_FAILED;
    g_api = api;
    g_self = self;
    for (const Build& b : kBuilds)
    {
        // GetMaxBpm's immediate is still 157 (0x9D): this build's sites
        if (b.max_bpm >= api->game->image_size || api->game->base[b.max_bpm] != 0x9D) continue;
        apply(b);
        api->log(self, FNL_LOG_NORMAL, (std::wstring(L"BPM range 60-200 (") + b.name + L" sites, " + std::to_wstring(g_patched) + L" of 8 bytes patched)").c_str());
        return FNL_OK;
    }
    api->log(self, FNL_LOG_NORMAL, L"no known BPM sites in this exe - the range stays 90-157");
    return FNL_UNSUPPORTED;
}

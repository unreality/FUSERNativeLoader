// FUSERQualityOfLifeImprovements.dll's entry: a game-stage mod on the C++ tier (mod.ini beside it), and the genre 9
// requests patch.
//
// The community's tenth genre. Custom songs use EGenre::Num (9) as "Indie/Alternative" (its display text comes from
// a Hype.locres override, "Fuser Genres Rework"). The game reads genre 9 by name and display text like any other,
// except the multiplayer live-requests screen: its song gather (FUN_1406440b0) builds one entry per genre for
// i < 9 and then dereferences a missing entry, so a crate song of genre 9 crashed it. Widening that loop to i < 10
// gives genre 9 its own entry - a requestable genre with its display text. Each build's site is checked byte for byte
// before the bound is changed; values >= 10 are still clamped by fusernative.cpp, and genre 9 too when the patch did
// not apply (fnl_genre9_requestable, read by clamp_song_genres).
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>
#include <string>

#include "fnl_mod.h"

auto fusernative_start(const FnlApi* api, FnlMod* self) -> void;

bool fnl_genre9_requestable = false;

namespace
{
    auto apply_genre_requests_patch(const FnlApi* api, FnlMod* self) -> void
    {
        // inc r14d; cmp r14d,9; jl <loop top>  (identical in all three builds)
        static const uint8_t site[] = {0x41, 0xFF, 0xC6, 0x41, 0x83, 0xFE, 0x09, 0x0F, 0x8C, 0x12, 0xFF, 0xFF, 0xFF};
        const uintptr_t candidates[] = {
                0x644381, // EGS v1.4.2 and EGS CL1317808
                0x67A3E1, // Steam CL1317755
        };
        uint8_t* base = api->game->base;
        for (uintptr_t rva : candidates)
        {
            if (rva + sizeof(site) > api->game->image_size || std::memcmp(base + rva, site, sizeof(site)) != 0) continue;
            const uint8_t bound = 0x0A; // cmp r14d,10
            fnl_genre9_requestable = api->patch(self, base + rva + 6, &bound, 1) != 0 && base[rva + 6] == bound;
            api->log(self, FNL_LOG_NORMAL, fnl_genre9_requestable ? L"genre patch: the live-requests genre loop now covers genre 9"
                                                                  : L"genre patch: the live-requests genre loop could NOT be written - genre 9 stays clamped");
            return;
        }
        api->log(self, FNL_LOG_NORMAL, L"genre patch: live-requests genre loop not found - genre 9 stays clamped");
    }
} // namespace

extern "C" FNL_MOD_EXPORT int fnl_mod_init(const FnlApi* api, FnlMod* self)
{
    if (!api || api->abi < FNL_MOD_ABI || !FNL_API_HAS(api, enable_setting)) return FNL_FAILED;
    apply_genre_requests_patch(api, self);
    fusernative_start(api, self);
    return FNL_OK;
}

// FUSERFifthDeck.dll's entry: a game-stage mod on the C++ tier (mod.ini beside it).
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "fifth_deck.hpp"

extern "C" FNL_MOD_EXPORT int fnl_mod_init(const FnlApi* api, FnlMod* self)
{
    if (!api || api->abi < FNL_MOD_ABI || !FNL_API_HAS(api, patch_strict)) return FNL_FAILED;
    fifth_deck_start(api, self);
    return FNL_OK;
}

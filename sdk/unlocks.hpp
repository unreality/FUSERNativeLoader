#pragma once
// The loader's free unlocks for custom and event songs (src/unlocks.cpp), for C++ tier mods.
#include "fue/fue.hpp"

// A row for a custom song installed after launch, added to the live DT_UnlocksSongs: a copy of the free bornthisway
// row named `name`. Returns the row (an existing one if the name has a row already), or nullptr. UnlockMgr built its
// name map from the table once, at its init, so the caller must also add the row there.
FNL_API auto unlocks_add_song_row(const fue::CharType* name) -> fue::uint8*;

#pragma once
// The fifth deck (fifth_deck.cpp): a deck on track Deck0A (17), with its own audio track and mixer strip, shown on a
// widened desk.
#include "fnl_mod.h"

// Called once from fnl_mod_init (plugin_entry.cpp): prepares the hooks, registers the tick, and adds its Mods tab rows
// (Fifth Deck, Hot Clip Volume Normalisation) on a build it supports.
auto fifth_deck_start(const FnlApi* api, FnlMod* self) -> void;

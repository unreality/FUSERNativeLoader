#pragma once
// The mod manager (mod_manager.cpp): the mods in mods/<folder>/, each described by its mod.ini, and the FnlApi table
// (sdk/fnl_mod.h) they get.

// fnl_start, at the loader's startup before any pak is mounted: every mod.ini read, the load order resolved, supersedes=
// applied, then the early mods loaded. fue_ok: fue::init succeeded (a known build); without it no game stage follows.
auto fnl_mods_early(bool fue_ok) -> void;

// fue, at the first ProcessEvent or GameEngine::Tick, before the unreal-init callbacks: the game-stage mods loaded.
// fue_ready: the self-check passed; C++ tier mods are skipped otherwise.
auto fnl_mods_game_stage(bool fue_ready) -> void;

#include <string>
#include <vector>

#include "fnl_mod.h"

// The mods that loaded (fnl_mod_init returned FNL_OK), in load order.
auto fnl_mods_loaded() -> std::vector<FnlMod*>;

// A mod's heading on the Settings screen's Mods tab: its name and version.
auto fnl_mod_heading(const FnlMod* mod) -> std::wstring;

// Every mod installed (a folder in mods/ with a mod.ini), for the About FUSER popup: its name and version, and why it
// isn't running when it isn't. The ones the loader started come first, in load order.
auto fnl_mods_about() -> std::vector<std::wstring>;

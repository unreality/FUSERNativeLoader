#pragma once
// Every change winhttp.dll and its mods make to the game's code goes through here (C++17, for dllmain.cpp too).
//
// The patch journal keeps the bytes a hook or patch overwrote, as they were before the first change, and
// code_find_pattern compares against those wherever code was changed. So a pattern always matches the game as
// shipped, whatever was hooked before the search. A copy of the code taken at DLL_PROCESS_ATTACH would not do: the
// Steam exe's code is still encrypted then.
//
// Hooks share one funchook copy and its range check (hook_range.hpp), and each hooked function's owner is kept, so
// the log names both when two owners hook one function.
#include <cstddef>
#include <cstdint>

#include "fnl_mod.h"

// The game exe's image, read from its PE headers at the first call.
uint8_t* code_exe_base();
size_t code_exe_size();

// The first match of pattern ("48 8B ?? 05", spaces optional) in [start, start + size), clamped to the game exe;
// start nullptr searches the whole exe. nullptr when absent or malformed.
uint8_t* code_find_pattern(const char* pattern, const uint8_t* start, size_t size);

// All of hooks[] or none (FnlApi::hook). owner: "loader", "fue" or a mod's folder name.
bool code_hook(const char* owner, const FnlHook* hooks, size_t count);

// Bytes written over code, the previous ones journalled (FnlApi::patch).
bool code_patch(const char* owner, void* at, const void* bytes, size_t count);

// The game exe's import slot of function redirected (FnlApi::redirect_import).
bool code_redirect_import(const char* owner, const char* dll, const char* function, void* replacement, void** original);

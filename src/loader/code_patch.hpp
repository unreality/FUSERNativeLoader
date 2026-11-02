#pragma once
// Every change winhttp.dll and its mods make to the game's code goes through here (C++17, for dllmain.cpp too).
//
// The patch journal keeps the bytes a hook or patch overwrote, as they were before the first change, and
// code_find_pattern compares against those wherever code was changed. So a pattern always matches the game as
// shipped, whatever was hooked before the search. A copy of the code taken at DLL_PROCESS_ATTACH would not do: the
// Steam exe's code is still encrypted then.
//
// Hooks share one funchook copy and its range check (hook_range.hpp). Each change's owner and range are kept (hooks
// from the bytes their jumps actually overwrote), so the log names both owners when two changes overlap, and
// code_changed answers who changed a range.
//
// A prepared hook set (code_hook_prepare) sends each function's jump to a gate: a few instructions in the loader's
// own memory that go to the detour while the set's flag is on, else straight to funchook's trampoline. So switching a
// set on and off never writes the game's code.
#include <cstddef>
#include <cstdint>
#include <string>

#include "fnl_mod.h"

struct CodeHookSet;

// The game exe's image, read from its PE headers at the first call.
uint8_t* code_exe_base();
size_t code_exe_size();

// The first match of pattern ("48 8B ?? 05", spaces optional) in [start, start + size), clamped to the game exe;
// start nullptr searches the whole exe. nullptr when absent or malformed.
uint8_t* code_find_pattern(const char* pattern, const uint8_t* start, size_t size);

// All of hooks[] or none (FnlApi::hook). owner: "loader", "fue" or a mod's folder name.
bool code_hook(const char* owner, const FnlHook* hooks, size_t count);

// hooks[] prepared as one set, switched off; nullptr on failure (FnlApi::hook_prepare, FNL_HOOK_INSTALL_LATER).
CodeHookSet* code_hook_prepare(const char* owner, const FnlHook* hooks, size_t count, uint32_t flags);

// A set switched on or off by its owner (FnlApi::hook_enable).
bool code_hook_enable(const char* owner, CodeHookSet* set, bool enabled);

// How many owners changed bytes of [at, at + size), leaving out skip_owner when it isn't nullptr; their names,
// comma-separated, in *owners when that isn't nullptr (FnlApi::changed).
size_t code_changed(const char* skip_owner, const void* at, size_t size, std::string* owners);

// Bytes written over code, the previous ones journalled (FnlApi::patch). strict: refused when anyone but owner has
// changed one of them (FnlApi::patch_strict).
bool code_patch(const char* owner, void* at, const void* bytes, size_t count, bool strict = false);

// The game exe's import slot of function redirected (FnlApi::redirect_import).
bool code_redirect_import(const char* owner, const char* dll, const char* function, void* replacement, void** original);

// The game exe's import slot of function; nullptr when it isn't imported. dll as for code_redirect_import.
void** code_import_slot(const char* dll, const char* function);

// Whether p lies in the game exe's .text section.
bool code_in_exe_text(const void* p);

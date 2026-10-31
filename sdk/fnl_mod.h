#pragma once
// The FUSERNativeLoader mod interface (C tier): what winhttp.dll gives each mod DLL it loads.
//
// A mod is a folder in Fuser/Binaries/Win64/mods/ holding a mod.ini and the DLL it names. The DLL exports
//
//     FNL_MOD_EXPORT int fnl_mod_init(const FnlApi* api, FnlMod* self);
//
// which the loader calls once, at the mod's stage:
//   - early: inside the loader's DllMain (DLL_THREAD_ATTACH), before any pak is mounted. The mod's own DllMain must do
//     nothing, it may import only system DLLs and the /MD CRT, and it must not wait on other threads;
//   - game: at the first ProcessEvent or GameEngine::Tick, before the unreal-init callbacks. Needs a game build the
//     loader knows.
// It returns FNL_OK, FNL_UNSUPPORTED (nothing to do here, e.g. an unknown build) or FNL_FAILED.
//
// FnlApi only grows: a field exists when api->size covers it (FNL_API_HAS). Every string is NUL-terminated. Code is
// patched only through hook / hook_prepare / patch / patch_strict / redirect_import, so later find_pattern calls (the
// loader's and every mod's) still see the game's code as shipped, and changed() knows who changed what.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FNL_MOD_ABI 1

// The C++ tier (fue and the loader's services): a mod.ini sdk= value must equal this exactly.
#define FNL_SDK_VERSION 1

#define FNL_MOD_EXPORT __declspec(dllexport)

// fnl_mod_init results
#define FNL_OK 0
#define FNL_UNSUPPORTED 1
#define FNL_FAILED 2

// log levels
#define FNL_LOG_VERBOSE 0
#define FNL_LOG_NORMAL 1
#define FNL_LOG_WARNING 2
#define FNL_LOG_ERROR 3

// ini_get: the key is not in FuserNative.ini
#define FNL_INI_ABSENT ((size_t)-1)

typedef struct FnlMod FnlMod; // the loader's record of one mod

typedef enum FnlEvent {
    FNL_EV_UNREAL_INIT = 1, // once, at the first ProcessEvent or GameEngine::Tick, after the game-stage mods' init; arg NULL
    FNL_EV_FIRST_TICK = 2,  // once, at the first GameEngine::Tick after registration; arg: const float* delta seconds
    FNL_EV_TICK = 3,        // every GameEngine::Tick, before the engine's; arg: const float* delta seconds
} FnlEvent;

typedef struct FnlGameInfo {
    uint32_t size;
    uint8_t* base;           // the game exe's image
    size_t image_size;       // its SizeOfImage
    uint32_t timestamp;      // its PE TimeDateStamp
    const wchar_t* build;    // "EGS v1.4.2", "EGS CL1317808", "Steam CL1317755"; NULL for a build the loader doesn't know
    int debug;               // nonzero in the diagnostic (FNL_DEBUG) loader
} FnlGameInfo;

typedef struct FnlHook {
    const char* name;        // for the log
    void** target;           // in: the function; out: the trampoline that calls the original
    void* detour;
} FnlHook;

typedef struct FnlHookSet FnlHookSet; // hooks prepared together by hook_prepare, switched on and off together

// hook_prepare flags
#define FNL_HOOK_INSTALL_LATER 1 // write the jumps at the set's first enable instead of in hook_prepare

// changed flags
#define FNL_CHANGED_BY_OTHERS 1  // leave the calling mod's own changes out

// changed: the flags held a bit this loader doesn't know (one from a newer SDK)
#define FNL_CHANGED_ERROR ((size_t)-1)

typedef enum FnlSettingKind {
    FNL_SETTING_TOGGLE = 1,  // stored 1 / 0
    FNL_SETTING_CHOICE = 2,  // a dropdown; stored: the entry's index, or its choice_values entry
    FNL_SETTING_SLIDER = 3,  // min..max in steps of step; stored: the value
} FnlSettingKind;

// on_change flags
#define FNL_SETTING_INITIAL 1 // the delivery at registration (before any row exists), not a change
#define FNL_SETTING_SAVED 2   // the value is in FuserNative.ini, not just the default

// A row on the Settings screen's Mods tab, under the mod's heading, saved as key=value in FuserNative.ini. A key is
// written only once its value is changed (on the Mods tab or with set_setting), and stays written after that.
typedef struct FnlSetting {
    uint32_t size;                   // sizeof(FnlSetting)
    const char* key;                 // FuserNative.ini key, unique across mods
    const wchar_t* label;
    FnlSettingKind kind;
    int32_t default_value;           // when the key isn't in FuserNative.ini (a stored value, as above)
    const wchar_t* const* choices;   // FNL_SETTING_CHOICE: the entries' labels
    uint32_t choice_count;
    const int32_t* choice_values;    // FNL_SETTING_CHOICE: each entry's stored value, or NULL for its index. A stored
                                     // value no entry has is kept until changed; the row shows the nearest entry
    int32_t min, max, step;          // FNL_SETTING_SLIDER
    // On the game thread at every change, and once inside add_setting with FNL_SETTING_INITIAL. Not called for
    // set_setting.
    void (*on_change)(void* ctx, int32_t value, int flags);
    void* ctx;
} FnlSetting;

typedef struct FnlApi {
    uint32_t size;
    uint32_t abi;            // FNL_MOD_ABI of the loader
    const char* loader_version;
    const FnlGameInfo* game;

    // A line in FuserNative.log, prefixed with the mod's folder name.
    void (*log)(FnlMod* self, int level, const wchar_t* text);

    // The first match of pattern ("48 8B ?? 05", spaces optional, ?? any byte) in [start, start + size), clamped to the
    // game exe; start NULL searches the whole exe. Bytes patched through this API compare as shipped. name: unused,
    // kept for the caller's own logging. NULL when not found or the pattern is malformed.
    uint8_t* (*find_pattern)(FnlMod* self, const char* name, const char* pattern, const uint8_t* start, size_t size);

    // Hooks every function in hooks[], or none: each target must be in jump range of its trampoline. Nonzero on success,
    // with each *target the trampoline; on failure the targets are unchanged. A function another mod hooked already is hooked
    // again (the detours chain, in install order) and the log names both.
    int (*hook)(FnlMod* self, const FnlHook* hooks, size_t count);

    // Writes bytes over the game's code (any protection), recording what was there. Nonzero on success. The log warns
    // when another mod (or the loader) changed any of those bytes already; patch_strict refuses instead.
    int (*patch)(FnlMod* self, void* at, const void* bytes, size_t count);

    // Points the game exe's import of function from dll (a case-insensitive prefix such as "api-ms-win-crt-runtime";
    // NULL for any DLL) at replacement, with *original set to what the slot held. Nonzero when the import was found.
    // Regular imports only: a delay-load slot would be rewritten by its own first call.
    int (*redirect_import)(FnlMod* self, const char* dll, const char* function, void* replacement, void** original);

    // cb(ctx, arg) on the event. Register from fnl_mod_init or an FNL_EV_UNREAL_INIT callback.
    void (*on)(FnlMod* self, FnlEvent event, void (*cb)(void* ctx, const void* arg), void* ctx);

    // The mod's folder.
    const wchar_t* (*mod_dir)(FnlMod* self);

    // A FuserNative.ini value: copied into out (truncated to cap - 1 characters, NUL-terminated) and its full length
    // returned, or FNL_INI_ABSENT.
    size_t (*ini_get)(FnlMod* self, const wchar_t* key, wchar_t* out, size_t cap);

    // A Mods tab row (FnlSetting). Register from fnl_mod_init or an FNL_EV_UNREAL_INIT callback; rows appear in
    // registration order. Nonzero on success; zero for a key already registered or a malformed row.
    int (*add_setting)(FnlMod* self, const FnlSetting* row);

    // Any registered key's current value (a stored value); 0 for an unknown key.
    int32_t (*setting)(FnlMod* self, const char* key);

    // Sets a registered key's value: saved, and its row updated. Its on_change isn't called.
    void (*set_setting)(FnlMod* self, const char* key, int32_t value);

    // Greys a key's row out (enabled 0) or back. Allowed before the key is registered.
    void (*enable_setting)(FnlMod* self, const char* key, int enabled);

    // Publishes a table of function pointers (a struct whose first member is its uint32_t size, so it can only grow)
    // under a dotted name owned by this mod, e.g. "fnl.mp.session". Nonzero on success; zero for a name already
    // provided. Provide from fnl_mod_init.
    int (*provide_interface)(FnlMod* self, const char* name, uint32_t version, const void* table);

    // A provided table whose version is at least min_version, or NULL (not provided, or older). From fnl_mod_init when
    // the provider loads first (requires= or after= it), otherwise from an FNL_EV_UNREAL_INIT callback.
    const void* (*find_interface)(FnlMod* self, const char* name, uint32_t min_version);

    // Prepares every hook in hooks[], or none, as one set that starts switched off; NULL on failure. Each trampoline
    // is reserved here, in jump range of its function, so prepare from fnl_mod_init: memory near the exe runs out as
    // the game runs (under Proton especially). While the set is off, each function runs as shipped; while it is on,
    // its detour runs. The jumps are written here, or at the first hook_enable with FNL_HOOK_INSTALL_LATER, and
    // never again: hook_enable only flips a flag, so a Mods tab toggle never rewrites running code. Each *target
    // becomes the trampoline when the jumps are written (until then it is still the function). With
    // FNL_HOOK_INSTALL_LATER, a function changed by someone else in between is prepared again at that enable, which
    // fails if its new trampoline is out of jump range. Hooks on a function another mod hooked chain, as with hook.
    // A flag this loader doesn't know (one from a newer SDK) fails the call: NULL, nothing prepared.
    FnlHookSet* (*hook_prepare)(FnlMod* self, const FnlHook* hooks, size_t count, uint32_t flags);

    // Switches a set's detours on (enabled nonzero) or off, from any thread; a call already inside a detour finishes
    // there. Nonzero on success; zero when the set isn't this mod's or its jumps couldn't be written (it stays off).
    int (*hook_enable)(FnlMod* self, FnlHookSet* set, int enabled);

    // How many owners (mod folder names, or the loader's "loader" and "fue") have changed a byte of [at, at + size)
    // through this API: hooks, patches, import slots; 0 for none. FNL_CHANGED_BY_OTHERS leaves this mod out. owners,
    // when not NULL, gets their names, comma-separated, in the order they first changed the range (truncated to
    // cap - 1 characters, NUL-terminated). A hook counts from when its jumps are written, as the bytes they
    // overwrote. Changes made outside this API (another hooking library, a private funchook copy) aren't known. A flag
    // this loader doesn't know (one from a newer SDK) returns FNL_CHANGED_ERROR, with owners empty.
    size_t (*changed)(FnlMod* self, const void* at, size_t size, uint32_t flags, char* owners, size_t cap);

    // patch, only over bytes no one else has changed: zero, and nothing written, when changed() with
    // FNL_CHANGED_BY_OTHERS finds anything in the range. The mod's own earlier changes don't count.
    int (*patch_strict)(FnlMod* self, void* at, const void* bytes, size_t count);
} FnlApi;

#define FNL_API_HAS(api, field) ((api)->size >= offsetof(FnlApi, field) + sizeof(((FnlApi*)0)->field))

typedef int (*FnlModInitFn)(const FnlApi* api, FnlMod* self);

#ifdef __cplusplus
}
#endif

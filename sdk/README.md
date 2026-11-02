# FUSERNativeLoader SDK

Mods for FUSER loaded by FUSERNativeLoader (`winhttp.dll`). A mod is a DLL plus a `mod.ini` in its own folder under
`Fuser\Binaries\Win64\mods\`; the loader starts it, gives it an API, lists it in Settings > About FUSER and shows its
settings on the Settings screen's **Mods** tab. The bundled mods (Quality of Life Improvements, Fifth Deck,
Multiplayer, BPM Range, Fixes) are built exactly this way.

Two tiers:

- **C tier** (`fnl_mod.h` only): logging, game build info, pattern search, hooks, code patches, import redirects,
  engine events, `FuserNative.ini` values, Mods tab settings and interfaces between mods. Stable: the `FnlApi` table
  only grows, so a C tier mod keeps working on later loaders. Start from `template/`.
- **C++ tier** (also `fue/`, `data_table.hpp`, `custom_content.hpp`, `content/`, `unlocks.hpp`, `json_lite.hpp`):
  UObject reflection, ProcessEvent and native hooks, data tables and custom content types, linked from
  `winhttp.dll`'s import library. Tied to one SDK version (`FNL_SDK_VERSION`): rebuild when it changes.

## Install layout

```
Fuser\Binaries\Win64\mods\<Name>\mod.ini, <Name>.dll (+ .pdb)
Fuser\Content\Paks\mods\<Name>_P.pak, .sig          (cooked assets, if any; the engine mounts them at startup)
```

Turn a mod off with `enabled=0` in its `mod.ini`, or by renaming its folder to end in `.disabled`.
`Fuser\Binaries\Win64\FuserNative.log` has a `[mods]` line for each mod (loaded, or why not) and every line a mod
logs, prefixed with its folder name.

## mod.ini

| Key | |
|---|---|
| `name=` | shown on the Mods tab and in About FUSER (default: the folder name) |
| `dll=` | the DLL in the folder; it exports `fnl_mod_init` |
| `version=` | shown with the name |
| `stage=` | `game` (default) or `early` |
| `api=` | the lowest `FNL_MOD_ABI` it needs (default 1) |
| `sdk=` | C++ tier only: the `FNL_SDK_VERSION` it was built with |
| `requires=` | folder names that must load first; if one doesn't, this mod isn't loaded |
| `after=` | folder names to load after when they load (order only) |
| `before=` | folder names to load before when they load (order only): a mod can go ahead of one it doesn't control. An early-stage mod already loads before every game-stage one; a game-stage mod can't go before an early one (the log says so) |
| `supersedes=` | files, relative to the game folder, renamed to `<file>.disabled` before any pak mounts (an older version's leftovers) |
| `enabled=` | `0` leaves the mod out |

Lists are comma-separated; `;` or `#` starts a comment. Mods load in dependency order, then by folder name.

## Stages

- **game** (the default): `fnl_mod_init` runs at the first ProcessEvent / GameEngine::Tick, before the unreal-init
  callbacks. Needs a game build the loader knows.
- **early**: on the game's main thread, after its static initialisers and before `WinMain`, so before any pak is
  mounted, however long the early mods take; for engine-level fixes that must be in place before the game starts (a
  pak list fix, say). C tier only. The engine isn't set up yet: use `GetCommandLineW` rather than `FCommandLine`, and
  `mod_dir` rather than the working directory. If the loader can't start there, it starts on the first new thread
  instead, inside its DllMain, where paks may be mounting already (`FuserNative.log` warns). So the DLL's own DllMain
  must do nothing, it may import only system DLLs and the `/MD` CRT, and it must not wait on other threads.

`fnl_mod_init(const FnlApi* api, FnlMod* self)` returns `FNL_OK`, `FNL_UNSUPPORTED` (nothing to do on this build) or
`FNL_FAILED`. Check `api->abi` and `FNL_API_HAS(api, field)` for anything newer than the loader you built against.

## The C tier (`fnl_mod.h`)

- **Patching:** patch only through `hook`, `hook_prepare`, `patch`, `patch_strict` and `redirect_import`. The loader
  records what each change overwrote and who made it, so `find_pattern` (yours and everyone's) always matches the
  game's code as shipped, whatever was hooked first, and the log warns when two owners change the same bytes. `hook`
  installs a batch all or none, checks each trampoline is in jump range, and chains onto another mod's hook of the
  same function. A private funchook copy (or any other hooking library) skips all of that.
- **Flags:** a flag a loader doesn't know fails the call (`hook_prepare` returns NULL, `changed` returns
  `FNL_CHANGED_ERROR`), so a mod built against a newer SDK finds out instead of getting the old behaviour.
- **Hooks you can switch:** `hook_prepare` reserves a batch's trampolines now and returns a set that starts off;
  `hook_enable(set, 1 / 0)` switches its detours on and off at any time, from any thread, without writing code (each
  jump leads to a small loader-owned gate that checks the set's flag). Prepare from `fnl_mod_init`: memory near the exe
  runs out as the game runs, under Proton especially. The jumps are written in `hook_prepare`, or with
  `FNL_HOOK_INSTALL_LATER` at the first enable (a function someone hooked in between is prepared again then); each
  `*target` becomes the trampoline when they are. A Mods tab toggle's `on_change` just calls `hook_enable`.
- **Who changed it:** `changed(at, size, flags, owners, cap)` returns how many owners (mod folder names, `loader`,
  `fue`) changed a byte of the range, and names them; `FNL_CHANGED_BY_OTHERS` leaves your own changes out. Check it
  before reading values out of code you found with `find_pattern` (offsets, call targets): if someone else changed
  it, back off. `patch_strict` is `patch` that refuses, writing nothing, when someone else changed one of the bytes.
- **Events:** `on(FNL_EV_UNREAL_INIT | FNL_EV_FIRST_TICK | FNL_EV_TICK, ...)`, registered from `fnl_mod_init` or an
  unreal-init callback.
- **Settings:** `add_setting` puts a toggle, dropdown or slider on the Mods tab under the mod's heading, saved as
  `key=value` in `FuserNative.ini` once changed. `on_change` gets the saved value inside `add_setting`
  (`FNL_SETTING_INITIAL`) and then every change; `set_setting` and `enable_setting` set or grey out a row from code.
  A mod with no rows has no heading on the Mods tab.
- **Interfaces:** `provide_interface(name, version, table)` publishes a struct of function pointers (its first member
  is its size, so it only grows); `find_interface(name, min_version)` returns it, or NULL. Find it from
  `fnl_mod_init` when the provider loads first (`requires=` or `after=` it), else at unreal-init.
- **Files:** `mod_dir` (the mod's folder), `ini_get` (any `FuserNative.ini` value).

An early-stage fix looks like this: in `fnl_mod_init`, `find_pattern` its functions in `api->game->base`, check what it
found, then `hook` them as one batch and `redirect_import` any imports it wraps (e.g. `"kernel32"`,
`"EnterCriticalSection"`); log and return `FNL_UNSUPPORTED` when the code isn't there.

## The C++ tier

Include `sdk\` and link `winhttp.lib` (from the SDK's `lib\`); put `sdk=<FNL_SDK_VERSION>` in `mod.ini`. MSVC x64,
`/MD`, C++20 (`std::format`). The declarations marked `FNL_API` are the supported exports of `winhttp.dll`. It also
exports funchook's own functions (`funchook_create`, `funchook_prepare`, ...), a side effect of the prebuilt funchook
library it links: they are **not supported**, not declared in the SDK, and may be removed in any release without an
`FNL_SDK_VERSION` change. Hook through `FnlApi` instead. `winhttp.lib` shares
its name with the Windows SDK's WinHTTP import library: a mod that also calls the system's WinHTTP loads it from
`System32` with `LoadLibraryEx` and `GetProcAddress`, as the multiplayer mod does.

- `fue/fue.hpp`: UObject views, properties, `ProcessEvent`, FName / FString / FText, object lookup and construction,
  `Hook::Register*` (ProcessEvent pre/post, engine tick), `UObjectGlobals::RegisterHook` (a native UFunction's
  pre/post), `on_unreal_init`, logging.
- `data_table.hpp`: loading tables, walking and adding rows, inserting into a manager's `TMap<FName, ...>`.
- `custom_content.hpp`, `content/content_types.hpp`: custom content. `content::register_type` adds a content type
  (a `content.json` `"type"`) from `fnl_mod_init`: its unlock table, manifest check, rows and live registration. A
  content pak can come from another player or a server, so a type checks its manifest and files in `validate`.
- `unlocks.hpp`: a free unlock row for a custom song added after launch.

A loader built with another `FNL_SDK_VERSION` refuses the mod before loading it (`[mods] <Name>: not loaded - built
for SDK ...`).

## Build the template

```powershell
cmake -S template -B build\template -A x64
cmake --build build\template --config Release
```

Copy `build\template\Release\TemplateMod.dll` and `template\mod.ini` to `Fuser\Binaries\Win64\mods\TemplateMod\`
and start the game: the log shows `[TemplateMod] hello from the template mod on <build>`, and Settings > Mods has a
Template Mod heading with its toggle.

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
| `supersedes=` | files, relative to the game folder, renamed to `<file>.disabled` before any pak mounts (an older version's leftovers) |
| `enabled=` | `0` leaves the mod out |

Lists are comma-separated; `;` or `#` starts a comment. Mods load in dependency order, then by folder name.

## Stages

- **game** (the default): `fnl_mod_init` runs at the first ProcessEvent / GameEngine::Tick, before the unreal-init
  callbacks. Needs a game build the loader knows.
- **early**: inside the loader's DllMain, before any pak is mounted; for engine-level fixes that must be in place
  before the game starts (a pak list fix, say). C tier only. The DLL's own DllMain must do nothing, it may import
  only system DLLs and the `/MD` CRT, and it must not wait on other threads.

`fnl_mod_init(const FnlApi* api, FnlMod* self)` returns `FNL_OK`, `FNL_UNSUPPORTED` (nothing to do on this build) or
`FNL_FAILED`. Check `api->abi` and `FNL_API_HAS(api, field)` for anything newer than the loader you built against.

## The C tier (`fnl_mod.h`)

- **Patching:** patch only through `hook`, `patch` and `redirect_import`. The loader records what each change
  overwrote, so `find_pattern` (yours and everyone's) always matches the game's code as shipped, whatever was hooked
  first. `hook` installs a batch all or none, checks each trampoline is in jump range, and chains onto another mod's
  hook of the same function (the log names both). A private funchook copy skips all of that.
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
`/MD`, C++20 (`std::format`). The declarations marked `FNL_API` are what `winhttp.dll` exports. That library shares
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

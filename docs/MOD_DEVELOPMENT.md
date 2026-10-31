# Mod development: a worked example

This tutorial adds a complete new feature to FUSERNativeLoader, from installing the tools to a new `.pak` that the
loader picks up. The feature is **Key Suitability**:

- every song item in the freestyle HUD gets a small face in its top-right corner;
- the face shows how far the song's key is from the session's **master key**, as a raw distance in semitones:
  - **happy** for the same key (distance 0);
  - **neutral** for 1–3 semitones;
  - **frown** for 4–6 semitones (6 is as far as two keys can be apart);
- a **Key Suitability** on/off toggle in Settings > Mods turns it on and off.

The distance is deliberately naive. There is no music theory here (no relative minors, no tonics), just the gap
between two notes on the circle of 12: `d = |songKey - masterKey| mod 12`, then `min(d, 12 - d)`. So C and B are 1
apart, not 11.

At the end you will have built:

| Output | Contains |
|---|---|
| `out\Release\Paks\mods\FUSERQualityOfLifeImprovements_P.pak` (+ `.sig`) | the mod's cooked assets, now with the new ones: a widget, a Blueprint function library, three textures |
| `out\Release\mods\FUSERQualityOfLifeImprovements\FUSERQualityOfLifeImprovements.dll` | the Quality of Life Improvements mod, with the native side of the feature compiled in |

---

## 1. How a feature is put together

Every FuserNative feature has the same three parts. It helps to know them before starting:

1. **Cooked assets** (`*.uasset` / `*.uexp`), authored in a UE 4.24 editor project (the **modkit**,
   `modkit/FuserModkit/`), cooked, and packed into a `*_P.pak`.
   - The loader lets any pak whose name contains `_P` through FUSER's signature check.
   - The engine mounts every pak in `Fuser\Content\Paks\` and its subfolders (the mods' paks are in `mods\`), so the
     assets appear in the game under `/Game/...`.
2. **A call point**: a function on a modkit **Blueprint Function Library** whose Blueprint body only returns
   defaults. At runtime the native code finds it by path and points its `Func` at a C++ thunk, marking it
   `FUNC_Native`. From then on, any Blueprint that calls it runs our C++. This is how a widget reaches game data that
   the modkit cannot see, because the modkit has no FUSER classes. Here that data is the song's key and the master
   key.
3. **Native code** in `src/qol/fusernative.cpp`, compiled into the Quality of Life Improvements mod
   (`FUSERQualityOfLifeImprovements.dll`, which the loader loads from `mods\`). It:
   - loads the assets;
   - binds the call point;
   - injects the widget into the game's own UI (here, every HUD song item as the game sets it up);
   - keeps the widget up to date;
   - adds the Settings row.

The existing key/BPM line is built exactly this way: `BPL_FuserNative` / `WBP_FuserKeyBpm` in the modkit, and
`bind()`, `ensure_ready()` and `on_setup_layout()` in `fusernative.cpp`. The new feature follows the same pattern
under its own names.

---

## 2. Install the tools

You need a Windows 10/11 x64 PC with a working FUSER install (Epic Games Store v1.4.2 or Steam).

1. **Visual Studio 2022** (Community or Build Tools), with the **Desktop development with C++** workload. It
   provides MSVC x64, a Windows SDK and CMake. Build from a **Developer PowerShell for VS 2022**, so that `cmake` is
   on `PATH`. A separate CMake 3.20+ from cmake.org also works.
2. **Python 3.10+** from python.org. Tick *Add python.exe to PATH*, or rely on the `py` launcher. The pak tool uses
   only the standard library.
3. **Unreal Engine 4.24.** In the Epic Games Launcher, open *Unreal Engine > Library*, add an engine version and
   pick **4.24.3**.
   - It must be **4.24**: FUSER is a UE 4.24 game, and assets cooked by another engine version will not load.
   - The optional components (starter content, templates, target platforms other than Windows) are not needed.
4. **Tell the scripts where the game is.** Point them at the folder that contains `Fuser\`:
   ```powershell
   [Environment]::SetEnvironmentVariable('FUSER_GAME_ROOT', 'D:\Games\FUSER', 'User')   # then open a new shell
   ```
5. **Check the base build before changing anything:**
   ```powershell
   cd <repo>\FUSERNativeLoader
   .\build.ps1                 # out\Release\: winhttp.dll, mods\<Name>\, Paks\mods\<Name>_P.pak / .sig
   .\install.ps1               # into $env:FUSER_GAME_ROOT (keeps your old winhttp.dll as winhttp.dll.fnl-backup)
   ```
   Start the game, then open `Fuser\Binaries\Win64\FuserNative.log`. It should show `exe verified`,
   `self-check passed` and `[settings] Mods tab: ...` once the Settings screen is built.

---

## 3. Names used in this tutorial

The native code finds everything **by path**, so these names must be typed exactly (no spaces, same case).

| Asset (modkit path) | Kind | Purpose |
|---|---|---|
| `/Game/Mods/KeySuitability/T_KS_Happy` | Texture | face: same key |
| `/Game/Mods/KeySuitability/T_KS_Neutral` | Texture | face: 1–3 semitones |
| `/Game/Mods/KeySuitability/T_KS_Sad` | Texture | face: 4–6 semitones |
| `/Game/Mods/KeySuitability/BPL_KeySuitability` | Blueprint Function Library | call point `GetKeySuitability` |
| `/Game/Mods/KeySuitability/WBP_KeySuitability` | Widget Blueprint | the face; function `SetSong` |

Rules the modkit imposes:

- **Everything goes under `/Game/Mods/`.** The project cooks that folder whatever else references it
  (`+DirectoriesToAlwaysCook=(Path="/Game/Mods")` in `Config/DefaultGame.ini`).
- **Game objects are passed as plain `Object` references.** The modkit doesn't know FUSER's classes, such as
  `CelSongSet`.
- **Call-point results are out-parameters**, that is, a function's *Outputs*. The C++ thunk writes them.

---

## 4. Author the assets in the modkit

### 4.1 Open the project

1. Open `FUSERNativeLoader\modkit\FuserModkit\FuserModkit.uproject`.
   - If Windows asks which engine to use, or the project won't open, right-click the `.uproject`, choose *Switch
     Unreal Engine version…*, and pick 4.24.
   - The first open compiles shaders, which takes a few minutes.
2. In the **Content Browser**, open `Content > Mods`, right-click, choose *New Folder*, and name it
   **`KeySuitability`**. Open it.

### 4.2 The three face textures

1. Make three **64 × 64 PNGs with a transparent background**, one per face, in any image editor: a simple happy
   face, a straight-mouth neutral face and a frown. Bold shapes read best at HUD size.
2. Drag them into the `KeySuitability` folder, or use *Import*. Rename them **`T_KS_Happy`**, **`T_KS_Neutral`** and
   **`T_KS_Sad`**.
3. Open each texture and set:
   - **Compression Settings:** `UserInterface2D (RGBA)`
   - **Mip Gen Settings:** `NoMipmaps`
   - **Texture Group:** `UI`
   - **Never Stream:** ticked (the pixel data then stays inside the cooked `.uexp`)
4. **Save All.**

### 4.3 The call point: `BPL_KeySuitability`

1. In the folder, choose *Add New > Blueprints > Blueprint Function Library*, and name it **`BPL_KeySuitability`**.
   Open it.
2. Rename the default function (`NewFunction_0`) to **`GetKeySuitability`**.
3. With the function selected, in *Details*:
   - **Inputs:** add `SongSet`, type **Object > Object Reference**.
   - **Outputs:** add `Distance` (**Integer**) and `Found` (**Boolean**).
4. The body: connect the function entry to the **Return Node**. On the Return Node, set **Distance = -1** and leave
   **Found** unticked.

   This body never runs in the game, because the C++ thunk replaces it. It is only what a caller would get if the
   loader weren't there: a harmless "not found".
5. **Compile**, then **Save**.

### 4.4 The widget: `WBP_KeySuitability`

1. Choose *Add New > User Interface > Widget Blueprint* and name it **`WBP_KeySuitability`**. Open it.
2. **Designer tab:**
   1. Delete the default Canvas Panel, if there is one.
   2. From the *Palette*, drag a **Size Box** in as the root. Tick *Width Override* and *Height Override*, and set
      both to **32**.
   3. Drag an **Image** into the Size Box. Rename it **`FaceImage`** and tick **Is Variable** at the top of
      *Details*. In *Appearance > Brush*, set the Image Size to 32 × 32.
3. **Graph tab: variables.** Add three variables of type **Texture 2D > Object Reference**, named `HappyFace`,
   `NeutralFace` and `SadFace`. Compile, then set their **Default Value**s to `T_KS_Happy`, `T_KS_Neutral` and
   `T_KS_Sad`.

   These defaults are what make the cooker include the textures: an unreferenced texture would not be cooked.
4. **Graph tab: the function.**
   1. Add a function named **`SetSong`** with one input, `SongSet` (**Object > Object Reference**), and no outputs.
   2. Build this chain, and **connect every white exec wire**. The Blueprint compiler silently drops nodes that
      aren't on the exec chain, and a `SetSong` that does nothing is the most common mistake here.

   ```text
   SetSong(SongSet)
    └─ Is Valid (SongSet) ──► Branch
         ├─ False ─► FaceImage.Set Visibility (Collapsed)
         └─ True ──► BPL_KeySuitability::Get Key Suitability (SongSet)  → Distance, Found
                      └─► Branch (Found)
                           ├─ False ─► FaceImage.Set Visibility (Collapsed)
                           └─ True ──► Branch (Distance == 0)
                                        ├─ True ──► FaceImage.Set Brush from Texture (HappyFace) ─┐
                                        └─ False ─► Branch (Distance >= 4)                        │
                                                     ├─ True ──► Set Brush from Texture (SadFace) ─┤
                                                     └─ False ─► Set Brush from Texture (NeutralFace)
                                                                                                  ▼
                                                              FaceImage.Set Visibility (Not Hit-Testable (Self & All Children))
   ```

   - **Is Valid:** use the *Utilities* node that takes an Object.
   - **Get Key Suitability:** drag it from the *My Blueprint* panel of `BPL_KeySuitability`, or search for it by
     name. It appears once that library has been compiled and saved.
   - **Set Brush from Texture:** leave *Match Size* unticked.
   - **The last Set Visibility:** all three texture branches join it.
   - **The thresholds** live here. To change what counts as "close", edit the two comparisons.
5. **Compile**, then **Save All**.

### 4.5 Cook

1. Choose **File > Cook Content for Windows**, then wait for *Cook complete* in the Output Log (*Window > Developer
   Tools > Output Log*). There should be no errors.
2. The cooked files are under:
   ```text
   modkit\FuserModkit\Saved\Cooked\WindowsNoEditor\FuserModkit\Content\Mods\KeySuitability\
   ```
   There is a `.uasset` + `.uexp` pair for each of the five assets. A texture can also have a `.ubulk`; if one
   appears, keep it with the others.
3. Copy that folder's files into the repository, **keeping the `Content\Mods\KeySuitability` path**:
   ```powershell
   $cooked = 'modkit\FuserModkit\Saved\Cooked\WindowsNoEditor\FuserModkit\Content\Mods\KeySuitability'
   $dest   = 'assets\FUSERQualityOfLifeImprovements\Content\Mods\KeySuitability'
   New-Item -ItemType Directory -Force $dest | Out-Null
   Copy-Item "$cooked\*" $dest
   ```
   The path under `Content\` becomes the in-game asset path: `Content\Mods\KeySuitability\WBP_KeySuitability.uasset`
   is `/Game/Mods/KeySuitability/WBP_KeySuitability`.

The editor-side sources (`modkit\FuserModkit\Content\Mods\KeySuitability\*.uasset`) are the real source of the
feature: keep them in the repository alongside the cooked copies.

---

## 5. The pak

Nothing to change: `build.ps1` packs every file under each bundled mod's `assets\<Mod>\Content\` into
`out\<Configuration>\Paks\mods\<Mod>_P.pak` (+ `.sig`), and `install.ps1` and the player package put it in
`Fuser\Content\Paks\mods\`. The Key Suitability assets copied in section 4 ride in
`FUSERQualityOfLifeImprovements_P.pak`, beside the key/BPM line's. A feature that should install or turn off on its
own belongs in a mod of its own: a folder under `src\` with its `mod.ini`, a CMake target, and an entry in
`build.ps1`'s and `install.ps1`'s `$bundledMods`.

A pak's name must end in **`_P`**. The loader's pak hooks (`src/dllmain.cpp`) wave through the signature check
for paks with `_P` in the name, and the mount point `../../../Fuser/Content/` maps the files onto `/Game/`.

---

## 6. The native code

All changes are in `src/qol/fusernative.cpp`. Each step below says where the code goes, relative to existing
code you can search for.

### 6.1 The setting

Next to the other settings flags, after the line `bool g_search_enabled = true;         // the song search box`:

```cpp
    bool g_key_suitability = true;        // Key Suitability faces on the HUD song items (Mods tab toggle)
```

The value is a row on the Settings screen's Mods tab (section 6.4). The loader's settings (`src/loader/settings.cpp`)
save it in `Fuser\Binaries\Win64\FuserNative.ini` as `KeySuitability=1` or `0`, once it has been changed.

### 6.2 The feature

Paste this block just **before** the comment line
`// The highlight animations drive the Genre . Year row's slot Padding`, which follows the deferred song-item setup
section.

```cpp
    // ---------------------------------------------------------------- key suitability -----------
    // A face in the top-right corner of every HUD song item: how far the song's key is from the session's master key,
    // as a raw semitone distance around the circle of 12 keys (0 = same key ... 6 = a tritone away). The widget and
    // its call point come from FUSERQualityOfLifeImprovements_P.pak (modkit: /Game/Mods/KeySuitability/); without that pak the
    // feature just stays off.
    constexpr const CharType* kKsLibClassPath = STR("/Game/Mods/KeySuitability/BPL_KeySuitability.BPL_KeySuitability_C");
    constexpr const CharType* kKsLibCdoPath = STR("/Game/Mods/KeySuitability/BPL_KeySuitability.Default__BPL_KeySuitability_C");
    constexpr const CharType* kKsFnSuitability = STR("/Game/Mods/KeySuitability/BPL_KeySuitability.BPL_KeySuitability_C:GetKeySuitability");
    constexpr const CharType* kKsWidgetClassPath = STR("/Game/Mods/KeySuitability/WBP_KeySuitability.WBP_KeySuitability_C");
    constexpr const CharType* kKsFnSetSong = STR("/Game/Mods/KeySuitability/WBP_KeySuitability.WBP_KeySuitability_C:SetSong");
    constexpr uint8_t kHitTestInvisible = 3;             // ESlateVisibility: shown, never takes the mouse
    constexpr float kKsSize = 32.0f, kKsMargin = 8.0f;  // the face's size and its inset from the corner

    UFunction* g_ks_fn = nullptr;       // BPL_KeySuitability::GetKeySuitability, bound to ks_thunk
    UClass* g_ks_widget = nullptr;      // WBP_KeySuitability_C
    UFunction* g_ks_set_song = nullptr; // its SetSong(SongSet)
    bool g_ks_attempted = false;
    FWeakObjectPtr g_ks_audio{};        // the live UHypeAudio (the session's key / tempo)
    int32 g_ks_master_key = -1;         // the master key ks_tick last saw

    struct KsItem
    {
        FWeakObjectPtr item{}, face{}, song{}; // a pooled song item, our face on it, and the song it shows now
    };
    std::vector<KsItem> g_ks_items{};

    // The session's master key (EKey 0-11), or -1 while there is no live HypeAudio
    auto ks_master_key() -> int32
    {
        UObject* audio = g_ks_audio.Get();
        if (!audio)
        {
            static uint64 last_search = ~0ull; // one object scan per tick at most
            if (last_search == g_tick) return -1;
            last_search = g_tick;
            std::vector<UObject*> all{};
            UObjectGlobals::FindAllOf(STR("HypeAudio"), all);
            for (UObject* o : all)
            {
                if (o->GetName().rfind(STR("Default__"), 0) == 0 || o->GetOuterPrivate()->GetName().rfind(STR("Default__"), 0) == 0) continue;
                audio = o; // the one HypeGameInstance.theHypeAudio points to (not a class default / template)
                break;
            }
            if (!audio) return -1;
            g_ks_audio = FWeakObjectPtr(audio);
        }
        static UFunction* get_key = find_fn(STR("/Script/Hype.HypeAudio:GetKey"));
        PCall c(get_key);
        if (!c.call(audio)) return -1;
        const uint8 key = c.get<uint8>(STR("ReturnValue"));
        return key < kKeyNone ? key : -1;
    }

    auto ks_distance(int32 a, int32 b) -> int32
    {
        const int32 d = std::abs(a - b) % 12;
        return std::min(d, 12 - d); // around the circle: C and B are 1 apart, not 11
    }

    // GetKeySuitability(SongSet) -> Distance (int), Found (bool): the call point WBP_KeySuitability.SetSong calls
    auto ks_thunk(UObject* /*Context*/, FFrame& Stack, void* result) -> void
    {
        NativeArgs args(g_ks_fn, Stack, result);
        UObject* song = args.in_object(STR("SongSet"));
        const SongKeyBpm k = compute_key_bpm(song);
        const int32 master = ks_master_key();
        const bool found = k.found && master >= 0;
        if (auto* a = args.out(STR("Distance"))) *reinterpret_cast<int32*>(a) = found ? ks_distance(k.key, master) : -1;
        if (auto* p = static_cast<FBoolProperty*>(find_param(g_ks_fn, STR("Found"))))
        {
            if (auto* a = args.out(p)) p->SetPropertyValue(a, found);
        }
    }

    // Load FUSERQualityOfLifeImprovements_P.pak's library and widget and bind the call point (tried once: a missing pak must not turn
    // every song setup into a failed package load)
    auto ks_ready() -> bool
    {
        if (g_ks_fn && g_ks_widget && g_ks_set_song) return true;
        if (g_ks_attempted) return false;
        g_ks_attempted = true;
        UClass* lib = load_class(kKsLibClassPath);
        UFunction* fn = lib ? find_fn(kKsFnSuitability) : nullptr;
        g_ks_widget = load_class(kKsWidgetClassPath);
        g_ks_set_song = g_ks_widget ? find_fn(kKsFnSetSong) : nullptr;
        if (!fn || !g_ks_widget || !g_ks_set_song)
        {
            Output::send<LogLevel::Warning>(STR("[QoL] key suitability: assets missing (GetKeySuitability {}, WBP_KeySuitability {}, SetSong {}) - is "
                                                "FUSERQualityOfLifeImprovements_P.pak installed?\n"),
                                            fn != nullptr, g_ks_widget != nullptr, g_ks_set_song != nullptr);
            return false;
        }
        for (UObject* o : {static_cast<UObject*>(lib), static_cast<UObject*>(fn), static_cast<UObject*>(g_ks_widget), static_cast<UObject*>(g_ks_set_song),
                           UObjectGlobals::StaticFindObject(nullptr, nullptr, kKsLibCdoPath)})
            root(o); // nothing the game owns references them: keep them from being garbage-collected
        fn->SetFuncPtr(&ks_thunk);
        fn->GetFunctionFlags() |= FUNC_Native; // script callers only use Func for FUNC_Native functions
        g_ks_fn = fn;
        Output::send<LogLevel::Verbose>(STR("[QoL] key suitability: ready (GetKeySuitability bound)\n"));
        return true;
    }

    // Our face into the song item's SongItemCanvasPanel (the whole song rectangle), pinned to its top-right corner
    auto ks_place(UObject* canvas, UObject* face) -> bool
    {
        static UFunction* add = find_fn(STR("/Script/UMG.CanvasPanel:AddChildToCanvas"));
        PCall c(add);
        c.set<UObject*>(STR("Content"), face);
        UObject* slot = c.call(canvas) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
        if (!slot) return false;
        const float anchors[4] = {1.0f, 0.0f, 1.0f, 0.0f}; // FAnchors {Minimum, Maximum}: both the top-right corner
        const float alignment[2] = {1.0f, 0.0f};          // the face's own top-right corner sits on the anchor
        const float position[2] = {-kKsMargin, kKsMargin};
        const float size[2] = {kKsSize, kKsSize};
        PCall(find_fn(STR("/Script/UMG.CanvasPanelSlot:SetAnchors"))).copy_in(STR("InAnchors"), anchors).call(slot);
        PCall(find_fn(STR("/Script/UMG.CanvasPanelSlot:SetAlignment"))).copy_in(STR("InAlignment"), alignment).call(slot);
        PCall(find_fn(STR("/Script/UMG.CanvasPanelSlot:SetPosition"))).copy_in(STR("InPosition"), position).call(slot);
        PCall(find_fn(STR("/Script/UMG.CanvasPanelSlot:SetSize"))).copy_in(STR("InSize"), size).call(slot);
        return true;
    }

    // Show / hide the face for the setting and the item's content, and let the widget pick its face
    auto ks_refresh(const KsItem& k) -> void
    {
        UObject* face = k.face.Get();
        if (!face) return;
        UObject* song = k.song.Get();
        const bool show = g_key_suitability && song;
        set_visibility(face, show ? kHitTestInvisible : kCollapsed);
        if (!show) return;
        PCall c(g_ks_set_song);
        c.set<UObject*>(STR("SongSet"), song).call(face);
    }

    // Every song-item setup (run_setup): songs get a face, saved moments (the same pooled items) hide it
    auto ks_on_setup(UObject* item, UObject* song, bool is_song) -> void
    {
        if (!ks_ready()) return;
        UObject* canvas = obj_prop(item, STR("SongItemCanvasPanel"));
        if (!canvas) return;
        UObject* face = find_child_of_class(canvas, g_ks_widget);
        if (!face)
        {
            if (!is_song) return;
            face = create_widget(item, g_ks_widget, owning_player(item));
            if (!face || !ks_place(canvas, face)) return;
        }
        auto it = std::find_if(g_ks_items.begin(), g_ks_items.end(), [&](const KsItem& k) { return k.item.Get() == item; });
        if (it == g_ks_items.end()) it = g_ks_items.insert(g_ks_items.end(), KsItem{FWeakObjectPtr(item)});
        it->face = FWeakObjectPtr(face);
        it->song = FWeakObjectPtr(is_song ? song : nullptr);
        ks_refresh(*it);
    }

    // The master key changes during a session: re-pick every face when it does (checked every 15 ticks)
    auto ks_tick() -> void
    {
        if (!g_key_suitability || g_ks_items.empty() || g_tick % 15 != 0) return;
        const int32 key = ks_master_key();
        if (key == g_ks_master_key) return;
        g_ks_master_key = key;
        std::erase_if(g_ks_items, [](const KsItem& k) { return !k.item.Get(); });
        for (const KsItem& k : g_ks_items) ks_refresh(k);
    }

    // The Settings toggle changed
    auto ks_apply_setting() -> void
    {
        for (const KsItem& k : g_ks_items) ks_refresh(k);
    }
```

What each piece does, and which existing helper it builds on:

| Function | Job | Built from |
|---|---|---|
| `ks_master_key` | Finds the live `UHypeAudio` (the object `HypeGameInstance.theHypeAudio` points to, whose key the in-game key/tempo readout shows) and calls its reflected `GetKey()`. `EKey` runs C=0, Db=1 … B=11. | `UObjectGlobals::FindAllOf`, `PCall` |
| `ks_thunk` | The C++ body of `GetKeySuitability`. It reads `SongSet`, takes the song's key from its cels exactly as the key/BPM line does, and writes the `Distance` / `Found` out-parameters. `NativeArgs` hides the two ways a native can be called: from script, with parameters evaluated from the caller's bytecode, or through ProcessEvent. | `NativeArgs`, `compute_key_bpm`, `find_param` |
| `ks_ready` | Loads the two classes from the pak (`load_class` goes through the asset registry, so it works for any mounted pak), roots them against garbage collection, and binds the call point: `SetFuncPtr` + `FUNC_Native`. It is tried once, so a missing pak costs one warning, not a load attempt per song. | `load_class`, `find_fn`, `root` |
| `ks_place` | Adds the face to the song item's `SongItemCanvasPanel` and pins it 8 px inside the top-right corner, at 32 × 32, through `UCanvasPanelSlot`'s reflected setters. | `PCall::copy_in` |
| `ks_on_setup` | Runs on every song-item setup. Song items are **pooled** (the same widget shows song after song, or a saved moment), so it reuses its face when there is one and records which song the item shows now. | `find_child_of_class`, `create_widget` |
| `ks_refresh` | Shows or hides the whole face for the setting, then calls the widget's `SetSong`, which picks the texture. | `set_visibility` |
| `ks_tick` | Polls the master key 4 times a second (every 15 ticks) and refreshes every face when it changes, e.g. when the player turns the key dial. | `g_tick` |

### 6.3 Call it for every song item

`run_setup` is where every HUD song-item setup ends up: immediately for the first 8 of a frame, deferred for the
rest. Above `auto run_setup(UObject* item, UObject* song, bool is_song) -> void`, declare the new function, since
it is defined further down:

```cpp
    auto ks_on_setup(UObject* item, UObject* song, bool is_song) -> void; // key suitability, below
```

Inside `run_setup`, right after the line `on_setup_layout(item, song, is_song);`:

```cpp
            ks_on_setup(item, song, is_song);
```

### 6.4 The Settings row

Rows are registered with the loader's settings (`src/loader/settings.hpp`). It builds them on the Settings screen's
Mods tab under the group's heading, puts the value on them each time the screen opens, saves every change and calls
back with the new value.

In `register_settings`, after the `SearchBar` row, add:

```cpp
        add(row("KeySuitability", STR("Key Suitability"), FNL_SETTING_TOGGLE, 1, [](void*, int32_t v, int flags) {
            g_key_suitability = v != 0;
            if (!(flags & FNL_SETTING_INITIAL)) ks_apply_setting();
        }));
```

The callback runs once inside `add` (`FnlApi::add_setting`) with `FNL_SETTING_INITIAL`, at unreal-init before any song item exists,
so that call only sets the flag; after that it runs on every change. A mod DLL registers its rows the same way
through `FnlApi::add_setting` (`sdk/fnl_mod.h`).

### 6.5 Tick and preload

In `fusernative_unreal_init`'s engine-tick callback:

1. After the preload log line (`[QoL] preload: key/BPM classes {} ({:.1f} ms)`), load the feature's assets
   while the game is still loading, rather than when the first song item appears:
   ```cpp
                           ks_ready(); // FUSERQualityOfLifeImprovements_P.pak's library + widget, also while the game is still loading
   ```
2. After `setup_tick();`, add:
   ```cpp
                       ks_tick();
   ```

That's all of the code. The tutorial's code was compile-checked against this tree.

---

## 7. Build and install

```powershell
.\build.ps1
```

`out\Release\` now holds:

| File | From |
|---|---|
| `mods\FUSERQualityOfLifeImprovements\FUSERQualityOfLifeImprovements.dll` (+ `.pdb`) | the mod with Key Suitability compiled in |
| `Paks\mods\FUSERQualityOfLifeImprovements_P.pak` / `.sig` | `assets\FUSERQualityOfLifeImprovements\Content`, now with the Key Suitability assets |
| `winhttp.dll` and the other mods | unchanged |

To install into your game:

```powershell
.\install.ps1          # the loader, every bundled mod and their paks
```

`.\build.ps1 -Package` puts all of this in `out\FUSERNativeLoader-<version>.zip` for players.

---

## 8. Test

1. **Start the game and check `Fuser\Binaries\Win64\FuserNative.log`** for:
   - `key suitability: ready (GetKeySuitability bound)`;
   - `[settings] Quality of Life Improvements <version>: KeySuitability = On (default)` (`(saved)` once it has been
     changed).
2. **Enter freestyle.** Each song item has a face in its top-right corner. Turn the key dial: within a quarter of a
   second, the songs in the new key turn happy and those far away frown.
3. **Settings > Mods > Key Suitability.** Off hides every face and On brings them back. Restart the game: the
   choice sticks (`KeySuitability=` in `FuserNative.ini`).

For more detail, build and install the diagnostic loader (`.\build.ps1 -Configuration Debug` and
`.\install.ps1 -Configuration Debug`). It adds a console window with the loader's tracing, plus debug keys; F8, for
example, dumps crate edit to the log.

### Troubleshooting

| Symptom | Likely cause |
|---|---|
| Log: `key suitability: assets missing …` | `FUSERQualityOfLifeImprovements_P.pak` in `Content\Paks\mods` is an old one (rebuild and reinstall), or an asset / function name differs from section 3 (a space, a typo, a different folder). The log line says which of the three was not found. |
| No faces, and no `ready` or `missing` line | `ks_ready` never ran: check the step 6.5 and 6.3 edits. |
| Faces never change, or stay hidden | `SetSong`'s exec chain isn't fully connected (step 4.4), or `Found` is false: some songs have no key in their data, and before a session starts there is no master key. |
| A face shows a white square | The texture wasn't cooked (the widget's texture variables have no default values), or its `.ubulk` wasn't copied. |
| The game crashes at startup after installing | Put the previous loader back with `.\install.ps1 -Restore`. |

---

## 9. What changed

| Where | What |
|---|---|
| `modkit/FuserModkit/Content/Mods/KeySuitability/` | the five editor assets (source) |
| `assets/FUSERQualityOfLifeImprovements/Content/Mods/KeySuitability/` | their cooked `.uasset` / `.uexp` (build input) |
| `src/qol/fusernative.cpp` | the setting, the feature block, the `run_setup` call, the Settings row, the tick |

To go further, keep the same shape:
- more call points on the library (each one a thunk plus a line in `ks_ready`);
- more widgets injected from `ks_on_setup`;
- more Mods tab rows (`add` with `FNL_SETTING_CHOICE` or `FNL_SETTING_SLIDER`).

To keep a larger feature's code in its own file, move the block into a new `src/qol/*.cpp`. The helpers it uses
(`PCall`, `NativeArgs`, `load_class`, …) then need to move from `fusernative.cpp`'s anonymous namespace into a
shared header.

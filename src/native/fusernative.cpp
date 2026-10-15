// FuserNative - FUSER's native call points and UI extensions, built into winhttp.dll on the fue core (fue provides
// the UE4SS API names the code was first written against).
//
// Key/BPM line: native call points + a modkit widget injected into the HUD song items.
//
// Call points: the cooked library /Game/Mods/FuserNative/BPL_FuserNative
//   BPL_FuserNative_C::GetSongKeyBpm(SongSet, __WorldContext) -> out Key, Mode, Bpm (int), Found (bool)
//   BPL_FuserNative_C::GetSongKeyText(SongSet, __WorldContext) -> out KeyText (Text)
// Their Blueprint bodies only return defaults. Binding makes them genuinely NATIVE: Func -> our thunk AND
// FUNC_Native set. Both flags matter: ProcessEvent always goes through Func, but a script caller
// (UObject::CallFunction) only does so for FUNC_Native functions - otherwise it runs the Blueprint body
// through ProcessLocalScriptFunction and never looks at Func. So each thunk has two calling modes:
//   - ProcessEvent: Stack is the callee's own frame (Node == the function); params are in Locals and
//     outputs are reached through OutParms;
//   - script call: Stack is the CALLER's frame; each parameter is an expression in the caller's
//     bytecode that we Step (inputs into a scratch buffer, outputs as lvalues via
//     MostRecentPropertyAddress), then skip EX_EndFunctionParms - exactly what UHT exec thunks do.
//
// Consumer: /Game/Mods/FuserNative/WBP_FuserKeyBpm (modkit widget: root TextBlock KeyBpmText, function
// SetSong(SongSet, StyleSource) that calls the two library functions). A ProcessEvent post-hook on
// USongListItemWidget::SetupLayoutForSong (a BlueprintImplementableEvent the game's C++ fires through
// ProcessEvent) adds one WBP_FuserKeyBpm per WBP_SongItem_Horizontal_Diamond (into VerticalBox_Text,
// the parent of SongMetadata_HorizontalBox) and calls SetSong(cellSongSet, SongYear). SetupLayoutForMoment
// collapses it (song items are pooled and reused for saved moments).
//
// Real song tabs: OnInitialized raises WBP_UberHudList's SongWidgetPool to the crate's
// song+moment slots; PreConstruct (and every GameFlow-driven rebuild) extends its `Containers` array with extra
// WBP_UberHudList_Container instances so RebuildUberList lays out ceil(songs/8) song tabs + instruments + moments.
//
// Song search: the game's own search field (WBP_Component_TextEntryBox) in crate edit's song
// library; typing rebuilds the lists via the pane, and a CelMgr::Sort post-hook keeps title/artist matches and applies
// the key: / bpm: filter words. The same hook orders the library by Key or BPM, two entries added to the pane's Sort
// dropdown (native BuildSortDropdown / RefreshSortLabel bodies).
//
// Song tiles: a WBP_FuserKeyBpm above the title of every crate-editor song tile, added from a SetCelSongSet post-hook
// (the crate's slots) and the library grid items' OnListItemInitialized.
//
// Crate-edit song tabs: WBP_Screen_CrateEdit gets ceil(songs/8) song tabs and ceil(moments/8) moment
// tabs (sized from the largest crate), built at its OnInitialized; the six Blueprint functions with literal tab indices
// are replaced by native C++ bodies (Func + FUNC_Native), so the edited crate's own capacity decides which tabs show.
//
// F6 (game thread): load + bind if needed, ProcessEvent self-test of both functions, then a script-call
//     test: create a WBP_FuserKeyBpm, call SetSong, read KeyBpmText back.
// F7 (game thread): reflection probe of the loaded songs.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <cstring>
#include <vector>

#include "fue/fue.hpp"

#ifndef FNL_VERSION
#define FNL_VERSION "dev" // CMake passes the VERSION file's contents
#endif
#define FNL_WIDEN2(x) L##x
#define FNL_WIDEN(x) FNL_WIDEN2(x)

using namespace fue;

extern bool fnl_genre9_requestable; // dllmain.cpp: the live-requests genre loop covers genre 9 (see clamp_song_genres)

namespace
{
    constexpr const CharType* kLibClassPath = STR("/Game/Mods/FuserNative/BPL_FuserNative.BPL_FuserNative_C");
    constexpr const CharType* kLibCdoPath = STR("/Game/Mods/FuserNative/BPL_FuserNative.Default__BPL_FuserNative_C");
    constexpr const CharType* kFnKeyBpm = STR("/Game/Mods/FuserNative/BPL_FuserNative.BPL_FuserNative_C:GetSongKeyBpm");
    constexpr const CharType* kFnKeyText = STR("/Game/Mods/FuserNative/BPL_FuserNative.BPL_FuserNative_C:GetSongKeyText");
    constexpr const CharType* kWidgetClassPath = STR("/Game/Mods/FuserNative/WBP_FuserKeyBpm.WBP_FuserKeyBpm_C");
    constexpr const CharType* kFnSetSong = STR("/Game/Mods/FuserNative/WBP_FuserKeyBpm.WBP_FuserKeyBpm_C:SetSong");

    constexpr uint8_t kKeyNone = 12;  // EKey::Num
    constexpr uint8_t kModeUnset = 2; // EKeyMode::Num
    constexpr uint8_t kVisible = 0;   // ESlateVisibility::Visible
    constexpr uint8_t kCollapsed = 1; // ESlateVisibility::Collapsed
    constexpr int32 kSongsTabType = 0; // ECollectionTabType: songs (BPFL_CollectionTabType's first branch)
    constexpr int kLogLimit = 6;      // per-category cap on repeated log lines
    constexpr int kPaddingLogLimit = 40;
    constexpr int32 kItemsPerContainer = 8; // WBP_UberHudList's GetContainerIdxForItemIdx IntConst
    constexpr int32 kStockContainers = 5;   // WBP_UberHudList_Container_1..5
    const CharType* const kKeyNames[12] = {STR("C"), STR("Db"), STR("D"), STR("Eb"), STR("E"), STR("F"),
                                           STR("Gb"), STR("G"), STR("Ab"), STR("A"), STR("Bb"), STR("B")};

    std::atomic<bool> g_probe_requested{false};
    std::atomic<bool> g_bind_test_requested{false};
    std::atomic<bool> g_crate_edit_dump_requested{false};

    bool g_bound = false;
    bool g_ready_attempted = false;
    UFunction* g_fn_kb = nullptr;
    UFunction* g_fn_kt = nullptr;
    UClass* g_widget_class = nullptr;
    UFunction* g_fn_set_song = nullptr;
    FName g_name_setup_song{};
    FName g_name_setup_moment{};
    FName g_name_diamond{};
    FName g_name_set_padding{};
    FName g_name_vbox_slot{};
    FName g_name_meta_row{};
    FName g_name_uberlist{}, g_name_on_initialized{}, g_name_preconstruct{}, g_name_construct{}, g_name_gameflow_event{};
    int g_log_tabs = 0;
    FName g_name_collection_pane{}, g_name_crate_edit{}, g_name_get_list_data{};
    int g_log_pe_calls = 0, g_log_script_calls = 0, g_log_inject = 0, g_log_set_song = 0, g_log_padding = 0;

    // Mod settings kept in FuserNative.ini; KeyBpmDisplay lives with its swap further down
    constexpr int32 kKeyLabelsFlats = 0, kKeyLabelsSharps = 1, kKeyLabelsBoth = 2;
    int32 g_key_labels = kKeyLabelsFlats; // stock
    bool g_search_enabled = true;         // the song search box
    bool g_extra_sort = true;             // Key and BPM in crate edit's Sort dropdown (ExtraSortOptions=0: ini only)
    // New-crate slot counts: -1 = never set in our Settings rows, the game's own value stands. Only
    // FuserNative.ini holds them (Game.ini is reset by the game, so it is neither read nor written for this).
    int32 g_new_crate_songs = -1, g_new_crate_moments = -1;
    // Song delivery: Download Songs and Download Mix Songs (No / Temporarily / Permanently), and Send Songs to
    // Other Players. FUSERMultiplayer.dll reads the same keys from FuserNative.ini; these are the screen's copies.
    constexpr int32 kDownloadNo = 0, kDownloadTemporarily = 1, kDownloadPermanently = 2;
    int32 g_download_songs = kDownloadPermanently;     // default: the behaviour before the setting existed
    int32 g_download_mix_songs = kDownloadTemporarily; // a shared mix's songs: kept for this run unless asked
    bool g_send_songs = false;                     // opt-in
    // Upload Speed Limit (SendSongsKBps, each transfer's pacing cap) and Simultaneous Uploads (SendSongsMax): read by
    // FUSERMultiplayer.dll when a transfer starts / a request arrives, so a change never touches transfers under way.
    // A hand-edited SendSongsKBps outside the list is kept until the row is changed (the row shows the nearest).
    constexpr int32 kSendKbpsOptions[] = {256, 512, 1024, 2048, 4096};
    constexpr int32 kSendMaxMin = 1, kSendMaxMax = 4;
    int32 g_send_kbps = 2048, g_send_max = 2; // the defaults FUSERMultiplayer.dll uses without the keys
    StringType g_key_text_cache[12][2]{}; // game_key_text; cleared when Key Labels changes

    // ---------------------------------------------------------------- song data (reflection) -----
    struct SongKeyBpm
    {
        int32 key = -1, mode = -1, bpm = -1;
        bool found = false;
    };

    // Key/Mode from the first cel with a set mode AND a real key (some songs' beat cel has
    // mode set but Key=None); BPM from the first cel that has one.
    auto compute_key_bpm(UObject* song) -> SongKeyBpm
    {
        SongKeyBpm r{};
        if (!song) return r;
        auto* cels = song->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("Cels"));
        if (!cels) return r;
        for (int32 i = 0; i < cels->Num(); ++i)
        {
            UObject* cel = (*cels)[i];
            if (!cel) continue;
            auto* key = cel->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Key"));
            auto* mode = cel->GetValuePtrByPropertyNameInChain<uint8_t>(STR("mode"));
            auto* bpm = cel->GetValuePtrByPropertyNameInChain<int32>(STR("BPM"));
            if (bpm && *bpm > 0 && r.bpm < 0) r.bpm = *bpm;
            if (!r.found && key && mode && *mode != kModeUnset && *key < kKeyNone)
            {
                r.key = *key;
                r.mode = *mode;
                r.found = true;
            }
        }
        return r;
    }

    // compute_key_bpm walks every cel's properties by name; the crate editor's sort, filter and tile labels ask for
    // hundreds of songs per list rebuild. Entries are checked against the song's shortName, so an address reused by
    // another song set is recomputed. A song with neither a key nor a BPM is never cached (its cels may not be loaded).
    struct CachedKeyBpm
    {
        FName name{};
        SongKeyBpm value{};
    };
    std::unordered_map<UObject*, CachedKeyBpm> g_key_bpm_cache{};

    auto cached_key_bpm(UObject* song) -> SongKeyBpm
    {
        if (!song) return {};
        auto* n = song->GetValuePtrByPropertyNameInChain<FName>(STR("songShortName"));
        const FName name = n ? *n : FName{};
        auto it = g_key_bpm_cache.find(song);
        if (it != g_key_bpm_cache.end() && it->second.name == name) return it->second.value;
        const SongKeyBpm r = compute_key_bpm(song);
        if (r.found || r.bpm > 0) g_key_bpm_cache[song] = CachedKeyBpm{name, r};
        return r;
    }

    auto game_key_text(int32 key, int32 mode) -> StringType; // defined after PCall

    auto key_text_for(const SongKeyBpm& k) -> StringType
    {
        if (!k.found) return StringType{};
        return game_key_text(k.key, k.mode);
    }

    auto song_name(UObject* song) -> StringType
    {
        auto* n = song ? song->GetValuePtrByPropertyNameInChain<FName>(STR("songShortName")) : nullptr;
        return n ? n->ToString() : StringType{STR("?")};
    }

    // ---------------------------------------------------------------- reflection helpers ---------
    // Never UE4SS's UStruct::FindProperty: it caches (struct pointer, name) -> FProperty* in a static map that is never
    // invalidated, and /Game Blueprint classes (with their functions and properties) are unloaded and reloaded on map
    // changes - a reloaded struct at a reused address would get a freed FProperty back.
    auto find_field(UStruct* st, const CharType* name) -> FProperty*
    {
        if (!st) return nullptr;
        const FName n(name, FNAME_Add);
        for (FProperty* p : TFieldRange<FProperty>(st, EFieldIterationFlags::IncludeSuper | EFieldIterationFlags::IncludeDeprecated))
        {
            if (p->GetFName() == n) return p;
        }
        return nullptr;
    }

    auto find_param(UFunction* fn, const CharType* name) -> FProperty*
    {
        return find_field(fn, name);
    }

    auto find_fn(const CharType* path) -> UFunction*
    {
        return UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path);
    }

    auto obj_prop(UObject* obj, const CharType* name) -> UObject*
    {
        auto* p = obj ? obj->GetValuePtrByPropertyNameInChain<UObject*>(name) : nullptr;
        return p ? *p : nullptr;
    }

    auto parms_size(UFunction* fn) -> size_t
    {
        int32 end = 0;
        for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
        {
            end = std::max(end, p->GetOffset_Internal() + p->GetSize());
        }
        return static_cast<size_t>(end);
    }

    // Parameters come first in a UFunction's property chain (ProcessEvent relies on this too).
    template <typename F>
    auto for_each_param(UFunction* fn, F&& f) -> void
    {
        for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
        {
            if (!p->HasAnyPropertyFlags(CPF_Parm)) break;
            f(p);
        }
    }

    // A ProcessEvent call with a correctly constructed/destroyed parameter buffer. Every value is
    // touched only through the engine's property ops, so FText/struct params are safe.
    class PCall
    {
      public:
        explicit PCall(UFunction* fn) : m_fn(fn), m_buf(fn ? parms_size(fn) + 16 : 0, 0)
        {
            if (m_fn) for_each_param(m_fn, [&](FProperty* p) { p->InitializeValue_InContainer(m_buf.data()); });
        }
        ~PCall()
        {
            if (m_fn) for_each_param(m_fn, [&](FProperty* p) { p->DestroyValue_InContainer(m_buf.data()); });
        }
        PCall(const PCall&) = delete;
        PCall& operator=(const PCall&) = delete;

        auto ok() const -> bool { return m_fn != nullptr; }
        auto addr(const CharType* name) -> uint8*
        {
            FProperty* p = find_param(m_fn, name);
            return p ? p->ContainerPtrToValuePtr<uint8>(m_buf.data()) : nullptr;
        }
        template <typename T>
        auto set(const CharType* name, T value) -> PCall&
        {
            if (auto* a = addr(name)) *reinterpret_cast<T*>(a) = value;
            return *this;
        }
        template <typename T>
        auto get(const CharType* name) -> T
        {
            auto* a = addr(name);
            return a ? *reinterpret_cast<T*>(a) : T{};
        }
        // Copy a value of the same type (e.g. a struct read from another object) into a parameter
        auto copy_in(const CharType* name, const void* src) -> PCall&
        {
            FProperty* p = find_param(m_fn, name);
            if (p && src) p->CopyCompleteValue(p->ContainerPtrToValuePtr<void>(m_buf.data()), src);
            return *this;
        }
        auto get_text(const CharType* name) -> StringType
        {
            FProperty* p = find_param(m_fn, name);
            if (!p) return {};
            FText read{};
            p->CopyCompleteValue(&read, p->ContainerPtrToValuePtr<void>(m_buf.data()));
            return read.ToString();
        }
        auto call(UObject* target) -> bool
        {
            if (!m_fn || !target) return false;
            target->ProcessEvent(m_fn, m_buf.data());
            return true;
        }

      private:
        UFunction* m_fn;
        std::vector<uint8_t> m_buf;
    };

    // The game's own key naming (UHypeBPFL::GetHypeKeyText / GetHypeKeyModeText, static BlueprintPure, localized
    // FText - e.g. the flat sign rather than a 'b'), the same functions the song item uses for saved moments.
    // 12 keys x 2 modes, so results are cached; the ASCII table is only a fallback if the functions are missing.
    auto game_key_text(int32 key, int32 mode) -> StringType
    {
        if (key < 0 || key >= 12) return StringType{};
        auto& cache = g_key_text_cache;
        static bool logged = false;
        const int32 m = mode == 1 ? 1 : 0;
        if (!cache[key][m].empty()) return cache[key][m];

        static UFunction* fn_key = find_fn(STR("/Script/Hype.HypeBPFL:GetHypeKeyText"));
        static UFunction* fn_mode = find_fn(STR("/Script/Hype.HypeBPFL:GetHypeKeyModeText"));
        static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Hype.Default__HypeBPFL"));
        StringType key_text{}, mode_text{};
        if (fn_key && fn_mode && cdo)
        {
            PCall k(fn_key);
            k.set<uint8_t>(STR("Key"), static_cast<uint8_t>(key)).call(cdo);
            key_text = k.get_text(STR("ReturnValue"));
            PCall md(fn_mode);
            md.set<uint8_t>(STR("Key"), static_cast<uint8_t>(m)).call(cdo);
            mode_text = md.get_text(STR("ReturnValue"));
        }
        if (key_text.empty()) key_text = kKeyNames[key];
        if (mode_text.empty()) mode_text = m == 1 ? STR("Minor") : STR("Major");
        cache[key][m] = key_text + STR(" ") + mode_text;

        if (!logged && fn_key && fn_mode && cdo)
        {
            logged = true;
            StringType all{};
            for (int32 i = 0; i < 12; ++i)
            {
                PCall k(fn_key);
                k.set<uint8_t>(STR("Key"), static_cast<uint8_t>(i)).call(cdo);
                all += (i ? STR(", ") : STR("")) + k.get_text(STR("ReturnValue"));
            }
            StringType modes{};
            for (int32 i = 0; i < 2; ++i)
            {
                PCall md(fn_mode);
                md.set<uint8_t>(STR("Key"), static_cast<uint8_t>(i)).call(cdo);
                modes += (i ? STR(", ") : STR("")) + md.get_text(STR("ReturnValue"));
            }
            Output::send<LogLevel::Verbose>(STR("[FuserNative] key names from HypeBPFL: keys [{}], modes [{}]\n"), all, modes);
        }
        return cache[key][m];
    }

    // ---------------------------------------------------------------- Key Labels ------------------
    // Every key name in the game comes from the native static UHypeBPFL::GetHypeKeyText(EKey), called from Blueprint
    // (WBP_GlobalControls / _KeyTempo, WBP_WorkspaceShortcuts, WBP_Moment_Abstract, the song item) and by our own label.
    // It returns the ST_Gameplay string table's flat names (MusicalKey.DFlat = "D<flat>"...); the table also holds
    // the sharp names (MusicalKey.CSharp...) the game never shows. A native post-hook rewrites the returned
    // FText for the five black keys: Flats = stock, Sharps = the MusicalKey.*Sharp entry, Both = "sharp/flat". The hook
    // can't see its Key argument after the exec thunk has consumed it, so it recognises the key from the returned flat
    // name, against a map built once from the function's own (unhooked) output.
    constexpr const CharType* kStringTableGameplay = STR("/Game/UI/StringTables/ST_Gameplay.ST_Gameplay");
    bool g_key_hook_busy = false;
    bool g_key_names_ready = false;
    StringType g_flat_names[12]{}, g_sharp_names[12]{};
    int g_log_keys = 0;

    auto sharp_key_name(int32 key) -> const CharType* // the MusicalKey entry holding the sharp spelling of a black key
    {
        switch (key)
        {
        case 1: return STR("MusicalKey.CSharp");
        case 3: return STR("MusicalKey.DSharp");
        case 6: return STR("MusicalKey.FSharp");
        case 8: return STR("MusicalKey.GSharp");
        case 10: return STR("MusicalKey.ASharp");
        default: return nullptr;
        }
    }

    auto build_key_names() -> void
    {
        static UFunction* fn_key = find_fn(STR("/Script/Hype.HypeBPFL:GetHypeKeyText"));
        static UObject* bpfl = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Hype.Default__HypeBPFL"));
        static UFunction* fn_table = find_fn(STR("/Script/Engine.KismetTextLibrary:TextFromStringTable"));
        static UObject* ktl = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetTextLibrary"));
        static const CharType* const fallback_sharps[12] = {STR(""), STR("C\u266F"), STR(""), STR("D\u266F"), STR(""), STR(""), STR("F\u266F"), STR(""), STR("G\u266F"), STR(""), STR("A\u266F"), STR("")};
        if (!fn_key || !bpfl) return;
        g_key_hook_busy = true;
        for (int32 i = 0; i < 12; ++i)
        {
            PCall k(fn_key);
            k.set<uint8_t>(STR("Key"), static_cast<uint8_t>(i)).call(bpfl);
            g_flat_names[i] = k.get_text(STR("ReturnValue"));
        }
        g_key_hook_busy = false;
        StringType sharps{};
        for (int32 i = 0; i < 12; ++i)
        {
            const CharType* entry = sharp_key_name(i);
            if (!entry) continue;
            StringType s{};
            if (fn_table && ktl)
            {
                PCall c(fn_table);
                c.set<FName>(STR("TableId"), FName(kStringTableGameplay, FNAME_Add));
                const FString key(entry);
                c.copy_in(STR("Key"), &key);
                if (c.call(ktl)) s = c.get_text(STR("ReturnValue"));
            }
            if (s.empty() || s.find(STR("MISSING")) != StringType::npos) s = fallback_sharps[i]; // not a localized entry after all
            g_sharp_names[i] = s;
            sharps += (sharps.empty() ? STR("") : STR(", ")) + s;
        }
        g_key_names_ready = true;
        Output::send<LogLevel::Verbose>(STR("[FuserNative] key labels: flats [{}, {}, {}, {}, {}], sharps [{}]\n"), g_flat_names[1], g_flat_names[3], g_flat_names[6], g_flat_names[8],
                                        g_flat_names[10], sharps);
    }

    auto on_key_text_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
    {
        try
        {
            if (g_key_labels == kKeyLabelsFlats || g_key_hook_busy || !ctx.RESULT_DECL) return;
            if (!g_key_names_ready) build_key_names();
            FProperty* ret = find_param(ctx.TheStack.Node() && ctx.TheStack.Node()->GetFName() == FName(STR("GetHypeKeyText"), FNAME_Add) ? ctx.TheStack.Node()
                                                                                                                                           : find_fn(STR("/Script/Hype.HypeBPFL:GetHypeKeyText")),
                                        STR("ReturnValue"));
            if (!ret) return;
            FText read{};
            ret->CopyCompleteValue(&read, ctx.RESULT_DECL);
            const StringType flat = read.ToString();
            int32 key = -1;
            for (int32 i = 0; i < 12; ++i)
            {
                if (sharp_key_name(i) && g_flat_names[i] == flat) key = i;
            }
            if (key < 0) return; // a white key, or a text we don't recognise
            StringType label = g_sharp_names[key];
            if (g_key_labels == kKeyLabelsBoth) label = flat.find(STR("/")) != StringType::npos ? flat : g_sharp_names[key] + STR("/") + flat; // a locres already combining them stays as is
            if (label.empty()) return;
            const FText value(label.c_str());
            ret->CopyCompleteValue(ctx.RESULT_DECL, &value);
            if (g_log_keys++ < kLogLimit) Output::send<LogLevel::Verbose>(STR("[FuserNative] key labels: '{}' -> '{}'\n"), flat, label);
        }
        catch (const std::exception& e)
        {
            Output::send<LogLevel::Error>(STR("[FuserNative] key labels: exception {}\n"), to_wstring(e.what()));
        }
    }

    // ---------------------------------------------------------------- native call adapter --------
    // Gives a thunk uniform access to its parameters in both calling modes (see header comment).
    class NativeArgs
    {
      public:
        NativeArgs(UFunction* fn, FFrame& stack, void* result) : m_fn(fn), m_stack(stack)
        {
            m_script_mode = stack.Node() != fn;
            if (!m_script_mode) return;
            m_scratch.assign(parms_size(fn) + 16, 0);
            for_each_param(fn, [&](FProperty* p) {
                uint8* slot = p->ContainerPtrToValuePtr<uint8>(m_scratch.data());
                p->InitializeValue_InContainer(m_scratch.data());
                if (p->HasAnyPropertyFlags(CPF_ReturnParm))
                {
                    m_outs.emplace_back(p, result ? static_cast<uint8*>(result) : slot); // not in the bytecode
                    return;
                }
                m_stack.MostRecentPropertyAddress() = nullptr;
                m_stack.Step(m_stack.Object(), slot);
                if (p->HasAnyPropertyFlags(CPF_OutParm))
                {
                    uint8* lvalue = m_stack.MostRecentPropertyAddress();
                    m_outs.emplace_back(p, lvalue ? lvalue : slot);
                }
            });
            if (m_stack.Code()) ++m_stack.Code(); // P_FINISH: skip EX_EndFunctionParms
        }
        ~NativeArgs()
        {
            if (m_script_mode) for_each_param(m_fn, [&](FProperty* p) { p->DestroyValue_InContainer(m_scratch.data()); });
        }
        NativeArgs(const NativeArgs&) = delete;
        NativeArgs& operator=(const NativeArgs&) = delete;

        auto script_mode() const -> bool { return m_script_mode; }

        auto in_object(const CharType* name) -> UObject*
        {
            FProperty* p = find_param(m_fn, name);
            if (!p) return nullptr;
            uint8* base = m_script_mode ? m_scratch.data() : m_stack.Locals();
            return *p->ContainerPtrToValuePtr<UObject*>(base);
        }

        // A plain input value (int32, an enum's uint8, ...)
        template <typename T>
        auto in(const CharType* name) -> T
        {
            FProperty* p = find_param(m_fn, name);
            if (!p) return T{};
            uint8* base = m_script_mode ? m_scratch.data() : m_stack.Locals();
            return *p->ContainerPtrToValuePtr<T>(base);
        }

        // Where an output must be written: the caller's storage.
        auto out(FProperty* p) -> uint8*
        {
            if (!p) return nullptr;
            if (m_script_mode)
            {
                for (auto& [prop, a] : m_outs)
                {
                    if (prop == p) return a;
                }
                return nullptr;
            }
            for (FOutParmRec* o = m_stack.OutParms(); o; o = o->NextOutParm)
            {
                if (o->Property == p) return o->PropAddr;
            }
            return p->ContainerPtrToValuePtr<uint8>(m_stack.Locals());
        }
        auto out(const CharType* name) -> uint8* { return out(find_param(m_fn, name)); }

      private:
        UFunction* m_fn;
        FFrame& m_stack;
        bool m_script_mode = false;
        std::vector<uint8_t> m_scratch{};
        std::vector<std::pair<FProperty*, uint8*>> m_outs{};
    };

    auto note_call(const NativeArgs& args, const CharType* what, UObject* song) -> void
    {
        int& n = args.script_mode() ? g_log_script_calls : g_log_pe_calls;
        if (n++ < kLogLimit)
        {
            Output::send<LogLevel::Verbose>(STR("[FuserNative] native {} ({} mode) for '{}'\n"), what,
                                            args.script_mode() ? STR("script-call") : STR("ProcessEvent"), song_name(song));
        }
    }

    // ---------------------------------------------------------------- native thunks --------------
    auto thunk_GetSongKeyBpm(UObject* /*Context*/, FFrame& Stack, void* result) -> void
    {
        NativeArgs args(g_fn_kb, Stack, result);
        UObject* song = args.in_object(STR("SongSet"));
        note_call(args, STR("GetSongKeyBpm"), song);
        const SongKeyBpm r = cached_key_bpm(song);
        if (auto* a = args.out(STR("Key"))) *reinterpret_cast<int32*>(a) = r.key;
        if (auto* a = args.out(STR("Mode"))) *reinterpret_cast<int32*>(a) = r.mode;
        if (auto* a = args.out(STR("Bpm"))) *reinterpret_cast<int32*>(a) = r.bpm;
        if (auto* p = static_cast<FBoolProperty*>(find_param(g_fn_kb, STR("Found"))))
        {
            if (auto* a = args.out(p)) p->SetPropertyValue(a, r.found);
        }
    }

    auto thunk_GetSongKeyText(UObject* /*Context*/, FFrame& Stack, void* result) -> void
    {
        NativeArgs args(g_fn_kt, Stack, result);
        UObject* song = args.in_object(STR("SongSet"));
        note_call(args, STR("GetSongKeyText"), song);
        const SongKeyBpm r = cached_key_bpm(song);
        FProperty* p = find_param(g_fn_kt, STR("KeyText"));
        if (auto* a = args.out(p))
        {
            // Engine copy-assign into the caller's slot; never treat engine memory as UE4SS's larger
            // FText object
            const FText value(key_text_for(r));
            p->CopyCompleteValue(a, &value);
        }
    }

    // ---------------------------------------------------------------- loading & binding ----------
    // AssetRegistryHelpers.GetAsset(FAssetData{ObjectPath}) loads by path.
    auto load_class(const CharType* class_path) -> UClass*
    {
        if (auto* cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, class_path)) return cls;
        auto* helpers = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/AssetRegistry.Default__AssetRegistryHelpers"));
        auto* get_asset = find_fn(STR("/Script/AssetRegistry.AssetRegistryHelpers:GetAsset"));
        auto* asset_data_struct = UObjectGlobals::StaticFindObject<UScriptStruct*>(nullptr, nullptr, STR("/Script/AssetRegistry.AssetData")); // 4.24: AssetRegistry module
        FProperty* path_prop = find_field(asset_data_struct, STR("ObjectPath"));
        if (!helpers || !get_asset || !path_prop)
        {
            Output::send<LogLevel::Warning>(STR("[FuserNative] load: lookup failed - AssetRegistryHelpers CDO {}, GetAsset {}, AssetData.ObjectPath {}\n"),
                                            helpers ? STR("ok") : STR("MISSING"), get_asset ? STR("ok") : STR("MISSING"),
                                            path_prop ? STR("ok") : STR("MISSING"));
            return nullptr;
        }
        PCall c(get_asset);
        auto* asset_data = c.addr(STR("InAssetData"));
        if (!asset_data) return nullptr;
        *path_prop->ContainerPtrToValuePtr<FName>(asset_data) = FName(class_path, FNAME_Add);
        c.call(helpers);
        auto* loaded = c.get<UObject*>(STR("ReturnValue"));
        Output::send<LogLevel::Verbose>(STR("[FuserNative] load: GetAsset({}) returned {}\n"), class_path,
                                        loaded ? loaded->GetFullName() : StringType{STR("null")});
        return UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, class_path);
    }

    // Our classes are referenced by nothing the game owns; root them (and what we hold pointers to)
    // so garbage collection can never unload them under us.
    auto root(UObject* o) -> void
    {
        if (o && !o->IsRootSet()) o->SetRootSet();
    }

    auto bind() -> bool
    {
        g_fn_kb = find_fn(kFnKeyBpm);
        g_fn_kt = find_fn(kFnKeyText);
        if (!g_fn_kb || !g_fn_kt)
        {
            Output::send<LogLevel::Warning>(STR("[FuserNative] bind: functions not found ({} / {})\n"), g_fn_kb != nullptr, g_fn_kt != nullptr);
            return false;
        }
        root(g_fn_kb);
        root(g_fn_kt);
        root(UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, kLibCdoPath));
        const uint32 old_kb = g_fn_kb->GetFunctionFlags(), old_kt = g_fn_kt->GetFunctionFlags();
        Output::send<LogLevel::Verbose>(STR("[FuserNative] bind: GetSongKeyBpm Func {} -> {}; GetSongKeyText Func {} -> {}\n"),
                                        reinterpret_cast<void*>(g_fn_kb->GetFuncPtr()), reinterpret_cast<void*>(&thunk_GetSongKeyBpm),
                                        reinterpret_cast<void*>(g_fn_kt->GetFuncPtr()), reinterpret_cast<void*>(&thunk_GetSongKeyText));
        g_fn_kb->SetFuncPtr(&thunk_GetSongKeyBpm);
        g_fn_kt->SetFuncPtr(&thunk_GetSongKeyText);
        g_fn_kb->GetFunctionFlags() |= FUNC_Native;
        g_fn_kt->GetFunctionFlags() |= FUNC_Native;
        Output::send<LogLevel::Verbose>(STR("[FuserNative] bind: FunctionFlags 0x{:08X} -> 0x{:08X}, 0x{:08X} -> 0x{:08X} (FUNC_Native)\n"),
                                        old_kb, g_fn_kb->GetFunctionFlags(), old_kt, g_fn_kt->GetFunctionFlags());
        return true;
    }

    // Load the library and the widget, and bind. The hook path tries once (a missing asset must not
    // turn every SetupLayoutForSong into a failed package load); F6 retries.
    auto ensure_ready(bool retry = false) -> bool
    {
        if (g_bound && g_widget_class && g_fn_set_song) return true;
        if (g_ready_attempted && !retry) return false;
        const bool first = !g_ready_attempted || retry;
        g_ready_attempted = true;
        if (!g_bound)
        {
            if (UClass* lib = load_class(kLibClassPath))
            {
                root(lib);
                g_bound = bind();
            }
            else if (first)
            {
                Output::send<LogLevel::Warning>(STR("[FuserNative] ready: BPL_FuserNative not loaded\n"));
            }
        }
        if (!g_widget_class)
        {
            g_widget_class = load_class(kWidgetClassPath);
            root(g_widget_class);
            g_fn_set_song = g_widget_class ? find_fn(kFnSetSong) : nullptr;
            root(g_fn_set_song);
            if (first && (!g_widget_class || !g_fn_set_song))
            {
                Output::send<LogLevel::Warning>(STR("[FuserNative] ready: WBP_FuserKeyBpm {} / SetSong {}\n"),
                                                g_widget_class ? STR("ok") : STR("MISSING"), g_fn_set_song ? STR("ok") : STR("MISSING"));
            }
        }
        return g_bound && g_widget_class && g_fn_set_song;
    }

    // ---------------------------------------------------------------- UMG helpers ----------------
    auto widget_parent(UObject* widget) -> UObject*
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.Widget:GetParent"));
        PCall c(fn);
        return c.call(widget) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    auto find_child_of_class(UObject* panel, UClass* cls) -> UObject*
    {
        static UFunction* fn_count = find_fn(STR("/Script/UMG.PanelWidget:GetChildrenCount"));
        static UFunction* fn_at = find_fn(STR("/Script/UMG.PanelWidget:GetChildAt"));
        PCall count(fn_count);
        if (!count.call(panel)) return nullptr;
        const int32 n = count.get<int32>(STR("ReturnValue"));
        for (int32 i = 0; i < n; ++i)
        {
            PCall at(fn_at);
            at.set<int32>(STR("Index"), i);
            if (!at.call(panel)) break;
            UObject* child = at.get<UObject*>(STR("ReturnValue"));
            if (child && child->GetClassPrivate() == cls) return child;
        }
        return nullptr;
    }

    auto create_widget(UObject* world_context, UClass* cls, UObject* owning_player) -> UObject*
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.WidgetBlueprintLibrary:Create"));
        static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
        PCall c(fn);
        c.set<UObject*>(STR("WorldContextObject"), world_context).set<UObject*>(STR("WidgetType"), cls).set<UObject*>(STR("OwningPlayer"), owning_player);
        return c.call(cdo) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    auto owning_player(UObject* widget) -> UObject*
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.Widget:GetOwningPlayer"));
        PCall c(fn);
        return c.call(widget) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    auto add_child(UObject* panel, UObject* child) -> UObject*
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.PanelWidget:AddChild"));
        PCall c(fn);
        c.set<UObject*>(STR("Content"), child);
        return c.call(panel) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    auto remove_child(UObject* panel, UObject* child) -> bool
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.PanelWidget:RemoveChild"));
        PCall c(fn);
        c.set<UObject*>(STR("Content"), child);
        return c.call(panel) && c.get<bool>(STR("ReturnValue"));
    }

    // Apply one VerticalBoxSlot's layout (size, padding, alignment) to another through the slot's own
    // setters, so the live Slate slot updates too.
    auto copy_slot_layout(UObject* from, UObject* to) -> void
    {
        struct Field { const CharType* prop; const CharType* setter; const CharType* param; };
        static const Field fields[] = {
                {STR("Size"), STR("/Script/UMG.VerticalBoxSlot:SetSize"), STR("InSize")},
                {STR("Padding"), STR("/Script/UMG.VerticalBoxSlot:SetPadding"), STR("InPadding")},
                {STR("HorizontalAlignment"), STR("/Script/UMG.VerticalBoxSlot:SetHorizontalAlignment"), STR("InHorizontalAlignment")},
                {STR("VerticalAlignment"), STR("/Script/UMG.VerticalBoxSlot:SetVerticalAlignment"), STR("InVerticalAlignment")},
        };
        if (!from || !to) return;
        for (const Field& f : fields)
        {
            FProperty* sp = find_field(from->GetClassPrivate(), f.prop);
            if (!sp) continue;
            PCall c(find_fn(f.setter));
            c.copy_in(f.param, sp->ContainerPtrToValuePtr<void>(from)).call(to);
        }
    }

    auto child_names(UObject* panel) -> StringType
    {
        static UFunction* fn_count = find_fn(STR("/Script/UMG.PanelWidget:GetChildrenCount"));
        static UFunction* fn_at = find_fn(STR("/Script/UMG.PanelWidget:GetChildAt"));
        PCall count(fn_count);
        if (!count.call(panel)) return {};
        StringType out{};
        for (int32 i = 0, n = count.get<int32>(STR("ReturnValue")); i < n; ++i)
        {
            PCall at(fn_at);
            at.set<int32>(STR("Index"), i).call(panel);
            UObject* child = at.get<UObject*>(STR("ReturnValue"));
            if (i) out += STR(", ");
            out += child ? child->GetName() : StringType{STR("null")};
        }
        return out;
    }

    auto set_visibility(UObject* widget, uint8_t visibility) -> void
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.Widget:SetVisibility"));
        PCall c(fn);
        c.set<uint8_t>(STR("InVisibility"), visibility);
        c.call(widget);
    }

    auto text_of(UObject* text_block) -> StringType
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.TextBlock:GetText"));
        PCall c(fn);
        return c.call(text_block) ? c.get_text(STR("ReturnValue")) : StringType{};
    }

    auto set_song(UObject* ours, UObject* song, UObject* style_source) -> void
    {
        PCall c(g_fn_set_song);
        c.set<UObject*>(STR("SongSet"), song).set<UObject*>(STR("StyleSource"), style_source);
        c.call(ours);
    }

    // ---------------------------------------------------------------- Key/BPM <-> Genre . Year swap
    // Our line (WBP_FuserKeyBpm) is overlaid exactly on the Genre . Year row (SongMetadata_HorizontalBox) instead of
    // sitting above it, and the two swap with a clipped "roll-up" slide:
    //   - layout: ours is the row's next sibling in the bottom-anchored VerticalBox_Text, with slot padding
    //     (L, -(rowBottom + h), R, rowBottom): its net height is 0 (nothing else moves) and its bottom edge sits on the
    //     row's content bottom; the row's animated Top (-25 at rest, 0 highlighted) doesn't affect it;
    //   - both the row and ours clip to their bounds (SetClipping(ClipToBounds)); no stock animation has transform or
    //     clipping tracks, and none touches the row's children (tools/anim_tracks);
    //   - progress p is the HOVER progress (0 at rest, 1 hovered): the label shown at rest (Genre . Year for On hover,
    //     Key/BPM for On idle) moves up by p*H and out of its clip, the hover label moves up from (1-p)*H below into
    //     place. So hovering always rolls up (like the stock Artist) and un-hovering always rolls down, in both modes.
    //     No opacity is touched (the highlight animations key the row's RenderOpacity);
    //   - hover changes animate (~0.2 s, ease-out) from the current p; a new song and a mode change snap;
    //   - "Off" is the stock song item: ours collapsed, the row's children untranslated and the
    //     row's clipping back to Inherit.
    constexpr int32 kKeyBpmOnIdle = 0, kKeyBpmOnHover = 1, kKeyBpmOff = 2;
    constexpr uint8_t kClipInherit = 0; // EWidgetClipping
    constexpr int32 kKeyBpmDefault = kKeyBpmOnHover;
    constexpr float kSwapSeconds = 0.2f;
    constexpr uint8_t kClipToBounds = 1; // EWidgetClipping
    int32 g_keybpm_mode = kKeyBpmDefault;
    bool g_clamp_genres = true; // ClampSongGenres (see clamp_song_genres)
    int g_log_swap = 0;

    // Our settings file: Fuser/Binaries/Win64/FuserNative.ini, next to winhttp.dll. Plain key=value lines.
    auto settings_path() -> std::filesystem::path
    {
        return std::filesystem::path(fue::module_dir()) / L"FuserNative.ini"; // next to winhttp.dll
    }

    auto load_settings() -> void
    {
        const auto path = settings_path();
        std::wifstream in(path);
        std::wstring line{};
        bool download_set = false, store_auto_off = false;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (line.rfind(L"DownloadSongs=", 0) == 0)
            {
                const int32 v = _wtoi(line.c_str() + 14);
                if (v >= kDownloadNo && v <= kDownloadPermanently) g_download_songs = v, download_set = true;
            }
            else if (line.rfind(L"DownloadMixSongs=", 0) == 0)
            {
                const int32 v = _wtoi(line.c_str() + 17);
                if (v >= kDownloadNo && v <= kDownloadPermanently) g_download_mix_songs = v;
            }
            else if (line.rfind(L"SendSongs=", 0) == 0)
            {
                g_send_songs = _wtoi(line.c_str() + 10) != 0;
            }
            else if (line.rfind(L"SendSongsKBps=", 0) == 0)
            {
                if (const int32 v = _wtoi(line.c_str() + 14); v > 0) g_send_kbps = v;
            }
            else if (line.rfind(L"SendSongsMax=", 0) == 0)
            {
                g_send_max = std::clamp(_wtoi(line.c_str() + 13), kSendMaxMin, kSendMaxMax);
            }
            else if (line.rfind(L"StoreAutoInstall=", 0) == 0)
            {
                store_auto_off = _wtoi(line.c_str() + 17) == 0; // the old key DownloadSongs replaces: 0 reads as No
            }
            else if (line.rfind(L"KeyBpmDisplay=", 0) == 0)
            {
                const int32 v = _wtoi(line.c_str() + 14);
                if (v >= kKeyBpmOnIdle && v <= kKeyBpmOff) g_keybpm_mode = v;
            }
            else if (line.rfind(L"KeyLabels=", 0) == 0)
            {
                const int32 v = _wtoi(line.c_str() + 10);
                if (v >= kKeyLabelsFlats && v <= kKeyLabelsBoth) g_key_labels = v;
            }
            else if (line.rfind(L"SearchBar=", 0) == 0)
            {
                g_search_enabled = _wtoi(line.c_str() + 10) != 0;
            }
            else if (line.rfind(L"ExtraSortOptions=", 0) == 0)
            {
                g_extra_sort = _wtoi(line.c_str() + 17) != 0;
            }
            else if (line.rfind(L"NewCrateSongSlots=", 0) == 0)
            {
                g_new_crate_songs = _wtoi(line.c_str() + 18);
            }
            else if (line.rfind(L"NewCrateSnapshotSlots=", 0) == 0)
            {
                g_new_crate_moments = _wtoi(line.c_str() + 22);
            }
            else if (line.rfind(L"ClampSongGenres=", 0) == 0)
            {
                g_clamp_genres = _wtoi(line.c_str() + 16) != 0;
            }
        }
        if (!download_set && store_auto_off) g_download_songs = kDownloadNo;
        static const CharType* const names[] = {STR("On idle"), STR("On hover"), STR("Off")};
        static const CharType* const labels[] = {STR("Flats"), STR("Sharps"), STR("Both")};
        static const CharType* const downloads[] = {STR("No"), STR("Temporarily"), STR("Permanently")};
        Output::send<LogLevel::Verbose>(STR("[FuserNative] settings: {} -> KeyBpmDisplay {} ({}), KeyLabels {} ({}), SearchBar {}, ExtraSortOptions {}, DownloadSongs {} ({}), "
                                            "DownloadMixSongs {} ({}), SendSongs {}\n"),
                                        path.wstring(), g_keybpm_mode, names[g_keybpm_mode], g_key_labels, labels[g_key_labels], g_search_enabled, g_extra_sort,
                                        g_download_songs, downloads[g_download_songs], g_download_mix_songs, downloads[g_download_mix_songs], g_send_songs);
    }

    // FuserNative.ini is shared: the settings screen's keys and every other component's (FUSERMultiplayer.dll's Host*,
    // Matchmaking*, P2P*, Store* ...). So a save rewrites only the screen's own lines, in place, and keeps every other
    // line - comments, blank lines and line endings - as it was. (It used to truncate the file and write the screen's
    // keys alone, erasing the rest at every settings change.) The file is read as
    // bytes, so non-ASCII text in other lines survives, and replaced through a temporary file.
    auto save_settings() -> void
    {
        const auto path = settings_path();
        std::vector<std::pair<std::string, std::string>> ours{
            {"KeyBpmDisplay=", std::to_string(g_keybpm_mode)},
            {"KeyLabels=", std::to_string(g_key_labels)},
            {"SearchBar=", g_search_enabled ? "1" : "0"},
            {"DownloadSongs=", std::to_string(g_download_songs)},
            {"DownloadMixSongs=", std::to_string(g_download_mix_songs)},
            {"SendSongs=", g_send_songs ? "1" : "0"},
            {"SendSongsKBps=", std::to_string(g_send_kbps)},
            {"SendSongsMax=", std::to_string(g_send_max)},
        };
        if (g_new_crate_songs >= 0 && g_new_crate_moments >= 0)
        {
            ours.emplace_back("NewCrateSongSlots=", std::to_string(g_new_crate_songs));
            ours.emplace_back("NewCrateSnapshotSlots=", std::to_string(g_new_crate_moments));
        }
        // ClampSongGenres is on by default: only the opt-out is kept, so "on" removes the line
        const std::string clamp_key = "ClampSongGenres=";
        std::vector<std::string> lines;
        bool crlf = false;
        {
            std::ifstream in(path, std::ios::binary);
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                    crlf = true;
                }
                lines.push_back(line);
            }
        }
        if (lines.empty()) lines.emplace_back("; FuserNative settings (the game's Settings screen keeps its own lines up to date)");
        std::vector<bool> written(ours.size(), false);
        std::vector<std::string> out_lines;
        for (const std::string& line : lines)
        {
            bool handled = false;
            for (size_t i = 0; i < ours.size() && !handled; ++i)
            {
                if (line.rfind(ours[i].first, 0) != 0) continue;
                handled = true;
                if (!written[i]) out_lines.push_back(ours[i].first + ours[i].second); // a repeat of the key is dropped
                written[i] = true;
            }
            if (!handled && line.rfind(clamp_key, 0) == 0)
            {
                handled = true;
                if (!g_clamp_genres) out_lines.push_back(clamp_key + "0");
            }
            if (!handled) out_lines.push_back(line);
        }
        for (size_t i = 0; i < ours.size(); ++i)
            if (!written[i]) out_lines.push_back(ours[i].first + ours[i].second);
        if (!g_clamp_genres && std::none_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.rfind(clamp_key, 0) == 0; }))
            out_lines.push_back(clamp_key + "0");
        auto tmp = path;
        tmp += L".tmp";
        bool ok = false;
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            for (const std::string& l : out_lines) out << l << (crlf ? "\r\n" : "\n");
            ok = out.good();
        }
        ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        Output::send<LogLevel::Verbose>(STR("[FuserNative] settings: saved KeyBpmDisplay {} KeyLabels {} SearchBar {} DownloadSongs {} SendSongs {} to {} ({}, {} lines kept)\n"),
                                        g_keybpm_mode, g_key_labels, g_search_enabled, g_download_songs, g_send_songs, path.wstring(), ok ? STR("ok") : STR("FAILED"),
                                        out_lines.size());
    }

    struct SwapParts
    {
        UObject* row = nullptr;   // SongMetadata_HorizontalBox
        UObject* ours = nullptr;  // WBP_FuserKeyBpm
        UObject* key = nullptr;   // its KeyBpmText
        UObject* genre = nullptr; // SizeBox_Genre
        UObject* year = nullptr;  // SizeBox_518 (separator + year)
        auto ok() const -> bool { return row && ours && key && genre && year; }
    };

    auto swap_parts(UObject* item) -> SwapParts
    {
        SwapParts s{};
        s.row = obj_prop(item, STR("SongMetadata_HorizontalBox"));
        s.ours = g_widget_class ? find_child_of_class(widget_parent(s.row), g_widget_class) : nullptr;
        s.key = obj_prop(s.ours, STR("KeyBpmText"));
        s.genre = obj_prop(item, STR("SizeBox_Genre"));
        s.year = widget_parent(obj_prop(item, STR("SongYear"))) ? widget_parent(widget_parent(obj_prop(item, STR("SongYear")))) : nullptr; // SongYear < HorizontalBox_183 < SizeBox_518
        return s;
    }

    auto desired_height(UObject* w) -> float
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.Widget:GetDesiredSize"));
        PCall c(fn);
        if (!c.call(w)) return 0.0f;
        auto* v = reinterpret_cast<float*>(c.addr(STR("ReturnValue")));
        return v ? v[1] : 0.0f;
    }

    auto set_translation_y(UObject* w, float y) -> void
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.Widget:SetRenderTranslation"));
        const float v[2] = {0.0f, y};
        PCall c(fn);
        c.copy_in(STR("Translation"), v).call(w);
    }

    auto set_clipping(UObject* w, uint8_t clipping) -> void
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.Widget:SetClipping"));
        PCall c(fn);
        c.set<uint8_t>(STR("InClipping"), clipping).call(w);
    }

    // Our slot's overlay padding from the row slot's current Padding (L, T, R, B) and our height.
    auto place_overlay(const SwapParts& s, const float* row_padding) -> bool
    {
        const float h = desired_height(s.ours);
        UObject* our_slot = obj_prop(s.ours, STR("Slot"));
        if (h <= 0.0f || !our_slot || !row_padding) return false;
        const float pad[4] = {row_padding[0], -(row_padding[3] + h), row_padding[2], row_padding[3]};
        static UFunction* fn = find_fn(STR("/Script/UMG.VerticalBoxSlot:SetPadding"));
        PCall c(fn);
        c.copy_in(STR("InPadding"), pad).call(our_slot); // re-enters the SetPadding hook for our slot: not the row, ignored
        return true;
    }

    auto row_padding(const SwapParts& s) -> float*
    {
        UObject* slot = obj_prop(s.row, STR("Slot"));
        return slot ? slot->GetValuePtrByPropertyNameInChain<float>(STR("Padding")) : nullptr;
    }

    // Apply hover progress p; false while the widgets have no measured height yet (before their first layout pass).
    auto apply_swap(const SwapParts& s, float p) -> bool
    {
        const float h = std::max(desired_height(s.row), desired_height(s.ours));
        if (h <= 0.0f) return false;
        const float rest_y = -p * h, hover_y = (1.0f - p) * h; // rest label rolls up and out, hover label rolls up in
        const bool key_at_rest = g_keybpm_mode == kKeyBpmOnIdle;
        set_translation_y(s.genre, key_at_rest ? hover_y : rest_y);
        set_translation_y(s.year, key_at_rest ? hover_y : rest_y);
        set_translation_y(s.key, key_at_rest ? rest_y : hover_y);
        return true;
    }

    struct SwapState
    {
        FWeakObjectPtr item{};
        UObject* raw = nullptr; // identity only, never dereferenced without the weak pointer
        float p = 0.0f, from = 0.0f, to = 0.0f;
        std::chrono::steady_clock::time_point t0{};
        bool animating = false;
        bool pending = true; // layout + progress still to apply (no measured height yet)
        bool moment = false; // the pooled item currently shows a saved moment: ours stays collapsed, nothing to apply
    };
    std::vector<SwapState> g_swaps{};

    auto is_hovered(UObject* item) -> bool
    {
        auto* hovered = item->GetValuePtrByPropertyNameInChain<bool>(STR("IsHighlighted"));
        return hovered && *hovered;
    }

    auto swap_state(UObject* item) -> SwapState&
    {
        for (auto& st : g_swaps)
        {
            if (st.raw == item && st.item.Get() == item) return st;
        }
        SwapState st{};
        st.item = FWeakObjectPtr(item);
        st.raw = item;
        st.p = st.from = st.to = is_hovered(item) ? 1.0f : 0.0f;
        g_swaps.push_back(st);
        return g_swaps.back();
    }

    // Retarget an item: animate from wherever it is (mid-slide included), or snap.
    auto set_swap_target(UObject* item, bool animate) -> void
    {
        SwapState& st = swap_state(item);
        const float to = is_hovered(item) ? 1.0f : 0.0f;
        if (animate && st.p != to)
        {
            st.from = st.p;
            st.to = to;
            st.t0 = std::chrono::steady_clock::now();
            st.animating = true;
        }
        else if (!animate)
        {
            st.p = st.from = st.to = to;
            st.animating = false;
            st.pending = true;
        }
    }

    // Every frame (engine tick): advance slides, and finish layouts that were waiting for a measured height.
    auto swap_tick() -> void
    {
        const auto now = std::chrono::steady_clock::now();
        for (size_t i = 0; i < g_swaps.size();)
        {
            SwapState& st = g_swaps[i];
            UObject* item = st.item.Get();
            if (!item)
            {
                g_swaps.erase(g_swaps.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (st.moment)
            {
                st.pending = st.animating = false;
            }
            else if (st.animating || st.pending)
            {
                const SwapParts s = swap_parts(item);
                if (st.animating)
                {
                    const float t = std::min(1.0f, std::chrono::duration<float>(now - st.t0).count() / kSwapSeconds);
                    const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); // ease-out cubic
                    st.p = st.from + (st.to - st.from) * eased;
                    if (t >= 1.0f) st.animating = false;
                }
                if (s.ok() && g_keybpm_mode == kKeyBpmOff)
                {
                    // stock song item
                    set_visibility(s.ours, kCollapsed);
                    set_translation_y(s.genre, 0.0f);
                    set_translation_y(s.year, 0.0f);
                    set_translation_y(s.key, 0.0f);
                    set_clipping(s.row, kClipInherit);
                    st.pending = st.animating = false;
                }
                else if (s.ok())
                {
                    if (st.pending)
                    {
                        set_visibility(s.ours, kVisible); // a collapsed widget measures 0: placed on a later tick
                        set_clipping(s.row, kClipToBounds);
                        if (place_overlay(s, row_padding(s)) && apply_swap(s, st.p)) st.pending = false;
                    }
                    else
                    {
                        apply_swap(s, st.p);
                    }
                }
            }
            ++i;
        }
    }

    // The setting changed: every live item swaps roles at its current hover progress (a snap, not a slide).
    auto apply_keybpm_mode_to_all() -> void
    {
        int n = 0;
        for (auto& st : g_swaps)
        {
            if (st.item.Get())
            {
                st.pending = true;
                ++n;
            }
        }
        Output::send<LogLevel::Verbose>(STR("[FuserNative] settings: KeyBpmDisplay {} applied to {} song items\n"), g_keybpm_mode, n);
    }

    // Key Labels changed: our cached key texts go, and every live song item re-renders its line (the game's own key
    // displays pick the new names up the next time they refresh their text).
    auto apply_key_labels_to_all() -> void
    {
        for (auto& row : g_key_text_cache)
        {
            for (auto& s : row) s.clear();
        }
        int n = 0;
        for (auto& st : g_swaps)
        {
            UObject* item = st.item.Get();
            if (!item || st.moment) continue;
            PCall song(item->GetFunctionByNameInChain(FName(STR("GetCelSongSet"), FNAME_Add)));
            if (!song.call(item)) continue;
            const SwapParts s = swap_parts(item);
            if (s.ours) set_song(s.ours, song.get<UObject*>(STR("ReturnValue")), obj_prop(item, STR("SongYear")));
            ++n;
        }
        Output::send<LogLevel::Verbose>(STR("[FuserNative] settings: KeyLabels {} applied to {} song items\n"), g_key_labels, n);
    }

    auto flush_deferred_setup(UObject* item) -> void; // below: a queued item's setup, now

    auto on_item_hover(UObject* item) -> void
    {
        if (g_keybpm_mode == kKeyBpmOff) return;
        flush_deferred_setup(item);
        if (!swap_parts(item).ours) return; // not one of ours (moment-only pooled item, or not injected yet)
        set_swap_target(item, true);
    }

    // ---------------------------------------------------------------- song-item injection --------
    auto on_setup_layout(UObject* item, UObject* song, bool is_song) -> void
    {
        if (!ensure_ready()) return;
        UObject* row = obj_prop(item, STR("SongMetadata_HorizontalBox"));
        UObject* panel = widget_parent(row); // VerticalBox_Text
        if (!panel)
        {
            if (g_log_inject++ < kLogLimit) Output::send<LogLevel::Warning>(STR("[FuserNative] inject: no VerticalBox_Text on {} (row {})\n"), item->GetFullName(), row != nullptr);
            return;
        }
        UObject* ours = find_child_of_class(panel, g_widget_class);
        if (!ours)
        {
            if (!is_song) return; // nothing to hide yet
            // Ours goes after the Genre . Year row (stock: the column's last child), with the row's slot settings
            // (HAlign_Center / VAlign_Bottom); place_overlay then pulls it up over the row. Both clip to their bounds.
            ours = create_widget(item, g_widget_class, owning_player(item));
            UObject* slot = ours ? add_child(panel, ours) : nullptr;
            copy_slot_layout(obj_prop(row, STR("Slot")), slot);
            if (ours)
            {
                set_clipping(ours, kClipToBounds);
                set_clipping(row, kClipToBounds);
            }
            if (g_log_inject++ < kLogLimit)
            {
                Output::send<LogLevel::Verbose>(STR("[FuserNative] inject: {} -> {} (slot {}): [{}]\n"), item->GetName(), ours ? ours->GetName() : StringType{STR("CREATE FAILED")},
                                                slot ? slot->GetName() : StringType{STR("none")}, child_names(panel));
            }
            if (!ours || !slot) return;
        }
        if (!is_song)
        {
            // Saved moments reuse pooled song items: hide ours and put the row's children back in place
            swap_state(item).moment = true;
            set_visibility(ours, kCollapsed);
            const SwapParts s = swap_parts(item);
            set_translation_y(s.genre, 0.0f);
            set_translation_y(s.year, 0.0f);
            return;
        }
        swap_state(item).moment = false;
        set_visibility(ours, g_keybpm_mode == kKeyBpmOff ? kCollapsed : kVisible);
        set_song(ours, song, obj_prop(item, STR("SongYear")));
        set_swap_target(item, false); // snap to the current mode / hover state; laid out once measured
        if (g_log_set_song++ < kLogLimit)
        {
            Output::send<LogLevel::Verbose>(STR("[FuserNative] SetSong '{}' on {}: KeyBpmText='{}' (expected key '{}', bpm {})\n"),
                                            song_name(song), item->GetName(), text_of(obj_prop(ours, STR("KeyBpmText"))),
                                            key_text_for(compute_key_bpm(song)), compute_key_bpm(song).bpm);
        }
    }

    // ---------------------------------------------------------------- deferred song-item setup ------
    // Entering freestyle fires SetupLayoutForSong for every song item of the crate in one frame (up to 64 with the
    // real tabs), and each costs a widget, its slot layout and a SetSong call: a visible pause. The first
    // kSetupImmediate items of a frame's burst (the first tab, which is what shows) are set up at once; the rest are
    // queued and set up from the tick within kSetupBudgetMs per frame. A hovered queued item is set up immediately; a
    // newer setup of a queued item replaces its entry. Moments stay immediate (they only hide ours).
    constexpr int kSetupImmediate = 8;
    constexpr double kSetupBudgetMs = 2.0;

    struct DeferredSetup
    {
        FWeakObjectPtr item{};
        UObject* raw = nullptr; // identity only
        FWeakObjectPtr song{};
    };
    std::vector<DeferredSetup> g_setup_queue{};
    uint64 g_tick = 0;           // FuserNative's engine-tick count
    uint64 g_burst_tick = ~0ull; // the tick of the current setup burst
    int g_burst_calls = 0, g_burst_now = 0, g_burst_queued = 0, g_log_burst = 0;
    double g_burst_ms = 0.0;
    uint64 g_drain_start_tick = 0;
    double g_drain_ms = 0.0;
    int g_drain_done = 0;

    auto now_ms() -> double
    {
        static const double freq = [] {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return static_cast<double>(f.QuadPart) / 1000.0;
        }();
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) / freq;
    }

    auto drop_queued(UObject* item) -> bool
    {
        for (auto it = g_setup_queue.begin(); it != g_setup_queue.end(); ++it)
        {
            if (it->raw == item)
            {
                g_setup_queue.erase(it);
                return true;
            }
        }
        return false;
    }

    auto run_setup(UObject* item, UObject* song, bool is_song) -> void
    {
        try
        {
            on_setup_layout(item, song, is_song);
        }
        catch (const std::exception& e)
        {
            if (g_log_inject++ < kLogLimit) Output::send<LogLevel::Error>(STR("[FuserNative] inject: exception {}\n"), to_wstring(e.what()));
        }
    }

    auto log_burst() -> void
    {
        if (g_burst_calls && g_log_burst++ < kLogLimit * 2)
            Output::send<LogLevel::Verbose>(STR("[FuserNative] song items: burst of {} setups at tick {}: {} now ({:.1f} ms), {} deferred\n"), g_burst_calls, g_burst_tick,
                                            g_burst_now, g_burst_ms, g_burst_queued);
        g_burst_calls = g_burst_now = g_burst_queued = 0;
        g_burst_ms = 0.0;
    }

    // SetupLayoutForSong / ForMoment on a Diamond song item (post-hook)
    auto on_setup_call(UObject* item, UFunction* fn, void* parms, bool is_song) -> void
    {
        if (g_tick != g_burst_tick)
        {
            log_burst();
            g_burst_tick = g_tick;
        }
        ++g_burst_calls;
        UObject* song = nullptr;
        if (is_song)
        {
            FProperty* song_param = find_param(fn, STR("cellSongSet"));
            song = song_param ? *song_param->ContainerPtrToValuePtr<UObject*>(parms) : nullptr;
        }
        const bool was_queued = drop_queued(item);
        if (is_song && !was_queued && g_burst_now >= kSetupImmediate && ensure_ready())
        {
            if (g_setup_queue.empty())
            {
                g_drain_start_tick = g_tick;
                g_drain_ms = 0.0;
                g_drain_done = 0;
            }
            g_setup_queue.push_back({FWeakObjectPtr(item), item, FWeakObjectPtr(song)});
            ++g_burst_queued;
            return;
        }
        const double t0 = now_ms();
        run_setup(item, song, is_song);
        g_burst_ms += now_ms() - t0;
        if (is_song) ++g_burst_now; // moments (they only hide ours) don't use up the immediate budget
    }

    auto flush_deferred_setup(UObject* item) -> void
    {
        for (auto it = g_setup_queue.begin(); it != g_setup_queue.end(); ++it)
        {
            if (it->raw != item) continue;
            UObject* song = it->song.Get();
            const bool live = it->item.Get() == item;
            g_setup_queue.erase(it);
            if (live) run_setup(item, song, true);
            return;
        }
    }

    // Every frame: close the last burst's log line, and set up queued items within the budget (at least one).
    auto setup_tick() -> void
    {
        ++g_tick;
        if (g_burst_calls && g_tick != g_burst_tick) log_burst();
        if (g_setup_queue.empty()) return;
        const double t0 = now_ms();
        do
        {
            DeferredSetup d = g_setup_queue.front();
            g_setup_queue.erase(g_setup_queue.begin());
            if (UObject* item = d.item.Get(); item && item == d.raw)
            {
                run_setup(item, d.song.Get(), true);
                ++g_drain_done;
            }
        } while (!g_setup_queue.empty() && now_ms() - t0 < kSetupBudgetMs);
        g_drain_ms += now_ms() - t0;
        if (g_setup_queue.empty() && g_log_burst < kLogLimit * 2)
            Output::send<LogLevel::Verbose>(STR("[FuserNative] song items: {} deferred setups done over {} ticks ({:.1f} ms total)\n"), g_drain_done,
                                            g_tick - g_drain_start_tick, g_drain_ms);
    }

    // The highlight animations drive the Genre . Year row's slot Padding (Top -25 at rest -> 0 highlighted, Bottom ~25),
    // and PatchLayout sets it from bytecode. Called after every VerticalBoxSlot::SetPadding (native UFunction
    // hook, so Sequencer and script calls alike): keeps our overlay on the row (place_overlay depends only on
    // the row's Bottom). Everything is re-derived from the slot, so pooled/destroyed items leave no stale state.
    auto on_slot_padding(UObject* slot) -> void
    {
        if (!g_widget_class || !slot) return;
        UObject* content = obj_prop(slot, STR("Content"));
        if (!content || content->GetFName() != g_name_meta_row) return;
        SwapParts s{};
        s.row = content;
        s.ours = find_child_of_class(obj_prop(slot, STR("Parent")), g_widget_class);
        if (!s.ours) return;
        auto* m = slot->GetValuePtrByPropertyNameInChain<float>(STR("Padding")); // FMargin: Left, Top, Right, Bottom
        const bool placed = place_overlay(s, m);
        if (g_log_padding++ < kPaddingLogLimit)
        {
            Output::send<LogLevel::Verbose>(STR("[FuserNative] padding: {} Genre row Top {:.2f} Bottom {:.2f} -> overlay {}\n"), slot->GetName(), m ? m[1] : 0.0f,
                                            m ? m[3] : 0.0f, placed ? STR("placed") : STR("waiting for layout"));
        }
    }
    auto on_set_padding_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
    {
        try
        {
            on_slot_padding(ctx.Context);
        }
        catch (const std::exception& e)
        {
            if (g_log_padding++ < kPaddingLogLimit) Output::send<LogLevel::Error>(STR("[FuserNative] padding: exception {}\n"), to_wstring(e.what()));
        }
    }

    // ---------------------------------------------------------------- real song tabs --------------
    // WBP_UberHudList's container logic is Blueprint and generic over its `Containers` array:
    // RebuildUberList fills songs, then instruments, then moments, each from the first empty container, 8 per
    // container, and WBP_UberTab spawns one icon per filled container. Only two things are fixed:
    //   - `Containers` is a literal [Container_1..5] set in PreConstruct -> we append extra containers
    //     (created from the container's own class, laid out like Container_1) right after it;
    //   - SongWidgetPool (songs AND moments) holds numItems=32, created by its RebuildWidget
    //     (autoInitialize) -> we raise numItems in OnInitialized, which runs before any Slate build.
    struct CrateSlots
    {
        int32 songs = 24, instruments = 8, moments = 8;
    };

    // Read one crate's FCrateData: the larger of each slot count and the actual array length (a crate keeps the
    // size it was created with, so it can exceed today's default - e.g. a 64-song crate with a default of 48).
    auto crate_counts(UObject* crate) -> CrateSlots
    {
        CrateSlots c{0, 0, 0};
        if (!crate) return c;
        auto* sp = static_cast<FStructProperty*>(find_field(crate->GetClassPrivate(), STR("CrateData")));
        UScriptStruct* st = sp ? sp->GetStruct().Get() : nullptr;
        if (!st) return c;
        uint8* data = sp->ContainerPtrToValuePtr<uint8>(crate);
        const auto int_field = [&](const CharType* name) -> int32 {
            FProperty* p = find_field(st, name);
            return p ? *p->ContainerPtrToValuePtr<int32>(data) : 0;
        };
        const auto array_num = [&](const CharType* name) -> int32 {
            FProperty* p = find_field(st, name);
            return p ? p->ContainerPtrToValuePtr<TArray<uint8>>(data)->Num() : 0; // TArray header layout is type-independent
        };
        c.songs = std::max(int_field(STR("NumSongSlots")), array_num(STR("songs")));
        c.instruments = std::max(int_field(STR("NumInstrumentSlots")), array_num(STR("instruments")));
        c.moments = std::max(int_field(STR("NumMomentSlots")), array_num(STR("moments")));
        return c;
    }

    // Layout to provision for: the max over Game.ini's defaults and every crate the player can bring into
    // freestyle (the pool can only be sized once per HUD, at OnInitialized).
    auto crate_slots(StringType* detail = nullptr) -> CrateSlots
    {
        CrateSlots s{};
        if (auto* settings = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Hype.Default__HypeSettings")))
        {
            if (auto* v = settings->GetValuePtrByPropertyNameInChain<int32>(STR("DefaultCrateSongSlotsNum"))) s.songs = *v;
            if (auto* v = settings->GetValuePtrByPropertyNameInChain<int32>(STR("DefaultCrateInstrumentSlotsNum"))) s.instruments = *v;
            if (auto* v = settings->GetValuePtrByPropertyNameInChain<int32>(STR("DefaultCrateMomentSlotsNum"))) s.moments = *v;
            // Not MaxInGameMomentSlotsNum (100 on this build): that would provision 164 widgets / 22 containers.
            // Moments are provisioned from crate moment slots only - stock shows one container of them too.
        }
        const CrateSlots ini = s;
        int32 crates = 0;
        std::vector<UObject*> mgrs{};
        UObjectGlobals::FindAllOf(STR("CrateMgr"), mgrs);
        for (UObject* mgr : mgrs)
        {
            if (!mgr || mgr->GetName().rfind(STR("Default__"), 0) == 0) continue;
            std::vector<UObject*> list{};
            for (const CharType* arr : {STR("MyCrates"), STR("_TEMPDefaultCrates")})
            {
                if (auto* a = mgr->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(arr))
                {
                    for (int32 i = 0; i < a->Num(); ++i) list.push_back((*a)[i]);
                }
            }
            for (const CharType* one : {STR("CurrentCrate"), STR("DefaultBattleCrate"), STR("EditNewCrate")}) list.push_back(obj_prop(mgr, one));
            for (UObject* crate : list)
            {
                if (!crate) continue;
                const CrateSlots c = crate_counts(crate);
                s.songs = std::max(s.songs, c.songs);
                s.instruments = std::max(s.instruments, c.instruments);
                s.moments = std::max(s.moments, c.moments);
                ++crates;
            }
        }
        if (detail)
        {
            *detail = fmt::format(STR("ini {}/{}/{}, {} crates"), ini.songs, ini.instruments, ini.moments, crates);
        }
        return s;
    }

    auto needed_containers(const CrateSlots& s) -> int32
    {
        const auto tabs = [](int32 n) { return n > 0 ? (n + kItemsPerContainer - 1) / kItemsPerContainer : 0; };
        return std::max(kStockContainers, tabs(s.songs) + tabs(s.instruments) + std::max(1, tabs(s.moments)));
    }

    auto on_uberlist_initialized(UObject* list) -> void
    {
        StringType detail{};
        const CrateSlots s = crate_slots(&detail);
        UObject* pool = obj_prop(list, STR("SongWidgetPool"));
        auto* num = pool ? pool->GetValuePtrByPropertyNameInChain<int32>(STR("numItems")) : nullptr;
        auto* inited = pool ? pool->GetValuePtrByPropertyNameInChain<bool>(STR("_isInitialized")) : nullptr;
        if (!num || !inited)
        {
            Output::send<LogLevel::Warning>(STR("[FuserNative] tabs: {} has no SongWidgetPool/numItems\n"), list->GetFullName());
            return;
        }
        const int32 want = s.songs + s.moments;
        const int32 before = *num;
        if (!*inited && *num < want) *num = want;
        Output::send<LogLevel::Verbose>(STR("[FuserNative] tabs: OnInitialized {} - provision songs {} instruments {} moments {} ({}); SongWidgetPool numItems {} -> {} (initialized {})\n"),
                                        list->GetName(), s.songs, s.instruments, s.moments, detail, before, *num, *inited);
    }

    // Copy a CanvasPanelSlot's layout through its own setters (live Slate slot updates too).
    auto copy_canvas_slot(UObject* from, UObject* to) -> void
    {
        struct Field { const CharType* prop; const CharType* setter; const CharType* param; };
        static const Field fields[] = {
                {STR("LayoutData"), STR("/Script/UMG.CanvasPanelSlot:SetLayout"), STR("InLayoutData")},
                {STR("bAutoSize"), STR("/Script/UMG.CanvasPanelSlot:SetAutoSize"), STR("InbAutoSize")},
                {STR("ZOrder"), STR("/Script/UMG.CanvasPanelSlot:SetZOrder"), STR("InZOrder")},
        };
        if (!from || !to) return;
        for (const Field& f : fields)
        {
            FProperty* sp = find_field(from->GetClassPrivate(), f.prop);
            if (!sp) continue;
            PCall c(find_fn(f.setter));
            c.copy_in(f.param, sp->ContainerPtrToValuePtr<void>(from)).call(to);
        }
    }

    // Make `Containers` hold every container child of Lists (re-appending ours after PreConstruct resets the
    // literal array), creating more until the crate's layout fits.
    auto ensure_containers(UObject* list, const CharType* when) -> void
    {
        auto* containers = list->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("Containers"));
        UObject* first = obj_prop(list, STR("WBP_UberHudList_Container_1"));
        UObject* lists = widget_parent(first);
        if (!containers || !first || !lists)
        {
            Output::send<LogLevel::Warning>(STR("[FuserNative] tabs: {} - Containers {} / Container_1 {} / Lists {} missing\n"), when,
                                            containers != nullptr, first != nullptr, lists != nullptr);
            return;
        }
        UClass* cls = first->GetClassPrivate();
        const int32 before = containers->Num();
        const auto contains = [&](UObject* o) {
            for (int32 i = 0; i < containers->Num(); ++i)
            {
                if ((*containers)[i] == o) return true;
            }
            return false;
        };

        // 1. containers we added earlier (children of Lists) that the array no longer lists
        static UFunction* fn_count = find_fn(STR("/Script/UMG.PanelWidget:GetChildrenCount"));
        static UFunction* fn_at = find_fn(STR("/Script/UMG.PanelWidget:GetChildAt"));
        PCall count(fn_count);
        count.call(lists);
        for (int32 i = 0, n = count.get<int32>(STR("ReturnValue")); i < n; ++i)
        {
            PCall at(fn_at);
            at.set<int32>(STR("Index"), i).call(lists);
            UObject* child = at.get<UObject*>(STR("ReturnValue"));
            if (child && child->GetClassPrivate() == cls && !contains(child)) containers->Add(child);
        }
        const int32 readded = containers->Num() - before;

        // 2. create the rest
        const int32 want = needed_containers(crate_slots());
        UObject* first_slot = obj_prop(first, STR("Slot"));
        int32 created = 0;
        while (containers->Num() < want)
        {
            UObject* c = create_widget(list, cls, owning_player(list));
            UObject* slot = c ? add_child(lists, c) : nullptr;
            if (!slot)
            {
                Output::send<LogLevel::Warning>(STR("[FuserNative] tabs: creating container {} failed\n"), containers->Num() + 1);
                break;
            }
            copy_canvas_slot(first_slot, slot);
            containers->Add(c);
            ++created;
        }
        if (g_log_tabs++ < kLogLimit * 2)
        {
            Output::send<LogLevel::Verbose>(STR("[FuserNative] tabs: {} on {} - Containers {} -> {} (re-added {}, created {}, wanted {})\n"), when,
                                            list->GetName(), before, containers->Num(), readded, created, want);
        }
    }

    // Stock WBP_UberHudList fires RebuildUberList from this GameFlow event (ubergraph 2530)
    // ---------------------------------------------------------------- song genre clamp --------------
    // The multiplayer live-requests screen crashes on a song whose Genre is not an EGenre it knows. Its song gather
    // (FUN_1406440b0) looks each crate song's Genre (CelSongSet +0xc8) up in a table it builds with keys 0..8
    // (None..Rock) and dereferences the miss: an access violation reading 0x8 at exe+0x644773 (FUSERNativeLoader
    // ISSUES.md #1, two client dumps). The era lookup cannot miss (its bucketing always returns 0..16). Only custom
    // songs carry such a Genre.
    // Genre 9 (EGenre::Num) is the community's "Indie/Alternative" (display text from a Hype.locres override). The
    // loader widens that loop to 0..9 (dllmain.cpp ApplyGenreRequestsPatch), making 9 a requestable genre; then only
    // values >= 10 (EGenre_MAX or garbage) are out of range. Without the patch (an unknown build) 9 is out of range too.
    // Every loaded CelSongSet with an out-of-range Genre gets a real genre - its SubGenre or SubSubGenre when that is
    // one, else Pop - and out-of-range sub-genres become None. Done right before the requests screen is shown, and
    // every few seconds for songs loaded later. ClampSongGenres=0 in FuserNative.ini turns it off.
    constexpr uint8 kGenreNum = 9, kGenrePop = 6; // EGenre
    constexpr uint8 kScreenLiveRequests = 56;     // EShellScreenType::MultiplayerLiveRequests
    int g_log_genres = 0;

    auto genre_limit() -> uint8 { return fnl_genre9_requestable ? kGenreNum + 1 : kGenreNum; } // first out-of-range value

    auto clamp_song_genres(const CharType* when) -> int
    {
        if (!g_clamp_genres) return 0;
        std::vector<UObject*> songs{};
        UObjectGlobals::FindAllOf(STR("CelSongSet"), songs);
        const uint8 limit = genre_limit();
        int fixed = 0;
        for (UObject* s : songs)
        {
            auto* genre = s ? s->GetValuePtrByPropertyNameInChain<uint8>(STR("Genre")) : nullptr;
            auto* sub = s ? s->GetValuePtrByPropertyNameInChain<uint8>(STR("SubGenre")) : nullptr;
            auto* subsub = s ? s->GetValuePtrByPropertyNameInChain<uint8>(STR("SubSubGenre")) : nullptr;
            if (!genre || !sub || !subsub || (*genre < limit && *sub < limit && *subsub < limit)) continue;
            const uint8 was[3] = {*genre, *sub, *subsub};
            if (*sub >= limit) *sub = 0;
            if (*subsub >= limit) *subsub = 0;
            if (*genre >= limit) *genre = *sub != 0 ? *sub : *subsub != 0 ? *subsub : kGenrePop;
            ++fixed;
            if (g_log_genres++ < 40)
                Output::send<LogLevel::Verbose>(STR("[FuserNative] genres: '{}' Genre/SubGenre/SubSubGenre {}/{}/{} -> {}/{}/{} ({})\n"), song_name(s), was[0], was[1],
                                                was[2], *genre, *sub, *subsub, when);
        }
        return fixed;
    }

    // Before ShellFlowMgr pushes / loads / replaces the live-requests screen (its gather runs as it is shown).
    auto clamp_before_requests_screen(UFunction* fn, void* parms) -> void
    {
        static const FName push(STR("PushShellScreen"), FNAME_Add), load(STR("LoadShellScreen"), FNAME_Add), replace(STR("ReplaceShellScreen"), FNAME_Add);
        const FName name = fn->GetFName();
        if (!parms || (name != push && name != load && name != replace)) return;
        FProperty* type = find_field(fn, STR("shellScreenType"));
        if (!type || *type->ContainerPtrToValuePtr<uint8>(parms) != kScreenLiveRequests) return;
        const int fixed = clamp_song_genres(STR("before the live-requests screen"));
        if (fixed && g_log_genres < 60)
            Output::send<LogLevel::Verbose>(STR("[FuserNative] genres: {} song(s) clamped before the live-requests screen\n"), fixed);
    }

    auto on_process_event_pre(UObject* context, UFunction* fn, void* parms) -> void
    {
        if (fn && g_clamp_genres) clamp_before_requests_screen(fn, parms);
        if (!context || !fn || fn->GetFName() != g_name_gameflow_event) return;
        if (context->GetClassPrivate()->GetFName() != g_name_uberlist) return;
        try
        {
            ensure_containers(context, STR("before GameFlow rebuild"));
        }
        catch (const std::exception& e)
        {
            Output::send<LogLevel::Error>(STR("[FuserNative] tabs: exception {}\n"), to_wstring(e.what()));
        }
    }

    auto on_uberlist_event(UObject* list, const FName& name) -> void
    {
        try
        {
            if (name == g_name_on_initialized) on_uberlist_initialized(list);
            else if (name == g_name_preconstruct) ensure_containers(list, STR("PreConstruct"));
            else if (name == g_name_construct && g_log_tabs < kLogLimit * 2)
            {
                auto* containers = list->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("Containers"));
                UObject* pool = obj_prop(list, STR("SongWidgetPool"));
                auto* num = pool ? pool->GetValuePtrByPropertyNameInChain<int32>(STR("numItems")) : nullptr;
                Output::send<LogLevel::Verbose>(STR("[FuserNative] tabs: Construct on {} - Containers {}, SongWidgetPool numItems {}\n"),
                                                list->GetName(), containers ? containers->Num() : -1, num ? *num : -1);
            }
        }
        catch (const std::exception& e)
        {
            Output::send<LogLevel::Error>(STR("[FuserNative] tabs: exception {}\n"), to_wstring(e.what()));
        }
    }

    // ---------------------------------------------------------------- song search -----------------
    // Crate edit's song library is WBP_CollectionPaneDefault -> WBP_CollectionListDefault lists. A rebuild asks
    // BPFL_CollectionTabType::"Get List Data for Tab Type", whose every songs-tab branch returns the whole list
    // (NumPages = 1) through the native static UCelMgr::Sort / SortSoftPtrs. So:
    //   - the game's own search field (WBP_Component_TextEntryBox, a native UHypeEditableTextWidget - the Store's
    //     search box) is created in the pane's top-left, level with the Sort/Filter bar;
    //   - a native post-hook on UHypeEditableTextWidget::_OnTextChanged records the query (debounced);
    //   - the engine tick then calls the pane's own RebuildAllCollectionLists;
    //   - a native post-hook on CelMgr::Sort / SortSoftPtrs, only when called from "Get List Data for Tab Type",
    //     keeps the songs whose title or artist contains the query (case-insensitive) and that pass its filter words
    //     (key:Am, key:C#,Eb, bpm:120, bpm:90-110, bpm:140-, bpm:-100);
    //   - the same post-hook orders the list by key or BPM when the pane's sort method is one of ours (see "crate-edit
    //     sort" below): Get List Data clamps its in_SortBy through GetValidValue before calling the native sort, whose
    //     comparator treats anything past Year as equal, so the raw value is read from Get List Data's own frame.
    constexpr const CharType* kTextEntryClassPath = STR("/Game/UI/Shared/Widgets/WBP_Component_TextEntryBox.WBP_Component_TextEntryBox_C");
    constexpr float kSearchBoxLayout[10] = {3.0f, 0.0f, 570.0f, 66.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}; // FAnchorData: X, Y, W, H; anchors (0,0); alignment (0,0)
    constexpr auto kSearchDebounce = std::chrono::milliseconds(250);
    constexpr uint8_t kSortKey = 4; // ECelSetSortMethod: Title, Artist, Genre, Year, then ours (4 is the enum's MAX,
    constexpr uint8_t kSortBpm = 5; // which the stock pane never lists)
    constexpr uint8_t kSortDescending = 1; // ESortDirection

    FWeakObjectPtr g_search_box{};
    FWeakObjectPtr g_search_pane{};
    StringType g_search_query{}; // lowercased

    // The query split into its words and filter words. An incomplete or unknown filter value (while "key:" is still
    // being typed) adds no condition.
    struct SearchFilter
    {
        StringType text{};                         // the plain words, matched as one substring of "title\nartist"
        std::vector<std::pair<int32, int32>> keys; // (EKey, EKeyMode or -1 for either mode); empty: any
        int32 bpm_lo = -1, bpm_hi = -1;            // inclusive; -1: no bound
        auto any() const -> bool { return !text.empty() || !keys.empty() || bpm_lo >= 0 || bpm_hi >= 0; }
    };
    SearchFilter g_search_filter{};
    bool g_search_dirty = false;
    std::chrono::steady_clock::time_point g_search_changed_at{};
    std::unordered_map<UObject*, StringType> g_song_text_cache{}; // song -> lowercased "title\nartist"
    int g_log_search = 0, g_log_sort = 0;

    auto lower(StringType s) -> StringType
    {
        for (auto& ch : s) ch = static_cast<CharType>(std::towlower(ch));
        return s;
    }

    auto owned_by_class(UObject* obj, const FName& class_name) -> bool
    {
        for (UObject* o = obj ? obj->GetOuterPrivate() : nullptr; o; o = o->GetOuterPrivate())
        {
            if (o->GetClassPrivate()->GetFName() == class_name) return true;
        }
        return false;
    }

    auto fstring_result(UObject* obj, const CharType* fn_path) -> StringType
    {
        PCall c(find_fn(fn_path));
        if (!c.call(obj)) return {};
        auto* s = reinterpret_cast<FString*>(c.addr(STR("ReturnValue")));
        return s && **s ? StringType(**s) : StringType{};
    }

    auto song_search_text(UObject* song) -> const StringType&
    {
        auto it = g_song_text_cache.find(song);
        if (it != g_song_text_cache.end()) return it->second;
        StringType t = fstring_result(song, STR("/Script/Hype.CelSongSet:GetSortingTitle"));
        t += STR("\n");
        t += fstring_result(song, STR("/Script/Hype.CelSongSet:GetSortingArtist"));
        return g_song_text_cache.emplace(song, lower(std::move(t))).first->second;
    }

    // A lowercased key name: root letter, optional sharp (# or the sharp sign) or flat (b or the flat sign), optional
    // mode (m / min / minor, maj / major). No mode matches either mode. "bb" is B flat, "bm" B minor.
    auto parse_key(const StringType& v, int32& key, int32& mode) -> bool
    {
        static const int32 roots[7] = {9, 11, 0, 2, 4, 5, 7}; // a..g
        if (v.empty() || v[0] < STR('a') || v[0] > STR('g')) return false;
        int32 k = roots[v[0] - STR('a')];
        size_t i = 1;
        if (i < v.size() && (v[i] == STR('#') || v[i] == STR('♯'))) ++k, ++i;
        else if (i < v.size() && (v[i] == STR('b') || v[i] == STR('♭'))) --k, ++i;
        const StringType rest = v.substr(i);
        if (rest.empty()) mode = -1;
        else if (rest == STR("m") || rest == STR("min") || rest == STR("minor")) mode = 1;
        else if (rest == STR("maj") || rest == STR("major")) mode = 0;
        else return false;
        key = (k + 12) % 12;
        return true;
    }

    // bpm:N, bpm:N-M, bpm:N- (N or more), bpm:-M (up to M)
    auto parse_bpm(const StringType& v, int32& lo, int32& hi) -> bool
    {
        auto number = [](const StringType& s, int32& out) -> bool {
            if (s.empty() || s.size() > 4 || !std::all_of(s.begin(), s.end(), [](CharType c) { return c >= STR('0') && c <= STR('9'); })) return false;
            out = std::stoi(s);
            return true;
        };
        const size_t dash = v.find(STR('-'));
        if (dash == StringType::npos)
        {
            int32 n = 0;
            if (!number(v, n)) return false;
            lo = hi = n;
            return true;
        }
        const StringType a = v.substr(0, dash), b = v.substr(dash + 1);
        int32 x = -1, y = -1;
        if ((!a.empty() && !number(a, x)) || (!b.empty() && !number(b, y)) || (a.empty() && b.empty())) return false;
        lo = x, hi = y;
        if (lo >= 0 && hi >= 0 && lo > hi) std::swap(lo, hi);
        return true;
    }

    auto parse_search(const StringType& query) -> SearchFilter
    {
        SearchFilter f{};
        StringType words{};
        bool had_filter = false;
        size_t pos = 0;
        while (pos < query.size())
        {
            const size_t end = std::min(query.find(STR(' '), pos), query.size());
            const StringType word = query.substr(pos, end - pos);
            pos = end + 1;
            if (word.rfind(STR("key:"), 0) == 0)
            {
                had_filter = true;
                const StringType list = word.substr(4);
                size_t p = 0;
                while (p <= list.size())
                {
                    const size_t comma = std::min(list.find(STR(','), p), list.size());
                    int32 key = 0, mode = -1;
                    if (parse_key(list.substr(p, comma - p), key, mode)) f.keys.emplace_back(key, mode);
                    p = comma + 1;
                }
            }
            else if (word.rfind(STR("bpm:"), 0) == 0)
            {
                had_filter = true;
                int32 lo = -1, hi = -1;
                if (parse_bpm(word.substr(4), lo, hi)) f.bpm_lo = lo, f.bpm_hi = hi;
            }
            else if (!word.empty())
            {
                if (!words.empty()) words += STR(" ");
                words += word;
            }
        }
        f.text = had_filter ? words : query; // a query without filter words matches exactly as typed
        return f;
    }

    auto song_passes(UObject* song, const SearchFilter& f) -> bool
    {
        if (!f.text.empty() && song_search_text(song).find(f.text) == StringType::npos) return false;
        if (f.keys.empty() && f.bpm_lo < 0 && f.bpm_hi < 0) return true;
        const SongKeyBpm k = cached_key_bpm(song);
        if (!f.keys.empty())
        {
            if (!k.found) return false;
            if (std::none_of(f.keys.begin(), f.keys.end(), [&](const auto& want) { return want.first == k.key && (want.second < 0 || want.second == k.mode); }))
                return false;
        }
        if (f.bpm_lo >= 0 || f.bpm_hi >= 0)
        {
            if (k.bpm <= 0) return false;
            if (f.bpm_lo >= 0 && k.bpm < f.bpm_lo) return false;
            if (f.bpm_hi >= 0 && k.bpm > f.bpm_hi) return false;
        }
        return true;
    }

    // Key: C..B, Major before Minor, then BPM. BPM: ascending, then key. Ties go by the sorting title, and songs
    // without the value sort last (by title) in either direction.
    auto sort_songs(TArray<UObject*>& songs, uint8_t method, bool descending) -> int32
    {
        struct Entry
        {
            UObject* song;
            int64 primary; // -1: no value
            const StringType* text;
        };
        std::vector<Entry> v;
        v.reserve(static_cast<size_t>(songs.Num()));
        int32 missing = 0;
        for (int32 i = 0; i < songs.Num(); ++i)
        {
            UObject* s = songs[i];
            const SongKeyBpm k = cached_key_bpm(s);
            const int64 key = k.found ? k.key * 2 + k.mode : -1;
            const int64 bpm = k.bpm > 0 ? k.bpm : -1;
            int64 primary = -1;
            if (method == kSortKey && key >= 0) primary = key * 10000 + std::max<int64>(bpm, 0);
            if (method == kSortBpm && bpm >= 0) primary = bpm * 100 + std::max<int64>(key, 0);
            if (primary < 0) ++missing;
            v.push_back({s, primary, s ? &song_search_text(s) : nullptr});
        }
        std::stable_sort(v.begin(), v.end(), [descending](const Entry& a, const Entry& b) {
            if ((a.primary < 0) != (b.primary < 0)) return b.primary < 0;
            if (a.primary != b.primary) return descending ? a.primary > b.primary : a.primary < b.primary;
            if (!a.text || !b.text) return a.text != nullptr && b.text == nullptr;
            return *a.text < *b.text; // "title\nartist", lowercased
        });
        for (int32 i = 0; i < songs.Num(); ++i) songs[i] = v[static_cast<size_t>(i)].song;
        return missing;
    }

    auto inject_search_box(UObject* pane) -> void
    {
        UObject* bar = widget_parent(obj_prop(pane, STR("SortSizeBox"))); // HorizontalBox_0 (Sort / Filter)
        UObject* root = widget_parent(bar);                                // InstancePlacementExtents
        if (!root)
        {
            Output::send<LogLevel::Warning>(STR("[FuserNative] search: {} - top bar / root canvas not found\n"), pane->GetName());
            return;
        }
        UClass* cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, kTextEntryClassPath);
        if (!cls) cls = load_class(kTextEntryClassPath);
        if (!cls) return;
        UObject* box = find_child_of_class(root, cls);
        const bool created = box == nullptr;
        if (created)
        {
            box = create_widget(pane, cls, owning_player(pane));
            UObject* slot = box ? add_child(root, box) : nullptr;
            if (!slot)
            {
                Output::send<LogLevel::Warning>(STR("[FuserNative] search: creating the search box failed\n"));
                return;
            }
            PCall layout(find_fn(STR("/Script/UMG.CanvasPanelSlot:SetLayout")));
            layout.copy_in(STR("InLayoutData"), kSearchBoxLayout).call(slot);
            const FText hint(STR("Search songs (key:Am bpm:90-110)"));
            PCall h(find_fn(STR("/Game/UI/Shared/Widgets/WBP_Component_TextEntryBox.WBP_Component_TextEntryBox_C:SetHintText")));
            h.copy_in(STR("in_HintText"), &hint).call(box);
        }
        if (g_search_box.Get() != box)
        {
            g_search_query.clear();
            g_search_filter = SearchFilter{};
            g_song_text_cache.clear();
        }
        g_search_box = FWeakObjectPtr(box);
        g_search_pane = FWeakObjectPtr(pane);
        Output::send<LogLevel::Verbose>(STR("[FuserNative] search: {} box {} on {}\n"), created ? STR("created") : STR("reusing"), box->GetName(), pane->GetFullName());
    }

    // Native post-hook: UHypeEditableTextWidget::_OnTextChanged (bound to its EditableText's OnTextChanged)
    auto on_text_changed_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
    {
        try
        {
            if (!ctx.Context || ctx.Context != g_search_box.Get()) return;
            PCall t(find_fn(STR("/Game/UI/Shared/Widgets/WBP_Component_TextEntryBox.WBP_Component_TextEntryBox_C:GetText")));
            t.call(ctx.Context);
            StringType q = lower(t.get_text(STR("OutText")));
            if (q == g_search_query) return;
            g_search_query = std::move(q);
            g_search_filter = parse_search(g_search_query);
            g_search_dirty = true;
            g_search_changed_at = std::chrono::steady_clock::now();
        }
        catch (const std::exception& e)
        {
            Output::send<LogLevel::Error>(STR("[FuserNative] search: exception {}\n"), to_wstring(e.what()));
        }
    }

    // Show the box only on the Songs tab (ECollectionTabType 0 - "Get List Data for Tab Type"'s songs branch); the
    // query only ever filters CelMgr::Sort, i.e. songs, so it would do nothing on Instruments/Moments/Fx. Same idea
    // as the pane's own ShowListAtIndex, which toggles FilterDropdown per tab type. Polled ~10x/s, applied on change.
    int32 g_search_last_type = -1; // the tab type the box's visibility was last set for (-1: re-evaluate)

    // The Song Search Bar setting changed: re-evaluate the box, and rebuild the lists so the query applies or lifts.
    auto apply_search_setting() -> void
    {
        g_search_last_type = -1;
        if (!g_search_query.empty())
        {
            g_search_dirty = true;
            g_search_changed_at = std::chrono::steady_clock::now() - kSearchDebounce;
        }
        Output::send<LogLevel::Verbose>(STR("[FuserNative] settings: SearchBar {} (query '{}')\n"), g_search_enabled, g_search_query);
    }

    auto search_visibility_tick() -> void
    {
        static auto last = std::chrono::steady_clock::time_point{};
        auto& last_type = g_search_last_type;
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::milliseconds(100)) return;
        last = now;
        UObject* pane = g_search_pane.Get();
        UObject* box = g_search_box.Get();
        if (!pane || !box)
        {
            last_type = -1;
            return;
        }
        // Resolve from the live pane every time - never cache a /Game Blueprint class's UFunction in a static: the
        // shell UI (and with it WBP_CollectionPaneDefault_C) is unloaded on the map change into freestyle and loaded
        // again at a new address on the way out, so a cached pointer dangles (the exit-freestyle crashes).
        // /Script native functions are safe to cache; they are never unloaded.
        static const FName name_type(STR("GetCurrentTabType"), FNAME_Add);
        UFunction* fn_type = pane->GetFunctionByNameInChain(name_type);
        PCall t(fn_type);
        if (!t.call(pane)) return;
        const int32 type = t.get<uint8_t>(STR("ReturnValue"));
        if (type == last_type) return;
        last_type = type;
        set_visibility(box, g_search_enabled && type == kSongsTabType ? kVisible : kCollapsed);
        if (g_log_search++ < kLogLimit * 4)
        {
            Output::send<LogLevel::Verbose>(STR("[FuserNative] search: tab type {} -> box {}\n"), type, type == kSongsTabType ? STR("shown") : STR("hidden"));
        }
    }

    // Engine tick: apply a settled query by letting the pane rebuild its lists the game's own way.
    auto search_tick() -> void
    {
        search_visibility_tick();
        if (!g_search_dirty || std::chrono::steady_clock::now() - g_search_changed_at < kSearchDebounce) return;
        g_search_dirty = false;
        UObject* pane = g_search_pane.Get();
        UObject* box = g_search_box.Get();
        if (!pane || !box) return;
        static UFunction* fn_editing = find_fn(STR("/Script/Hype.HypeEditableTextWidget:GetIsEditing"));
        static UFunction* fn_start = find_fn(STR("/Script/Hype.HypeEditableTextWidget:StartEditing"));
        PCall before(fn_editing);
        const bool was_editing = before.call(box) && before.get<bool>(STR("ReturnValue"));
        PCall rebuild(find_fn(STR("/Game/UI/Shared/WBP_CollectionPaneDefault.WBP_CollectionPaneDefault_C:RebuildAllCollectionLists")));
        rebuild.call(pane);
        PCall after(fn_editing);
        const bool still_editing = after.call(box) && after.get<bool>(STR("ReturnValue"));
        if (was_editing && !still_editing)
        {
            PCall start(fn_start);
            start.call(box); // the rebuild can move focus to the list; keep typing in the box
        }
        if (g_log_search++ < kLogLimit * 4)
        {
            Output::send<LogLevel::Verbose>(STR("[FuserNative] search: query '{}' -> rebuilt lists (editing {} -> {})\n"), g_search_query, was_editing, still_editing);
        }
    }

    // Native post-hook: CelMgr::Sort / SortSoftPtrs -> TArray<UCelSongSet*> ReturnValue
    auto on_sort_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
    {
        try
        {
            if (!ctx.RESULT_DECL) return;
            // The caller is "Get List Data for Tab Type". Under UE4SS a script-called native ran in a new frame (Node =
            // the native, PreviousFrame = the caller); fue passes the caller's own frame through, and a
            // ProcessEvent call still has the native's frame first - so the caller is this frame or the previous one.
            FFrame* list_data = nullptr;
            for (FFrame* f : {&ctx.TheStack, ctx.TheStack.PreviousFrame()})
            {
                if (f && f->Node() && f->Node()->GetFName() == g_name_get_list_data) list_data = f;
            }
            if (!list_data) return;
            auto* songs = static_cast<TArray<UObject*>*>(ctx.RESULT_DECL);
            if (g_search_enabled && g_search_filter.any() && g_search_box.Get())
            {
                const int32 before = songs->Num();
                int32 kept = 0;
                for (int32 i = 0; i < before; ++i)
                {
                    UObject* s = (*songs)[i];
                    if (s && song_passes(s, g_search_filter)) (*songs)[kept++] = s;
                }
                if (kept < before) songs->RemoveAt(kept, before - kept, false);
                if (g_log_search++ < kLogLimit * 4)
                {
                    Output::send<LogLevel::Verbose>(STR("[FuserNative] search: {} '{}' (text '{}', {} keys, bpm {}..{}) kept {} of {}\n"), ctx.TheStack.Node()->GetName(),
                                                    g_search_query, g_search_filter.text, g_search_filter.keys.size(), g_search_filter.bpm_lo, g_search_filter.bpm_hi, kept,
                                                    before);
                }
            }
            if (!g_extra_sort) return;
            // Get List Data's own parameters: the raw sort method and direction. Only a crate-edit list (RebuildList,
            // the frame before Get List Data) is re-sorted: other screens' panes never list our methods.
            UFunction* node = list_data->Node();
            FProperty* by = find_param(node, STR("in_SortBy"));
            FProperty* dir = find_param(node, STR("in_SortDirection"));
            const uint8_t method = by ? *by->ContainerPtrToValuePtr<uint8_t>(list_data->Locals()) : 0;
            if (method != kSortKey && method != kSortBpm) return;
            FFrame* caller = list_data->PreviousFrame();
            UObject* list = caller ? caller->Object() : nullptr;
            if (!list || !owned_by_class(list, g_name_crate_edit)) return;
            const bool descending = dir && *dir->ContainerPtrToValuePtr<uint8_t>(list_data->Locals()) == kSortDescending;
            const int32 missing = sort_songs(*songs, method, descending);
            if (g_log_sort++ < kLogLimit * 2)
            {
                Output::send<LogLevel::Verbose>(STR("[FuserNative] sort: {} by {}{} - {} songs, {} without a {}\n"), list->GetName(), method == kSortKey ? STR("Key") : STR("BPM"),
                                                descending ? STR(" (descending)") : STR(""), songs->Num(), missing, method == kSortKey ? STR("key") : STR("BPM"));
            }
        }
        catch (const std::exception& e)
        {
            Output::send<LogLevel::Error>(STR("[FuserNative] search: exception {}\n"), to_wstring(e.what()));
        }
    }

    // ---------------------------------------------------------------- crate-edit tabs -------------
    // WBP_Screen_CrateEdit hard-codes 3 song tabs and 1 moment tab (logs #41/#42). Each tab's page is a TopListsSwitcher
    // child: a CanvasPanel holding a WBP_HypeList_Helper and a GenericUserWidgetPool, with a ScrollBox in the helper's
    // ListContentSlot. At the screen's OnInitialized (after its DoOnInitialized has wired the six stock lists), and again
    // whenever a crate needing more tabs is opened (RefreshEquippedItemLists):
    //   - extra song / moment pages are built like SongListSlotC / MomentListSlot and appended to the switcher under
    //     their own names (FuserNative_SongPage_<k>); page_for_tab maps tabs to pages, so live pages are never moved;
    //   - the data the stock logic reads generically is extended: TopTabs.TabArray (the TabList builds its buttons
    //     from it at PreConstruct, or rebuilds them via RebuildTabsWithNewStrings), UnlockableTypeByTabIndex,
    //     _NumSongsTabs, _LastSongTabIndex;
    //   - the seven Blueprint functions with literal tab indices / song-only logic get native C++ bodies (Func + FUNC_Native, as in
    //     the key/BPM call points), re-bound whenever the class has been reloaded.
    // Tab layout for S song tabs and M moment tabs: [0, S) songs, S instruments, S+1 fx, [S+2, S+2+M) moments. The
    // edited crate's own capacity decides which tabs show. The overrides read S and M back from the screen, so an
    // unextended screen (S=3, M=1) keeps the stock layout.
    constexpr int32 kStockSongTabs = 3;
    constexpr int32 kStockTabs = 6;              // Songs x3, Instruments, Fx, Moments
    constexpr uint8_t kUnlockSong = 1;           // EUnlockableType
    constexpr uint8_t kUnlockInstrument = 3;
    constexpr uint8_t kUnlockAudioEffects = 4;
    constexpr uint8_t kUnlockMomentsTab = 8;     // the stock moments tab's own capacity query (Mission/Venue/VenueItem share it)
    int g_log_crate_edit = 0;

    auto child_count(UObject* panel) -> int32
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.PanelWidget:GetChildrenCount"));
        PCall c(fn);
        return c.call(panel) ? c.get<int32>(STR("ReturnValue")) : 0;
    }

    auto child_at(UObject* panel, int32 index) -> UObject*
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.PanelWidget:GetChildAt"));
        PCall c(fn);
        c.set<int32>(STR("Index"), index);
        return c.call(panel) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    auto int_prop(UObject* obj, const CharType* name, int32 fallback) -> int32
    {
        auto* p = obj ? obj->GetValuePtrByPropertyNameInChain<int32>(name) : nullptr;
        return p ? *p : fallback;
    }

    auto array_prop(UObject* obj, const CharType* name) -> FArrayProperty*
    {
        return obj ? CastField<FArrayProperty>(find_field(obj->GetClassPrivate(), name)) : nullptr;
    }

    // A reflected TArray as raw FScriptArray memory {Data, Num, Max}. Not UE4SS's FScriptArrayHelper: it reads
    // FArrayProperty::ArrayFlags, which does not exist before 4.25, and throws on this engine.
    struct RawArray
    {
        uint8* data;
        int32 num;
        int32 max;
    };

    struct ArrayView
    {
        FArrayProperty* prop = nullptr;
        RawArray* raw = nullptr;
        ArrayView(FArrayProperty* p, void* container) : prop(p), raw(p && container ? p->ContainerPtrToValuePtr<RawArray>(container) : nullptr) {}
        auto ok() const -> bool { return raw != nullptr; }
        auto num() const -> int32 { return raw ? raw->num : 0; }
        auto elem_size() const -> size_t { return static_cast<size_t>(prop->GetInner()->GetElementSize()); }
        auto at(int32 i) const -> uint8* { return raw->data + static_cast<size_t>(i) * elem_size(); }

        // Insert `count` constructed elements at `index`, as FScriptArray::Insert + ConstructItems do (the memory
        // comes from the game's GMalloc through FMemory, like the engine's own FHeapAllocator).
        auto insert(int32 index, int32 count) -> void
        {
            FProperty* inner = prop->GetInner();
            const size_t es = elem_size();
            if (raw->num + count > raw->max)
            {
                raw->data = static_cast<uint8*>(FMemory::Realloc(raw->data, static_cast<size_t>(raw->num + count) * es));
                raw->max = raw->num + count;
            }
            std::memmove(at(index + count), at(index), static_cast<size_t>(raw->num - index) * es);
            raw->num += count;
            for (int32 k = 0; k < count; ++k)
            {
                if (inner->HasAnyPropertyFlags(CPF_ZeroConstructor)) std::memset(at(index + k), 0, es);
                else inner->InitializeValue(at(index + k));
            }
        }
    };

    auto array_num(UObject* obj, const CharType* name) -> int32
    {
        const ArrayView a(array_prop(obj, name), obj);
        return a.ok() ? a.num() : -1;
    }

    // Insert `count` copies of element `src` at `dst` (src < dst, or src == the last element when appending).
    auto insert_copies(UObject* obj, const CharType* name, int32 src, int32 dst, int32 count) -> bool
    {
        ArrayView a(array_prop(obj, name), obj);
        if (!a.ok() || count <= 0) return count == 0;
        if (src < 0 || src >= a.num() || dst < 0 || dst > a.num() || src >= dst) return false;
        a.insert(dst, count);
        for (int32 k = 0; k < count; ++k) a.prop->GetInner()->CopyCompleteValue(a.at(dst + k), a.at(src));
        return true;
    }

    // Call a function of a /Game object by name, resolved from the live instance (never cached).
    auto own_fn(UObject* obj, const CharType* name) -> UFunction*
    {
        return obj ? obj->GetFunctionByNameInChain(FName(name, FNAME_Add)) : nullptr;
    }

    struct TabLayout
    {
        int32 per = 8, songs = kStockSongTabs, moments = 1;
        auto instruments() const -> int32 { return songs; }
        auto fx() const -> int32 { return songs + 1; }
        auto first_moment() const -> int32 { return songs + 2; }
        auto total() const -> int32 { return songs + 2 + moments; }
    };

    auto tab_layout(UObject* screen) -> TabLayout
    {
        TabLayout l{};
        l.per = std::max(1, int_prop(screen, STR("_NumEquippedSongsPerTab"), 8));
        l.songs = std::max(1, int_prop(screen, STR("_NumSongsTabs"), kStockSongTabs));
        const int32 n = array_num(screen, STR("UnlockableTypeByTabIndex"));
        l.moments = std::max(1, (n < 0 ? kStockTabs : n) - l.songs - 2);
        return l;
    }

    enum class TabKind { Song, Instrument, Fx, Moment, None };
    struct TabRef
    {
        TabKind kind = TabKind::None;
        int32 page = 0;
    };

    auto tab_ref(const TabLayout& l, int32 t) -> TabRef
    {
        if (t >= 0 && t < l.songs) return {TabKind::Song, t};
        if (t == l.instruments()) return {TabKind::Instrument, 0};
        if (t == l.fx()) return {TabKind::Fx, 0};
        if (t >= l.first_moment() && t < l.total()) return {TabKind::Moment, t - l.first_moment()};
        return {};
    }

    // GetHypeGameInstance(self).theCrateMgr.EditNewCrate - the stock screen's path to the crate being edited.
    auto edit_crate(UObject* context) -> UObject*
    {
        static UFunction* fn = find_fn(STR("/Script/Hype.HypeGameInstance:GetHypeGameInstance"));
        static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Hype.Default__HypeGameInstance"));
        PCall c(fn);
        c.set<UObject*>(STR("contextObj"), context);
        UObject* gi = c.call(cdo) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
        return obj_prop(obj_prop(gi, STR("theCrateMgr")), STR("EditNewCrate"));
    }

    // The edited crate's FCrateData, read through reflection.
    struct CrateView
    {
        UObject* crate = nullptr;
        UScriptStruct* st = nullptr;
        uint8* data = nullptr;
        bool mission = false;

        explicit CrateView(UObject* c) : crate(c)
        {
            auto* sp = c ? static_cast<FStructProperty*>(find_field(c->GetClassPrivate(), STR("CrateData"))) : nullptr;
            st = sp ? sp->GetStruct().Get() : nullptr;
            data = st ? sp->ContainerPtrToValuePtr<uint8>(c) : nullptr;
            static UFunction* fn_mission = find_fn(STR("/Script/Hype.Crate:IsMissionCrate"));
            PCall m(fn_mission);
            mission = data && m.call(c) && m.get<bool>(STR("ReturnValue"));
        }
        auto ok() const -> bool { return data != nullptr; }
        auto field(const CharType* name) const -> FProperty* { return find_field(st, name); }
        auto i32(const CharType* name) const -> int32
        {
            FProperty* p = field(name);
            return p ? *p->ContainerPtrToValuePtr<int32>(data) : 0;
        }
        auto num(const CharType* name) const -> int32
        {
            auto* p = CastField<FArrayProperty>(field(name));
            return ArrayView(p, data).num();
        }
        auto constraint(const CharType* name) const -> int32 // MissionConstraint.<name>
        {
            auto* sp = static_cast<FStructProperty*>(field(STR("MissionConstraint")));
            UScriptStruct* cs = sp ? sp->GetStruct().Get() : nullptr;
            FProperty* p = find_field(cs, name);
            return p ? *p->ContainerPtrToValuePtr<int32>(sp->ContainerPtrToValuePtr<uint8>(data)) : 0;
        }
    };

    // GetMaxNumSongsInCrate. Stock: mission crates Min(NumSongSlots, MissionConstraint.NumSongSlots), every other crate
    // 8 x _NumSongsTabs (24). Now a non-mission crate uses its own capacity, max(NumSongSlots, songs), capped at the
    // tabs built.
    auto max_songs(UObject* screen, const TabLayout& l) -> int32
    {
        const CrateView c(edit_crate(screen));
        if (!c.ok()) return l.per * l.songs;
        if (c.mission) return std::min(c.i32(STR("NumSongSlots")), c.constraint(STR("NumSongSlots")));
        const int32 cap = std::max(c.i32(STR("NumSongSlots")), c.num(STR("songs")));
        return cap > 0 ? std::min(cap, l.per * l.songs) : l.per * kStockSongTabs;
    }

    // GetMaxCapacityForUnlockType. Stock: the moments branch (Mission/Venue/OutfitPartVariant/VenueItem) returns
    // _NumEquippedSongsPerTab (8) for non-mission crates, NumMomentSlots for missions; now non-mission crates use their
    // own moment capacity, capped at the moment tabs built. Songs, instruments and Fx branches are stock.
    auto max_capacity(UObject* screen, const TabLayout& l, uint8_t type) -> int32
    {
        switch (type)
        {
        case kUnlockSong:
            return max_songs(screen, l);
        case 0:
        case 2:
        case 5:
        case kUnlockMomentsTab: {
            const CrateView c(edit_crate(screen));
            if (!c.ok()) return l.per;
            if (c.mission) return c.i32(STR("NumMomentSlots"));
            // No stock-style fallback to 8: a crate made with DefaultCrateMomentSlotsNum = 0 has no moment slots, and
            // its moments tab is hidden.
            const int32 cap = std::max(c.i32(STR("NumMomentSlots")), c.num(STR("moments")));
            return std::min(cap, l.per * l.moments);
        }
        case kUnlockInstrument: {
            const CrateView c(edit_crate(screen));
            return c.ok() && c.mission ? c.i32(STR("NumInstrumentSlots")) : l.per;
        }
        case kUnlockAudioEffects: {
            const CrateView c(edit_crate(screen));
            return c.ok() && c.mission ? c.i32(STR("NumAudioEffectSlots")) : l.per;
        }
        default:
            return 0;
        }
    }

    auto current_tab(UObject* screen) -> int32
    {
        UObject* tabs = obj_prop(screen, STR("TopTabs"));
        PCall c(own_fn(tabs, STR("GetCurrentTabIndex")));
        return c.call(tabs) ? c.get<int32>(STR("ActiveWidgetIndex")) : 0;
    }

    // Our pages are appended to TopListsSwitcher (never inserted: live pages are never detached), so switcher index and
    // tab index differ. TopTabs has no _MyWidgetSwitcher, so the TabList never reads the switcher itself.
    // Page names are "FuserNative_SongPage_<k>_<serial>": the prefix identifies the tab, the serial keeps every name
    // unique in the WidgetTree (so no name lookup is ever needed before creating one - it crashed).
    auto page_prefix(bool moment, int32 page) -> StringType
    {
        return fmt::format(STR("FuserNative_{}Page_{}_"), moment ? STR("Moment") : STR("Song"), page);
    }

    auto new_page_name(bool moment, int32 page) -> StringType
    {
        static int32 serial = 0;
        return page_prefix(moment, page) + fmt::format(STR("{}"), serial++);
    }

    // The switcher page of tab t: a stock page (the parent of its named helper), or one of ours found by name.
    auto page_for_tab(UObject* screen, const TabLayout& l, int32 tab) -> UObject*
    {
        static const CharType* const stock_songs[kStockSongTabs] = {STR("EquippedSongListHelperA"), STR("EquippedSongListHelperB"), STR("SongListHelperC")};
        const TabRef r = tab_ref(l, tab);
        const CharType* stock = nullptr;
        switch (r.kind)
        {
        case TabKind::Song:
            if (r.page < kStockSongTabs) stock = stock_songs[r.page];
            break;
        case TabKind::Instrument:
            stock = STR("EquippedInstrumentListHelper");
            break;
        case TabKind::Fx:
            stock = STR("EquippedFxListHelper");
            break;
        case TabKind::Moment:
            if (r.page == 0) stock = STR("EquippedMomentListHelper");
            break;
        case TabKind::None:
            return nullptr;
        }
        if (stock) return widget_parent(obj_prop(screen, stock));
        UObject* switcher = obj_prop(screen, STR("TopListsSwitcher"));
        const StringType want = page_prefix(r.kind == TabKind::Moment, r.page);
        for (int32 i = 0, n = child_count(switcher); i < n; ++i)
        {
            UObject* c = child_at(switcher, i);
            if (c && c->GetName().rfind(want, 0) == 0) return c;
        }
        return nullptr;
    }

    auto child_index(UObject* panel, UObject* child) -> int32
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.PanelWidget:GetChildIndex"));
        PCall c(fn);
        c.set<UObject*>(STR("Content"), child);
        return c.call(panel) ? c.get<int32>(STR("ReturnValue")) : -1;
    }

    // The list helper of tab t: the WBP_HypeList_Helper on its page.
    auto helper_for_tab(UObject* screen, int32 tab) -> UObject*
    {
        UObject* stock = obj_prop(screen, STR("EquippedSongListHelperA"));
        UObject* page = page_for_tab(screen, tab_layout(screen), tab);
        return stock && page ? find_child_of_class(page, stock->GetClassPrivate()) : nullptr;
    }

    // Diagnostics: one tab's page, helper, pool and item parent state.
    int g_log_crate_edit_lists = 0;
    FWeakObjectPtr g_crate_edit_screen{};

    auto describe_tab(UObject* screen, int32 tab) -> StringType
    {
        UObject* page = page_for_tab(screen, tab_layout(screen), tab);
        UObject* helper = helper_for_tab(screen, tab);
        if (!helper) return fmt::format(STR("page {} - NO HELPER"), page ? page->GetName() : StringType{STR("null")});
        const auto call_int = [](UObject* o, const CharType* fn) -> int32 {
            PCall c(find_fn(fn));
            return c.call(o) ? c.get<int32>(STR("ReturnValue")) : -1;
        };
        const auto call_bool = [](UObject* o, const CharType* fn) -> int32 {
            PCall c(find_fn(fn));
            return c.call(o) ? (c.get<bool>(STR("ReturnValue")) ? 1 : 0) : -1;
        };
        const auto size_of = [](UObject* o) -> StringType {
            PCall c(find_fn(STR("/Script/UMG.Widget:GetDesiredSize")));
            if (!c.call(o)) return STR("?");
            auto* v = reinterpret_cast<float*>(c.addr(STR("ReturnValue")));
            return v ? fmt::format(STR("{:.0f}x{:.0f}"), v[0], v[1]) : StringType{STR("?")};
        };
        UObject* pool = obj_prop(helper, STR("widgetPool"));
        UObject* parent = obj_prop(helper, STR("ItemParentWidget"));
        UObject* scroll = obj_prop(helper, STR("ListScrollBox"));
        UObject* first = child_at(parent, 0);
        return fmt::format(STR("page {} helper {} [vis {} {}] list items {}; pool {} init {} in use {} avail {}; item parent {} ({} children, {}), scroll {}; first item {} [vis {} {}]"),
                           page ? page->GetName() : StringType{STR("null")}, helper->GetName(), call_bool(helper, STR("/Script/UMG.Widget:IsVisible")), size_of(helper),
                           call_int(helper, STR("/Script/Hype.HypeListWidget:GetNumListItems")), pool ? pool->GetName() : StringType{STR("null")},
                           call_bool(pool, STR("/Script/Hype.GenericUserWidgetPool:IsInitialized")), call_int(pool, STR("/Script/Hype.GenericUserWidgetPool:GetNumInUseItems")),
                           call_int(pool, STR("/Script/Hype.GenericUserWidgetPool:GetNumAvailableItems")), parent ? parent->GetName() : StringType{STR("null")},
                           child_count(parent), size_of(parent), scroll ? scroll->GetName() : StringType{STR("null")},
                           first ? first->GetName() : StringType{STR("none")}, call_bool(first, STR("/Script/UMG.Widget:IsVisible")), size_of(first));
    }

    // Diagnostics: from `w` up its parents to `stop`: name, Visibility property, Slate visibility (IsVisible is false
    // without a Slate widget too) and the last arranged size.
    auto widget_chain(UObject* w, UObject* stop) -> StringType
    {
        static UFunction* fn_vis = find_fn(STR("/Script/UMG.Widget:GetVisibility"));
        static UFunction* fn_is = find_fn(STR("/Script/UMG.Widget:IsVisible"));
        static UFunction* fn_geo = find_fn(STR("/Script/UMG.Widget:GetCachedGeometry"));
        static UFunction* fn_size = find_fn(STR("/Script/UMG.SlateBlueprintLibrary:GetLocalSize"));
        static UObject* sbl = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__SlateBlueprintLibrary"));
        StringType out{};
        for (int depth = 0; w && depth < 12; ++depth, w = widget_parent(w))
        {
            PCall v(fn_vis), is(fn_is), geo(fn_geo);
            v.call(w);
            is.call(w);
            geo.call(w);
            PCall size(fn_size);
            size.copy_in(STR("Geometry"), geo.addr(STR("ReturnValue"))).call(sbl);
            auto* s = reinterpret_cast<float*>(size.addr(STR("ReturnValue")));
            out += fmt::format(STR("{}{} [vis {} slate {} {:.0f}x{:.0f}]"), depth ? STR(" < ") : STR(""), w->GetName(), v.get<uint8_t>(STR("ReturnValue")),
                               is.get<bool>(STR("ReturnValue")) ? 1 : 0, s ? s[0] : -1.0f, s ? s[1] : -1.0f);
            if (w == stop) break;
        }
        return out;
    }

    // Diagnostics: a widget's slot settings, on-screen box (absolute position and size) and desired size.
    auto layout_desc(UObject* w) -> StringType
    {
        if (!w) return STR("null");
        static UFunction* fn_geo = find_fn(STR("/Script/UMG.Widget:GetCachedGeometry"));
        static UFunction* fn_abs = find_fn(STR("/Script/UMG.SlateBlueprintLibrary:LocalToAbsolute"));
        static UFunction* fn_size = find_fn(STR("/Script/UMG.SlateBlueprintLibrary:GetAbsoluteSize"));
        static UFunction* fn_desired = find_fn(STR("/Script/UMG.Widget:GetDesiredSize"));
        static UObject* sbl = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__SlateBlueprintLibrary"));
        PCall geo(fn_geo), desired(fn_desired);
        geo.call(w);
        desired.call(w);
        PCall abs(fn_abs), size(fn_size);
        abs.copy_in(STR("Geometry"), geo.addr(STR("ReturnValue"))).call(sbl);
        size.copy_in(STR("Geometry"), geo.addr(STR("ReturnValue"))).call(sbl);
        const auto v2 = [](PCall& c) -> StringType {
            auto* v = reinterpret_cast<float*>(c.addr(STR("ReturnValue")));
            return v ? fmt::format(STR("{:.0f},{:.0f}"), v[0], v[1]) : StringType{STR("?")};
        };
        StringType slot_text{};
        UObject* slot = obj_prop(w, STR("Slot"));
        if (slot)
        {
            const StringType cls = slot->GetClassPrivate()->GetName();
            if (auto* ld = slot->GetValuePtrByPropertyNameInChain<float>(STR("LayoutData"))) // FAnchorData: offsets LTRB, anchors min/max, alignment
            {
                auto* autosize = slot->GetValuePtrByPropertyNameInChain<bool>(STR("bAutoSize"));
                slot_text = fmt::format(STR("{} off({:.0f},{:.0f},{:.0f},{:.0f}) anch({:.2f},{:.2f}-{:.2f},{:.2f}) align({:.2f},{:.2f}) auto {}"), cls, ld[0], ld[1], ld[2], ld[3], ld[4],
                                        ld[5], ld[6], ld[7], ld[8], ld[9], autosize && *autosize);
            }
            else if (auto* sz = slot->GetValuePtrByPropertyNameInChain<float>(STR("Size"))) // FSlateChildSize {float Value; uint8 SizeRule}
            {
                auto* pad = slot->GetValuePtrByPropertyNameInChain<float>(STR("Padding"));
                auto* ha = slot->GetValuePtrByPropertyNameInChain<uint8_t>(STR("HorizontalAlignment"));
                slot_text = fmt::format(STR("{} size({:.2f} rule {}) pad({:.0f},{:.0f},{:.0f},{:.0f}) halign {}"), cls, sz[0], reinterpret_cast<uint8_t*>(sz)[4], pad ? pad[0] : 0.0f,
                                        pad ? pad[1] : 0.0f, pad ? pad[2] : 0.0f, pad ? pad[3] : 0.0f, ha ? *ha : 255);
            }
            else
            {
                slot_text = cls;
            }
        }
        StringType extra{};
        if (auto* wo = w->GetValuePtrByPropertyNameInChain<float>(STR("WidthOverride")))
        {
            auto* flag = static_cast<FBoolProperty*>(find_field(w->GetClassPrivate(), STR("bOverride_WidthOverride")));
            extra = fmt::format(STR(" WidthOverride {:.0f} (on {})"), *wo, flag && flag->GetPropertyValue(flag->ContainerPtrToValuePtr<void>(w)));
        }
        return fmt::format(STR("{} at {} size {} desired {} [{}]{}"), w->GetName(), v2(abs), v2(size), v2(desired), slot_text, extra);
    }

    auto dump_tab_bar(UObject* screen) -> void
    {
        UObject* tabs = obj_prop(screen, STR("TopTabs"));
        UObject* total = obj_prop(tabs, STR("TotalTabsWidth"));
        UObject* bg_size = widget_parent(total);
        UObject* crate = widget_parent(bg_size);
        UObject* row = widget_parent(crate);
        Output::send<LogLevel::Verbose>(STR("[FuserNative] F8: tab bar {} (TotalWidth {:.0f})\n"), layout_desc(tabs),
                                        tabs && tabs->GetValuePtrByPropertyNameInChain<float>(STR("TotalWidth")) ? *tabs->GetValuePtrByPropertyNameInChain<float>(STR("TotalWidth")) : -1.0f);
        Output::send<LogLevel::Verbose>(STR("[FuserNative] F8:   row {}\n"), layout_desc(row));
        for (int32 i = 0, n = child_count(row); i < n; ++i) Output::send<LogLevel::Verbose>(STR("[FuserNative] F8:   row child {}: {}\n"), i, layout_desc(child_at(row, i)));
        Output::send<LogLevel::Verbose>(STR("[FuserNative] F8:   BGSize {}\n"), layout_desc(bg_size));
        for (int32 i = 0, n = child_count(bg_size); i < n; ++i) Output::send<LogLevel::Verbose>(STR("[FuserNative] F8:   BGSize child {}: {}\n"), i, layout_desc(child_at(bg_size, i)));
        UObject* strip = obj_prop(tabs, STR("HorizontalBox_Tabs"));
        for (int32 i = 0, n = child_count(strip); i < n; ++i) Output::send<LogLevel::Verbose>(STR("[FuserNative] F8:   tab button {}: {}\n"), i, layout_desc(child_at(strip, i)));
        Output::send<LogLevel::Verbose>(STR("[FuserNative] F8:   W prompt {} / E prompt {}\n"), layout_desc(obj_prop(tabs, STR("Canvas_LB"))),
                                        layout_desc(obj_prop(tabs, STR("Canvas_RB"))));
    }

    // Tab strip width. TabList.TryRecalculateTabWidths sets TotalTabsWidth.WidthOverride = tabs x 60 + widest label + 42,
    // but a tab button is 68 wide and the selected one label + 62 (174 + 11 x 68 = 922 vs 874), so the formula
    // is exact at the stock 6 tabs and 8 short per extra tab; at 12 the strip spills under the W/E hints. The TabList
    // recomputes it from several places (its label-width ticker, Construct, language change, our visibility refresh),
    // so ~10x/s the crate-edit strip's override is raised to the buttons' measured width when that is larger. At 6 tabs
    // or fewer the two agree and nothing changes.
    auto tab_strip_tick() -> void
    {
        static auto last = std::chrono::steady_clock::time_point{};
        static int logged = 0;
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::milliseconds(100)) return;
        last = now;
        UObject* screen = g_crate_edit_screen.Get();
        if (!screen) return;
        UObject* tabs = obj_prop(screen, STR("TopTabs"));
        UObject* box = obj_prop(tabs, STR("TotalTabsWidth"));
        UObject* strip = obj_prop(tabs, STR("HorizontalBox_Tabs"));
        // bOverride_WidthOverride is a 1-bit bitfield sharing its byte with the other overrides: read it through its mask
        auto* flag = box ? static_cast<FBoolProperty*>(find_field(box->GetClassPrivate(), STR("bOverride_WidthOverride"))) : nullptr;
        const bool override_on = flag && flag->GetPropertyValue(flag->ContainerPtrToValuePtr<void>(box));
        auto* width = box ? box->GetValuePtrByPropertyNameInChain<float>(STR("WidthOverride")) : nullptr;
        auto* stock = tabs ? tabs->GetValuePtrByPropertyNameInChain<float>(STR("TotalWidth")) : nullptr;
        if (!override_on || !width || !stock) return;
        static UFunction* fn_desired = find_fn(STR("/Script/UMG.Widget:GetDesiredSize"));
        float content = 0.0f;
        for (int32 i = 0, n = child_count(strip); i < n; ++i)
        {
            PCall d(fn_desired);
            if (!d.call(child_at(strip, i))) continue;
            if (auto* v = reinterpret_cast<float*>(d.addr(STR("ReturnValue")))) content += v[0];
        }
        const float target = std::max(*stock, content);
        if (content <= 0.0f || std::abs(*width - target) < 0.5f) return;
        static UFunction* fn_set = find_fn(STR("/Script/UMG.SizeBox:SetWidthOverride"));
        const float before = *width;
        PCall set(fn_set);
        set.set<float>(STR("InWidthOverride"), target).call(box);
        if (logged++ < kLogLimit * 2)
        {
            Output::send<LogLevel::Verbose>(STR("[FuserNative] crate edit: tab strip width {:.0f} -> {:.0f} (TabList formula {:.0f}, buttons {:.0f})\n"), before, target, *stock, content);
        }
    }

    // F8: every tab of the live crate-edit screen.
    auto dump_crate_edit() -> void
    {
        UObject* screen = g_crate_edit_screen.Get();
        if (!screen)
        {
            Output::send<LogLevel::Verbose>(STR("[FuserNative] F8: no crate-edit screen\n"));
            return;
        }
        UObject* tabs = obj_prop(screen, STR("TopTabs"));
        UObject* sw = obj_prop(tabs, STR("_MyWidgetSwitcher"));
        const TabLayout l = tab_layout(screen);
        Output::send<LogLevel::Verbose>(STR("[FuserNative] F8: {} - {} song tabs, {} moment tabs, current tab {}, TopTabs._MyWidgetSwitcher {}\n"), screen->GetName(), l.songs,
                                        l.moments, current_tab(screen), sw ? sw->GetName() : StringType{STR("null")});
        for (int32 t = 0; t < l.total(); ++t) Output::send<LogLevel::Verbose>(STR("[FuserNative] F8:   tab {}: {}\n"), t, describe_tab(screen, t));
        dump_tab_bar(screen);
        UObject* switcher = obj_prop(screen, STR("TopListsSwitcher"));
        for (int32 t : {kStockSongTabs - 1, kStockSongTabs})
        {
            UObject* helper = helper_for_tab(screen, t);
            UObject* first = child_at(obj_prop(helper, STR("ItemParentWidget")), 0);
            UObject* named = nullptr;
            {
                PCall c(find_fn(STR("/Script/UMG.ContentWidget:GetContent")));
                UObject* slot = obj_prop(helper, STR("ListContentSlot"));
                named = c.call(slot) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
            }
            Output::send<LogLevel::Verbose>(STR("[FuserNative] F8:   tab {} chain: {}\n"), t, widget_chain(first ? first : obj_prop(helper, STR("ItemParentWidget")), switcher));
            Output::send<LogLevel::Verbose>(STR("[FuserNative] F8:   tab {} helper chain: {}; ListContentSlot content {}; helper root {}\n"), t, widget_chain(helper, switcher),
                                            named ? named->GetName() : StringType{STR("null")},
                                            obj_prop(obj_prop(helper, STR("WidgetTree")), STR("RootWidget")) ? obj_prop(obj_prop(helper, STR("WidgetTree")), STR("RootWidget"))->GetName() : StringType{STR("null")});
        }
    }

    // HypeListWidget::BuildList(listData, addUniqueOnly = true, maxNumItems = -1, initialSelectionIndex = 0), as stock.
    // Either a list of objects, or a TArray<UObject-derived*> of the crate copied in whole.
    auto build_list(UObject* helper, const std::vector<UObject*>* items, const void* whole_array = nullptr) -> void
    {
        static UFunction* fn = find_fn(STR("/Script/Hype.HypeListWidget:BuildList"));
        if (!helper) return;
        PCall c(fn);
        if (whole_array)
        {
            c.copy_in(STR("listData"), whole_array);
        }
        else if (auto* list = reinterpret_cast<TArray<UObject*>*>(c.addr(STR("listData"))); list && items)
        {
            for (UObject* o : *items) list->Add(o);
        }
        c.set<bool>(STR("addUniqueOnly"), true).set<int32>(STR("maxNumItems"), -1).set<int32>(STR("initialSelectionIndex"), 0);
        c.call(helper);
    }

    // Conv_SoftObjectReferenceToObject: the object if loaded, else null (what the stock bytecode does per song).
    auto soft_to_object(const void* soft_ptr) -> UObject*
    {
        static UFunction* fn = find_fn(STR("/Script/Engine.KismetSystemLibrary:Conv_SoftObjectReferenceToObject"));
        static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        PCall c(fn);
        c.copy_in(STR("SoftObject"), soft_ptr);
        return c.call(cdo) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    // RefreshEquippedItemLists. Stock: songs i -> page i/8 of {A, B, C} (a switch with no default, so songs past 24
    // are dropped), each page AddUnique(Cast<CelSongSet>(soft ptr)); then instruments, Fx and moments into their
    // single lists. Now S song pages, and M moment pages of 8 (the last one takes any overflow, as the single stock
    // list did). With M = 1 moments are built exactly as stock.
    auto ensure_tabs_for_crate(UObject* screen) -> void; // below

    auto refresh_equipped_lists(UObject* screen) -> void
    {
        ensure_tabs_for_crate(screen);
        const TabLayout l = tab_layout(screen);
        const CrateView c(edit_crate(screen));
        std::vector<std::vector<UObject*>> song_pages(static_cast<size_t>(l.songs));
        int32 n_songs = 0, n_null = 0;
        if (auto* sp = c.ok() ? CastField<FArrayProperty>(c.field(STR("songs"))) : nullptr)
        {
            static UClass* song_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Hype.CelSongSet"));
            const ArrayView h(sp, c.data);
            for (int32 i = 0; i < h.num(); ++i)
            {
                const int32 page = i / l.per;
                if (page >= l.songs) continue;
                UObject* song = soft_to_object(h.at(i));
                if (song && song_class && !song->IsA(song_class)) song = nullptr;
                ++n_songs;
                if (!song) ++n_null;
                auto& v = song_pages[static_cast<size_t>(page)];
                if (std::find(v.begin(), v.end(), song) == v.end()) v.push_back(song);
            }
        }
        for (int32 p = 0; p < l.songs; ++p) build_list(helper_for_tab(screen, p), &song_pages[static_cast<size_t>(p)]);
        if (g_log_crate_edit_lists++ < kLogLimit)
        {
            StringType pages{};
            for (int32 p = 0; p < l.songs; ++p)
            {
                const auto& v = song_pages[static_cast<size_t>(p)];
                pages += fmt::format(STR("\n    song page {}: {} entries, first '{}' -> {}"), p, v.size(), v.empty() || !v[0] ? StringType{STR("-")} : song_name(v[0]),
                                     describe_tab(screen, p));
            }
            Output::send<LogLevel::Verbose>(STR("[FuserNative] crate edit: RefreshEquippedItemLists - crate {} songs {} ({} not loaded / not CelSongSet), NumSongSlots {}{}\n"),
                                            c.crate ? c.crate->GetName() : StringType{STR("null")}, n_songs, n_null, c.ok() ? c.i32(STR("NumSongSlots")) : -1, pages);
        }
        if (!c.ok()) return; // stock: the crate reads fail, the other lists are left alone

        const auto whole = [&](const CharType* name) -> const void* {
            FProperty* p = c.field(name);
            return p ? p->ContainerPtrToValuePtr<void>(c.data) : nullptr;
        };
        build_list(obj_prop(screen, STR("EquippedInstrumentListHelper")), nullptr, whole(STR("instruments")));
        build_list(obj_prop(screen, STR("EquippedFxListHelper")), nullptr, whole(STR("audioEffects")));
        if (l.moments == 1)
        {
            build_list(obj_prop(screen, STR("EquippedMomentListHelper")), nullptr, whole(STR("moments")));
            return;
        }
        std::vector<std::vector<UObject*>> moment_pages(static_cast<size_t>(l.moments));
        if (auto* mp = CastField<FArrayProperty>(c.field(STR("moments"))))
        {
            const ArrayView h(mp, c.data);
            for (int32 i = 0; i < h.num(); ++i)
            {
                moment_pages[static_cast<size_t>(std::min(i / l.per, l.moments - 1))].push_back(*reinterpret_cast<UObject**>(h.at(i)));
            }
        }
        for (int32 p = 0; p < l.moments; ++p) build_list(helper_for_tab(screen, l.first_moment() + p), &moment_pages[static_cast<size_t>(p)]);
    }

    // RefreshSongTabsVisibility. Stock: TabWidgetArray[1/2].SetIsPresent(max songs > 8 / 16), then TryRecalculateTabWidths.
    // Now every song tab k >= 1 (max songs > 8k) and every moment tab k >= 0 (max moments > 8k: a crate without moment
    // slots shows no moments tab; stock always showed it).
    auto refresh_tabs_visibility(UObject* screen) -> void
    {
        const TabLayout l = tab_layout(screen);
        UObject* tabs = obj_prop(screen, STR("TopTabs"));
        auto* buttons = tabs ? tabs->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("TabWidgetArray")) : nullptr;
        const auto set_present = [&](int32 index, bool present) {
            UObject* b = buttons && index >= 0 && index < buttons->Num() ? (*buttons)[index] : nullptr;
            PCall c(own_fn(b, STR("SetIsPresent")));
            c.set<bool>(STR("in_IsPresent"), present).call(b);
        };
        const int32 songs = max_songs(screen, l);
        for (int32 k = 1; k < l.songs; ++k) set_present(k, songs > l.per * k);
        const int32 moments = max_capacity(screen, l, kUnlockMomentsTab);
        for (int32 k = 0; k < l.moments; ++k) set_present(l.first_moment() + k, moments > l.per * k);
        PCall w(own_fn(tabs, STR("TryRecalculateTabWidths")));
        w.call(tabs);
        if (g_log_crate_edit++ < kLogLimit * 4)
        {
            Output::send<LogLevel::Verbose>(STR("[FuserNative] crate edit: tabs visibility - max songs {} ({} of {} song tabs), max moments {} ({} of {} moment tabs), {} buttons\n"),
                                            songs, std::clamp((songs + l.per - 1) / l.per, 1, l.songs), l.songs, moments,
                                            std::clamp((moments + l.per - 1) / l.per, 0, l.moments), l.moments, buttons ? buttons->Num() : -1);
        }
    }

    // DoOnToggledTabChanged. Stock: switch(ActiveIndex 0..5) -> TopListsSwitcher index, the collection pane's list
    // (songs 0 / instruments 1 / Fx 2 / moments 3), DoChangeFiltersForSnapshots(ActiveIndex), and the number of
    // unlocked background slots / visible hitboxes: song tab k min(max songs - 8k, 8), instruments / Fx their
    // capacity, moments GetMaxCapacityForUnlockType(8); then the collection pane's RefreshFilter. Now the same per tab
    // kind, with DoChangeFiltersForSnapshots given the stock index of the tab's kind (song 0, instruments 3, Fx 4,
    // moments 5) and moment page k treated like song page k.
    auto on_tab_changed(UObject* screen, int32 active) -> void
    {
        const TabLayout l = tab_layout(screen);
        const TabRef r = tab_ref(l, active);
        UObject* pane = obj_prop(screen, STR("CollectionPane"));
        UObject* switcher = obj_prop(screen, STR("TopListsSwitcher"));
        const int32 page_index = child_index(switcher, page_for_tab(screen, l, active));
        {
            static UFunction* fn = find_fn(STR("/Script/UMG.WidgetSwitcher:SetActiveWidgetIndex"));
            PCall c(fn);
            c.set<int32>(STR("Index"), page_index >= 0 ? page_index : active).call(switcher);
        }
        int32 pane_list = 0, stock_index = active, visible = 0;
        switch (r.kind)
        {
        case TabKind::Song:
            pane_list = 0, stock_index = 0;
            visible = std::min(max_songs(screen, l) - l.per * r.page, l.per);
            break;
        case TabKind::Instrument:
            pane_list = 1, stock_index = 3;
            visible = max_capacity(screen, l, kUnlockInstrument);
            break;
        case TabKind::Fx:
            pane_list = 2, stock_index = 4;
            visible = max_capacity(screen, l, kUnlockAudioEffects);
            break;
        case TabKind::Moment: {
            pane_list = 3, stock_index = 5;
            const int32 cap = max_capacity(screen, l, kUnlockMomentsTab);
            visible = l.moments == 1 ? cap : std::min(cap - l.per * r.page, l.per);
            break;
        }
        case TabKind::None:
            break;
        }
        {
            PCall c(own_fn(pane, STR("ShowListAtIndex")));
            c.set<int32>(STR("in_Index"), pane_list).call(pane);
        }
        {
            PCall c(own_fn(screen, STR("DoChangeFiltersForSnapshots")));
            c.set<int32>(STR("ActiveIndex"), stock_index).call(screen);
        }
        static UClass* size_box = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.SizeBox"));
        static const FName panel_class(STR("WBP_CrateEditPanel_C"), FNAME_Add);
        UObject* bg = obj_prop(screen, STR("BgImagesContainer"));
        UObject* hitboxes = obj_prop(screen, STR("FgHitboxes"));
        for (int32 i = 0, n = child_count(bg); i < n; ++i)
        {
            UObject* box = child_at(bg, i);
            if (!box || !size_box || !box->IsA(size_box)) continue;
            UObject* panel = child_at(box, 0);
            if (!panel || panel->GetClassPrivate()->GetFName() != panel_class) continue;
            PCall lock(own_fn(panel, STR("showIsLocked")));
            lock.set<bool>(STR("isLocked"), !(i < visible)).call(panel);
            set_visibility(child_at(hitboxes, i), i < visible ? kVisible : kCollapsed);
        }
        {
            PCall c(own_fn(pane, STR("RefreshFilter")));
            c.call(pane);
        }
        if (g_log_crate_edit++ < kLogLimit * 4)
        {
            static const CharType* const kinds[] = {STR("song"), STR("instruments"), STR("fx"), STR("moment"), STR("invalid")};
            Output::send<LogLevel::Verbose>(STR("[FuserNative] crate edit: tab {} = {} page {} -> switcher page {}, pane list {}, filters {}, {} slots unlocked\n"), active,
                                            kinds[static_cast<int>(r.kind)], r.page, page_index, pane_list, stock_index, visible);
        }
    }

    // GetFullDataIndexByIndexOnCurrentPage. Stock: idx + switch(tab){0:0; 1:8; 2:16; else 0}. Now song page k and
    // moment page k both add 8k.
    auto full_index(UObject* screen, int32 index_on_page) -> int32
    {
        const TabLayout l = tab_layout(screen);
        const TabRef r = tab_ref(l, current_tab(screen));
        return r.kind == TabKind::Song || r.kind == TabKind::Moment ? index_on_page + l.per * r.page : index_on_page;
    }

    // UpdateSelectedSongTab (TrySetIsDataInCrate calls it after every successful add, of any kind). Stock: on a song tab
    // before the last, if songs > 8 x (tab + 1), TrySetTabIndex(Clamp(songs / 8, 0, _LastSongTabIndex)) - i.e. follow
    // the added song onto the next page. Now songs as stock, and the same for moment pages.
    auto update_selected_tab(UObject* screen) -> void
    {
        const TabLayout l = tab_layout(screen);
        const int32 cur = current_tab(screen);
        const TabRef r = tab_ref(l, cur);
        const CrateView c(edit_crate(screen));
        if (!c.ok()) return;
        int32 target = -1;
        if (r.kind == TabKind::Song && cur < int_prop(screen, STR("_LastSongTabIndex"), l.songs - 1))
        {
            const int32 n = c.num(STR("songs"));
            if (n > (cur + 1) * l.per) target = std::clamp(n / l.per, 0, int_prop(screen, STR("_LastSongTabIndex"), l.songs - 1));
        }
        else if (r.kind == TabKind::Moment && r.page < l.moments - 1)
        {
            const int32 n = c.num(STR("moments"));
            if (n > (r.page + 1) * l.per) target = l.first_moment() + std::clamp(n / l.per, 0, l.moments - 1);
        }
        if (target < 0) return;
        UObject* tabs = obj_prop(screen, STR("TopTabs"));
        PCall set(own_fn(tabs, STR("TrySetTabIndex")));
        set.set<int32>(STR("in_CurrentTabIndex"), target).set<bool>(STR("in_ShouldSlamAnim"), false).call(tabs);
        if (g_log_crate_edit++ < kLogLimit * 4)
        {
            Output::send<LogLevel::Verbose>(STR("[FuserNative] crate edit: added item -> tab {} to {} (set {})\n"), cur, target, set.get<bool>(STR("out_DidSetToDesiredTabIndex")));
        }
    }

    // ---- native bodies (script-call or ProcessEvent mode, via NativeArgs). The UFunction is re-resolved from the
    // screen every call: the class is reloaded on map changes.
    template <typename Body>
    auto crate_edit_thunk(UObject* screen, FFrame& stack, void* result, const CharType* name, Body&& body) -> void
    {
        UFunction* fn = own_fn(screen, name);
        if (!fn) return; // cannot happen: bound from this very class
        NativeArgs args(fn, stack, result);
        try
        {
            body(args);
        }
        catch (const std::exception& e)
        {
            Output::send<LogLevel::Error>(STR("[FuserNative] crate edit: {} exception {}\n"), name, to_wstring(e.what()));
        }
    }

    auto set_int_out(NativeArgs& args, int32 v) -> void
    {
        if (auto* a = args.out(STR("ReturnValue"))) *reinterpret_cast<int32*>(a) = v;
    }

    auto thunk_GetMaxNumSongsInCrate(UObject* ctx, FFrame& stack, void* result) -> void
    {
        crate_edit_thunk(ctx, stack, result, STR("GetMaxNumSongsInCrate"), [&](NativeArgs& a) { set_int_out(a, max_songs(ctx, tab_layout(ctx))); });
    }

    auto thunk_GetMaxCapacityForUnlockType(UObject* ctx, FFrame& stack, void* result) -> void
    {
        crate_edit_thunk(ctx, stack, result, STR("GetMaxCapacityForUnlockType"),
                         [&](NativeArgs& a) { set_int_out(a, max_capacity(ctx, tab_layout(ctx), a.in<uint8_t>(STR("in_UnlockType")))); });
    }

    auto thunk_RefreshSongTabsVisibility(UObject* ctx, FFrame& stack, void* result) -> void
    {
        crate_edit_thunk(ctx, stack, result, STR("RefreshSongTabsVisibility"), [&](NativeArgs&) { refresh_tabs_visibility(ctx); });
    }

    auto thunk_RefreshEquippedItemLists(UObject* ctx, FFrame& stack, void* result) -> void
    {
        crate_edit_thunk(ctx, stack, result, STR("RefreshEquippedItemLists"), [&](NativeArgs&) { refresh_equipped_lists(ctx); });
    }

    auto thunk_GetFullDataIndexByIndexOnCurrentPage(UObject* ctx, FFrame& stack, void* result) -> void
    {
        crate_edit_thunk(ctx, stack, result, STR("GetFullDataIndexByIndexOnCurrentPage"),
                         [&](NativeArgs& a) { set_int_out(a, full_index(ctx, a.in<int32>(STR("in_IndexOnPage")))); });
    }

    auto thunk_DoOnToggledTabChanged(UObject* ctx, FFrame& stack, void* result) -> void
    {
        crate_edit_thunk(ctx, stack, result, STR("DoOnToggledTabChanged"), [&](NativeArgs& a) { on_tab_changed(ctx, a.in<int32>(STR("ActiveIndex"))); });
    }

    auto thunk_UpdateSelectedSongTab(UObject* ctx, FFrame& stack, void* result) -> void
    {
        crate_edit_thunk(ctx, stack, result, STR("UpdateSelectedSongTab"), [&](NativeArgs&) { update_selected_tab(ctx); });
    }

    struct Override
    {
        const CharType* name;
        UnrealScriptFunction thunk;
    };
    const Override kCrateEditOverrides[] = {
            {STR("GetMaxNumSongsInCrate"), &thunk_GetMaxNumSongsInCrate},
            {STR("GetMaxCapacityForUnlockType"), &thunk_GetMaxCapacityForUnlockType},
            {STR("RefreshSongTabsVisibility"), &thunk_RefreshSongTabsVisibility},
            {STR("RefreshEquippedItemLists"), &thunk_RefreshEquippedItemLists},
            {STR("GetFullDataIndexByIndexOnCurrentPage"), &thunk_GetFullDataIndexByIndexOnCurrentPage},
            {STR("DoOnToggledTabChanged"), &thunk_DoOnToggledTabChanged},
            {STR("UpdateSelectedSongTab"), &thunk_UpdateSelectedSongTab},
    };

    // Bind (or re-bind, after a class reload) every override on this screen's class.
    auto bind_crate_edit(UObject* screen) -> bool
    {
        int bound = 0, already = 0;
        StringType missing{};
        for (const Override& o : kCrateEditOverrides)
        {
            UFunction* f = own_fn(screen, o.name);
            if (!f)
            {
                missing += StringType(missing.empty() ? STR("") : STR(", ")) + o.name;
                continue;
            }
            if (f->GetFuncPtr() == o.thunk && (f->GetFunctionFlags() & FUNC_Native))
            {
                ++already;
                continue;
            }
            f->SetFuncPtr(o.thunk);
            f->GetFunctionFlags() |= FUNC_Native;
            ++bound;
        }
        if (bound || !missing.empty())
        {
            Output::send<LogLevel::Verbose>(STR("[FuserNative] crate edit: {} - bound {} overrides, {} already bound{}{}\n"), screen->GetClassPrivate()->GetFullName(),
                                            bound, already, missing.empty() ? STR("") : STR(", MISSING: "), missing);
        }
        return missing.empty();
    }

    // ---------------------------------------------------------------- crate-edit sort -------------
    // WBP_CollectionPaneDefault's Sort dropdown lists ECelSetSortMethod 0..3 (Title, Artist, Genre, Year), or 7
    // EDLCSortMethod entries on the store screen, which uses the same pane class. A picked index is stored unchecked in
    // the pane's _SortMethod and handed raw to the lists' RebuildList, so Key (kSortKey) and BPM (kSortBpm) only need
    // listing and labelling: BuildSortDropdown and RefreshSortLabel get native bodies that do what the stock ones do,
    // plus our two entries on a crate-edit pane (the stock label lookup has no names past Year). The ordering itself
    // is done by on_sort_post. CycleSortMethod (modulo 4) stays stock: no widget in the pane can call it.
    constexpr uint8_t kStoreScreen = 53; // EShellScreenType: the store, where the pane lists EDLCSortMethod
    constexpr int32 kStockSortEntries = 4, kStoreSortEntries = 7;
    const CharType* const kExtraSortLabels[2] = {STR("Key"), STR("BPM")};
    constexpr const CharType* kStringTableShell = STR("/Game/UI/StringTables/ST_Shell.ST_Shell");
    int g_log_sort_ui = 0;

    auto current_screen(UObject* context) -> int32
    {
        static UFunction* fn_mgr = find_fn(STR("/Script/Hype.ShellFlowMgr:GetShellFlowMgr"));
        static UFunction* fn_screen = find_fn(STR("/Script/Hype.ShellFlowMgr:GetCurrentScreen"));
        static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Hype.Default__ShellFlowMgr"));
        PCall m(fn_mgr);
        m.set<UObject*>(STR("contextObj"), context);
        if (!cdo || !m.call(cdo)) return -1;
        PCall s(fn_screen);
        return s.call(m.get<UObject*>(STR("ReturnValue"))) ? s.get<uint8_t>(STR("ReturnValue")) : -1;
    }

    auto lists_extra_sorts(UObject* pane) -> bool
    {
        return g_extra_sort && owned_by_class(pane, g_name_crate_edit);
    }

    // The stock header is Format(ST_Shell "Shell_Sorted_By_Label", Method = label).
    auto sorted_by_text(const CharType* label) -> StringType
    {
        static StringType pattern{};
        static bool read = false;
        if (!read)
        {
            read = true;
            static UFunction* fn_table = find_fn(STR("/Script/Engine.KismetTextLibrary:TextFromStringTable"));
            static UObject* ktl = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetTextLibrary"));
            PCall c(fn_table);
            c.set<FName>(STR("TableId"), FName(kStringTableShell, FNAME_Add));
            const FString key(STR("Shell_Sorted_By_Label"));
            c.copy_in(STR("Key"), &key);
            if (fn_table && ktl && c.call(ktl)) pattern = c.get_text(STR("ReturnValue"));
            Output::send<LogLevel::Verbose>(STR("[FuserNative] sort: header pattern '{}'\n"), pattern);
        }
        const size_t at = pattern.find(STR("{Method}"));
        if (at == StringType::npos) return label;
        StringType s = pattern;
        s.replace(at, 8, label);
        return s;
    }

    auto build_sort_dropdown(UObject* pane) -> void
    {
        UObject* dropdown = obj_prop(pane, STR("SortDropdown"));
        UFunction* fn_set = own_fn(dropdown, STR("SetDropdownEntries"));
        UFunction* fn_label = own_fn(pane, STR("GetLabelForSortEnum"));
        auto* prop = CastField<FArrayProperty>(find_param(fn_set, STR("in_dropdownEntries")));
        if (!fn_label || !prop) return;
        const bool extra = lists_extra_sorts(pane);
        const int32 stock = current_screen(pane) == kStoreScreen ? kStoreSortEntries : kStockSortEntries;
        PCall set(fn_set);
        ArrayView entries(prop, nullptr);
        entries.raw = reinterpret_cast<RawArray*>(set.addr(STR("in_dropdownEntries")));
        entries.insert(0, stock + (extra ? 2 : 0));
        FProperty* inner = prop->GetInner();
        for (int32 i = 0; i < stock; ++i)
        {
            PCall l(fn_label);
            l.set<uint8_t>(STR("InSortMethod"), static_cast<uint8_t>(i)).call(pane);
            inner->CopyCompleteValue(entries.at(i), l.addr(STR("ReturnValue")));
        }
        for (int32 j = 0; extra && j < 2; ++j)
        {
            const FText t(kExtraSortLabels[j]);
            inner->CopyCompleteValue(entries.at(stock + j), &t);
        }
        set.call(dropdown);
        if (g_log_sort_ui++ < kLogLimit)
        {
            Output::send<LogLevel::Verbose>(STR("[FuserNative] sort: dropdown built with {} entries on {}\n"), entries.num(), pane->GetName());
        }
    }

    auto refresh_sort_label(UObject* pane) -> void
    {
        UObject* dropdown = obj_prop(pane, STR("SortDropdown"));
        PCall get(own_fn(pane, STR("Get Sort Method")));
        if (!dropdown || !get.call(pane)) return;
        const uint8_t method = get.get<uint8_t>(STR("byte"));
        PCall sel(own_fn(dropdown, STR("SelectListItemByIndex")));
        sel.set<int32>(STR("listItemIndex"), method).set<bool>(STR("shouldSelect"), true).set<bool>(STR("isInitializing"), true).call(dropdown);
        UObject* header = obj_prop(dropdown, STR("Dropdown_Header"));
        UFunction* fn_header = own_fn(header, STR("SetHeaderLabel"));
        FProperty* param = nullptr;
        if (fn_header) for_each_param(fn_header, [&](FProperty* p) { if (!param) param = p; });
        if (!param) return;
        const StringType param_name = param->GetFName().ToString();
        PCall h(fn_header);
        if ((method == kSortKey || method == kSortBpm) && lists_extra_sorts(pane))
        {
            const FText t(sorted_by_text(kExtraSortLabels[method - kSortKey]).c_str());
            h.copy_in(param_name.c_str(), &t);
        }
        else
        {
            PCall l(own_fn(pane, STR("GetLabelForSortEnumHeader")));
            l.set<uint8_t>(STR("InSortMethod"), method).call(pane);
            h.copy_in(param_name.c_str(), l.addr(STR("ReturnValue")));
        }
        h.call(header);
    }

    template <typename Body>
    auto pane_thunk(UObject* pane, FFrame& stack, void* result, const CharType* name, Body&& body) -> void
    {
        UFunction* fn = own_fn(pane, name);
        if (!fn) return; // cannot happen: bound from this very class
        NativeArgs args(fn, stack, result);
        try
        {
            body();
        }
        catch (const std::exception& e)
        {
            Output::send<LogLevel::Error>(STR("[FuserNative] sort: {} exception {}\n"), name, to_wstring(e.what()));
        }
    }

    auto thunk_BuildSortDropdown(UObject* ctx, FFrame& stack, void* result) -> void
    {
        pane_thunk(ctx, stack, result, STR("BuildSortDropdown"), [&] { build_sort_dropdown(ctx); });
    }

    auto thunk_RefreshSortLabel(UObject* ctx, FFrame& stack, void* result) -> void
    {
        pane_thunk(ctx, stack, result, STR("RefreshSortLabel"), [&] { refresh_sort_label(ctx); });
    }

    const Override kPaneOverrides[] = {
            {STR("BuildSortDropdown"), &thunk_BuildSortDropdown},
            {STR("RefreshSortLabel"), &thunk_RefreshSortLabel},
    };

    // Bound when a crate-edit pane is constructed, before its first OnScreenActivated builds the dropdown; again after
    // a class reload. The bodies serve every pane of the class, the store's included.
    auto bind_sort_dropdown(UObject* pane) -> void
    {
        if (!g_extra_sort) return;
        int bound = 0;
        StringType missing{};
        for (const Override& o : kPaneOverrides)
        {
            UFunction* f = own_fn(pane, o.name);
            if (!f)
            {
                missing += StringType(missing.empty() ? STR("") : STR(", ")) + o.name;
                continue;
            }
            if (f->GetFuncPtr() == o.thunk && (f->GetFunctionFlags() & FUNC_Native)) continue;
            f->SetFuncPtr(o.thunk);
            f->GetFunctionFlags() |= FUNC_Native;
            ++bound;
        }
        if (bound || !missing.empty())
        {
            Output::send<LogLevel::Verbose>(STR("[FuserNative] sort: {} - bound {} overrides{}{}\n"), pane->GetClassPrivate()->GetFullName(), bound,
                                            missing.empty() ? STR("") : STR(", MISSING: "), missing);
        }
    }

    // ---------------------------------------------------------------- song tiles: key/BPM label ----
    // The crate editor's song tiles (the library's grid items and the crate's own slots) are
    // WBP_Element_Collection_Browse_HypeSongWidget. The crate's slots fill theirs through the native
    // UHypeSongWidget::SetCelSongSet (a post-hook); the library's WBP_GridItem_Collection_Browse_Song never calls it - its
    // OnListItemInitialized (ProcessEvent-dispatched from the native InitListItem) caches its list data as
    // _CachedCelSongSet and calls the tile's Blueprint InitializeItem, so that event's post-callback hands the song over.
    // Either way one WBP_FuserKeyBpm per tile goes into its SongStatsExtents canvas, centred just above the Title / Artist /
    // Genre . Year column (VerticalBox_0) and styled like YearField. Inside SongStatsExtents the label rises with the
    // highlight animations (they move that canvas's slot) and fades with A_FadeOutAllText (its RenderOpacity), as the
    // stock rows do. Tiles are pooled: a tile keeps its label, and each SetCelSongSet re-fills it. Key and BPM Display
    // = Off hides the labels; a song with neither a key nor a BPM shows none.
    FName g_name_song_tile{}, g_name_song_grid_item{}, g_name_list_item_initialized{};
    struct TileLabel
    {
        FWeakObjectPtr tile{};
        FWeakObjectPtr ours{};
        FWeakObjectPtr style{}; // YearField
        FWeakObjectPtr song{};  // the song last shown, for a settings change
    };
    std::unordered_map<UObject*, TileLabel> g_tile_labels{};
    constexpr size_t kTileLabelsPrune = 4096;
    constexpr uint8_t kHitTestInvisible = 3; // ESlateVisibility
    constexpr float kStockColumnTop = -5.0f; // VerticalBox_0's top in SongStatsExtents (the asset's value)
    int g_log_tiles = 0;

    // A named widget under `w`, through panel children (not into nested user widgets).
    auto find_named(UObject* w, const FName& name, int depth = 0) -> UObject*
    {
        static const FName fn_children(STR("GetChildrenCount"), FNAME_Add);
        if (!w || depth > 12) return nullptr;
        if (w->GetFName() == name) return w;
        if (!w->GetFunctionByNameInChain(fn_children)) return nullptr; // not a panel
        for (int32 i = 0, n = child_count(w); i < n; ++i)
        {
            if (UObject* hit = find_named(child_at(w, i), name, depth + 1)) return hit;
        }
        return nullptr;
    }

    // A designer variable of the tile, or the widget of that name in its tree when the variable is unset.
    auto tile_widget(UObject* tile, const CharType* name) -> UObject*
    {
        if (UObject* w = obj_prop(tile, name)) return w;
        return find_named(obj_prop(obj_prop(tile, STR("WidgetTree")), STR("RootWidget")), FName(name, FNAME_Add));
    }

    auto show_tile_label(UObject* ours, UObject* song, UObject* style) -> void
    {
        const SongKeyBpm k = song ? cached_key_bpm(song) : SongKeyBpm{};
        if (!song || g_keybpm_mode == kKeyBpmOff || (!k.found && k.bpm <= 0))
        {
            set_visibility(ours, kCollapsed);
            return;
        }
        set_song(ours, song, style);
        set_visibility(ours, kHitTestInvisible); // over the album art: never takes the tile's hover or clicks
    }

    auto create_tile_label(UObject* tile) -> TileLabel
    {
        UObject* column = widget_parent(tile_widget(tile, STR("SongTitle_HorizontalBox"))); // VerticalBox_0
        UObject* stats = column ? widget_parent(column) : tile_widget(tile, STR("SongStatsExtents"));
        UObject* style = tile_widget(tile, STR("YearField"));
        if (!stats)
        {
            if (g_log_tiles++ < kLogLimit) Output::send<LogLevel::Warning>(STR("[FuserNative] tiles: no SongStatsExtents on {}\n"), tile->GetFullName());
            return {};
        }
        float top = kStockColumnTop;
        if (UObject* col_slot = obj_prop(column, STR("Slot")))
        {
            if (FProperty* ld = find_field(col_slot->GetClassPrivate(), STR("LayoutData"))) top = ld->ContainerPtrToValuePtr<float>(col_slot)[1]; // Offsets.Top
        }
        UObject* ours = find_child_of_class(stats, g_widget_class);
        if (!ours)
        {
            ours = create_widget(tile, g_widget_class, owning_player(tile));
            UObject* slot = ours ? add_child(stats, ours) : nullptr;
            if (!slot)
            {
                if (g_log_tiles++ < kLogLimit) Output::send<LogLevel::Warning>(STR("[FuserNative] tiles: creating the label failed on {}\n"), tile->GetName());
                return {};
            }
            // FAnchorData: offsets (X 0, Y = the column's top), anchors (0.5, 0)-(0.5, 0), alignment (0.5, 1); auto size
            const float layout[10] = {0.0f, top, 0.0f, 0.0f, 0.5f, 0.0f, 0.5f, 0.0f, 0.5f, 1.0f};
            PCall l(find_fn(STR("/Script/UMG.CanvasPanelSlot:SetLayout")));
            l.copy_in(STR("InLayoutData"), layout).call(slot);
            PCall a(find_fn(STR("/Script/UMG.CanvasPanelSlot:SetAutoSize")));
            a.set<bool>(STR("InbAutoSize"), true).call(slot);
            if (g_log_tiles++ < kLogLimit)
            {
                Output::send<LogLevel::Verbose>(STR("[FuserNative] tiles: label {} on {} (column top {}, style {})\n"), ours->GetName(), tile->GetName(), top,
                                                style ? style->GetName() : StringType{STR("none")});
            }
        }
        return {FWeakObjectPtr(tile), FWeakObjectPtr(ours), FWeakObjectPtr(style)};
    }

    auto on_tile_song(UObject* tile, UObject* song) -> void
    {
        auto it = g_tile_labels.find(tile);
        const bool known = it != g_tile_labels.end() && it->second.tile.Get() == tile && it->second.ours.Get();
        if (!known)
        {
            if (!song || g_keybpm_mode == kKeyBpmOff || !ensure_ready()) return;
            if (g_tile_labels.size() >= kTileLabelsPrune)
            {
                for (auto e = g_tile_labels.begin(); e != g_tile_labels.end();)
                    e = e->second.tile.Get() ? std::next(e) : g_tile_labels.erase(e);
            }
            TileLabel made = create_tile_label(tile);
            if (!made.ours.Get()) return;
            it = g_tile_labels.insert_or_assign(tile, made).first;
        }
        it->second.song = FWeakObjectPtr(song);
        show_tile_label(it->second.ours.Get(), song, it->second.style.Get());
    }

    // Native post-hook: UHypeSongWidget::SetCelSongSet (the crate's own slots)
    auto on_set_cel_song_set_post(UnrealScriptFunctionCallableContext& ctx, void*) -> void
    {
        try
        {
            if (ctx.Context && ctx.Context->GetClassPrivate()->GetFName() == g_name_song_tile) on_tile_song(ctx.Context, obj_prop(ctx.Context, STR("_myCelSongSet")));
        }
        catch (const std::exception& e)
        {
            Output::send<LogLevel::Error>(STR("[FuserNative] tiles: exception {}\n"), to_wstring(e.what()));
        }
    }

    // ProcessEvent post-callback: WBP_GridItem_Collection_Browse_Song::OnListItemInitialized (the library's tiles)
    auto on_song_grid_item_initialized(UObject* item) -> void
    {
        UObject* tile = obj_prop(item, STR("ItemWidget"));
        if (!tile || tile->GetClassPrivate()->GetFName() != g_name_song_tile) return;
        UObject* song = obj_prop(item, STR("_CachedCelSongSet")); // the list data, already cast to CelSongSet
        static int logged = 0;
        if (logged++ < kLogLimit)
        {
            Output::send<LogLevel::Verbose>(STR("[FuserNative] tiles: library item {} -> '{}'\n"), item->GetName(), song ? song_name(song) : StringType{STR("no song")});
        }
        on_tile_song(tile, song);
    }

    // Key and BPM Display or Key Labels changed: every live tile label is shown, hidden or re-filled.
    auto apply_to_tile_labels() -> void
    {
        int n = 0;
        for (auto& [raw, e] : g_tile_labels)
        {
            UObject* tile = e.tile.Get();
            UObject* ours = e.ours.Get();
            if (!tile || !ours) continue;
            show_tile_label(ours, e.song.Get(), e.style.Get());
            ++n;
        }
        Output::send<LogLevel::Verbose>(STR("[FuserNative] tiles: settings applied to {} labels\n"), n);
    }

    // One stock list page's parts: the switcher child (CanvasPanel), its helper and pool, and the helper's ScrollBox.
    struct PageParts
    {
        UObject* canvas = nullptr;
        UObject* helper = nullptr;
        UObject* pool = nullptr;
        UObject* scroll = nullptr;
        auto ok() const -> bool { return canvas && helper && pool && scroll; }
    };

    auto page_parts(UObject* screen, const CharType* helper, const CharType* pool, const CharType* scroll) -> PageParts
    {
        PageParts p{};
        p.helper = obj_prop(screen, helper);
        p.pool = obj_prop(screen, pool);
        p.scroll = obj_prop(screen, scroll);
        p.canvas = widget_parent(p.helper);
        if (p.canvas && widget_parent(p.pool) != p.canvas) p.canvas = nullptr;
        return p;
    }

    struct SlotField
    {
        const CharType* prop;
        const CharType* setter;
        const CharType* param;
    };

    auto copy_slot_fields(UObject* from, UObject* to, const SlotField* fields, size_t n) -> void
    {
        if (!from || !to) return;
        for (size_t i = 0; i < n; ++i)
        {
            FProperty* sp = find_field(from->GetClassPrivate(), fields[i].prop);
            if (!sp) continue;
            PCall c(find_fn(fields[i].setter));
            c.copy_in(fields[i].param, sp->ContainerPtrToValuePtr<void>(from)).call(to);
        }
    }

    auto copy_switcher_slot(UObject* from, UObject* to) -> void
    {
        static const SlotField fields[] = {
                {STR("Padding"), STR("/Script/UMG.WidgetSwitcherSlot:SetPadding"), STR("InPadding")},
                {STR("HorizontalAlignment"), STR("/Script/UMG.WidgetSwitcherSlot:SetHorizontalAlignment"), STR("InHorizontalAlignment")},
                {STR("VerticalAlignment"), STR("/Script/UMG.WidgetSwitcherSlot:SetVerticalAlignment"), STR("InVerticalAlignment")},
        };
        copy_slot_fields(from, to, fields, std::size(fields));
    }

    // Copy the plain configuration values (bool/byte/int/float/enum/name/class, and value structs such as styles when
    // asked) a struct declares from one object to another; object references, containers, delegates and transient
    // state are left alone.
    auto copy_plain_properties(UStruct* owner, UObject* from, UObject* to, const std::vector<StringType>& skip = {}, bool with_structs = false) -> int
    {
        static const FName struct_type(STR("StructProperty"), FNAME_Add);
        static const FName plain[] = {FName(STR("BoolProperty"), FNAME_Add), FName(STR("ByteProperty"), FNAME_Add), FName(STR("IntProperty"), FNAME_Add),
                                      FName(STR("FloatProperty"), FNAME_Add), FName(STR("EnumProperty"), FNAME_Add), FName(STR("NameProperty"), FNAME_Add),
                                      FName(STR("ClassProperty"), FNAME_Add)};
        if (!owner || !from || !to) return 0;
        int copied = 0;
        for (FProperty* p : TFieldRange<FProperty>(owner, EFieldIterationFlags::None))
        {
            if (p->HasAnyPropertyFlags(CPF_Transient | CPF_Parm)) continue;
            const FName type = p->GetClass().GetFName();
            if (std::find(std::begin(plain), std::end(plain), type) == std::end(plain) && !(with_structs && type == struct_type)) continue;
            if (std::find(skip.begin(), skip.end(), p->GetName()) != skip.end()) continue;
            p->CopyCompleteValue(p->ContainerPtrToValuePtr<void>(to), p->ContainerPtrToValuePtr<void>(from));
            ++copied;
        }
        return copied;
    }

    // A new list page like `tpl`, named `name`: CanvasPanel [helper, pool], ScrollBox in the helper's ListContentSlot,
    // wired the way DoOnInitialized wires the stock ones (SetScrollBox(box, true), SetUserWidgetPool(pool)). Not yet in
    // the switcher. Works before and after the screen's Slate exists (pages may be added when a bigger crate opens).
    auto create_list_page(UObject* screen, const PageParts& tpl, const StringType& name) -> UObject*
    {
        UObject* tree = tpl.helper->GetOuterPrivate(); // the screen's WidgetTree
        UObject* canvas = UObjectGlobals::NewObject<UObject>(tree, tpl.canvas->GetClassPrivate(), FName(name.c_str(), FNAME_Add));
        // A fresh ScrollBox with its stock twin's settings: its own values including the style structs, and the plain
        // UWidget ones (Visibility, Clipping, render transform...). Not a template copy: once the twin holds items, a
        // copy would share its Slots, and it carries the twin's Slot and delegate bindings.
        static UClass* widget_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Widget"));
        UObject* scroll = UObjectGlobals::NewObject<UObject>(tree, tpl.scroll->GetClassPrivate());
        if (scroll)
        {
            copy_plain_properties(tpl.scroll->GetClassPrivate(), tpl.scroll, scroll, {}, true);
            copy_plain_properties(widget_class, tpl.scroll, scroll, {STR("bIsVariable"), STR("bCreatedByConstructionScript")}, true);
        }
        // A fresh pool with the stock one's settings (a template copy would share its item sets).
        UObject* pool = UObjectGlobals::NewObject<UObject>(tree, tpl.pool->GetClassPrivate());
        if (pool) copy_plain_properties(tpl.pool->GetClassPrivate(), tpl.pool, pool, {STR("_isInitialized")});
        UObject* helper = create_widget(screen, tpl.helper->GetClassPrivate(), owning_player(screen));
        if (!canvas || !scroll || !pool || !helper)
        {
            Output::send<LogLevel::Warning>(STR("[FuserNative] crate edit: creating a page failed (canvas {}, scroll {}, pool {}, helper {})\n"), canvas != nullptr,
                                            scroll != nullptr, pool != nullptr, helper != nullptr);
            return nullptr;
        }
        static UClass* list_widget = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Hype.HypeListWidget"));
        static UClass* widget = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Widget"));
        const int copied = copy_plain_properties(list_widget, tpl.helper, helper) + copy_plain_properties(tpl.helper->GetClassPrivate(), tpl.helper, helper);
        if (FProperty* vis = find_field(widget, STR("Visibility")))
        {
            vis->CopyCompleteValue(vis->ContainerPtrToValuePtr<void>(helper), vis->ContainerPtrToValuePtr<void>(tpl.helper));
        }
        copy_canvas_slot(obj_prop(tpl.helper, STR("Slot")), add_child(canvas, helper));
        copy_canvas_slot(obj_prop(tpl.pool, STR("Slot")), add_child(canvas, pool));
        // The ScrollBox goes into the helper's ListContentSlot (a NamedSlot). UUserWidget::SetContentForSlot is not a
        // UFUNCTION in this build (a PCall of it silently did nothing), so set the NamedSlot's content directly.
        {
            static UFunction* fn = find_fn(STR("/Script/UMG.ContentWidget:SetContent"));
            UObject* named = obj_prop(helper, STR("ListContentSlot"));
            PCall c(fn);
            c.set<UObject*>(STR("Content"), scroll).call(named);
            if (!fn || !named || widget_parent(scroll) != named)
            {
                Output::send<LogLevel::Warning>(STR("[FuserNative] crate edit: placing {} in {}.ListContentSlot failed (SetContent {}, slot {}, parent {})\n"), scroll->GetName(),
                                                helper->GetName(), fn != nullptr, named != nullptr,
                                                widget_parent(scroll) ? widget_parent(scroll)->GetName() : StringType{STR("null")});
                return nullptr;
            }
        }
        {
            static UFunction* fn = find_fn(STR("/Script/Hype.HypeListWidget:SetScrollBox"));
            PCall c(fn);
            c.set<UObject*>(STR("inScrollbox"), scroll).set<bool>(STR("setAsItemParent"), true).call(helper);
        }
        {
            static UFunction* fn = find_fn(STR("/Script/Hype.HypeListWidget:SetUserWidgetPool"));
            PCall c(fn);
            c.set<UObject*>(STR("inWidgetPool"), pool).call(helper);
        }
        if (g_log_crate_edit++ < kLogLimit * 4)
        {
            Output::send<LogLevel::Verbose>(STR("[FuserNative] crate edit: page {} = [{}] like {} ({} helper settings copied; ListItemWidget {}, pool numItems {})\n"),
                                            canvas->GetName(), child_names(canvas), tpl.canvas->GetName(), copied,
                                            obj_prop(helper, STR("ListItemWidget")) ? obj_prop(helper, STR("ListItemWidget"))->GetName() : StringType{STR("null")},
                                            int_prop(pool, STR("numItems"), -1));
        }
        return canvas;
    }

    auto add_to_switcher(UObject* switcher, UObject* page, UObject* layout_from) -> bool
    {
        UObject* slot = add_child(switcher, page);
        copy_switcher_slot(layout_from, slot);
        return slot != nullptr;
    }

    auto tabs_for(int32 n, int32 per) -> int32
    {
        return n > 0 ? (n + per - 1) / per : 0;
    }

    // Grow the screen to at least `songs` song tabs and `moments` moment tabs (never shrinks: a smaller crate hides
    // the tabs it doesn't use). New pages are appended to the switcher; TabArray / UnlockableTypeByTabIndex get Songs
    // entries after the last song tab and Moments entries at the end. If the TabList has already built its buttons,
    // they are rebuilt with its own RebuildTabsWithNewStrings (its language-change path).
    auto ensure_tabs(UObject* screen, int32 songs, int32 moments, const CharType* why) -> bool
    {
        const TabLayout l = tab_layout(screen);
        songs = std::max(songs, l.songs);
        moments = std::max(moments, l.moments);
        if (songs == l.songs && moments == l.moments) return true;
        UObject* switcher = obj_prop(screen, STR("TopListsSwitcher"));
        UObject* tabs = obj_prop(screen, STR("TopTabs"));
        const int32 n_tab_array = array_num(tabs, STR("TabArray")), n_types = array_num(screen, STR("UnlockableTypeByTabIndex"));
        bool pages_ok = true;
        for (int32 t = 0; t < l.total(); ++t) pages_ok &= page_for_tab(screen, l, t) != nullptr;
        if (!switcher || n_tab_array != l.total() || n_types != l.total() || !pages_ok)
        {
            Output::send<LogLevel::Warning>(STR("[FuserNative] crate edit: {} - unexpected layout (TabArray {}, UnlockableTypeByTabIndex {}, {} tabs, pages {}); not growing\n"), why,
                                            n_tab_array, n_types, l.total(), pages_ok);
            return false;
        }
        const PageParts song_tpl = page_parts(screen, STR("SongListHelperC"), STR("EquippedSongListPoolC"), STR("SongListScrollBoxC"));
        const PageParts moment_tpl = page_parts(screen, STR("EquippedMomentListHelper"), STR("EquippedMomentsListPool"), STR("MomentListScrollBox"));
        if (!song_tpl.ok() || !moment_tpl.ok())
        {
            Output::send<LogLevel::Warning>(STR("[FuserNative] crate edit: {} - stock pages not found (songs {}, moments {}); not growing\n"), why, song_tpl.ok(), moment_tpl.ok());
            return false;
        }

        // 1. every new page first, so a failure leaves the screen as it was
        std::vector<UObject*> song_pages{}, moment_pages{};
        for (int32 p = l.songs; p < songs; ++p) song_pages.push_back(create_list_page(screen, song_tpl, new_page_name(false, p)));
        for (int32 p = l.moments; p < moments; ++p) moment_pages.push_back(create_list_page(screen, moment_tpl, new_page_name(true, p)));
        for (UObject* p : song_pages)
        {
            if (!p) return false;
        }
        for (UObject* p : moment_pages)
        {
            if (!p) return false;
        }

        // 2. append them to the switcher
        bool ok = true;
        for (UObject* p : song_pages) ok &= add_to_switcher(switcher, p, obj_prop(song_tpl.canvas, STR("Slot")));
        for (UObject* p : moment_pages) ok &= add_to_switcher(switcher, p, obj_prop(moment_tpl.canvas, STR("Slot")));

        // 3. tab data
        const int32 extra_songs = songs - l.songs, extra_moments = moments - l.moments, mid = l.total() + extra_songs;
        for (UObject* o : {tabs, screen})
        {
            const CharType* arr = o == tabs ? STR("TabArray") : STR("UnlockableTypeByTabIndex");
            ok &= insert_copies(o, arr, l.songs - 1, l.songs, extra_songs);
            ok &= insert_copies(o, arr, mid - 1, mid, extra_moments);
        }
        if (auto* v = screen->GetValuePtrByPropertyNameInChain<int32>(STR("_NumSongsTabs"))) *v = songs;
        if (auto* v = screen->GetValuePtrByPropertyNameInChain<int32>(STR("_LastSongTabIndex"))) *v = songs - 1;

        auto* buttons = tabs->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("TabWidgetArray"));
        const int32 n_buttons = buttons ? buttons->Num() : -1;
        if (n_buttons > 0)
        {
            PCall c(own_fn(tabs, STR("RebuildTabsWithNewStrings")));
            c.call(tabs);
        }
        const TabLayout now = tab_layout(screen);
        Output::send<LogLevel::Verbose>(STR("[FuserNative] crate edit: {} - grew to {} song tabs, {} moment tabs{}; switcher [{}]; TabArray {}, UnlockableTypeByTabIndex {}, "
                                            "buttons {} -> {}\n"),
                                        why, now.songs, now.moments, ok ? STR("") : STR(" WITH ERRORS"), child_names(switcher), array_num(tabs, STR("TabArray")),
                                        array_num(screen, STR("UnlockableTypeByTabIndex")), n_buttons, buttons ? buttons->Num() : -1);
        return ok;
    }

    // Tabs the edited crate needs: its song / moment capacity (stock mission rules), 8 per tab.
    auto ensure_tabs_for_crate(UObject* screen) -> void
    {
        const CrateView c(edit_crate(screen));
        if (!c.ok()) return;
        const TabLayout l = tab_layout(screen);
        const int32 songs = c.mission ? std::min(c.i32(STR("NumSongSlots")), c.constraint(STR("NumSongSlots")))
                                      : std::max(c.i32(STR("NumSongSlots")), c.num(STR("songs")));
        const int32 moments = c.mission ? c.i32(STR("NumMomentSlots")) : std::max(c.i32(STR("NumMomentSlots")), c.num(STR("moments")));
        if (tabs_for(songs, l.per) > l.songs || tabs_for(moments, l.per) > l.moments)
        {
            ensure_tabs(screen, tabs_for(songs, l.per), tabs_for(moments, l.per), fmt::format(STR("crate {} ({} songs, {} moments)"), c.crate->GetName(), songs, moments).c_str());
        }
    }

    // At OnInitialized (before Slate): tabs for the largest crate known then, and Game.ini. The CrateMgr lists are
    // still empty when the shell builds this screen (0 crates), so a bigger crate grows it when it is opened.
    auto extend_crate_edit(UObject* screen) -> void
    {
        StringType detail{};
        const CrateSlots want = crate_slots(&detail);
        const TabLayout l = tab_layout(screen);
        const int32 songs = std::max(kStockSongTabs, tabs_for(want.songs, l.per));
        const int32 moments = std::max(1, tabs_for(want.moments, l.per));
        Output::send<LogLevel::Verbose>(STR("[FuserNative] crate edit: {} OnInitialized - provision songs {} moments {} ({}) -> {} song tabs, {} moment tabs; "
                                            "switcher children {}, TabArray {}, UnlockableTypeByTabIndex {}, _NumSongsTabs {}\n"),
                                        screen->GetName(), want.songs, want.moments, detail, songs, moments, child_count(obj_prop(screen, STR("TopListsSwitcher"))),
                                        array_num(obj_prop(screen, STR("TopTabs")), STR("TabArray")), array_num(screen, STR("UnlockableTypeByTabIndex")), l.songs);
        if (l.songs != kStockSongTabs || l.moments != 1 || child_count(obj_prop(screen, STR("TopListsSwitcher"))) != kStockTabs)
        {
            Output::send<LogLevel::Warning>(STR("[FuserNative] crate edit: unexpected stock layout; leaving the screen unextended\n"));
            return;
        }
        ensure_tabs(screen, songs, moments, STR("OnInitialized"));
    }
    auto on_crate_edit_initialized(UObject* screen) -> void
    {
        g_crate_edit_screen = FWeakObjectPtr(screen);
        try
        {
            bind_crate_edit(screen);
            extend_crate_edit(screen);
        }
        catch (const std::exception& e)
        {
            Output::send<LogLevel::Error>(STR("[FuserNative] crate edit: exception {}\n"), to_wstring(e.what()));
        }
    }

    // ---------------------------------------------------------------- mod settings ----------------
    // A "Key and BPM Display" dropdown row in the Settings screen's General tab, after
    // ControllerPrompts. WBP_Screen_Settings' General tab is General_ScrollBox -> CanvasPanel_General (a
    // VerticalBox) of designer-placed WBP_Setting_* rows; a WBP_Setting_Dropdown takes its label, entries and value
    // through SetLabelContent / AddUniqueDropdownEntry / SetCurrentValue and reports a pick through
    // OnDropdownSelectionChanged_Event_0 (bound to its DropdownMenu's delegate, so ProcessEvent-dispatched). Its
    // Construct copies HypeListParent (a designer value) into the menu, so ours takes the Language row's.
    // The value is kept in our settings.ini (g_keybpm_mode, above) and restored at every OnScreenActivated (one Settings
    // screen instance serves every opening, freestyle's pause menu included).
    FName g_name_settings{}, g_name_selection_changed{}, g_name_on_highlighted{}, g_name_on_unhighlighted{}, g_name_screen_activated{},
            g_name_destruct{}, g_name_toggle_changed{};
    FWeakObjectPtr g_settings_screen{}, g_settings_row{};
    int g_log_settings = 0, g_log_hover = 0;

    auto set_text_param(UObject* obj, const CharType* fn_name, const CharType* param, const CharType* text) -> bool
    {
        PCall c(own_fn(obj, fn_name));
        const FText value(text); // engine copy through the property (as for the search box's hint text)
        return c.copy_in(param, &value).call(obj);
    }

    // Insert `row` into VerticalBox `box` right after `after`: UMG has no Blueprint insert-at-index, so the rows after it
    // are removed and re-added behind ours, each keeping its slot layout (remove and re-add).
    auto insert_after(UObject* box, UObject* after, UObject* row) -> bool
    {
        const int32 at = child_index(box, after);
        if (at < 0) return false;
        std::vector<std::pair<UObject*, UObject*>> tail{}; // child, old slot
        while (child_count(box) > at + 1)
        {
            UObject* c = child_at(box, at + 1);
            tail.emplace_back(c, obj_prop(c, STR("Slot")));
            if (!remove_child(box, c)) return false;
        }
        UObject* slot = add_child(box, row);
        copy_slot_layout(obj_prop(after, STR("Slot")), slot);
        for (auto& [c, old] : tail) copy_slot_layout(old, add_child(box, c));
        return slot != nullptr;
    }

    // A dropdown row like `tpl` (a stock WBP_Setting_Dropdown) with our label and entries, inserted after `after`.
    auto make_dropdown_row(UObject* screen, UObject* tpl, UObject* after, const CharType* label, std::initializer_list<const CharType*> options) -> UObject*
    {
        UObject* box = widget_parent(after);
        UObject* row = box ? create_widget(screen, tpl->GetClassPrivate(), owning_player(screen)) : nullptr;
        if (!row) return nullptr;
        if (auto* dst = row->GetValuePtrByPropertyNameInChain<UObject*>(STR("HypeListParent"))) *dst = obj_prop(tpl, STR("HypeListParent"));
        // The row class's defaults carry 4 graphics-quality entries (ours became entries 5 and 6): empty it first
        ArrayView entries(array_prop(row, STR("_DropdownEntries")), row);
        if (entries.ok())
        {
            for (int32 i = 0; i < entries.num(); ++i) entries.prop->GetInner()->DestroyValue(entries.at(i));
            entries.raw->num = 0;
        }
        set_text_param(row, STR("SetLabelContent"), STR("in_LabelContent"), label);
        for (const CharType* o : options) set_text_param(row, STR("AddUniqueDropdownEntry"), STR("NewDropdownOption"), o);
        const bool placed = insert_after(box, after, row);
        Output::send<LogLevel::Verbose>(STR("[FuserNative] settings: dropdown '{}' = {} ({} entries) placed {}\n"), label, row->GetName(), array_num(row, STR("_DropdownEntries")), placed);
        return placed ? row : nullptr;
    }

    // A toggle row like `tpl` (a stock WBP_Setting_Toggle, whose On / Off labels it copies) inserted after `after`.
    auto make_toggle_row(UObject* screen, UObject* tpl, UObject* after, const CharType* label) -> UObject*
    {
        UObject* box = widget_parent(after);
        UObject* row = box ? create_widget(screen, tpl->GetClassPrivate(), owning_player(screen)) : nullptr;
        if (!row) return nullptr;
        for (const CharType* name : {STR("_OnLabel"), STR("_OffLabel")})
        {
            FProperty* f = find_field(tpl->GetClassPrivate(), name);
            if (f) f->CopyCompleteValue(f->ContainerPtrToValuePtr<void>(row), f->ContainerPtrToValuePtr<void>(tpl));
        }
        if (auto* dst = row->GetValuePtrByPropertyNameInChain<UObject*>(STR("HypeListParent"))) *dst = obj_prop(tpl, STR("HypeListParent"));
        set_text_param(row, STR("SetLabelContent"), STR("in_LabelContent"), label);
        const bool placed = insert_after(box, after, row);
        Output::send<LogLevel::Verbose>(STR("[FuserNative] settings: toggle '{}' = {} placed {}\n"), label, row->GetName(), placed);
        return placed ? row : nullptr;
    }

    // ---------------------------------------------------------------- new-crate slot sliders -------
    // "New Crate Song Slots" (24-144) and "New Crate Snapshot Slots" (0-120), in steps of 8, songs + snapshots <= 144.
    // Raising one past the cap lowers the other by the overflow; lowering one never raises the other.
    // The game uses CrateMgr's copies of HypeSettings' DefaultCrate*SlotsNum (not reflected: its getters
    // GetDefaultCrateSongSlotsNum / ...MomentSlotsNum return CrateMgr+0x88 / +0x94, and CreateCustomCrate copies them
    // into each new crate), so a value goes to both: the HypeSettings CDO (read at CrateMgr's init, and by our tab
    // provisioning) and the live CrateMgr (checked against its getters first). Existing crates keep their own counts.
    // WBP_Setting_Slider snaps by itself: _LabelGridSize grid-snaps the value on release and in its value text, and
    // SetupNotchedBehavior (its Construct) turns the grid into notches. SetCurrentValueNonAlpha(v, IsInitializing =
    // true) sets it without broadcasting.
    constexpr int32 kSlotStep = 8, kSlotCap = 144;
    constexpr int32 kSongSlotsMin = 24, kSongSlotsMax = kSlotCap, kMomentSlotsMin = 0, kMomentSlotsMax = kSlotCap - kSongSlotsMin;
    constexpr size_t kCrateMgrSongSlots = 0x88, kCrateMgrMomentSlots = 0x94;
    FWeakObjectPtr g_row_songs{}, g_row_moments{}, g_crate_mgr{};
    int32 g_slider_songs = -1, g_slider_moments = -1; // what the rows show
    bool g_crate_mgr_synced = false;
    FName g_name_slider_value{};

    auto snap_slots(float v, int32 lo, int32 hi) -> int32
    {
        const int32 snapped = static_cast<int32>(std::lround(v / kSlotStep)) * kSlotStep;
        return std::clamp(snapped, lo, hi);
    }

    auto settings_cdo() -> UObject* { return UObjectGlobals::StaticFindObject(nullptr, nullptr, STR("/Script/Hype.Default__HypeSettings")); }

    auto cdo_slots(const CharType* prop) -> int32
    {
        UObject* cdo = settings_cdo();
        auto* v = cdo ? cdo->GetValuePtrByPropertyNameInChain<int32>(prop) : nullptr;
        return v ? *v : -1;
    }

    // A live instance: not a class default object or archetype (EObjectFlags @0x8), nor a subobject of one
    auto is_live_object(UObject* o) -> bool
    {
        if (*reinterpret_cast<const uint32*>(reinterpret_cast<const uint8*>(o) + 0x8) & 0x30) return false;
        for (UObject* x = o; x; x = x->GetOuterPrivate())
            if (x->GetName().rfind(STR("Default__"), 0) == 0) return false;
        return true;
    }

    auto crate_mgr() -> UObject*
    {
        if (UObject* m = g_crate_mgr.Get()) return m;
        std::vector<UObject*> all{};
        UObjectGlobals::FindAllOf(STR("CrateMgr"), all);
        for (UObject* o : all)
            if (is_live_object(o))
            {
                g_crate_mgr = FWeakObjectPtr(o);
                return o;
            }
        return nullptr;
    }

    auto crate_mgr_getter(UObject* mgr, const CharType* fn) -> int32
    {
        PCall c(own_fn(mgr, fn));
        return c.call(mgr) ? c.get<int32>(STR("ReturnValue")) : -1;
    }

    // Our values into the CDO and, once it exists, the live CrateMgr. False while there is no CrateMgr yet.
    auto apply_new_crate_slots(const CharType* when) -> bool
    {
        if (g_new_crate_songs < 0 || g_new_crate_moments < 0) return true; // never set: the game's own values stand
        if (UObject* cdo = settings_cdo())
        {
            if (auto* v = cdo->GetValuePtrByPropertyNameInChain<int32>(STR("DefaultCrateSongSlotsNum"))) *v = g_new_crate_songs;
            if (auto* v = cdo->GetValuePtrByPropertyNameInChain<int32>(STR("DefaultCrateMomentSlotsNum"))) *v = g_new_crate_moments;
        }
        UObject* mgr = crate_mgr();
        if (!mgr) return false;
        auto* raw = reinterpret_cast<uint8*>(mgr);
        const int32 got_songs = crate_mgr_getter(mgr, STR("GetDefaultCrateSongSlotsNum")), got_moments = crate_mgr_getter(mgr, STR("GetDefaultCrateMomentSlotsNum"));
        int32& songs = *reinterpret_cast<int32*>(raw + kCrateMgrSongSlots);
        int32& moments = *reinterpret_cast<int32*>(raw + kCrateMgrMomentSlots);
        if (songs != got_songs || moments != got_moments)
        {
            Output::send<LogLevel::Warning>(STR("[FuserNative] crate slots: CrateMgr +0x88/+0x94 = {}/{} but its getters say {}/{} - not written ({})\n"), songs, moments,
                                            got_songs, got_moments, when);
            return true; // don't retry a layout we don't trust
        }
        const int32 before_songs = songs, before_moments = moments;
        songs = g_new_crate_songs;
        moments = g_new_crate_moments;
        Output::send<LogLevel::Verbose>(STR("[FuserNative] crate slots ({}): new crates {} songs / {} snapshots (CrateMgr was {}/{}, getters now {}/{})\n"), when, songs,
                                        moments, before_songs, before_moments, crate_mgr_getter(mgr, STR("GetDefaultCrateSongSlotsNum")),
                                        crate_mgr_getter(mgr, STR("GetDefaultCrateMomentSlotsNum")));
        return true;
    }

    // The rows' starting values: ours if set, else the game's current ones - snapped, clamped and capped for display
    auto init_slider_values() -> void
    {
        const int32 songs = g_new_crate_songs >= 0 ? g_new_crate_songs : cdo_slots(STR("DefaultCrateSongSlotsNum"));
        const int32 moments = g_new_crate_moments >= 0 ? g_new_crate_moments : cdo_slots(STR("DefaultCrateMomentSlotsNum"));
        g_slider_songs = snap_slots(static_cast<float>(songs < 0 ? kSongSlotsMin : songs), kSongSlotsMin, kSongSlotsMax);
        g_slider_moments = snap_slots(static_cast<float>(moments < 0 ? kMomentSlotsMin : moments), kMomentSlotsMin, kMomentSlotsMax);
        if (g_slider_songs + g_slider_moments > kSlotCap) g_slider_moments = kSlotCap - g_slider_songs;
    }

    auto set_slider_value(UObject* row, int32 value) -> void
    {
        PCall set(own_fn(row, STR("SetCurrentValueNonAlpha")));
        set.set<float>(STR("in_NewValue"), static_cast<float>(value)).set<bool>(STR("IsInitializing"), true).call(row);
    }

    // A slider row like `tpl` (a stock WBP_Setting_Slider) over [lo, hi] in steps of kSlotStep, inserted after `after`.
    auto make_slider_row(UObject* screen, UObject* tpl, UObject* after, const CharType* label, int32 lo, int32 hi, int32 value) -> UObject*
    {
        UObject* box = widget_parent(after);
        UObject* row = box ? create_widget(screen, tpl->GetClassPrivate(), owning_player(screen)) : nullptr;
        if (!row) return nullptr;
        if (auto* dst = row->GetValuePtrByPropertyNameInChain<UObject*>(STR("HypeListParent"))) *dst = obj_prop(tpl, STR("HypeListParent"));
        if (auto* grid = row->GetValuePtrByPropertyNameInChain<float>(STR("_LabelGridSize"))) *grid = static_cast<float>(kSlotStep);
        if (auto* bias = row->GetValuePtrByPropertyNameInChain<float>(STR("LabelValueRoundingBias"))) *bias = 0.0f;
        if (FProperty* units = find_field(row->GetClassPrivate(), STR("_UnitsText")))
        {
            const FText plain(STR("{Quantity}")); // the value text: just the number (the stock rows show e.g. a percentage)
            units->CopyCompleteValue(units->ContainerPtrToValuePtr<void>(row), &plain);
        }
        set_text_param(row, STR("SetLabelContent"), STR("in_LabelContent"), label);
        PCall extents(own_fn(row, STR("SetLabelExtents")));
        extents.set<float>(STR("in_MinLabelValue"), static_cast<float>(lo)).set<float>(STR("in_MaxLabelValue"), static_cast<float>(hi)).call(row);
        PCall reset(own_fn(row, STR("OverrideDoubleClickToResetSlider")));
        reset.set<bool>(STR("DoubleClickToReset"), false).call(row); // no meaningful stock value to reset to
        const bool placed = insert_after(box, after, row);          // Construct -> SetupNotchedBehavior with our grid
        PCall values(own_fn(row, STR("SetSliderValuesNonAlpha")));
        values.set<float>(STR("MinNonAlpha"), static_cast<float>(lo))
                .set<float>(STR("MaxNonAlpha"), static_cast<float>(hi))
                .set<float>(STR("DefaultValueNonAlpha"), static_cast<float>(value))
                .set<float>(STR("CurrentValueNonAlpha"), static_cast<float>(value))
                .set<bool>(STR("IsInitializing"), true)
                .call(row);
        PCall notches(own_fn(row, STR("SetupNotchedBehavior")));
        notches.call(row); // again, in case Construct ran before the grid was set
        Output::send<LogLevel::Verbose>(STR("[FuserNative] settings: slider '{}' = {} [{}..{} step {}] value {} placed {}\n"), label, row->GetName(), lo, hi, kSlotStep, value,
                                        placed);
        return placed ? row : nullptr;
    }

    // A slider moved (HypeSliderValueChanged, alpha 0..1, while dragging or stepping)
    auto on_slot_slider(bool songs_row, float alpha) -> void
    {
        const int32 lo = songs_row ? kSongSlotsMin : kMomentSlotsMin, hi = songs_row ? kSongSlotsMax : kMomentSlotsMax;
        const int32 v = snap_slots(static_cast<float>(lo) + alpha * static_cast<float>(hi - lo), lo, hi);
        int32& mine = songs_row ? g_slider_songs : g_slider_moments;
        int32& other = songs_row ? g_slider_moments : g_slider_songs;
        if (v == mine && g_new_crate_songs >= 0) return;
        mine = v;
        if (mine + other > kSlotCap)
        {
            other = kSlotCap - mine; // lowered by the overflow, 8 per notch past the cap
            if (UObject* row = (songs_row ? g_row_moments : g_row_songs).Get()) set_slider_value(row, other);
        }
        g_new_crate_songs = g_slider_songs;
        g_new_crate_moments = g_slider_moments;
        save_settings();
        apply_new_crate_slots(STR("Settings"));
    }

    // Every frame until done: CrateMgr takes its copies at its init; ours must reach them even if that came first
    auto crate_slots_tick() -> void
    {
        static uint64 n = 0;
        if (g_crate_mgr_synced || g_new_crate_songs < 0 || n++ % 60 != 0) return;
        g_crate_mgr_synced = apply_new_crate_slots(STR("startup"));
    }

    FWeakObjectPtr g_row_key_labels{}, g_row_search{}, g_row_download{}, g_row_download_mix{}, g_row_send{}, g_row_send_kbps{}, g_row_send_max{};

    // The Upload Speed Limit row's entry for a cap: the nearest option.
    auto send_kbps_index(int32 kbps) -> int32
    {
        int32 best = 0;
        for (int32 i = 1; i < static_cast<int32>(std::size(kSendKbpsOptions)); ++i)
            if (std::abs(kSendKbpsOptions[i] - kbps) < std::abs(kSendKbpsOptions[best] - kbps)) best = i;
        return best;
    }

    // The two upload rows only matter while Send Songs to Other Players is on: greyed out (UWidget::SetIsEnabled)
    // otherwise.
    auto apply_send_rows_enabled() -> void
    {
        for (UObject* row : {g_row_send_kbps.Get(), g_row_send_max.Get()})
        {
            if (!row) continue;
            PCall enable(own_fn(row, STR("SetIsEnabled")));
            enable.set<bool>(STR("bInIsEnabled"), g_send_songs).call(row);
        }
    }

    // Our rows, after ControllerPrompts in General: Key and BPM Display, Key Labels, Song Search Bar, the two crate-slot
    // sliders, then Download Songs, Download Mix Songs, Send Songs to Other Players, Upload Speed Limit and
    // Simultaneous Uploads.
    auto inject_settings_row(UObject* screen) -> void
    {
        UObject* language = obj_prop(screen, STR("Language"));
        UObject* prompts = obj_prop(screen, STR("ControllerPrompts"));
        UObject* toggle_tpl = obj_prop(screen, STR("SubtitlesSetting"));
        if (!language || !prompts || !toggle_tpl || !widget_parent(prompts))
        {
            Output::send<LogLevel::Warning>(STR("[FuserNative] settings: Language {} / ControllerPrompts {} / SubtitlesSetting {} / General box missing\n"), language != nullptr,
                                            prompts != nullptr, toggle_tpl != nullptr);
            return;
        }
        UObject* keybpm = make_dropdown_row(screen, language, prompts, STR("Key and BPM Display"), {STR("On idle"), STR("On hover"), STR("Off")});
        UObject* labels = keybpm ? make_dropdown_row(screen, language, keybpm, STR("Key Labels"), {STR("Flats"), STR("Sharps"), STR("Both")}) : nullptr;
        UObject* search = labels ? make_toggle_row(screen, toggle_tpl, labels, STR("Song Search Bar")) : nullptr;
        UObject* slider_tpl = obj_prop(screen, STR("ScrollSpeedSlider"));
        init_slider_values();
        UObject* songs = search && slider_tpl ? make_slider_row(screen, slider_tpl, search, STR("New Crate Song Slots"), kSongSlotsMin, kSongSlotsMax, g_slider_songs) : nullptr;
        UObject* moments = songs ? make_slider_row(screen, slider_tpl, songs, STR("New Crate Snapshot Slots"), kMomentSlotsMin, kMomentSlotsMax, g_slider_moments) : nullptr;
        UObject* tail = moments ? moments : search; // the sliders need ScrollSpeedSlider; without it these follow the search toggle
        UObject* download = tail ? make_dropdown_row(screen, language, tail, STR("Download Songs"), {STR("No"), STR("Temporarily"), STR("Permanently")}) : nullptr;
        UObject* download_mix = download ? make_dropdown_row(screen, language, download, STR("Download Mix Songs"), {STR("No"), STR("Temporarily"), STR("Permanently")})
                                         : nullptr;
        UObject* send = download_mix ? make_toggle_row(screen, toggle_tpl, download_mix, STR("Send Songs to Other Players")) : nullptr;
        UObject* send_kbps = send ? make_dropdown_row(screen, language, send, STR("Upload Speed Limit"),
                                                      {STR("256 KB/s"), STR("512 KB/s"), STR("1 MB/s"), STR("2 MB/s"), STR("4 MB/s")})
                                  : nullptr;
        UObject* send_max = send_kbps ? make_dropdown_row(screen, language, send_kbps, STR("Simultaneous Uploads"), {STR("1"), STR("2"), STR("3"), STR("4")}) : nullptr;
        g_row_download = FWeakObjectPtr(download);
        g_row_download_mix = FWeakObjectPtr(download_mix);
        g_row_send = FWeakObjectPtr(send);
        g_row_send_kbps = FWeakObjectPtr(send_kbps);
        g_row_send_max = FWeakObjectPtr(send_max);
        g_row_songs = FWeakObjectPtr(songs);
        g_row_moments = FWeakObjectPtr(moments);
        g_settings_row = FWeakObjectPtr(keybpm);
        g_row_key_labels = FWeakObjectPtr(labels);
        g_row_search = FWeakObjectPtr(search);
        Output::send<LogLevel::Verbose>(STR("[FuserNative] settings: General = [{}]\n"), child_names(widget_parent(prompts)));
    }
    auto set_row_value(UObject* row, int32 index, const CharType* when) -> void
    {
        PCall set(own_fn(row, STR("SetCurrentValue")));
        set.set<float>(STR("in_NewValue"), static_cast<float>(index)).set<bool>(STR("IsInitializing"), true).call(row);
        PCall get(own_fn(row, STR("GetCurrentValue")));
        get.call(row);
        Output::send<LogLevel::Verbose>(STR("[FuserNative] settings: {} - SetCurrentValue({}) -> GetCurrentValue {:.0f}\n"), when, index, get.get<float>(STR("ReturnValue")));
    }

    auto on_settings_event(UObject* screen, const FName& name) -> void
    {
        if (g_log_settings++ < kLogLimit * 4)
        {
            Output::send<LogLevel::Verbose>(STR("[FuserNative] settings: {} {} (row {})\n"), screen->GetName(), name.ToString(),
                                            g_settings_row.Get() ? g_settings_row.Get()->GetName() : StringType{STR("none")});
        }
        if (name == g_name_on_initialized)
        {
            g_settings_screen = FWeakObjectPtr(screen);
            inject_settings_row(screen);
        }
        else if (name == g_name_screen_activated)
        {
            if (UObject* row = g_settings_row.Get()) set_row_value(row, g_keybpm_mode, STR("Key and BPM Display"));
            if (UObject* row = g_row_key_labels.Get()) set_row_value(row, g_key_labels, STR("Key Labels"));
            if (UObject* row = g_row_search.Get())
            {
                // As the stock screen does for its toggles (Subtitles, Crossplay, HDR): InitializeToggleState also runs the
                // switch's InitializeGenericToggleState, which snaps its graphic; SetCurrentValue left the switch showing
                // off under an "On" label until the first click.
                PCall init(own_fn(row, STR("InitializeToggleState")));
                init.set<bool>(STR("inToggled"), g_search_enabled).call(row);
                Output::send<LogLevel::Verbose>(STR("[FuserNative] settings: Song Search Bar - InitializeToggleState({})\n"), g_search_enabled);
            }
            init_slider_values();
            if (UObject* row = g_row_songs.Get()) set_slider_value(row, g_slider_songs);
            if (UObject* row = g_row_moments.Get()) set_slider_value(row, g_slider_moments);
            if (UObject* row = g_row_download.Get()) set_row_value(row, g_download_songs, STR("Download Songs"));
            if (UObject* row = g_row_download_mix.Get()) set_row_value(row, g_download_mix_songs, STR("Download Mix Songs"));
            if (UObject* row = g_row_send.Get())
            {
                PCall init(own_fn(row, STR("InitializeToggleState"))); // as for Song Search Bar, above
                init.set<bool>(STR("inToggled"), g_send_songs).call(row);
                Output::send<LogLevel::Verbose>(STR("[FuserNative] settings: Send Songs to Other Players - InitializeToggleState({})\n"), g_send_songs);
            }
            if (UObject* row = g_row_send_kbps.Get()) set_row_value(row, send_kbps_index(g_send_kbps), STR("Upload Speed Limit"));
            if (UObject* row = g_row_send_max.Get()) set_row_value(row, g_send_max - kSendMaxMin, STR("Simultaneous Uploads"));
            apply_send_rows_enabled();
        }
    }

    auto dump_settings() -> void
    {
        UObject* row = g_settings_row.Get();
        UObject* screen = g_settings_screen.Get();
        if (!row || !screen) return;
        Output::send<LogLevel::Verbose>(STR("[FuserNative] F8: settings row {}\n"), layout_desc(row));
        Output::send<LogLevel::Verbose>(STR("[FuserNative] F8:   ControllerPrompts {}\n"), layout_desc(obj_prop(screen, STR("ControllerPrompts"))));
        Output::send<LogLevel::Verbose>(STR("[FuserNative] F8:   row's DropdownMenu {} (HypeListDropdownParent {})\n"), layout_desc(obj_prop(row, STR("DropdownMenu"))),
                                        obj_prop(obj_prop(row, STR("DropdownMenu")), STR("HypeListDropdownParent"))
                                                ? obj_prop(obj_prop(row, STR("DropdownMenu")), STR("HypeListDropdownParent"))->GetName()
                                                : StringType{STR("null")});
    }

    auto on_process_event_post(UObject* context, UFunction* fn, void* parms) -> void
    {
        if (!context || !fn) return;
        const FName name = fn->GetFName();
        if (name == g_name_on_initialized && context->GetClassPrivate()->GetFName() == g_name_crate_edit)
        {
            on_crate_edit_initialized(context);
            return;
        }
        if ((name == g_name_on_initialized || name == g_name_construct || name == g_name_destruct || name == g_name_screen_activated) &&
            context->GetClassPrivate()->GetFName() == g_name_settings)
        {
            try
            {
                on_settings_event(context, name);
            }
            catch (const std::exception& e)
            {
                Output::send<LogLevel::Error>(STR("[FuserNative] settings: exception {}\n"), to_wstring(e.what()));
            }
            return;
        }
        if (name == g_name_selection_changed)
        {
            FProperty* p = find_param(fn, STR("SelectedIndex"));
            const int32 index = p ? *p->ContainerPtrToValuePtr<int32>(parms) : -1;
            const bool ours = context == g_settings_row.Get();
            if (ours || g_log_settings++ < kLogLimit * 4)
            {
                Output::send<LogLevel::Verbose>(STR("[FuserNative] settings: {} selection -> {} ({})\n"), context->GetName(), index, ours ? STR("OUR ROW") : STR("stock row"));
            }
            if (ours && index >= kKeyBpmOnIdle && index <= kKeyBpmOff && index != g_keybpm_mode)
            {
                g_keybpm_mode = index;
                save_settings();
                apply_keybpm_mode_to_all();
                apply_to_tile_labels();
            }
            if (context == g_row_key_labels.Get() && index >= kKeyLabelsFlats && index <= kKeyLabelsBoth && index != g_key_labels)
            {
                g_key_labels = index;
                save_settings();
                apply_key_labels_to_all();
                apply_to_tile_labels();
            }
            // Download Songs: FUSERMultiplayer.dll reads DownloadSongs from the file, so saving is all it takes
            if (context == g_row_download.Get() && index >= kDownloadNo && index <= kDownloadPermanently && index != g_download_songs)
            {
                g_download_songs = index;
                save_settings();
                Output::send<LogLevel::Normal>(STR("[FuserNative] settings: Download Songs -> {}\n"),
                                               index == kDownloadNo ? STR("No") : index == kDownloadTemporarily ? STR("Temporarily") : STR("Permanently"));
            }
            // Download Mix Songs: read from the file the same way, when a shared mix is played
            if (context == g_row_download_mix.Get() && index >= kDownloadNo && index <= kDownloadPermanently && index != g_download_mix_songs)
            {
                g_download_mix_songs = index;
                save_settings();
                Output::send<LogLevel::Normal>(STR("[FuserNative] settings: Download Mix Songs -> {}\n"),
                                               index == kDownloadNo ? STR("No") : index == kDownloadTemporarily ? STR("Temporarily") : STR("Permanently"));
            }
            // Upload Speed Limit / Simultaneous Uploads: saved; FUSERMultiplayer.dll reads them for the next transfer
            if (context == g_row_send_kbps.Get() && index >= 0 && index < static_cast<int32>(std::size(kSendKbpsOptions)) &&
                kSendKbpsOptions[index] != g_send_kbps)
            {
                g_send_kbps = kSendKbpsOptions[index];
                save_settings();
                Output::send<LogLevel::Normal>(STR("[FuserNative] settings: Upload Speed Limit -> {} KB/s\n"), g_send_kbps);
            }
            if (context == g_row_send_max.Get() && index >= 0 && index <= kSendMaxMax - kSendMaxMin && index + kSendMaxMin != g_send_max)
            {
                g_send_max = index + kSendMaxMin;
                save_settings();
                Output::send<LogLevel::Normal>(STR("[FuserNative] settings: Simultaneous Uploads -> {}\n"), g_send_max);
            }
            return;
        }
        if (name == g_name_slider_value && (context == g_row_songs.Get() || context == g_row_moments.Get()))
        {
            FProperty* p = find_param(fn, STR("newValue"));
            if (p) on_slot_slider(context == g_row_songs.Get(), *p->ContainerPtrToValuePtr<float>(parms));
            return;
        }
        if (name == g_name_toggle_changed && context == g_row_send.Get())
        {
            FProperty* p = find_param(fn, STR("isToggled"));
            const bool on = p && static_cast<FBoolProperty*>(p)->GetPropertyValue(p->ContainerPtrToValuePtr<void>(parms));
            if (on != g_send_songs)
            {
                g_send_songs = on;
                save_settings();
                apply_send_rows_enabled();
                Output::send<LogLevel::Normal>(STR("[FuserNative] settings: Send Songs to Other Players -> {}\n"), on ? STR("Yes") : STR("No"));
            }
            return;
        }
        if (name == g_name_toggle_changed && context == g_row_search.Get())
        {
            FProperty* p = find_param(fn, STR("isToggled"));
            const bool on = p && static_cast<FBoolProperty*>(p)->GetPropertyValue(p->ContainerPtrToValuePtr<void>(parms));
            Output::send<LogLevel::Verbose>(STR("[FuserNative] settings: Song Search Bar toggled -> {}\n"), on);
            if (on != g_search_enabled)
            {
                g_search_enabled = on;
                save_settings();
                apply_search_setting();
            }
            return;
        }
        if ((name == g_name_on_highlighted || name == g_name_on_unhighlighted) && context->GetClassPrivate()->GetFName() == g_name_diamond)
        {
            if (g_log_hover++ < kLogLimit)
            {
                auto* hw = context->GetValuePtrByPropertyNameInChain<bool>(STR("IsHighlighted"));
                Output::send<LogLevel::Verbose>(STR("[FuserNative] hover: {} {} (IsHighlighted {})\n"), context->GetName(), name.ToString(), hw && *hw);
            }
            try
            {
                on_item_hover(context);
            }
            catch (const std::exception& e)
            {
                Output::send<LogLevel::Error>(STR("[FuserNative] hover: exception {}\n"), to_wstring(e.what()));
            }
            return;
        }
        if (name == g_name_list_item_initialized && context->GetClassPrivate()->GetFName() == g_name_song_grid_item)
        {
            try
            {
                on_song_grid_item_initialized(context);
            }
            catch (const std::exception& e)
            {
                Output::send<LogLevel::Error>(STR("[FuserNative] tiles: exception {}\n"), to_wstring(e.what()));
            }
            return;
        }
        if (name == g_name_construct && context->GetClassPrivate()->GetFName() == g_name_collection_pane)
        {
            try
            {
                if (owned_by_class(context, g_name_crate_edit))
                {
                    inject_search_box(context);
                    bind_sort_dropdown(context);
                }
            }
            catch (const std::exception& e)
            {
                Output::send<LogLevel::Error>(STR("[FuserNative] search: exception {}\n"), to_wstring(e.what()));
            }
            return;
        }
        if ((name == g_name_on_initialized || name == g_name_preconstruct || name == g_name_construct) &&
            context->GetClassPrivate()->GetFName() == g_name_uberlist)
        {
            on_uberlist_event(context, name);
            return;
        }
        const bool is_song = name == g_name_setup_song;
        if (!is_song && name != g_name_setup_moment) return;
        if (context->GetClassPrivate()->GetFName() != g_name_diamond) return;
        on_setup_call(context, fn, parms, is_song);
    }

    // ---------------------------------------------------------------- F6 self-tests --------------
    auto call_and_log(const CharType* phase, UObject* song) -> void
    {
        auto* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, kLibCdoPath);
        auto* fn_kb = find_fn(kFnKeyBpm);
        auto* fn_kt = find_fn(kFnKeyText);
        if (!cdo || !fn_kb || !fn_kt) return;
        const SongKeyBpm expect = compute_key_bpm(song);

        PCall kb(fn_kb);
        kb.set<UObject*>(STR("SongSet"), song).call(cdo);
        const int32 key = kb.get<int32>(STR("Key")), mode = kb.get<int32>(STR("Mode")), bpm = kb.get<int32>(STR("Bpm"));
        auto* found_p = static_cast<FBoolProperty*>(find_param(fn_kb, STR("Found")));
        const bool found = found_p->GetPropertyValue(kb.addr(STR("Found")));

        PCall kt(fn_kt);
        kt.set<UObject*>(STR("SongSet"), song).call(cdo);
        const StringType text = kt.get_text(STR("KeyText"));

        const bool match = key == expect.key && mode == expect.mode && bpm == expect.bpm && found == expect.found &&
                           text == key_text_for(expect);
        Output::send<LogLevel::Verbose>(STR("[FuserNative] {} '{}': Key={} Mode={} Bpm={} Found={} KeyText='{}' | expected {} {} {} {} '{}' -> {}\n"),
                                        phase, song_name(song), key, mode, bpm, found, text,
                                        expect.key, expect.mode, expect.bpm, expect.found, key_text_for(expect),
                                        match ? STR("MATCH") : STR("differs"));
    }

    auto test_songs() -> std::vector<UObject*>
    {
        std::vector<UObject*> all{}, picked{};
        UObjectGlobals::FindAllOf(STR("CelSongSet"), all);
        for (UObject* s : all) // allstar first: exercises the "mode set but Key=None" beat cel
        {
            if (s && song_name(s) == STR("allstar")) picked.push_back(s);
        }
        for (UObject* s : all)
        {
            if (picked.size() >= 4) break;
            if (s && (picked.empty() || s != picked.front())) picked.push_back(s);
        }
        return picked;
    }

    // A live world to create a test widget in (not the World CDO).
    auto any_world() -> UObject*
    {
        std::vector<UObject*> worlds{};
        UObjectGlobals::FindAllOf(STR("World"), worlds);
        for (UObject* w : worlds)
        {
            if (w && w->GetName().rfind(STR("Default__"), 0) != 0) return w;
        }
        return nullptr;
    }

    auto bind_and_test() -> void
    {
        const bool was_bound = g_bound;
        if (!was_bound)
        {
            if (!load_class(kLibClassPath))
            {
                Output::send<LogLevel::Warning>(STR("[FuserNative] F6: BPL_FuserNative not loaded (see the load: line above)\n"));
                return;
            }
        }
        const auto songs = test_songs();
        Output::send<LogLevel::Verbose>(STR("[FuserNative] F6: {} test songs; bound={}\n"), songs.size(), g_bound);
        if (!was_bound)
        {
            for (UObject* s : songs) call_and_log(STR("BEFORE bind"), s);
        }
        ensure_ready(true);
        for (UObject* s : songs) call_and_log(STR("ProcessEvent"), s);

        // Script-call path: WBP_FuserKeyBpm.SetSong's own bytecode calls the library functions
        if (!g_widget_class || !g_fn_set_song)
        {
            Output::send<LogLevel::Warning>(STR("[FuserNative] F6: WBP_FuserKeyBpm not available; script-call test skipped\n"));
            return;
        }
        UObject* world = any_world();
        UObject* test = world ? create_widget(world, g_widget_class, nullptr) : nullptr;
        Output::send<LogLevel::Verbose>(STR("[FuserNative] F6: test widget {} in {}\n"), test ? test->GetFullName() : StringType{STR("CREATE FAILED")},
                                        world ? world->GetFullName() : StringType{STR("no world")});
        if (!test) return;
        for (UObject* s : songs)
        {
            set_song(test, s, nullptr);
            const SongKeyBpm expect = compute_key_bpm(s);
            Output::send<LogLevel::Verbose>(STR("[FuserNative] script-call '{}': KeyBpmText='{}' (expected key '{}', bpm {})\n"), song_name(s),
                                            text_of(obj_prop(test, STR("KeyBpmText"))), key_text_for(expect), expect.bpm);
        }
    }

    // ---------------------------------------------------------------- stage-1 probe (F7) ---------
    auto probe_songs() -> void
    {
        std::vector<UObject*> songs{};
        UObjectGlobals::FindAllOf(STR("CelSongSet"), songs);
        Output::send<LogLevel::Verbose>(STR("[FuserNative] probe: {} CelSongSet instances\n"), songs.size());
        int shown = 0;
        for (UObject* song : songs)
        {
            if (!song || shown++ >= 8) break;
            const SongKeyBpm r = compute_key_bpm(song);
            Output::send<LogLevel::Verbose>(STR("[FuserNative]  '{}': Key={} Mode={} Bpm={} Found={} '{}'\n"),
                                            song_name(song), r.key, r.mode, r.bpm, r.found, key_text_for(r));
        }
    }
} // namespace

namespace
{
    // F6 / F7 / F8: polled on the game thread, debug builds only.
    auto debug_keys() -> void
    {
#if FNL_DEBUG
        static bool was[3]{};
        const int keys[3] = {VK_F6, VK_F7, VK_F8};
        std::atomic<bool>* flags[3] = {&g_bind_test_requested, &g_probe_requested, &g_crate_edit_dump_requested};
        for (int i = 0; i < 3; ++i)
        {
            const bool down = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
            if (down && !was[i]) *flags[i] = true;
            was[i] = down;
        }
#endif
    }

    auto fusernative_unreal_init() -> void
    {
        load_settings();
        g_name_setup_song = FName(STR("SetupLayoutForSong"), FNAME_Add);
        g_name_setup_moment = FName(STR("SetupLayoutForMoment"), FNAME_Add);
        g_name_diamond = FName(STR("WBP_SongItem_Horizontal_Diamond_C"), FNAME_Add);
        g_name_set_padding = FName(STR("SetPadding"), FNAME_Add);
        g_name_vbox_slot = FName(STR("VerticalBoxSlot"), FNAME_Add);
        g_name_meta_row = FName(STR("SongMetadata_HorizontalBox"), FNAME_Add);
        g_name_uberlist = FName(STR("WBP_UberHudList_C"), FNAME_Add);
        g_name_collection_pane = FName(STR("WBP_CollectionPaneDefault_C"), FNAME_Add);
        g_name_crate_edit = FName(STR("WBP_Screen_CrateEdit_C"), FNAME_Add);
        g_name_song_tile = FName(STR("WBP_Element_Collection_Browse_HypeSongWidget_C"), FNAME_Add);
        g_name_song_grid_item = FName(STR("WBP_GridItem_Collection_Browse_Song_C"), FNAME_Add);
        g_name_list_item_initialized = FName(STR("OnListItemInitialized"), FNAME_Add);
        g_name_settings = FName(STR("WBP_Screen_Settings_C"), FNAME_Add);
        g_name_selection_changed = FName(STR("OnDropdownSelectionChanged_Event_0"), FNAME_Add);
        g_name_on_highlighted = FName(STR("OnHighlighted"), FNAME_Add);
        g_name_on_unhighlighted = FName(STR("OnUnhighlighted"), FNAME_Add);
        g_name_screen_activated = FName(STR("OnScreenActivated"), FNAME_Add);
        g_name_destruct = FName(STR("Destruct"), FNAME_Add);
        g_name_toggle_changed = FName(STR("BndEvt__WBP_Component_ToggleSwitch_Generic_K2Node_ComponentBoundEvent_1_OnToggleStateChanged__DelegateSignature"), FNAME_Add);
        g_name_slider_value = FName(STR("BndEvt__Slider_Contrast_K2Node_ComponentBoundEvent_1_HypeSliderValueChanged__DelegateSignature"), FNAME_Add);
        // our new-crate slot counts into the HypeSettings CDO before CrateMgr's init copies it (the live CrateMgr is
        // also written once it exists: crate_slots_tick)
        if (g_new_crate_songs >= 0 && g_new_crate_moments >= 0)
        {
            g_new_crate_songs = snap_slots(static_cast<float>(g_new_crate_songs), kSongSlotsMin, kSongSlotsMax);
            g_new_crate_moments = snap_slots(static_cast<float>(g_new_crate_moments), kMomentSlotsMin, std::min(kMomentSlotsMax, kSlotCap - g_new_crate_songs));
            apply_new_crate_slots(STR("unreal-init"));
        }
        g_name_get_list_data = FName(STR("Get List Data for Tab Type"), FNAME_Add);
        g_name_on_initialized = FName(STR("OnInitialized"), FNAME_Add);
        g_name_preconstruct = FName(STR("PreConstruct"), FNAME_Add);
        g_name_construct = FName(STR("Construct"), FNAME_Add);
        g_name_gameflow_event = FName(STR("BndEvt__GameFlowEventListenerWidget_127_K2Node_ComponentBoundEvent_0_GameFlowEvent__DelegateSignature"), FNAME_Add);
        Hook::RegisterEngineTickPreCallback(
                [](Hook::TCallbackIterationData<void>&, UEngine*, float, bool) {
                    debug_keys();
                    if (g_bind_test_requested.exchange(false)) bind_and_test();
                    if (g_probe_requested.exchange(false)) probe_songs();
                    if (g_crate_edit_dump_requested.exchange(false))
                    {
                        dump_crate_edit();
                        dump_settings();
                    }
                    if (g_tick == 0)
                    {
                        // BPL_FuserNative + WBP_FuserKeyBpm: a synchronous load, done while the game is still loading
                        // rather than when the first HUD song item appears
                        const double t0 = now_ms();
                        const bool ok = ensure_ready();
                        Output::send<LogLevel::Verbose>(STR("[FuserNative] preload: key/BPM classes {} ({:.1f} ms)\n"), ok ? STR("ready") : STR("NOT ready"), now_ms() - t0);
                    }
                    setup_tick();
                    if (g_tick % 300 == 150) clamp_song_genres(STR("sweep")); // songs loaded since (see clamp_song_genres)
                    crate_slots_tick();
                    search_tick();
                    tab_strip_tick();
                    {
                        static int slow = 0;
                        const double t0 = now_ms();
                        swap_tick();
                        const double ms = now_ms() - t0;
                        if (ms > 4.0 && slow++ < kLogLimit)
                            Output::send<LogLevel::Verbose>(STR("[FuserNative] swap_tick: {:.1f} ms ({} items tracked) at tick {}\n"), ms, g_swaps.size(), g_tick);
                    }
                },
                {false, false, STR("FuserNative"), STR("GameThreadActions")});
        Hook::RegisterProcessEventPostCallback(
                [](Hook::TCallbackIterationData<void>&, UObject* context, UFunction* fn, void* parms) { on_process_event_post(context, fn, parms); },
                {false, false, STR("FuserNative"), STR("SongItemInject")});
        Hook::RegisterProcessEventPreCallback(
                [](Hook::TCallbackIterationData<void>&, UObject* context, UFunction* fn, void* parms) { on_process_event_pre(context, fn, parms); },
                {false, false, STR("FuserNative"), STR("UberListContainers")});
        if (UFunction* set_padding = find_fn(STR("/Script/UMG.VerticalBoxSlot:SetPadding")))
        {
            UObjectGlobals::RegisterHook(set_padding, [](UnrealScriptFunctionCallableContext&, void*) {}, &on_set_padding_post, nullptr);
            Output::send<LogLevel::Verbose>(STR("[FuserNative] on_unreal_init: VerticalBoxSlot::SetPadding post-hook registered\n"));
        }
        else
        {
            Output::send<LogLevel::Warning>(STR("[FuserNative] on_unreal_init: VerticalBoxSlot::SetPadding not found; hover mirroring disabled\n"));
        }
        if (UFunction* key_text = find_fn(STR("/Script/Hype.HypeBPFL:GetHypeKeyText")))
        {
            UObjectGlobals::RegisterHook(key_text, [](UnrealScriptFunctionCallableContext&, void*) {}, &on_key_text_post, nullptr);
            Output::send<LogLevel::Verbose>(STR("[FuserNative] on_unreal_init: HypeBPFL::GetHypeKeyText post-hook registered (Key Labels)\n"));
        }
        for (const CharType* path : {STR("/Script/Hype.HypeEditableTextWidget:_OnTextChanged"), STR("/Script/Hype.CelMgr:Sort"), STR("/Script/Hype.CelMgr:SortSoftPtrs")})
        {
            UFunction* f = find_fn(path);
            if (!f)
            {
                Output::send<LogLevel::Warning>(STR("[FuserNative] on_unreal_init: {} not found; song search disabled\n"), path);
                continue;
            }
            const bool is_text = f->GetFName() == FName(STR("_OnTextChanged"), FNAME_Add);
            UObjectGlobals::RegisterHook(f, [](UnrealScriptFunctionCallableContext&, void*) {}, is_text ? &on_text_changed_post : &on_sort_post, nullptr);
            Output::send<LogLevel::Verbose>(STR("[FuserNative] on_unreal_init: {} post-hook registered\n"), path);
        }
        if (UFunction* set_song_set = find_fn(STR("/Script/Hype.HypeSongWidget:SetCelSongSet")))
        {
            UObjectGlobals::RegisterHook(set_song_set, [](UnrealScriptFunctionCallableContext&, void*) {}, &on_set_cel_song_set_post, nullptr);
            Output::send<LogLevel::Verbose>(STR("[FuserNative] on_unreal_init: HypeSongWidget::SetCelSongSet post-hook registered (song tile key/BPM)\n"));
        }
        else
        {
            Output::send<LogLevel::Warning>(STR("[FuserNative] on_unreal_init: HypeSongWidget::SetCelSongSet not found; song tile key/BPM disabled\n"));
        }
        Output::send<LogLevel::Verbose>(STR("[FuserNative] on_unreal_init: game-thread actions + SetupLayoutForSong post-hook registered\n"));
    }
} // namespace

// Called by the loader once fue is up (fnl_start); everything else runs from fue's first GameEngine::Tick.
auto fusernative_start() -> void
{
#if FNL_DEBUG
    const CharType* keys = STR("; F6 test, F7 probe, F8 dump");
#else
    const CharType* keys = STR("");
#endif
    Output::send<LogLevel::Verbose>(STR("[FuserNative] {} (FUSERNativeLoader): native call points, key/BPM line and setting, real song tabs, "
                                        "song search and Key/BPM sort, song tile key/BPM, crate-edit tabs, Key Labels, new-crate slots{}\n"),
                                    FNL_WIDEN(FNL_VERSION), keys);
    Output::send<LogLevel::Verbose>(STR("[FuserNative] genres: genre 9 (Indie/Alternative) {}\n"),
                                    fnl_genre9_requestable ? STR("is requestable - only genres >= 10 are clamped")
                                                           : STR("is NOT requestable on this build - it is clamped with genres >= 10"));
    fue::on_unreal_init(&fusernative_unreal_init);
}

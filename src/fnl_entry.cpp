// FUSERNativeLoader's entry (fnl_start, called from dllmain.cpp) and, in debug builds, the fue probe: it exercises
// the core read-only (plus ProcessEvent calls) at the first GameEngine::Tick and on F9, writing to FuserNative.log.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "fue/fue.hpp"
#include "loader/mod_manager.hpp"
#include "loader/settings.hpp"
#include "pak_diag.hpp"

using namespace fue;

namespace
{
    // A ProcessEvent call with a parameter buffer built through the parameters' properties (FuserNative's PCall).
    struct Call
    {
        UFunction* fn;
        std::vector<uint8> buf;
        explicit Call(UFunction* f) : fn(f), buf(f ? static_cast<size_t>(f->GetPropertiesSize()) + 16 : 0, 0)
        {
            for_params([&](FProperty* p) { p->InitializeValue_InContainer(buf.data()); });
        }
        ~Call()
        {
            for_params([&](FProperty* p) { p->DestroyValue_InContainer(buf.data()); });
        }
        template <typename F>
        void for_params(F&& f)
        {
            if (!fn) return;
            for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
            {
                if (!p->HasAnyPropertyFlags(CPF_Parm)) break;
                f(p);
            }
        }
        auto addr(const CharType* n) -> uint8*
        {
            FProperty* p = fn ? fn->FindProperty(FName(n, FNAME_Add)) : nullptr;
            return p ? p->ContainerPtrToValuePtr<uint8>(buf.data()) : nullptr;
        }
        auto text(const CharType* n) -> StringType
        {
            FProperty* p = fn ? fn->FindProperty(FName(n, FNAME_Add)) : nullptr;
            if (!p) return {};
            FText t{};
            p->CopyCompleteValue(&t, addr(n));
            return t.ToString();
        }
    };

    auto probe(const CharType* when) -> void
    {
        const auto t0 = GetTickCount64();
        Output::send<LogLevel::Verbose>(STR("[probe] {}: {} objects\n"), when, UObjectGlobals::object_count());

        // 1. object lookup by path, reflection walk, UFunction layout
        auto* fn_key = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Hype.HypeBPFL:GetHypeKeyText"));
        auto* bpfl = UObjectGlobals::StaticFindObject(nullptr, nullptr, STR("/Script/Hype.Default__HypeBPFL"));
        Output::send<LogLevel::Verbose>(STR("[probe] GetHypeKeyText {} (flags 0x{:08X}, Func {}, PropertiesSize {}), HypeBPFL CDO {}\n"),
                                        fn_key ? fn_key->GetFullName() : StringType{STR("NOT FOUND")}, fn_key ? fn_key->GetFunctionFlags() : 0,
                                        fn_key ? reinterpret_cast<void*>(fn_key->GetFuncPtr()) : nullptr, fn_key ? fn_key->GetPropertiesSize() : -1,
                                        bpfl ? bpfl->GetFullName() : StringType{STR("NOT FOUND")});
        if (fn_key)
        {
            StringType params{};
            for (FProperty* p : TFieldRange<FProperty>(fn_key, EFieldIterationFlags::IncludeDeprecated))
            {
                params += std::format(STR("{}{}:{} @{} size {} flags 0x{:X}"), params.empty() ? STR("") : STR("; "), p->GetName(), p->GetClass().GetFName().ToString(),
                                      p->GetOffset_Internal(), p->GetElementSize(), p->GetPropertyFlags());
            }
            Output::send<LogLevel::Verbose>(STR("[probe] params: {}\n"), params);
        }

        // 2. ProcessEvent calls with engine FText results (12 keys)
        if (fn_key && bpfl)
        {
            StringType keys{};
            for (int32 i = 0; i < 12; ++i)
            {
                Call c(fn_key);
                if (uint8* k = c.addr(STR("Key"))) *k = static_cast<uint8>(i);
                bpfl->ProcessEvent(fn_key, c.buf.data());
                keys += (i ? STR(", ") : STR("")) + c.text(STR("ReturnValue"));
            }
            Output::send<LogLevel::Verbose>(STR("[probe] key names: [{}]\n"), keys);
        }

        // 3. FText round trip and FName
        {
            const FText t(STR("Round trip ♯♭"));
            Output::send<LogLevel::Verbose>(STR("[probe] FText round trip: '{}'; FName('SetupLayoutForSong') = {} -> '{}'\n"), t.ToString(),
                                            FName(STR("SetupLayoutForSong")).ComparisonIndex, FName(STR("SetupLayoutForSong")).ToString());
        }

        // 4. CDO property reads (int / bool through the property walk)
        if (auto* settings = UObjectGlobals::StaticFindObject(nullptr, nullptr, STR("/Script/Hype.Default__HypeSettings")))
        {
            auto* songs = settings->GetValuePtrByPropertyNameInChain<int32>(STR("DefaultCrateSongSlotsNum"));
            auto* moments = settings->GetValuePtrByPropertyNameInChain<int32>(STR("DefaultCrateMomentSlotsNum"));
            Output::send<LogLevel::Verbose>(STR("[probe] HypeSettings CDO: DefaultCrateSongSlotsNum {}, DefaultCrateMomentSlotsNum {}\n"), songs ? *songs : -1, moments ? *moments : -1);
        }

        // 5. class scans and weak pointers
        std::vector<UObject*> songs{}, items{};
        UObjectGlobals::FindAllOf(STR("CelSongSet"), songs);
        UObjectGlobals::FindAllOf(STR("WBP_SongItem_Horizontal_Diamond_C"), items);
        const FWeakObjectPtr weak(songs.empty() ? nullptr : songs.front());
        StringType first{};
        if (!songs.empty())
        {
            UObject* s = songs.size() > 1 ? songs[1] : songs.front();
            auto* name = s->GetValuePtrByPropertyNameInChain<FName>(STR("songShortName"));
            auto* cels = s->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("Cels"));
            first = std::format(STR("{} (songShortName '{}', {} cels)"), s->GetName(), name ? name->ToString() : StringType{STR("?")}, cels ? cels->Num() : -1);
        }
        Output::send<LogLevel::Verbose>(STR("[probe] CelSongSet x{} (e.g. {}), Diamond song items x{}, weak ptr {} ({} ms total)\n"), songs.size(), first, items.size(),
                                        weak.Get() == (songs.empty() ? nullptr : songs.front()) ? STR("ok") : STR("MISMATCH"), GetTickCount64() - t0);
    }
} // namespace

auto unlocks_start() -> void;     // unlocks.cpp
auto custom_content_start() -> void; // custom_content.cpp
auto unlock_probe_start() -> void; // unlock_probe.cpp (debug builds)

// A line in FuserNative.log, for dllmain.cpp (C++17, without fue). level: FNL_LOG_*.
void fnl_log(int level, const wchar_t* text)
{
    Output::write(static_cast<LogLevel>(level), text);
}

// The loader's entry (dllmain.cpp, after FUSER_HOOK): the fue core, the mods' manifests and early stage, the Mods tab,
// the unlocks and custom content. The game features are mods, loaded at the first engine callback (fue); the probe
// (F9) is debug-only.
bool fnl_start(void* self_module)
{
    const bool fue_ok = fue::init(self_module);
    fnl_mods_early(fue_ok); // before any pak is mounted, whether or not fue is up: early mods are C tier
    if (!fue_ok) return false;
    pak_diag_start();
    settings_start(); // the Settings screen's Mods tab, for the mods' rows
    unlocks_start(); // first: its unreal-init step must precede UnlockMgr's init
    custom_content_start(); // its rows, too, must precede UnlockMgr's and UVenueMgr's table reads
#if FNL_DEBUG
    unlock_probe_start();
#endif
#if FNL_DEBUG
    Hook::RegisterEngineTickPreCallback([](Hook::TCallbackIterationData<void>&, UEngine*, float, bool) {
        static bool first = true; // the probe wants a loaded menu: its first run is the first tick, not unreal-init
        if (first)
        {
            first = false;
            probe(STR("first tick"));
        }
        static bool was_down = false;
        const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !was_down) probe(STR("F9"));
        was_down = down;
    });
#endif
    return true;
}

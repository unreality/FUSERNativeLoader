// fue - implementation. See fue.hpp.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "fue.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <array>
#include <mutex>
#include <share.h>
#include <utility>
#include <unordered_map>

#include <funchook.h>

#include "hook_range.hpp"

auto fnl_load_optional_multiplayer() -> void;

namespace fue
{
    namespace
    {
        // ---------------------------------------------------------------- addresses, per game build --
        // Every FUSER build runs engine 4.24.3 hype_release, so the member offsets and vtable slots below are shared;
        // only these 8 addresses differ. EGS v1.4.2's come from UE4SS's own scan of FuserEOS-Win64-Shipping.exe.
        // Other builds are derived statically by tools/derive_build.py, which prints the entry to add here. A build is selected by its PE identity. verify holds the first bytes of each function we detour
        // or call (?? = a RIP-relative / rel32 field) and is checked before anything is patched; the globals are
        // sanity-checked at the first ProcessEvent (self_check). Either failing disables fue (and FuserNative),
        // leaving the game and the song loader untouched.
        struct GameBuild
        {
            const CharType* name;
            uint32 stamp, size; // PE TimeDateStamp / SizeOfImage
            uintptr_t process_event, static_construct, fname_to_string, fname_ctor, engine_tick, object_array, gmalloc, natives;
            const char* verify[5]; // process_event, static_construct, fname_to_string, fname_ctor, engine_tick
        };
        constexpr GameBuild kBuilds[] = {
            {
                STR("EGS v1.4.2"), 0x6143e115, 0x04138000,
                /* ProcessEvent */ 0xb78890, /* StaticConstructObject */ 0xb7ee90, /* FName::ToString */ 0x9f6a30, /* FName ctor */ 0x9db500,
                /* GameEngine::Tick */ 0x1a76e80, /* GUObjectArray */ 0x3be0028, /* GMalloc */ 0x3bc2d38, /* GNatives */ 0x3bdf430,
                {"40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC F0", "4C 89 44 24 18 55 53 56 57 41 54 41 55 41 56 41", "48 89 5C 24 18 55 56 57 48 8B EC 48 83 EC 30 8B", "48 89 5C 24 08 57 48 83 EC 30 48 8B D9 48 89 54", "48 8B C4 44 88 40 18 48 89 48 08 55 57 41 55 48"},
            },
            // EGS, later build (FuserEOS-Win64-Shipping.exe, engine CL 1317808 - the Jan 2023 update, a sibling of Steam's
            // 1317755; same SizeOfImage as v1.4.2). Derived statically against v1.4.2: all 8 addresses CONFIRMED, vtable
            // slots matching, and the engine functions fue / unlocks / FuserNative rely on instruction-identical to
            // v1.4.2 (code moved +0x30, GUObjectArray / GNatives -0x10). Not yet run on an install.
            {
                STR("EGS CL1317808"), 0x63c882a2, 0x04138000,
                /* ProcessEvent */ 0xb788c0, /* StaticConstructObject */ 0xb7eec0, /* FName::ToString */ 0x9f6a60, /* FName ctor */ 0x9db530,
                /* GameEngine::Tick */ 0x1a76eb0, /* GUObjectArray */ 0x3be0018, /* GMalloc */ 0x3bc2d38, /* GNatives */ 0x3bdf420,
                {"40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC F0", "4C 89 44 24 18 55 53 56 57 41 54 41 55 41 56 41", "48 89 5C 24 18 55 56 57 48 8B EC 48 83 EC 30 8B", "48 89 5C 24 08 57 48 83 EC 30 48 8B D9 48 89 54", "48 8B C4 44 88 40 18 48 89 48 08 55 57 41 55 48"},
            },
            // Steam (Fuser-Win64-Shipping.exe, engine CL 1317755 - a later build than EGS's 1310155). SteamStub-wrapped:
            // derived from a Steamless-unpacked copy (all 8 addresses CONFIRMED, vtable slots matching), but the identity
            // is the shipped, packed exe's - the one that runs (SteamStub decrypts .text in place, RVAs unchanged). The
            // engine functions whose layout fue / unlocks / FuserNative rely on (ProcessEvent, ProcessInternal,
            // StaticConstructObject, FName, UDataTable Add/Remove/Empty, CrateMgr's slot getters and CreateCustomCrate,
            // UnlockMgr's loader) are instruction-identical to EGS. Not yet run on a Steam install.
            {
                STR("Steam CL1317755"), 0x63bcb6a2, 0x041ae000,
                /* ProcessEvent */ 0xbadb50, /* StaticConstructObject */ 0xbb4150, /* FName::ToString */ 0xa2bf40, /* FName ctor */ 0xa10a10,
                /* GameEngine::Tick */ 0x1aab890, /* GUObjectArray */ 0x3c1f498, /* GMalloc */ 0x3c021b8, /* GNatives */ 0x3c1e8a0,
                {"40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC F0", "4C 89 44 24 18 55 53 56 57 41 54 41 55 41 56 41", "48 89 5C 24 18 55 56 57 48 8B EC 48 83 EC 30 8B", "48 89 5C 24 08 57 48 83 EC 30 48 8B D9 48 89 54", "48 8B C4 44 88 40 18 48 89 48 08 55 57 41 55 48"},
            },
        };

        // 4.24 FUObjectArray / FChunkedFixedUObjectArray / FUObjectItem (UE4SS's layouts)
        constexpr size_t kObjObjects = 0x10, kMasterSerialNumber = 0x128;
        constexpr size_t kChunkObjects = 0x0, kChunkMaxElements = 0x10, kChunkNumElements = 0x14;
        constexpr int32 kElementsPerChunk = 64 * 1024;
        constexpr size_t kItemSize = 0x18, kItemObject = 0x0, kItemFlags = 0x8, kItemSerial = 0x10;
        constexpr int32 kRootSet = 1 << 30, kPendingKill = 1 << 29, kUnreachable = 1 << 28; // EInternalObjectFlags

        // UProperty virtual slots (as UE4SS computed them for FUSER)
        constexpr size_t kVCopyValuesInternal = 0x2C0, kVDestroyValueInternal = 0x2F8, kVInitializeValueInternal = 0x300;
        constexpr size_t kVProcessEvent = 0x210;
        constexpr size_t kMallocMalloc = 0x10, kMallocRealloc = 0x18, kMallocFree = 0x20; // FMalloc vtable

        using FNameCtorFn = FName* (*)(FName*, const TCHAR*, EFindName);
        using FNameToStringFn = void (*)(const FName*, FString&);
        using StaticConstructObjectFn = UObject* (*)(const UClass*, UObject*, FName, uint32, uint32, UObject*, bool, void*, bool);
        using ProcessEventFn = void (*)(UObject*, UFunction*, void*);
        using EngineTickFn = void (*)(UEngine*, float, bool);
        using NativeFn = void (*)(UObject*, FFrame&, void*);

        uint8* g_base = nullptr;
        const GameBuild* g_build = nullptr;
        bool g_ready = false;
        HMODULE g_self = nullptr;
        FNameCtorFn g_fname_ctor = nullptr;
        FNameToStringFn g_fname_to_string = nullptr;
        StaticConstructObjectFn g_static_construct = nullptr;
        ProcessEventFn g_process_event_original = nullptr;
        EngineTickFn g_engine_tick_original = nullptr;
        uint8* g_object_array = nullptr;
        void*** g_gmalloc = nullptr; // FMalloc** (GMalloc)
        NativeFn* g_natives = nullptr;

        std::vector<std::function<void()>> g_unreal_init{};
        bool g_unreal_init_done = false;
        std::vector<Hook::EngineTickCallback> g_tick_pre{};
        std::vector<Hook::ProcessEventCallback> g_pe_pre{}, g_pe_post{};

        std::mutex g_log_mutex{};
        FILE* g_log = nullptr;

        template <typename T>
        auto vcall(const void* obj, size_t offset) -> T
        {
            return reinterpret_cast<T>((*reinterpret_cast<void* const* const*>(obj))[offset / sizeof(void*)]);
        }

        // ---------------------------------------------------------------- object array ------------
        auto num_objects() -> int32 { return detail::at<int32>(g_object_array, kObjObjects + kChunkNumElements); }

        auto item_at(int32 index) -> uint8*
        {
            if (index < 0 || index >= num_objects()) return nullptr;
            uint8** chunks = detail::at<uint8**>(g_object_array, kObjObjects + kChunkObjects);
            uint8* chunk = chunks ? chunks[index / kElementsPerChunk] : nullptr;
            return chunk ? chunk + static_cast<size_t>(index % kElementsPerChunk) * kItemSize : nullptr;
        }

        auto object_at(int32 index) -> UObject*
        {
            uint8* item = item_at(index);
            return item ? detail::at<UObject*>(item, kItemObject) : nullptr;
        }

        template <typename F>
        auto for_each_object(F&& f) -> void
        {
            for (int32 i = 0, n = num_objects(); i < n; ++i)
            {
                uint8* item = item_at(i);
                UObject* o = item ? detail::at<UObject*>(item, kItemObject) : nullptr;
                if (!o || (detail::at<int32>(item, kItemFlags) & (kPendingKill | kUnreachable))) continue;
                if (!f(o)) return;
            }
        }

        // ---------------------------------------------------------------- detours -----------------
        // ---------------------------------------------------------------- build checks ------------
        // pattern: "48 8B ?? ..." as tools/derive_build.py prints it
        auto matches_pattern(const uint8* p, const char* pattern) -> bool
        {
            for (const char* c = pattern; *c;)
            {
                if (*c == ' ') { ++c; continue; }
                if (c[0] == '?' && c[1] == '?') { ++p; c += 2; continue; }
                const char hex[3] = {c[0], c[1], 0};
                if (!c[1] || *p != static_cast<uint8>(strtoul(hex, nullptr, 16))) return false;
                ++p;
                c += 2;
            }
            return true;
        }

        auto in_image(const void* p) -> bool
        {
            const auto* a = static_cast<const uint8*>(p);
            return a >= g_base && a < g_base + g_build->size;
        }

        // The globals and the vtable slot a wrong build entry would get wrong, checked once the object system is up
        // (the first ProcessEvent). Only reads; any failure disables fue before FuserNative runs.
        auto self_check() -> bool
        {
            bool ok = true;
            auto fail = [&](const CharType* what) {
                Output::send<LogLevel::Error>(STR("[fue] self-check failed ({}): {}\n"), g_build->name, what);
                ok = false;
            };
            // GUObjectArray: a plausible element count, and object 0 (a UPackage) whose class chain ends in the
            // metaclass UClass, which is its own class
            const int32 n = num_objects(), max = detail::at<int32>(g_object_array, kObjObjects + kChunkMaxElements);
            UObject* o0 = n > 0 && n <= max && max <= (1 << 24) ? object_at(0) : nullptr;
            UClass* meta = o0 && o0->GetClassPrivate() ? o0->GetClassPrivate()->GetClassPrivate() : nullptr;
            if (!meta || meta->GetClassPrivate() != meta)
            {
                fail(STR("GUObjectArray"));
                return false;
            }
            // UObject::ProcessEvent at vtable 0x210 (a UPackage does not override it)
            if (vcall<uint8*>(o0, kVProcessEvent) != g_base + g_build->process_event) fail(STR("ProcessEvent vtable slot"));
            // FName: entry 0 prints as "None", and a real name (object 0's class) round-trips through ToString and a
            // find-only construction (a failed find would give None, so "None" alone proves nothing)
            const FName cls_name = o0->GetClassPrivate()->GetFName();
            const StringType cls_text = cls_name.ToString();
            if (FName{}.ToString() != STR("None") || cls_name.IsNone() || cls_text.empty() || FName(cls_text.c_str(), FNAME_Find) != cls_name)
                fail(STR("FName ctor / ToString"));
            // GMalloc: an allocator object whose vtable is in the exe
            if (!*g_gmalloc || !in_image(**g_gmalloc)) fail(STR("GMalloc"));
            // GNatives: the script VM's opcode table, filled by static init - every set entry is code in the exe
            int natives = 0;
            for (int i = 0; i < 0x100; ++i)
                if (g_natives[i]) natives += in_image(reinterpret_cast<void*>(g_natives[i])) ? 1 : -1000;
            if (natives < 64) fail(STR("GNatives"));
            if (ok)
                Output::send<LogLevel::Verbose>(STR("[fue] self-check passed ({}): {} objects, object 0 {} ({}), {} script opcodes\n"), g_build->name, n, o0->GetName(), cls_text, natives);
            return ok;
        }

        auto run_unreal_init(const CharType* when) -> void
        {
            if (g_unreal_init_done) return;
            g_unreal_init_done = true;
            if (!self_check())
            {
                g_ready = false; // fue's API goes inert; the detours only pass through
                Output::send<LogLevel::Error>(STR("[fue] {}: the build entry does not fit this exe - FuserNative features are disabled\n"), when);
                return;
            }
            Output::send<LogLevel::Verbose>(STR("[fue] {}: {} objects, thread {}, running {} init callbacks\n"), when, num_objects(), GetCurrentThreadId(), g_unreal_init.size());
            for (auto& cb : g_unreal_init) cb();
        }

        auto process_event_detour(UObject* self, UFunction* fn, void* parms) -> void
        {
            fnl_load_optional_multiplayer();
            // UE4SS's on_unreal_init equivalent: the first ProcessEvent (the object system is up, and it comes before
            // the shell creates its screens - at the first GameEngine::Tick, WBP_Screen_Settings / _CrateEdit had
            // already been initialized)
            if (!g_unreal_init_done) run_unreal_init(STR("first ProcessEvent"));
            Hook::TCallbackIterationData<void> data{};
            for (auto& cb : g_pe_pre) cb(data, self, fn, parms);
            g_process_event_original(self, fn, parms);
            for (auto& cb : g_pe_post) cb(data, self, fn, parms);
        }

        auto engine_tick_detour(UEngine* self, float delta, bool idle) -> void
        {
            fnl_load_optional_multiplayer();
            if (!g_unreal_init_done) run_unreal_init(STR("first GameEngine::Tick"));
            Hook::TCallbackIterationData<void> data{};
            for (auto& cb : g_tick_pre) cb(data, self, delta, idle);
            g_engine_tick_original(self, delta, idle);
        }

        auto install_hook(void** target, void* hook, const CharType* what) -> bool
        {
            funchook_t* h = funchook_create();
            if (!h) return false;
            void* const function = *target;
            const int prep = funchook_prepare(h, target, hook);
            if (prep == 0 && !hook_in_range(function, *target))
            {
                Output::send<LogLevel::Error>(STR("[fue] hook {}: funchook's trampoline {} is out of jump range of {} - not installed\n"), what, *target, function);
                funchook_destroy(h);
                *target = function;
                return false;
            }
            const int inst = prep == 0 ? funchook_install(h, 0) : -1;
            Output::send<LogLevel::Verbose>(STR("[fue] hook {}: prepare {} install {}\n"), what, prep, inst);
            return prep == 0 && inst == 0;
        }

        // ---------------------------------------------------------------- native UFunction hooks -
        // Each hooked native gets its own thunk (one template instance per slot), so the thunk always knows which
        // function it stands for. Not Stack.CurrentNativeFunction: UFunction::Invoke sets it, but 4.24's
        // execCallMathFunction (how Blueprint calls static natives such as CelMgr::Sort or HypeBPFL::GetHypeKeyText)
        // calls Func directly with that guard commented out - a lookup through it failed, the thunk returned without
        // consuming the call's parameters, and the script VM ran them as statements (a stall in crate edit).
        struct NativeHook
        {
            UFunction* fn = nullptr;
            UnrealScriptFunction original = nullptr;
            std::vector<std::pair<UnrealScriptFunctionCallable, void*>> pre{}, post{};
        };
        constexpr int kMaxNativeHooks = 64;
        NativeHook g_native_hooks[kMaxNativeHooks]{};
        int g_native_hook_count = 0;

        template <int Slot>
        auto native_hook_thunk(UObject* context, FFrame& stack, void* result) -> void
        {
            NativeHook& h = g_native_hooks[Slot];
            UnrealScriptFunctionCallableContext ctx{context, stack, result};
            for (auto& [cb, data] : h.pre) cb(ctx, data);
            h.original(context, stack, result);
            for (auto& [cb, data] : h.post) cb(ctx, data);
        }

        template <int... Slots>
        constexpr auto make_thunks(std::integer_sequence<int, Slots...>) -> std::array<UnrealScriptFunction, sizeof...(Slots)>
        {
            return {&native_hook_thunk<Slots>...};
        }
        constexpr std::array<UnrealScriptFunction, kMaxNativeHooks> g_native_thunks = make_thunks(std::make_integer_sequence<int, kMaxNativeHooks>{});

        // ---------------------------------------------------------------- text helpers ------------
        struct Kismet
        {
            UFunction* to_text = nullptr;   // KismetTextLibrary::Conv_StringToText(InString) -> Text
            UFunction* to_string = nullptr; // KismetTextLibrary::Conv_TextToString(InText) -> String
            UObject* cdo = nullptr;
            FProperty* text_prop = nullptr; // Conv_TextToString's InText: the TextProperty we manage FTexts with
            bool tried = false;
        } g_kismet{};

        auto kismet() -> Kismet&
        {
            if (!g_kismet.tried && g_ready)
            {
                g_kismet.tried = true;
                g_kismet.to_text = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.KismetTextLibrary:Conv_StringToText"));
                g_kismet.to_string = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.KismetTextLibrary:Conv_TextToString"));
                g_kismet.cdo = UObjectGlobals::StaticFindObject(nullptr, nullptr, STR("/Script/Engine.Default__KismetTextLibrary"));
                g_kismet.text_prop = g_kismet.to_string ? g_kismet.to_string->FindProperty(FName(STR("InText"), FNAME_Add)) : nullptr;
            }
            return g_kismet;
        }

        // A ProcessEvent parameter buffer, constructed and destroyed through the parameters' own properties.
        class Call
        {
          public:
            explicit Call(UFunction* fn) : m_fn(fn), m_buf(fn ? static_cast<size_t>(fn->GetPropertiesSize()) + 16 : 0, 0)
            {
                for (FProperty* p : params()) p->InitializeValue_InContainer(m_buf.data());
            }
            ~Call()
            {
                for (FProperty* p : params()) p->DestroyValue_InContainer(m_buf.data());
            }
            auto addr(const CharType* name) -> void*
            {
                FProperty* p = m_fn ? m_fn->FindProperty(FName(name, FNAME_Add)) : nullptr;
                return p ? p->ContainerPtrToValuePtr(m_buf.data()) : nullptr;
            }
            auto prop(const CharType* name) -> FProperty* { return m_fn ? m_fn->FindProperty(FName(name, FNAME_Add)) : nullptr; }
            auto call(UObject* target) -> bool
            {
                if (!m_fn || !target) return false;
                target->ProcessEvent(m_fn, m_buf.data());
                return true;
            }

          private:
            auto params() const -> std::vector<FProperty*>
            {
                std::vector<FProperty*> out{};
                if (!m_fn) return out;
                for (FProperty* p : TFieldRange<FProperty>(m_fn, EFieldIterationFlags::IncludeDeprecated))
                {
                    if (!p->HasAnyPropertyFlags(CPF_Parm)) break;
                    out.push_back(p);
                }
                return out;
            }
            UFunction* m_fn;
            std::vector<uint8> m_buf;
        };

        // ---------------------------------------------------------------- object lookup -----------
        struct PathParts
        {
            std::vector<FName> names{}; // innermost last; [0] is the package
            bool ok = false;
        };

        auto split_path(const CharType* path) -> PathParts
        {
            PathParts out{};
            StringType s(path ? path : L"");
            const size_t dot = s.find(L'.');
            std::vector<StringType> parts{};
            parts.push_back(s.substr(0, dot));
            if (dot != StringType::npos)
            {
                StringType cur{};
                for (size_t i = dot + 1; i < s.size(); ++i)
                {
                    if (s[i] == L'.' || s[i] == L':')
                    {
                        parts.push_back(cur);
                        cur.clear();
                    }
                    else
                    {
                        cur += s[i];
                    }
                }
                parts.push_back(cur);
            }
            for (const auto& p : parts)
            {
                FName n(p.c_str(), FNAME_Find);
                if (n.IsNone()) return out; // a name the engine has never seen: no such object
                out.names.push_back(n);
            }
            out.ok = !out.names.empty();
            return out;
        }

        auto matches_path(const UObject* o, const PathParts& p) -> bool
        {
            const UObject* cur = o;
            for (size_t i = p.names.size(); i-- > 0;)
            {
                if (!cur || cur->GetFName() != p.names[i]) return false;
                cur = cur->GetOuterPrivate();
            }
            return cur == nullptr; // the package has no outer
        }

        std::unordered_map<StringType, FWeakObjectPtr> g_find_cache{};
    } // namespace

    // -------------------------------------------------------------------- lifecycle ---------------
    auto to_wstring(const char* s) -> StringType
    {
        if (!s) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        StringType out(n > 0 ? static_cast<size_t>(n - 1) : 0, L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
        return out;
    }

    auto module_dir() -> std::wstring
    {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(g_self, buf, MAX_PATH);
        return std::filesystem::path(buf).parent_path().wstring();
    }

    auto exe_dir() -> std::wstring
    {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return std::filesystem::path(buf).parent_path().wstring();
    }

    auto Output::write(LogLevel level, const StringType& text) -> void
    {
        std::lock_guard lock(g_log_mutex);
        if (!g_log)
        {
            // One log per running instance: the first takes FuserNative.log, a second game started from the same
            // install (e.g. a local multiplayer test) FuserNative.2.log, and so on - instead of truncating the first's.
            // The named mutex is held for the process lifetime.
            int instance = 1;
            for (; instance < 8; ++instance)
            {
                HANDLE m = CreateMutexW(nullptr, FALSE, std::format(L"Local\\FuserNative.log.{}", instance).c_str());
                if (m && GetLastError() != ERROR_ALREADY_EXISTS) break;
                if (m) CloseHandle(m);
            }
            const auto path = std::filesystem::path(module_dir()) /
                              (instance == 1 ? std::wstring(L"FuserNative.log") : std::format(L"FuserNative.{}.log", instance));
            g_log = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYNO); // readable while the game runs
        }
        if (!g_log) return;
        static const wchar_t* const tags[] = {L"", L"", L"WARNING ", L"ERROR "};
        SYSTEMTIME t{};
        GetLocalTime(&t);
        fwprintf(g_log, L"[%02u:%02u:%02u.%03u] %s%s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, tags[static_cast<int>(level)], text.c_str());
        if (text.empty() || text.back() != L'\n') fputwc(L'\n', g_log);
        fflush(g_log);
    }

    auto init(void* self_module) -> bool
    {
        g_self = static_cast<HMODULE>(self_module);
        g_base = reinterpret_cast<uint8*>(GetModuleHandleW(nullptr));
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
        const uint32 stamp = nt->FileHeader.TimeDateStamp, size = nt->OptionalHeader.SizeOfImage;
        for (const auto& b : kBuilds)
            if (b.stamp == stamp && b.size == size) g_build = &b;
        if (!g_build)
        {
            StringType known{};
            for (const auto& b : kBuilds) known += std::format(STR("{}{} (0x{:08X} / 0x{:08X})"), known.empty() ? STR("") : STR(", "), b.name, b.stamp, b.size);
            Output::send<LogLevel::Warning>(STR("[fue] unsupported game executable (TimeDateStamp 0x{:08X}, SizeOfImage 0x{:08X}; supported: {}): "
                                                "FuserNative features are disabled\n"),
                                            stamp, size, known);
            return false;
        }
        const std::pair<uintptr_t, const CharType*> fns[5] = {{g_build->process_event, STR("UObject::ProcessEvent")},
                                                              {g_build->static_construct, STR("StaticConstructObject_Internal")},
                                                              {g_build->fname_to_string, STR("FName::ToString")},
                                                              {g_build->fname_ctor, STR("FName::FName")},
                                                              {g_build->engine_tick, STR("UGameEngine::Tick")}};
        for (int i = 0; i < 5; ++i)
        {
            if (fns[i].first + 64 < size && matches_pattern(g_base + fns[i].first, g_build->verify[i])) continue;
            Output::send<LogLevel::Error>(STR("[fue] {}: {} at RVA 0x{:X} does not match its expected bytes - FuserNative features are disabled\n"),
                                          g_build->name, fns[i].second, fns[i].first);
            return false;
        }
        g_fname_ctor = reinterpret_cast<FNameCtorFn>(g_base + g_build->fname_ctor);
        g_fname_to_string = reinterpret_cast<FNameToStringFn>(g_base + g_build->fname_to_string);
        g_static_construct = reinterpret_cast<StaticConstructObjectFn>(g_base + g_build->static_construct);
        g_object_array = g_base + g_build->object_array;
        g_gmalloc = reinterpret_cast<void***>(g_base + g_build->gmalloc);
        g_natives = reinterpret_cast<NativeFn*>(g_base + g_build->natives);
        g_process_event_original = reinterpret_cast<ProcessEventFn>(g_base + g_build->process_event);
        g_engine_tick_original = reinterpret_cast<EngineTickFn>(g_base + g_build->engine_tick);
        const bool pe = install_hook(reinterpret_cast<void**>(&g_process_event_original), reinterpret_cast<void*>(&process_event_detour), STR("UObject::ProcessEvent"));
        const bool tick = install_hook(reinterpret_cast<void**>(&g_engine_tick_original), reinterpret_cast<void*>(&engine_tick_detour), STR("UGameEngine::Tick"));
        g_ready = pe && tick;
        Output::send<LogLevel::Verbose>(STR("[fue] init: base {}, {} exe verified, detours {} (loader thread {})\n"), static_cast<void*>(g_base), g_build->name, g_ready ? STR("installed") : STR("FAILED"), GetCurrentThreadId());
        return g_ready;
    }

    auto ready() -> bool { return g_ready; }
    auto build_name() -> const CharType* { return g_build ? g_build->name : STR("unsupported"); }

    auto on_unreal_init(std::function<void()> callback) -> void { g_unreal_init.push_back(std::move(callback)); }

    // -------------------------------------------------------------------- memory ------------------
    auto FMemory::Malloc(size_t count, uint32 alignment) -> void*
    {
        void* m = *g_gmalloc;
        return vcall<void* (*)(void*, size_t, uint32)>(m, kMallocMalloc)(m, count, alignment);
    }
    auto FMemory::Realloc(void* original, size_t count, uint32 alignment) -> void*
    {
        void* m = *g_gmalloc;
        return vcall<void* (*)(void*, void*, size_t, uint32)>(m, kMallocRealloc)(m, original, count, alignment);
    }
    auto FMemory::Free(void* original) -> void
    {
        if (!original) return;
        void* m = *g_gmalloc;
        vcall<void (*)(void*, void*)>(m, kMallocFree)(m, original);
    }

    FString::FString(const TCHAR* s)
    {
        const size_t n = s ? std::wcslen(s) : 0;
        if (!n) return;
        Chars.Data = static_cast<TCHAR*>(FMemory::Malloc((n + 1) * sizeof(TCHAR)));
        std::memcpy(Chars.Data, s, (n + 1) * sizeof(TCHAR));
        Chars.ArrayNum = Chars.ArrayMax = static_cast<int32>(n + 1);
    }
    FString::~FString() { FMemory::Free(Chars.Data); }

    // -------------------------------------------------------------------- names -------------------
    FName::FName(const CharType* name, EFindName find)
    {
        if (g_fname_ctor && name) g_fname_ctor(this, name, find);
    }

    auto FName::ToString() const -> StringType
    {
        if (!g_fname_to_string) return {};
        FString out{};
        g_fname_to_string(this, out);
        return StringType(*out);
    }

    // -------------------------------------------------------------------- objects -----------------
    auto UObject::GetPathName() const -> StringType
    {
        std::vector<const UObject*> chain{};
        for (const UObject* o = this; o; o = o->GetOuterPrivate()) chain.push_back(o);
        StringType out{};
        for (size_t i = chain.size(); i-- > 0;)
        {
            if (i + 1 < chain.size()) out += (i + 2 == chain.size() || !chain[i + 1]->GetOuterPrivate() ? L"." : L":");
            out += chain[i]->GetName();
        }
        return out;
    }

    auto UObject::GetFullName() const -> StringType
    {
        const UClass* c = GetClassPrivate();
        return (c ? c->GetName() : StringType{L"?"}) + L" " + GetPathName();
    }

    auto UObject::IsA(const UClass* cls) const -> bool
    {
        for (const UStruct* s = GetClassPrivate(); s; s = s->GetSuperStruct())
        {
            if (s == cls) return true;
        }
        return false;
    }

    auto UObject::IsRootSet() const -> bool
    {
        uint8* item = item_at(GetInternalIndex());
        return item && (detail::at<int32>(item, kItemFlags) & kRootSet);
    }

    auto UObject::SetRootSet() -> void
    {
        if (uint8* item = item_at(GetInternalIndex())) InterlockedOr(reinterpret_cast<volatile LONG*>(item + kItemFlags), kRootSet);
    }

    auto UObject::ProcessEvent(UFunction* fn, void* parms) -> void { vcall<ProcessEventFn>(this, kVProcessEvent)(this, fn, parms); }

    auto UStruct::FindProperty(FName name) const -> FProperty*
    {
        for (FProperty* p : TFieldRange<FProperty>(this, EFieldIterationFlags::IncludeSuper | EFieldIterationFlags::IncludeDeprecated))
        {
            if (p->GetFName() == name) return p;
        }
        return nullptr;
    }

    auto UObject::GetPropertyByNameInChain(const CharType* name) const -> FProperty*
    {
        const UClass* c = GetClassPrivate();
        return c ? c->FindProperty(FName(name, FNAME_Add)) : nullptr;
    }

    auto UObject::GetValuePtrByPropertyNameInChain(const CharType* name) -> void*
    {
        FProperty* p = GetPropertyByNameInChain(name);
        return p ? p->ContainerPtrToValuePtr(this) : nullptr;
    }

    auto UObject::GetFunctionByNameInChain(FName name) const -> UFunction*
    {
        static const FName function_class(STR("Function"), FNAME_Add);
        for (const UStruct* s = GetClassPrivate(); s; s = s->GetSuperStruct())
        {
            for (UField* f = s->GetChildren(); f; f = f->GetNext())
            {
                if (f->GetFName() == name && class_is(f, STR("Function"))) return static_cast<UFunction*>(f);
            }
        }
        (void)function_class;
        return nullptr;
    }

    auto class_is(const UObject* obj, const CharType* class_name) -> bool
    {
        const UClass* c = obj ? obj->GetClassPrivate() : nullptr;
        return c && c->GetName() == class_name;
    }

    auto is_property(const UObject* field) -> bool
    {
        static const FName property(STR("Property"), FNAME_Add);
        for (const UStruct* s = field ? field->GetClassPrivate() : nullptr; s; s = s->GetSuperStruct())
        {
            if (s->GetFName() == property) return true;
        }
        return false;
    }

    // -------------------------------------------------------------------- property value ops ------
    auto FProperty::InitializeValue(void* dest) const -> void
    {
        if (HasAnyPropertyFlags(CPF_ZeroConstructor)) std::memset(dest, 0, static_cast<size_t>(GetSize()));
        else vcall<void (*)(const FProperty*, void*)>(this, kVInitializeValueInternal)(this, dest);
    }

    auto FProperty::DestroyValue(void* dest) const -> void
    {
        if (!HasAnyPropertyFlags(CPF_NoDestructor)) vcall<void (*)(const FProperty*, void*)>(this, kVDestroyValueInternal)(this, dest);
    }

    auto FProperty::CopyCompleteValue(void* dest, const void* src) const -> void
    {
        if (dest == src) return;
        if (HasAnyPropertyFlags(CPF_IsPlainOldData)) std::memcpy(dest, src, static_cast<size_t>(GetSize()));
        else vcall<void (*)(const FProperty*, void*, const void*, int32)>(this, kVCopyValuesInternal)(this, dest, src, GetArrayDim());
    }

    // -------------------------------------------------------------------- text --------------------
    FText::FText()
    {
        if (FProperty* p = kismet().text_prop) p->InitializeValue(m_storage);
    }

    FText::FText(const CharType* s) : FText()
    {
        Kismet& k = kismet();
        if (!k.to_text || !k.cdo || !k.text_prop) return;
        Call c(k.to_text);
        const FString in(s);
        if (FProperty* p = c.prop(STR("InString"))) p->CopyCompleteValue(c.addr(STR("InString")), &in);
        if (c.call(k.cdo)) k.text_prop->CopyCompleteValue(m_storage, c.addr(STR("ReturnValue")));
    }

    FText::~FText()
    {
        if (FProperty* p = kismet().text_prop) p->DestroyValue(m_storage);
    }

    auto FText::ToString() const -> StringType
    {
        Kismet& k = kismet();
        if (!k.to_string || !k.cdo || !k.text_prop) return {};
        Call c(k.to_string);
        k.text_prop->CopyCompleteValue(c.addr(STR("InText")), m_storage);
        if (!c.call(k.cdo)) return {};
        auto* s = static_cast<FString*>(c.addr(STR("ReturnValue")));
        return s ? StringType(**s) : StringType{};
    }

    // -------------------------------------------------------------------- weak pointers -----------
    FWeakObjectPtr::FWeakObjectPtr(const UObject* o)
    {
        if (!o) return;
        ObjectIndex = o->GetInternalIndex();
        uint8* item = item_at(ObjectIndex);
        if (!item)
        {
            ObjectIndex = -1;
            return;
        }
        auto* serial = reinterpret_cast<volatile LONG*>(item + kItemSerial);
        if (*serial == 0) // FUObjectArray::AllocateSerialNumber
        {
            const LONG next = InterlockedIncrement(reinterpret_cast<volatile LONG*>(g_object_array + kMasterSerialNumber));
            InterlockedCompareExchange(serial, next, 0);
        }
        ObjectSerialNumber = *serial;
    }

    auto FWeakObjectPtr::Get() const -> UObject*
    {
        uint8* item = ObjectSerialNumber ? item_at(ObjectIndex) : nullptr;
        if (!item || detail::at<int32>(item, kItemSerial) != ObjectSerialNumber) return nullptr;
        if (detail::at<int32>(item, kItemFlags) & (kPendingKill | kUnreachable)) return nullptr;
        return detail::at<UObject*>(item, kItemObject);
    }

    // -------------------------------------------------------------------- script VM ---------------
    auto FFrame::Step(UObject* context, void* result) -> void
    {
        const uint8 op = *Code()++;
        g_natives[op](context, *this, result);
    }

    // -------------------------------------------------------------------- hooks -------------------
    auto Hook::RegisterEngineTickPreCallback(EngineTickCallback cb, FCallbackOptions) -> void { g_tick_pre.push_back(std::move(cb)); }
    auto Hook::RegisterProcessEventPreCallback(ProcessEventCallback cb, FCallbackOptions) -> void { g_pe_pre.push_back(std::move(cb)); }
    auto Hook::RegisterProcessEventPostCallback(ProcessEventCallback cb, FCallbackOptions) -> void { g_pe_post.push_back(std::move(cb)); }

    // -------------------------------------------------------------------- object globals ----------
    auto UObjectGlobals::object_count() -> int32 { return g_ready ? num_objects() : 0; }

    auto UObjectGlobals::StaticFindObject(UClass*, UObject*, const CharType* path) -> UObject*
    {
        if (!g_ready || !path) return nullptr;
        const StringType key(path);
        if (auto it = g_find_cache.find(key); it != g_find_cache.end())
        {
            if (UObject* o = it->second.Get(); o && o->GetFName() == FName(key.substr(key.find_last_of(L".:") + 1).c_str(), FNAME_Find)) return o;
            g_find_cache.erase(it); // unloaded (e.g. a /Game class after a map change): look again
        }
        const PathParts parts = split_path(path);
        if (!parts.ok) return nullptr;
        UObject* found = nullptr;
        for_each_object([&](UObject* o) {
            if (o->GetFName() != parts.names.back() || !matches_path(o, parts)) return true;
            found = o;
            return false;
        });
        if (found) g_find_cache[key] = FWeakObjectPtr(found);
        return found;
    }

    auto UObjectGlobals::FindAllOf(const CharType* class_name, std::vector<UObject*>& out) -> void
    {
        if (!g_ready) return;
        const FName name(class_name, FNAME_Find);
        if (name.IsNone()) return;
        for_each_object([&](UObject* o) {
            const UClass* c = o->GetClassPrivate();
            if (c && c->GetFName() == name) out.push_back(o);
            return true;
        });
    }

    auto UObjectGlobals::StaticConstructObject(UClass* cls, UObject* outer, FName name, EObjectFlags flags, UObject* tmpl) -> UObject*
    {
        if (!g_ready || !cls) return nullptr;
        return g_static_construct(cls, outer, name, flags, 0, tmpl, false, nullptr, false);
    }

    auto UObjectGlobals::RegisterHook(UFunction* fn, UnrealScriptFunctionCallable pre, UnrealScriptFunctionCallable post, void* data) -> std::pair<int, int>
    {
        if (!fn) return {-1, -1};
        int slot = -1;
        for (int i = 0; i < g_native_hook_count; ++i)
        {
            if (g_native_hooks[i].fn == fn) slot = i;
        }
        if (slot < 0)
        {
            if (g_native_hook_count >= kMaxNativeHooks)
            {
                Output::send<LogLevel::Error>(STR("[fue] native hook on {}: all {} slots used\n"), fn->GetFullName(), kMaxNativeHooks);
                return {-1, -1};
            }
            slot = g_native_hook_count++;
            g_native_hooks[slot].fn = fn;
            g_native_hooks[slot].original = fn->GetFuncPtr();
            fn->SetFuncPtr(g_native_thunks[static_cast<size_t>(slot)]);
        }
        NativeHook& h = g_native_hooks[slot];
        if (pre) h.pre.emplace_back(std::move(pre), data);
        if (post) h.post.emplace_back(std::move(post), data);
        Output::send<LogLevel::Verbose>(STR("[fue] native hook slot {} on {} (original Func {})\n"), slot, fn->GetFullName(), reinterpret_cast<void*>(h.original));
        return {static_cast<int>(h.pre.size()), static_cast<int>(h.post.size())};
    }
} // namespace fue

// FUSERMultiplayer.dll: the multiplayer client side. Sessions are hosted by the FDS dedicated server;
// this DLL hosts nothing. It covers the matchmaking join (with a stub for player-hosted sessions,
// see "player-hosted sessions"), the P2P tunnel and its signaling, custom songs (catalogue / community installs,
// song transfers between players, the sync notices), the Diamond store, the missing-stem skip and the diagnostics.
//
// The loader used to host as well: a player's own game as a listen server, or a second headless game process (the
// sidecar), with the dedicated server's compiled-out gameplay authority re-created around it. FDS replaced both, and
// that code was removed. So were the client-side fixes every joiner ran in that era, once FDS was confirmed to
// behave correctly.
//
// Keys (debug builds only, game window focused): Ctrl+Shift+J join HostAddress (+ JoinOptions) directly,
// Ctrl+Shift+T trace 8 s of ProcessEvent functions, Ctrl+Shift+R report, Ctrl+Shift+U install StoreTestInstall,
// Ctrl+Shift+N STUN probe, Ctrl+Shift+X song transfer test. After a join (with DiagnosticLogs=1) or a key, state
// changes are logged on their own. FuserNative.ini: HostAddress= (default 127.0.0.1:7777), JoinOptions=
// (default none; an FDS needs ?playerSessionId=?gameSessionId=?hypeUserId=?buildConfig=), JoinAddressOverride=
// redirects a matchmaking join. Output: FuserNative.log (a second instance: FuserNative.2.log), lines tagged [host].
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "fue/fue.hpp"
#include "multiplayer/community_fetch.hpp"
#include "multiplayer/json_lite.hpp"
#include "multiplayer/p2p_tunnel.hpp"
#include "multiplayer/pak_check.hpp"

#include <funchook.h>
#include "hook_range.hpp"

#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

#ifndef FNL_VERSION
#define FNL_VERSION "dev" // CMake passes the VERSION file's contents (reported to fuserd)
#endif

using namespace fue;

auto unlocks_add_song_row(const CharType* name) -> uint8*; // unlocks.cpp (winhttp.dll)

namespace
{
    // ---------------------------------------------------------------- layout --------------------------------------
    // A game server object in GameInstance._gameServer (UHypeMultiplayerServerBase), read by the state report only.
    constexpr size_t kModeByte = 0x30;       // 4 freestyle, 5 battle, 6 main stage
    constexpr size_t kHostBeacon = 0x40;     // _hostBeacon, the first reflected member
    constexpr size_t kStartedFlag = 0xe8;    // set by the first Tick, after the venue has been chosen
    constexpr size_t kSessionId = 0xf0;      // FString, "local-dev-gamesession-<GUID>" after init

    // UMultiplayerGameplayState (the game state's MultiplayerGameplayState component): the multiplayer set's stages.
    constexpr size_t kStageIndex = 0x370;    // current index into _gameStages (not reflected)
    constexpr size_t kStageSize = 0x34;      // FMultiplayerGameStage; checked against the reflected array

    // Every exe address this DLL calls or hooks (RVAs), per game build, keyed by fue's build name. EGS v1.4.2 is the
    // reference; the other rows come from tools/derive_mp_build.py, which matches each function (prologue, callers,
    // whole body, references, vtable neighbours) and checks its body is the same, so the field offsets and vtable
    // slots used here hold too. Members are in derive_mp_build.py's SYMBOLS order: its output pastes in as a row.
    // (It still derives the addresses of the removed listen-host code and client workarounds, as RETIRED symbols, but
    // leaves them out of the row.) A build is supported only when its row is complete (verified_build()); with any
    // address 0 nothing installs.
    struct VerifiedBuild
    {
        const CharType* name;
        // drop hooks (g_drop_hooks)
        uintptr_t drop_song_item_play, drop_desk_spawn_disc, drop_local_disc_spawn, drop_send_spawn_disc;
        // gameplay
        uintptr_t deck_disc_of, deck_desk_of, venue_travel, venue_end_play, stock_kick, mm_join, mm_set_state;
        uintptr_t cel_missing_queue, cel_missing_snapshot, audio_of, clock_tick;
        // song install (Route B, song transfers)
        uintptr_t platform_file_manager, find_platform_file, pak_mount, cel_mgr_of, cel_mgr_register;
        uintptr_t unlock_map_find, unlock_map_alloc, fname_hash, unlock_map_link;
        // event highway and the Diamond Store
        uintptr_t new_highway_event, ec_list_done, ec_purchase_done, claimed_done, ec_purchase_request;
    };
    constexpr VerifiedBuild kVerified[] = {
        {.name = STR("EGS v1.4.2"),
            .drop_song_item_play = 0x7b34d0, .drop_desk_spawn_disc = 0x5fdbd0,
            .drop_local_disc_spawn = 0x6109f0, .drop_send_spawn_disc = 0x68af70, .deck_disc_of = 0x62e320,
            .deck_desk_of = 0x5f8b50, .venue_travel = 0x7b2de0, .venue_end_play = 0x68bca0, .stock_kick = 0x695990,
            .mm_join = 0x706890, .mm_set_state = 0x71bdd0, .cel_missing_queue = 0x62a920,
            .cel_missing_snapshot = 0x621700, .audio_of = 0x591640, .clock_tick = 0x5928e0,
            .platform_file_manager = 0x912720, .find_platform_file = 0x9103c0,
            .pak_mount = 0x17c5330, .cel_mgr_of = 0x5915c0, .cel_mgr_register = 0x59c740,
            .unlock_map_find = 0x4640e0, .unlock_map_alloc = 0x341bf0, .fname_hash = 0x9eab10,
            .unlock_map_link = 0x358bc0, .new_highway_event = 0x771540, .ec_list_done = 0x790530,
            .ec_purchase_done = 0x791730, .claimed_done = 0x7901c0, .ec_purchase_request = 0x789280},
        // derived by tools/derive_mp_build.py (2026-10-04): every address CONFIRMED, every body the same
        {.name = STR("EGS CL1317808"),
            .drop_song_item_play = 0x7b3500, .drop_desk_spawn_disc = 0x5fdbd0,
            .drop_local_disc_spawn = 0x6109f0, .drop_send_spawn_disc = 0x68af70, .deck_disc_of = 0x62e320,
            .deck_desk_of = 0x5f8b50, .venue_travel = 0x7b2e10, .venue_end_play = 0x68bca0, .stock_kick = 0x695990,
            .mm_join = 0x7068c0, .mm_set_state = 0x71be00, .cel_missing_queue = 0x62a920,
            .cel_missing_snapshot = 0x621700, .audio_of = 0x591640, .clock_tick = 0x5928e0,
            .platform_file_manager = 0x912750, .find_platform_file = 0x9103f0,
            .pak_mount = 0x17c5360, .cel_mgr_of = 0x5915c0, .cel_mgr_register = 0x59c740,
            .unlock_map_find = 0x4640e0, .unlock_map_alloc = 0x341bf0, .fname_hash = 0x9eab40,
            .unlock_map_link = 0x358bc0, .new_highway_event = 0x771570, .ec_list_done = 0x790560,
            .ec_purchase_done = 0x791760, .claimed_done = 0x7901f0, .ec_purchase_request = 0x7892b0},
        {.name = STR("Steam CL1317755"),
            .drop_song_item_play = 0x7e8ec0, .drop_desk_spawn_disc = 0x6347d0,
            .drop_local_disc_spawn = 0x646a90, .drop_send_spawn_disc = 0x6c0c40, .deck_disc_of = 0x664380,
            .deck_desk_of = 0x62f710, .venue_travel = 0x7e87d0, .venue_end_play = 0x6c1970, .stock_kick = 0x6cb570,
            .mm_join = 0x73c3e0, .mm_set_state = 0x751920, .cel_missing_queue = 0x660980,
            .cel_missing_snapshot = 0x657760, .audio_of = 0x5c8c70, .clock_tick = 0x5c9f10,
            .platform_file_manager = 0x947940, .find_platform_file = 0x9455e0,
            .pak_mount = 0x17f9e40, .cel_mgr_of = 0x5c8bf0, .cel_mgr_register = 0x5d3d70,
            .unlock_map_find = 0x464660, .unlock_map_alloc = 0x341fc0, .fname_hash = 0xa20020,
            .unlock_map_link = 0x358f90, .new_highway_event = 0x7a6ef0, .ec_list_done = 0x7c5ef0,
            .ec_purchase_done = 0x7c70f0, .claimed_done = 0x7c5b80, .ec_purchase_request = 0x7bec40},
    };
    // every member after the name is a uintptr_t: complete() walks them as an array
    constexpr size_t kAddrCount = (sizeof(VerifiedBuild) - offsetof(VerifiedBuild, drop_song_item_play)) / sizeof(uintptr_t);
    static_assert(offsetof(VerifiedBuild, drop_song_item_play) + kAddrCount * sizeof(uintptr_t) == sizeof(VerifiedBuild));

    auto exe_base() -> uintptr_t { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

    // ---------------------------------------------------------------- raw memory (no C++ objects: SEH) ------
    auto read_ptr(const void* base, size_t off, void** out) -> bool
    {
        __try
        {
            *out = *reinterpret_cast<void* const*>(static_cast<const uint8*>(base) + off);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto read_u8(const void* base, size_t off, uint8* out) -> bool
    {
        __try
        {
            *out = *(static_cast<const uint8*>(base) + off);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto write_ptr(void* base, size_t off, void* value) -> bool
    {
        __try
        {
            *reinterpret_cast<void**>(static_cast<uint8*>(base) + off) = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto read_i32(const void* base, size_t off, int32* out) -> bool
    {
        __try
        {
            *out = *reinterpret_cast<const int32*>(static_cast<const uint8*>(base) + off);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto read_f32(const void* base, size_t off, float* out) -> bool
    {
        __try
        {
            *out = *reinterpret_cast<const float*>(static_cast<const uint8*>(base) + off);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Copies an FString's characters out of engine memory (at most `cap`).
    auto read_fstring(const void* base, size_t off, wchar_t* out, int cap) -> int
    {
        __try
        {
            const auto* s = static_cast<const uint8*>(base) + off;
            const wchar_t* data = *reinterpret_cast<wchar_t* const*>(s);
            const int32 num = *reinterpret_cast<const int32*>(s + 8);
            if (!data || num <= 1) return 0;
            const int n = num - 1 < cap - 1 ? num - 1 : cap - 1;
            for (int i = 0; i < n; ++i) out[i] = data[i];
            out[n] = 0;
            return n;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    auto hex(const void* p) -> StringType { return p ? std::format(STR("{}"), p) : StringType{STR("null")}; }

    // ---------------------------------------------------------------- reflection helpers -------------------
    // A ProcessEvent call with a parameter buffer built and destroyed through the parameters' properties.
    class Call
    {
      public:
        explicit Call(const CharType* path) : Call(UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path)) {}
        explicit Call(UFunction* fn) : m_fn(fn)
        {
            if (m_fn) m_buf.assign(static_cast<size_t>(m_fn->GetPropertiesSize()) + 16, 0);
            for_params([&](FProperty* p) { p->InitializeValue_InContainer(m_buf.data()); });
        }
        ~Call()
        {
            for_params([&](FProperty* p) { p->DestroyValue_InContainer(m_buf.data()); });
        }
        Call(const Call&) = delete;
        Call& operator=(const Call&) = delete;

        auto ok() const -> bool { return m_fn != nullptr; }
        auto param(const CharType* name) -> FProperty* { return m_fn ? m_fn->FindProperty(FName(name, FNAME_Add)) : nullptr; }
        template <typename T>
        auto set(const CharType* name, T value) -> Call&
        {
            if (FProperty* p = param(name)) *p->ContainerPtrToValuePtr<T>(m_buf.data()) = value;
            return *this;
        }
        template <typename T>
        auto get(const CharType* name) -> T
        {
            FProperty* p = param(name);
            return p ? *p->ContainerPtrToValuePtr<T>(m_buf.data()) : T{};
        }
        // Copy a value of the parameter's own type in (e.g. a soft object pointer read from another object).
        auto copy_in(const CharType* name, const void* src) -> Call&
        {
            if (FProperty* p = param(name); p && src) p->CopyCompleteValue(p->ContainerPtrToValuePtr<void>(m_buf.data()), src);
            return *this;
        }
        // The parameter's storage (e.g. an out TArray to read in place, without copying it).
        auto raw(const CharType* name) -> void*
        {
            FProperty* p = param(name);
            return p ? p->ContainerPtrToValuePtr<void>(m_buf.data()) : nullptr;
        }
        // Copy a value into the first parameter, whatever its name (Blueprint parameter names vary).
        auto copy_in_first(const void* src) -> Call&
        {
            FProperty* first = nullptr;
            for_params([&](FProperty* p) {
                if (!first) first = p;
            });
            if (first && src) first->CopyCompleteValue(first->ContainerPtrToValuePtr<void>(m_buf.data()), src);
            return *this;
        }
        auto set_string(const CharType* name, const CharType* value) -> Call&
        {
            const FString s(value);
            if (FProperty* p = param(name)) p->CopyCompleteValue(p->ContainerPtrToValuePtr<void>(m_buf.data()), &s);
            return *this;
        }
        auto get_string(const CharType* name) -> StringType
        {
            FProperty* p = param(name);
            if (!p) return {};
            FString s{};
            p->CopyCompleteValue(&s, p->ContainerPtrToValuePtr<void>(m_buf.data()));
            return *s;
        }
        auto call(UObject* target) -> bool
        {
            if (!m_fn || !target) return false;
            target->ProcessEvent(m_fn, m_buf.data());
            return true;
        }

      private:
        template <typename F>
        void for_params(F&& f)
        {
            if (!m_fn) return;
            for (FProperty* p : TFieldRange<FProperty>(m_fn, EFieldIterationFlags::IncludeDeprecated))
            {
                if (!p->HasAnyPropertyFlags(CPF_Parm)) break;
                f(p);
            }
        }
        UFunction* m_fn = nullptr;
        std::vector<uint8> m_buf{};
    };

    auto cdo(const CharType* path) -> UObject* { return UObjectGlobals::StaticFindObject(nullptr, nullptr, path); }

    auto is_default(const UObject* o) -> bool { return o->GetName().rfind(STR("Default__"), 0) == 0; }

    // A live instance: not a class default object or archetype (EObjectFlags @0x8: RF_ClassDefaultObject 0x10,
    // RF_ArchetypeObject 0x20), nor inside one - e.g. a widget Blueprint's "WidgetArchetype" template, which has no
    // world (calling a screen function on it crashed the host).
    auto is_live(UObject* o) -> bool
    {
        for (UObject* x = o; x; x = x->GetOuterPrivate())
        {
            uint32 flags = 0;
            std::memcpy(&flags, reinterpret_cast<const uint8*>(x) + 0x8, sizeof(flags));
            if ((flags & 0x30) || is_default(x)) return false;
        }
        return true;
    }

    auto live_objects(const CharType* class_name) -> std::vector<UObject*>
    {
        std::vector<UObject*> all{}, out{};
        UObjectGlobals::FindAllOf(class_name, all);
        for (UObject* o : all)
            if (is_live(o)) out.push_back(o);
        return out;
    }

    // The GameInstance, cached: finding it walks every UObject (FindAllOf; ~850k in a running game), and dozens of
    // routines ask for it. It lives for the whole process, so the weak pointer only goes stale at shutdown; then, or
    // before it exists, a lookup runs at most every 250 ms. Locked: a few callers run off the game thread.
    auto game_instance() -> UObject*
    {
        static std::mutex mu;
        static FWeakObjectPtr cached{};
        static uint64 retry_at = 0;
        static int scans = 0;
        std::lock_guard lock(mu);
        if (UObject* gi = cached.Get(); gi && is_live(gi)) return gi;
        const uint64 now = GetTickCount64();
        if (now < retry_at) return nullptr;
        LARGE_INTEGER freq{}, t0{}, t1{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        const auto gis = live_objects(STR("BP_HypeGameInstance_C"));
        QueryPerformanceCounter(&t1);
        UObject* gi = gis.empty() ? nullptr : gis.front();
        if (!gi)
        {
            retry_at = now + 250;
            return nullptr;
        }
        cached = FWeakObjectPtr(gi);
        if (++scans <= 3)
            Output::send<LogLevel::Normal>(STR("[host] game instance found: scan of {} objects took {:.2f} ms; cached\n"),
                                           UObjectGlobals::object_count(), 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart));
        return gi;
    }

    auto game_server_slot(UObject* gi) -> UObject** { return gi ? gi->GetValuePtrByPropertyNameInChain<UObject*>(STR("_gameServer")) : nullptr; }

    auto world_bool(const CharType* fn, UObject* ctx) -> int
    {
        Call c(fn);
        if (!c.ok() || !ctx) return -1;
        c.set<UObject*>(STR("WorldContextObject"), ctx);
        c.call(cdo(STR("/Script/Engine.Default__KismetSystemLibrary")));
        return c.get<bool>(STR("ReturnValue")) ? 1 : 0;
    }

    auto level_name(UObject* ctx) -> StringType
    {
        Call c(STR("/Script/Engine.GameplayStatics:GetCurrentLevelName"));
        if (!c.ok() || !ctx) return STR("?");
        c.set<UObject*>(STR("WorldContextObject"), ctx).set<bool>(STR("bRemovePrefixString"), true);
        c.call(cdo(STR("/Script/Engine.Default__GameplayStatics")));
        return c.get_string(STR("ReturnValue"));
    }

    auto console_command(UObject* ctx, const StringType& command) -> bool
    {
        Call c(STR("/Script/Engine.KismetSystemLibrary:ExecuteConsoleCommand"));
        if (!c.ok() || !ctx) return false;
        c.set<UObject*>(STR("WorldContextObject"), ctx).set_string(STR("Command"), command.c_str());
        return c.call(cdo(STR("/Script/Engine.Default__KismetSystemLibrary")));
    }

    // EGameMapType (StartupMap, Shell, Transition, Venue, Cutscene) of the persistent level: the world settings whose
    // level's world is named like the current level (each streaming sublevel has its own). The venue begin-play that
    // starts hosting is skipped on Transition maps.
    auto map_type(const StringType& level_name) -> StringType
    {
        static const CharType* const names[] = {STR("StartupMap"), STR("Shell"), STR("Transition"), STR("Venue"), STR("Cutscene")};
        for (UObject* ws : live_objects(STR("HypeWorldSettings")))
        {
            const UObject* level = ws->GetOuterPrivate();
            const UObject* world = level ? level->GetOuterPrivate() : nullptr;
            if (!world || world->GetName() != level_name) continue;
            auto* t = ws->GetValuePtrByPropertyNameInChain<uint8>(STR("GameMapType"));
            return t ? (*t < 5 ? names[*t] : STR("?")) : STR("n/a");
        }
        return STR("map type not found");
    }

    // Each net driver with its role (NetDriverName: GameNetDriver, BeaconNetDriver, ...).
    auto net_drivers() -> StringType
    {
        StringType out{};
        for (UObject* d : live_objects(STR("IpNetDriver")))
        {
            auto* role = d->GetValuePtrByPropertyNameInChain<FName>(STR("NetDriverName"));
            out += std::format(STR("{}{} ({})"), out.empty() ? STR("") : STR(", "), d->GetName(), role ? role->ToString() : StringType{STR("?")});
        }
        return out.empty() ? STR("none") : out;
    }

    // Each beacon host with the port it listens on (AOnlineBeaconHost::ListenPort).
    auto beacon_hosts() -> StringType
    {
        StringType out{};
        for (UObject* b : live_objects(STR("HypeMultiplayerBeaconHost")))
        {
            auto* port = b->GetValuePtrByPropertyNameInChain<int32>(STR("ListenPort"));
            out += std::format(STR("{}{} (ListenPort {})"), out.empty() ? STR("") : STR(", "), b->GetName(), port ? *port : -1);
        }
        return out.empty() ? STR("none") : out;
    }

    auto complete(const VerifiedBuild& b) -> bool
    {
        const auto* a = &b.drop_song_item_play;
        return std::all_of(a, a + kAddrCount, [](uintptr_t v) { return v != 0; });
    }

    // The running build's row, if it is listed and complete. Cached once fue has named the build.
    auto verified_build() -> const VerifiedBuild*
    {
        static const VerifiedBuild* cached = nullptr;
        static bool resolved = false;
        if (resolved) return cached;
        const CharType* name = build_name();
        if (!name || !*name) return nullptr;
        resolved = true;
        for (const auto& b : kVerified)
            if (StringType(name) == b.name) cached = complete(b) ? &b : nullptr;
        Output::send<LogLevel::Normal>(STR("[host] build {}: multiplayer addresses {}\n"), name,
                                       cached ? STR("verified") : STR("NOT verified - multiplayer hooks stay off"));
        return cached;
    }

    // The absolute address of one of the running build's functions or variables (0 on an unverified build: callers
    // are all behind a verified_build() check).
    auto at(uintptr_t VerifiedBuild::*field) -> uintptr_t
    {
        const VerifiedBuild* b = verified_build();
        return b ? exe_base() + b->*field : 0;
    }

    // ---------------------------------------------------------------- hooks (funchook) --------------------------
    // funchook reserves a hook's trampoline page by searching upward from the function for free memory, with no range
    // check (hook_range.hpp). Under Proton the game's allocations fill the memory above the exe, so a page reserved
    // once the game is running can be out of jump range (Steam Deck, 2026-10-08: the drop hooks, prepared at venue
    // entry, got a page 2.8 GB away, and the first disc spawn jumped 4 GB too low). So every hook is prepared at
    // host_hook_start - the first engine callback, while that memory is still free; funchook_prepare only reserves
    // and fills the page, and later hooks reuse pages of that reservation when in range - and installed when it was
    // before, on the game thread. A hook whose page is still out of range is never installed.
    struct PreparedHook
    {
        funchook_t* handle;
        void* detour;
        void* trampoline; // the original function, callable, once installed
        int prepare;
    };
    std::unordered_map<uintptr_t, PreparedHook> g_prepared_hooks{}; // by function address; game thread only
    auto prepare_hook(uintptr_t address, void* detour) -> void
    {
        if (!address || g_prepared_hooks.contains(address)) return;
        PreparedHook p{funchook_create(), detour, reinterpret_cast<void*>(address), -1};
        if (p.handle) p.prepare = funchook_prepare(p.handle, &p.trampoline, detour);
        g_prepared_hooks.emplace(address, p);
    }
    // Installs the hook on address (prepared at startup, or now) and sets *original. Returns {prepare, install};
    // install is -2 when funchook's page is out of jump range (the hook stays off and the error is logged).
    auto install_hook(uintptr_t address, void* detour, void** original, const CharType* what) -> std::pair<int, int>
    {
        prepare_hook(address, detour);
        const auto it = g_prepared_hooks.find(address);
        if (it == g_prepared_hooks.end()) return {-1, -1};
        PreparedHook& p = it->second;
        if (p.detour != detour)
        {
            Output::send<LogLevel::Error>(STR("[host] hook {}: prepared for another detour - not installed\n"), what);
            return {-1, -1};
        }
        if (p.prepare != 0) return {p.prepare, -1};
        if (!hook_in_range(reinterpret_cast<void*>(address), p.trampoline))
        {
            Output::send<LogLevel::Error>(STR("[host] hook {}: funchook's trampoline {} is out of jump range of {} - not installed\n"), what, p.trampoline,
                                          reinterpret_cast<void*>(address));
            funchook_destroy(p.handle);
            p.prepare = -2; // asking again gives the same answer
            return {0, -2};
        }
        const int inst = funchook_install(p.handle, 0);
        if (inst == 0) *original = p.trampoline;
        return {0, inst};
    }

    // ---------------------------------------------------------------- gameplay flow ----------------------------
    // The world's game mode and game state (GameplayStatics), and the game state's MultiplayerGameplayState
    // component: its stage (EMultiplayerGameplayStage), DJ queue and spectators - what the multiplayer set is doing.
    auto world_object(const CharType* fn, UObject* ctx) -> UObject*
    {
        Call c(fn);
        if (!c.ok() || !ctx) return nullptr;
        c.set<UObject*>(STR("WorldContextObject"), ctx);
        c.call(cdo(STR("/Script/Engine.Default__GameplayStatics")));
        return c.get<UObject*>(STR("ReturnValue"));
    }

    auto mp_stage_name(uint8 v) -> const CharType*
    {
        static const CharType* const names[] = {STR("None"), STR("Lobby"), STR("RoundStart_SetupGameplay"), STR("RoundStart_DJOrderIntro"), STR("LoadStageKit"),
                                                STR("UpcomingDJ"), STR("Playing"), STR("RoundComplete_FarShot"), STR("RoundComplete_HottestMixIntro"),
                                                STR("RoundComplete_HottestMixPlayback"), STR("Results"), STR("Results_XPPayout"), STR("Battle_WaitForPlayers"),
                                                STR("Battle_MatchIntro"), STR("Battle_RoundStart"), STR("Battle_Gameplay"), STR("Battle_RoundEnd"), STR("Battle_RoundContinue"),
                                                STR("Battle_MatchEnd"), STR("Battle_Results"), STR("Battle_Results_XPPayout")};
        return v < std::size(names) ? names[v] : STR("?");
    }

    struct Flow
    {
        StringType game_mode{}, game_state{};
        int32 players = -1;
        int stage = -1, djs = -1, spectators = -1, round = -1;
        auto operator==(const Flow&) const -> bool = default;
    };

    auto array_num(UObject* o, const CharType* fn) -> int
    {
        Call c(fn);
        if (!c.ok() || !o || !c.call(o)) return -1;
        return c.param(STR("ReturnValue")) ? c.get<TArray<UObject*>>(STR("ReturnValue")).Num() : -1; // a view: the Call owns the array
    }

    auto capture_flow(UObject* gi) -> Flow
    {
        Flow f{};
        UObject* gm = world_object(STR("/Script/Engine.GameplayStatics:GetGameMode"), gi);
        UObject* gs = world_object(STR("/Script/Engine.GameplayStatics:GetGameState"), gi);
        f.game_mode = gm ? gm->GetClassPrivate()->GetName() : STR("none (client)");
        f.game_state = gs ? gs->GetClassPrivate()->GetName() : STR("none");
        if (auto* players = gs ? gs->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("PlayerArray")) : nullptr) f.players = players->Num();
        UObject** mpp = gs ? gs->GetValuePtrByPropertyNameInChain<UObject*>(STR("MultiplayerGameplayState")) : nullptr;
        UObject* mp = mpp ? *mpp : nullptr;
        if (mp)
        {
            Call stage(STR("/Script/Hype.MultiplayerGameplayState:GetMultiplayerGameplayStage"));
            if (stage.call(mp)) f.stage = stage.get<uint8>(STR("ReturnValue"));
            Call round(STR("/Script/Hype.MultiplayerGameplayState:GetCurrentStageRoundNumber"));
            if (round.call(mp)) f.round = round.get<int32>(STR("ReturnValue"));
            f.djs = array_num(mp, STR("/Script/Hype.MultiplayerGameplayState:GetDJQueue"));
            f.spectators = array_num(mp, STR("/Script/Hype.MultiplayerGameplayState:GetSpectators"));
        }
        return f;
    }

    auto flow_text(const Flow& f) -> StringType
    {
        return std::format(STR("game mode {}, game state {}, players {}, MP stage {}, round {}, DJ queue {}, spectators {}"), f.game_mode, f.game_state, f.players,
                           f.stage < 0 ? STR("n/a") : mp_stage_name(static_cast<uint8>(f.stage)), f.round, f.djs, f.spectators);
    }

    // ---------------------------------------------------------------- state ------------------------------------
    struct State
    {
        StringType level{};
        int is_server = -1, is_dedicated = -1, is_standalone = -1;
        void* server = nullptr;
        uint8 mode = 0xff, started = 0xff;
        void* host_beacon = nullptr;
        void* host_beacon_object = nullptr;
        size_t beacon_hosts = 0, net_drivers = 0;
        Flow flow{};
        auto operator==(const State&) const -> bool = default;
    };

    bool g_armed = false; // after a join or a key: poll for changes
    State g_last{};

    // ---------------------------------------------------------------- incoming connections ---------------------
    // Every tick: each net driver's ClientConnections (a UPROPERTY), so a join that PreLogin rejects - its connection
    // lives only a few frames - is still seen. Logs each connection's arrival, its PlayerController (set once login
    // got past PreLogin) and its departure.
    struct Conn
    {
        FWeakObjectPtr obj{};
        const void* raw = nullptr;
        bool has_pc = false;
        uint64 since = 0;
    };
    struct DriverWatch
    {
        FWeakObjectPtr driver{};
        StringType label{};
        std::vector<Conn> conns{};
    };
    std::vector<DriverWatch> g_watch{};

    auto driver_label(UObject* d) -> StringType
    {
        auto* role = d->GetValuePtrByPropertyNameInChain<FName>(STR("NetDriverName"));
        const StringType r = role ? role->ToString() : StringType{};
        return r == STR("GameNetDriver") ? STR("game") : STR("beacon"); // the beacon driver is named after itself
    }

    auto refresh_watch() -> void
    {
        std::vector<DriverWatch> next{};
        for (UObject* d : live_objects(STR("IpNetDriver")))
        {
            DriverWatch w{FWeakObjectPtr(d), driver_label(d), {}};
            for (auto& old : g_watch)
                if (old.driver.Get() == d) w.conns = std::move(old.conns);
            next.push_back(std::move(w));
        }
        g_watch = std::move(next);
    }

    auto watch_connections() -> void
    {
        const uint64 now = GetTickCount64();
        for (auto& w : g_watch)
        {
            UObject* d = w.driver.Get();
            auto* conns = d ? d->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("ClientConnections")) : nullptr;
            if (!conns) continue;
            for (int32 i = 0; i < conns->Num(); ++i)
            {
                UObject* c = (*conns)[i];
                if (!c) continue;
                auto it = std::find_if(w.conns.begin(), w.conns.end(), [&](const Conn& k) { return k.raw == c && k.obj.Get() == c; });
                if (it == w.conns.end())
                {
                    w.conns.push_back({FWeakObjectPtr(c), c, false, now});
                    Output::send<LogLevel::Normal>(STR("[host] connection + {} on the {} driver ({} open)\n"), c->GetName(), w.label, conns->Num());
                    it = w.conns.end() - 1;
                }
                UObject** pc = c->GetValuePtrByPropertyNameInChain<UObject*>(STR("PlayerController"));
                if (!it->has_pc && pc && *pc)
                {
                    it->has_pc = true;
                    Output::send<LogLevel::Normal>(STR("[host] connection {} logged in: PlayerController {} after {} ms\n"), c->GetName(), (*pc)->GetName(), now - it->since);
                }
            }
            for (auto it = w.conns.begin(); it != w.conns.end();)
            {
                UObject* c = it->obj.Get();
                bool still = false;
                for (int32 i = 0; c && i < conns->Num(); ++i) still = still || (*conns)[i] == c;
                if (still)
                {
                    ++it;
                    continue;
                }
                Output::send<LogLevel::Normal>(STR("[host] connection - {} on the {} driver after {} ms{}\n"), c ? c->GetName() : StringType{STR("(destroyed)")}, w.label, now - it->since,
                                               it->has_pc ? STR("") : STR(" (never logged in: rejected before or at PreLogin)"));
                it = w.conns.erase(it);
            }
        }
    }

    auto capture() -> State
    {
        State s{};
        UObject* gi = game_instance();
        if (!gi) return s;
        s.level = level_name(gi);
        s.is_server = world_bool(STR("/Script/Engine.KismetSystemLibrary:IsServer"), gi);
        s.is_dedicated = world_bool(STR("/Script/Engine.KismetSystemLibrary:IsDedicatedServer"), gi);
        s.is_standalone = world_bool(STR("/Script/Engine.KismetSystemLibrary:IsStandalone"), gi);
        if (UObject** gs = game_server_slot(gi); gs && *gs)
        {
            s.server = *gs;
            read_u8(*gs, kModeByte, &s.mode);
            read_u8(*gs, kStartedFlag, &s.started);
            read_ptr(*gs, kHostBeacon, &s.host_beacon);
            read_ptr(*gs, kHostBeacon + 8, &s.host_beacon_object);
        }
        s.beacon_hosts = live_objects(STR("HypeMultiplayerBeaconHost")).size();
        s.net_drivers = live_objects(STR("IpNetDriver")).size();
        s.flow = capture_flow(gi);
        return s;
    }

    // UE's IsServer is also true in Standalone, so the net mode comes from all three.
    auto net_mode(const State& s) -> const CharType*
    {
        if (s.is_server < 0) return STR("?");
        if (s.is_standalone == 1) return STR("Standalone");
        if (s.is_dedicated == 1) return STR("DedicatedServer");
        return s.is_server == 1 ? STR("ListenServer") : STR("Client");
    }

    auto log_state(const CharType* why, const State& s, bool full) -> void
    {
        Output::send<LogLevel::Normal>(STR("[host] {}: level '{}' ({}), net mode {}, _gameServer {}, mode {}, started {}, _hostBeacon {}, _hostBeaconObject {}, "
                                           "beacon hosts {}, IpNetDrivers {}\n"),
                                       why, s.level, map_type(s.level), net_mode(s), hex(s.server), s.mode == 0xff ? -1 : s.mode, s.started == 0xff ? -1 : s.started,
                                       hex(s.host_beacon), hex(s.host_beacon_object), s.beacon_hosts, s.net_drivers);
        Output::send<LogLevel::Normal>(STR("[host]   {}\n"), flow_text(s.flow));
        if (!full) return;
        Output::send<LogLevel::Normal>(STR("[host]   net drivers: {}; beacon hosts: {}\n"), net_drivers(), beacon_hosts());
        if (s.server)
        {
            wchar_t id[128]{};
            const int n = read_fstring(s.server, kSessionId, id, 128);
            Output::send<LogLevel::Normal>(STR("[host]   server {}: session id '{}'\n"), static_cast<UObject*>(s.server)->GetFullName(), n > 0 ? id : STR(""));
        }
    }

    // A key=value line of FuserNative.ini, or the fallback.
    auto ini_value(const CharType* key, const CharType* fallback) -> StringType
    {
        const StringType prefix = StringType(key) + STR("=");
        std::wifstream in(std::filesystem::path(module_dir()) / L"FuserNative.ini");
        std::wstring line{};
        while (std::getline(in, line))
        {
            if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
        }
        return fallback;
    }

#if FNL_DEBUG
    // The direct join (Ctrl+Shift+J, debug builds): what matchmaking does before its travel and a plain `open` does
    // not - the lobby music asset and the multiplayer freestyle game mode.

    // The lobby music. MultiplayerGameplayState's BeginPlay caches HypeMultiplayerSettings.MultiplayerTransition (a
    // soft object pointer) and builds its playback - only if the asset is already loaded. The matchmaking path
    // presumably loads it; a direct `open` / `?listen` travel does not, and without it no lobby music plays, the
    // music timeline never loops and the lobby entrance (camera, ready) never starts. Loaded here, before a venue
    // loads, and kept rooted.
    auto preload_mp_transition() -> void
    {
        static bool done = false;
        if (done) return;
        UObject* settings = cdo(STR("/Script/Hype.Default__HypeMultiplayerSettings"));
        FProperty* soft = settings ? settings->GetPropertyByNameInChain(STR("MultiplayerTransition")) : nullptr;
        Call load(STR("/Script/Engine.KismetSystemLibrary:LoadAsset_Blocking"));
        if (!soft || !load.ok())
        {
            Output::send<LogLevel::Error>(STR("[host] transition: MultiplayerTransition setting ({}) or LoadAsset_Blocking missing\n"), soft != nullptr);
            return;
        }
        load.copy_in(STR("Asset"), soft->ContainerPtrToValuePtr<void>(settings));
        load.call(cdo(STR("/Script/Engine.Default__KismetSystemLibrary")));
        UObject* asset = load.get<UObject*>(STR("ReturnValue"));
        if (asset && !asset->IsRootSet()) asset->SetRootSet();
        done = asset != nullptr;
        Output::send<LogLevel::Normal>(STR("[host] transition: MultiplayerTransition {}\n"), asset ? asset->GetFullName() : StringType{STR("did NOT load")});
    }

    // The game instance's ULevelLoadConfig._gameMode (EHypeGameMode) is what IsInMultiplayerHypeGameMode reads
    // (MultiplayerFreestyle / Battle / MainStage = 4..6). The multiplayer lobby screen only sets itself up - finds the
    // local player, wires the ready button, fills the performer count - when it is a multiplayer mode. Matchmaking
    // configures it before the travel; a direct `open ip:port` does not, so the joining client stays Freestyle (0).
    auto configure_mp_freestyle() -> void
    {
        const auto configs = live_objects(STR("LevelLoadConfig"));
        UObject* llc = configs.empty() ? nullptr : configs.front();
        auto mode = [&]() -> int {
            auto* m = llc ? llc->GetValuePtrByPropertyNameInChain<uint8>(STR("_gameMode")) : nullptr;
            return m ? *m : -1;
        };
        const int before = mode();
        Call c(STR("/Script/Hype.LevelLoadConfig:ConfigureMultiplayerFreestyle"));
        const bool ok = llc && c.call(llc);
        Output::send<LogLevel::Normal>(STR("[host] game mode: LevelLoadConfig {} _gameMode {} -> {} ({})\n"), hex(llc), before, mode(),
                                       ok ? STR("ConfigureMultiplayerFreestyle") : STR("FAILED"));
    }
#endif

    auto open_url(const StringType& url) -> void
    {
        const StringType cmd = STR("open ") + url;
        const bool ok = console_command(game_instance(), cmd);
        Output::send<LogLevel::Normal>(STR("[host] '{}': {}\n"), cmd, ok ? STR("sent") : STR("FAILED (no game instance or ExecuteConsoleCommand)"));
        g_armed = true;
    }

#if FNL_DEBUG
    // Ctrl+Shift+J's join: HostAddress + JoinOptions.
    auto open_join() -> void { open_url(ini_value(STR("HostAddress"), STR("127.0.0.1:7777")) + ini_value(STR("JoinOptions"), STR(""))); }
#endif

    auto mp_state(UObject* gi) -> UObject*
    {
        UObject* gs = world_object(STR("/Script/Engine.GameplayStatics:GetGameState"), gi);
        UObject** mps = gs ? gs->GetValuePtrByPropertyNameInChain<UObject*>(STR("MultiplayerGameplayState")) : nullptr;
        return mps ? *mps : nullptr;
    }

    auto host_player_state(UObject* gi) -> UObject*
    {
        Call c(STR("/Script/Engine.GameplayStatics:GetPlayerController"));
        if (!c.ok() || !gi) return nullptr;
        c.set<UObject*>(STR("WorldContextObject"), gi).set<int32>(STR("PlayerIndex"), 0);
        c.call(cdo(STR("/Script/Engine.Default__GameplayStatics")));
        UObject* pc = c.get<UObject*>(STR("ReturnValue"));
        UObject** ps = pc ? pc->GetValuePtrByPropertyNameInChain<UObject*>(STR("PlayerState")) : nullptr;
        return ps ? *ps : nullptr;
    }

    auto world_seconds(UObject* ctx) -> float
    {
        Call c(STR("/Script/Engine.GameplayStatics:GetTimeSeconds"));
        if (!c.ok() || !ctx) return -1.0f;
        c.set<UObject*>(STR("WorldContextObject"), ctx);
        c.call(cdo(STR("/Script/Engine.Default__GameplayStatics")));
        return c.get<float>(STR("ReturnValue"));
    }

    // The reflected _gameStages, as raw 0x34-byte FMultiplayerGameStage entries.
    auto stages(UObject* mps) -> TArray<uint8>* { return mps ? mps->GetValuePtrByPropertyNameInChain<TArray<uint8>>(STR("_gameStages")) : nullptr; }

    auto stage_type(UObject* mps, int32 i) -> int
    {
        TArray<uint8>* s = stages(mps);
        return s && i >= 0 && i < s->Num() ? s->Data[static_cast<size_t>(i) * kStageSize] : -1;
    }

    auto stage_label(UObject* mps, int32 i) -> StringType
    {
        const int t = stage_type(mps, i);
        return t < 0 ? StringType{STR("-")} : std::format(STR("{} ({})"), mp_stage_name(static_cast<uint8>(t)), i);
    }

    // ---------------------------------------------------------------- players and teams ------------------------
    // The players of the current venue, and what the report and the client-side lobby fix-ups read from them.
    auto players(UObject* mps) -> std::vector<UObject*>
    {
        std::vector<UObject*> out{};
        UObject* gs = world_object(STR("/Script/Engine.GameplayStatics:GetGameState"), mps);
        if (auto* arr = gs ? gs->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("PlayerArray")) : nullptr)
            for (int32 i = 0; i < arr->Num(); ++i)
                if ((*arr)[i]) out.push_back((*arr)[i]);
        return out;
    }

    template <typename T>
    auto prop(UObject* o, const CharType* name) -> T
    {
        auto* p = o ? o->GetValuePtrByPropertyNameInChain<T>(name) : nullptr;
        return p ? *p : T{};
    }

#if FNL_DEBUG
    // From here to shell_flow_mgr: what the debug report (Ctrl+Shift+R) and the trace logs read - debug builds only.
    auto max_djs() -> int32
    {
        return prop<int32>(cdo(STR("/Script/Hype.Default__HypeMultiplayerSettings")), STR("MaxNumDJsInSession"));
    }

    auto team_name(uint8 t) -> const CharType* { return t == 1 ? STR("DJ") : t == 2 ? STR("Spectator") : STR("Unknown"); }

    // HypeGameStateVenue's tick moves a player from _incomingHypePlayerStates to _hypePlayerStates - the list the
    // whole multiplayer flow uses (DJ queue, teams, readiness) - only once: _hypeUserId != -1, _localUserName is
    // non-empty (not replicated: each machine needs it for every player), _platform is a real platform (not
    // NumPlatforms = 6), _team is set, and the player's DJPlayerState has received its avatar and venue config
    // (unreflected flags at +0x390 / +0x3f8, set by ServerSetAvatarConfig / ServerSetVenueConfig presumably).
    constexpr size_t kDjAvatarReceived = 0x390, kDjVenueReceived = 0x3f8;

    auto name_len(UObject* ps) -> int32
    {
        auto* s = ps->GetValuePtrByPropertyNameInChain<TArray<wchar_t>>(STR("_localUserName"));
        return s ? s->Num() : -1;
    }

    auto dj_flags(UObject* ps, uint8* avatar, uint8* venue) -> UObject*
    {
        UObject* djps = prop<UObject*>(ps, STR("_djPlayerState"));
        *avatar = *venue = 0xff;
        if (djps)
        {
            read_u8(djps, kDjAvatarReceived, avatar);
            read_u8(djps, kDjVenueReceived, venue);
        }
        return djps;
    }

    auto player_line(UObject* ps) -> StringType
    {
        uint8 avatar = 0, venue = 0;
        UObject* djps = dj_flags(ps, &avatar, &venue);
        return std::format(STR("{}: hypeUserId {}, team {}, djIndex {}, ready 0x{:x}, leader {}, name len {}, platform {}, DJPlayerState {} (avatar {}, venue {})"),
                           ps->GetName(), prop<int32>(ps, STR("_hypeUserId")), team_name(prop<uint8>(ps, STR("_team"))), prop<int32>(ps, STR("_djIndex")),
                           prop<uint8>(ps, STR("_isReadyCheck")), prop<bool>(ps, STR("_isSessionLeader")) ? 1 : 0, name_len(ps), prop<uint8>(ps, STR("_platform")),
                           djps ? STR("yes") : STR("none"), avatar == 0xff ? -1 : avatar, venue == 0xff ? -1 : venue);
    }

    auto widget_visibility(UObject* w) -> int
    {
        Call c(STR("/Script/UMG.Widget:GetVisibility"));
        if (!c.ok() || !w) return -1;
        c.call(w);
        return c.get<uint8>(STR("ReturnValue"));
    }
    auto switcher_index(UObject* w) -> int32
    {
        Call c(STR("/Script/UMG.WidgetSwitcher:GetActiveWidgetIndex"));
        if (!c.ok() || !w) return -1;
        c.call(w);
        return c.get<int32>(STR("ReturnValue"));
    }
    auto name_of(UObject* o) -> StringType { return o ? o->GetName() : StringType{STR("null")}; }

    // Shell screens are created by the game instance (no owning player): require that, or a World, as an outer - the
    // screen functions need a world context; a class's template has neither.
    auto in_game(UObject* w, UObject* gi) -> bool
    {
        for (UObject* x = w ? w->GetOuterPrivate() : nullptr; x; x = x->GetOuterPrivate())
            if (x == gi || (x->GetClassPrivate() && x->GetClassPrivate()->GetName() == STR("World"))) return true;
        return false;
    }

    // The report's view of each WBP_Screen_MultiplayerLobby: its local-player part (DoUpdateLocalPlayerRelatedElements:
    // the ready button vs "waiting", the DJ / viewers label) needs _CachedLocalPS, set by its WaitForLocalHypePlayerState.
    auto report_lobby_widgets() -> void
    {
        UObject* gi = game_instance();
        UObject* me = host_player_state(gi);
        Call local(STR("/Script/Hype.HypePlayerState:IsLocallyControlled"));
        if (me && local.ok()) local.call(me);
        const bool me_local = me && local.ok() && local.get<bool>(STR("ReturnValue"));
        for (UObject* w : live_objects(STR("WBP_Screen_MultiplayerLobby_C")))
        {
            if (!in_game(w, gi)) continue;
            UObject* ready = prop<UObject*>(w, STR("ToggleReadyButton"));
            UObject* ready_ps = ready ? prop<UObject*>(ready, STR("PlayerState")) : nullptr;
            Output::send<LogLevel::Normal>(STR("[host] lobby: {} _CachedLocalPS {}, _CachedHypeGameState {}; ready switcher {} (0 button / 1 waiting), ready button PS {} "
                                               "visibility {}; local PS {} locally controlled {}\n"),
                                           w->GetName(), name_of(prop<UObject*>(w, STR("_CachedLocalPS"))), name_of(prop<UObject*>(w, STR("_CachedHypeGameState"))),
                                           switcher_index(prop<UObject*>(w, STR("ReadyButtonStateSwitcher"))), name_of(ready_ps), widget_visibility(ready), name_of(me),
                                           me_local ? 1 : 0);
        }
    }

    auto shell_flow_mgr(UObject* ctx) -> UObject*
    {
        Call get(STR("/Script/Hype.ShellFlowMgr:GetShellFlowMgr"));
        if (!get.ok() || !ctx) return nullptr;
        get.set<UObject*>(STR("contextObj"), ctx);
        get.call(cdo(STR("/Script/Hype.Default__ShellFlowMgr")));
        return get.get<UObject*>(STR("ReturnValue"));
    }
#endif

    std::atomic<DWORD> g_game_thread{0}; // set by the engine tick callback

    // A native pass-through detour's original: integer / pointer arguments only, so a generic 9 x uint64 signature is
    // exact on x64.
    using Native9 = uint64 (*)(uint64, uint64, uint64, uint64, uint64, uint64, uint64, uint64, uint64);

    // ---- trace hooks: debug builds only (FNL_DEBUG) ------------------------------------------------------------------
    // The hooks from here to the end of the leave trace only log; a release build never prepares or installs them.
#if FNL_DEBUG
    // Lobby-related server RPCs this machine sends (an RPC runs through ProcessEvent on the caller too), logged by name.
    std::vector<UFunction*> g_watched_rpcs{};

    // A trace window (Ctrl+Shift+T): every distinct function run through ProcessEvent for a few seconds.
    uint64 g_trace_until = 0;
    std::vector<UFunction*> g_traced{};

    // Logging pass-through detours on the native drop chain (verified builds): a play can reach the song item (pip
    // drop animation) but never a spawn or ServerSpawnDisc, and native calls are invisible to a ProcessEvent trace.
    struct DropHook
    {
        uintptr_t VerifiedBuild::*field;
        const CharType* what;
        Native9 original;
        int logged;
    };
    DropHook g_drop_hooks[] = {
        {&VerifiedBuild::drop_song_item_play, STR("SongListItemGameplayWidget play (FUN_1407b34d0)"), nullptr, 0},
        {&VerifiedBuild::drop_desk_spawn_disc, STR("DeskState spawn disc (FUN_1405fdbd0)"), nullptr, 0},
        {&VerifiedBuild::drop_local_disc_spawn, STR("local disc spawn (FUN_1406109f0)"), nullptr, 0},
        {&VerifiedBuild::drop_send_spawn_disc, STR("performer send ServerSpawnDisc (FUN_14068af70)"), nullptr, 0},
    };
    // Deck trace (DeckTrace=1, default on): for 10 s after each local disc spawn, every ADeck disc event is logged
    // with the deck's playing / queued discs, the multiplayer stage and the timeline tick - to see why a stem queued at
    // a turn's start can stay queued (2026-10-03: a BT queued 0.75 s into Playing never dropped).
    std::atomic<uint64> g_deck_trace_until{0};

    template <int I>
    auto drop_detour(uint64 a, uint64 b, uint64 c, uint64 d, uint64 e, uint64 f, uint64 g, uint64 h, uint64 i) -> uint64
    {
        DropHook& k = g_drop_hooks[I];
        if (I == 2) g_deck_trace_until.store(GetTickCount64() + 10000);
        StringType extra{};
        if (I == 0) // the song item: its input flag, disc controller and desk state
        {
            uint8 enabled = 0xff;
            void *disc = nullptr, *desk = nullptr;
            read_u8(reinterpret_cast<void*>(a), 0x3a0, &enabled);
            read_ptr(reinterpret_cast<void*>(a), 0x390, &disc);
            read_ptr(reinterpret_cast<void*>(a), 0x398, &desk);
            // What the item's own NativeOnInitialized would bind now (local PC +0x608, HypeGameState +0x298): a
            // mismatch is a link left over from an earlier world.
            void* disc_now = nullptr;
            void* desk_now = nullptr;
            if (verified_build())
            {
                using Getter = void* (*)(void*);
                disc_now = reinterpret_cast<Getter>(at(&VerifiedBuild::deck_disc_of))(reinterpret_cast<void*>(a));
                desk_now = reinterpret_cast<Getter>(at(&VerifiedBuild::deck_desk_of))(reinterpret_cast<void*>(a));
            }
            extra = std::format(STR(" item {} input {} _discController {}{} _deskState {}{} p3 {} p4 {}"), reinterpret_cast<void*>(a), static_cast<int>(enabled), disc,
                                disc == disc_now ? STR("") : std::format(STR(" (STALE: now {})"), disc_now), desk,
                                desk == desk_now ? STR("") : std::format(STR(" (STALE: now {})"), desk_now), static_cast<int>(c & 0xff), static_cast<int>(d & 0xff));
        }
        else if (I == 1 || I == 2)
            extra = std::format(STR(" desk {} cel/transform {} action {} trackType {}"), reinterpret_cast<void*>(a), reinterpret_cast<void*>(b), static_cast<int>((I == 1 ? c : d) & 0xff),
                                static_cast<int>((I == 1 ? d : e) & 0xff));
        const uint64 r = k.original(a, b, c, d, e, f, g, h, i);
        if (k.logged++ < 40) Output::send<LogLevel::Normal>(STR("[host] drop: {}{} -> {}\n"), k.what, extra, I == 2 ? std::format(STR("{}"), r & 0xff) : StringType{STR("returned")});
        return r;
    }
    auto clock_ticks(void* ctx, int32* out) -> bool; // defined with log_clock

    // EActionAfterDiscSpawn (the drop hooks' "action"): Grab 0, Play 1, Queue 2. EDiscMovementState: Grabbed 0,
    // Playing 1, Queued 2, Warp 3, Init 4, Exploding 5.
    auto deck_event(UObject* self, UFunction* fn, void* parms) -> void
    {
        static const bool on = ini_value(STR("DeckTrace"), STR("1")) == STR("1");
        if (!on || !fn || !self || GetCurrentThreadId() != g_game_thread) return; // every deck, local and remote DJs'
        static const FName names[] = {FName(STR("OnDiscQueued"), FNAME_Add), FName(STR("OnDiscPlayed"), FNAME_Add), FName(STR("OnDiscUnqueued"), FNAME_Add),
                                      FName(STR("OnDiscEjected"), FNAME_Add), FName(STR("OnDiscDoomed"), FNAME_Add), FName(STR("OnDiscReleased"), FNAME_Add)};
        const FName n = fn->GetFName();
        int which = -1;
        for (int i = 0; i < static_cast<int>(std::size(names)); ++i)
            if (n == names[i]) which = i;
        if (which < 0) return;
        thread_local bool busy = false;
        if (busy) return;
        busy = true;
        const auto cel = [](UObject* disc) -> StringType {
            if (!disc) return STR("-");
            auto* name = disc->GetValuePtrByPropertyNameInChain<FName>(STR("celShortName"));
            auto* state = disc->GetValuePtrByPropertyNameInChain<uint8>(STR("MovementState"));
            return std::format(STR("{}[{}]"), name ? name->ToString() : STR("?"), state ? static_cast<int>(*state) : -1);
        };
        const auto* parm = static_cast<const uint8*>(parms);
        StringType args;
        if (parm && (which == 0 || which == 1)) // (ADisc*, bool replaced, bool isHotSwap, bool isFromGrab / isManualDrop)
            args = std::format(STR(" {} replaced {} hotswap {} {} {}"), cel(*reinterpret_cast<UObject* const*>(parm)), parm[8], parm[9],
                               which == 0 ? STR("fromGrab") : STR("manual"), parm[10]);
        else if (parm && which == 2) args = std::format(STR(" fromHotswap {}"), parm[0]);
        else if (parm && which == 3) args = std::format(STR(" grab {} vfx {}"), parm[0], parm[1]);
        else if (parm && which == 4) args = std::format(STR(" reason {}"), parm[0]);
        auto* track = self->GetValuePtrByPropertyNameInChain<uint8>(STR("track"));
        UObject* mps = mp_state(game_instance());
        int stage = -1;
        int32 t[3] = {-1, -1, -1};
        if (mps)
        {
            Call c(STR("/Script/Hype.MultiplayerGameplayState:GetMultiplayerGameplayStage"));
            if (c.call(mps)) stage = c.get<uint8>(STR("ReturnValue"));
            clock_ticks(mps, t);
        }
        Output::send<LogLevel::Normal>(STR("[host] deck: {} track {} {}{}; playing {} queued {}; stage {} tick {}\n"), self->GetName(), track ? static_cast<int>(*track) : -1,
                                       n.ToString(), args, cel(prop<UObject*>(self, STR("CurPlayingDisc"))), cel(prop<UObject*>(self, STR("CurQueuedDisc"))),
                                       stage < 0 ? STR("-") : mp_stage_name(static_cast<uint8>(stage)), t[0]);
        busy = false;
    }

    // Audio side of the deck trace: in the same window, every 250 ms, the AudioTrackMgr's view of tracks 0-3 (the cel
    // set on the track, whether anything is playing on it, muted, gain), logged when a track's line changes. A disc
    // can be Playing on its deck while its audio track is silent (2026-10-03: BT after the turn's queue drop; queueing
    // on that deck again brought it in).
    auto audio_trace() -> void
    {
        static const bool on = ini_value(STR("DeckTrace"), STR("1")) == STR("1");
        static uint64 last = 0;
        static StringType seen[4];
        const uint64 now = GetTickCount64();
        // the throttle advances before anything is looked up: outside a session this is a 250 ms check, not per frame
        if (!on || now - last < 250) return; // logs only changes
        last = now;
        UObject* gi = game_instance();
        UObject* mps = mp_state(gi);
        if (!mps) return;
        UObject* audio = prop<UObject*>(gi, STR("theHypeAudio"));
        Call get_mgr(STR("/Script/Hype.HypeAudio:GetAudioTrackMgr"));
        UObject* mgr = audio && get_mgr.call(audio) ? get_mgr.get<UObject*>(STR("ReturnValue")) : nullptr;
        if (!mgr) return;
        int32 t[3] = {-1, -1, -1};
        if (mps) clock_ticks(mps, t);
        for (uint8 track = 0; track < 4; ++track)
        {
            Call cel(STR("/Script/Hype.AudioTrackMgr:GetTrackCelShortname"));
            Call playing(STR("/Script/Hype.AudioTrackMgr:IsAnythingPlayingOnTrack"));
            Call muted(STR("/Script/Hype.AudioTrackMgr:GetTrackIsMuted"));
            Call gain(STR("/Script/Hype.AudioTrackMgr:GetGainForTrack"));
            for (Call* c : {&cel, &playing, &muted, &gain}) c->set<uint8>(STR("trackType"), track).call(mgr);
            const StringType line = std::format(STR("cel {} playing {} muted {} gain {:.2f}"), cel.get<FName>(STR("ReturnValue")).ToString(),
                                                static_cast<int>(playing.get<bool>(STR("ReturnValue"))), static_cast<int>(muted.get<bool>(STR("ReturnValue"))),
                                                gain.get<float>(STR("ReturnValue")));
            if (line == seen[track]) continue;
            seen[track] = line;
            Output::send<LogLevel::Normal>(STR("[host] audio: track {} {}; tick {}\n"), track, line, t[0]);
        }
    }

    void* const g_drop_detours[] = {reinterpret_cast<void*>(&drop_detour<0>), reinterpret_cast<void*>(&drop_detour<1>),
                                    reinterpret_cast<void*>(&drop_detour<2>), reinterpret_cast<void*>(&drop_detour<3>)};
    static_assert(std::size(g_drop_detours) == std::size(g_drop_hooks));

    auto install_drop_hooks() -> void
    {
        static bool done = false;
        if (done || !verified_build()) return;
        done = true;
        Hook::RegisterProcessEventPostCallback([](Hook::TCallbackIterationData<void>&, UObject* self, UFunction* fn, void* parms) { deck_event(self, fn, parms); });
        for (size_t n = 0; n < std::size(g_drop_hooks); ++n)
        {
            const auto [prep, inst] = install_hook(at(g_drop_hooks[n].field), g_drop_detours[n], reinterpret_cast<void**>(&g_drop_hooks[n].original), g_drop_hooks[n].what);
            Output::send<LogLevel::Normal>(STR("[host] drop: hook {} - prepare {} install {}\n"), g_drop_hooks[n].what, prep, inst);
        }
    }

    // Leave trace (LeaveTrace=1; off by default). Every path that takes a player out of a venue is logged, with the exe
    // frames of the native call stack: the stock kick helper and its reason, ClientWasKicked, the menu returns, client
    // travel, the game instance's network / travel errors, the DJs' AFK reports, VenueTravelMgr travel and the venue's
    // end. (It found why the old sidecar left its own venue 66 s into an idle Lobby, 2026-10-03: the intro cinematic's
    // end.) The stock idle timeouts are long: EmptyLobbySecondsTillShutdown 600 s; afk_dj 600 / 900 / 1200 s by turn
    // length.
    auto leave_trace_on() -> bool
    {
        static const bool on = ini_value(STR("LeaveTrace"), STR("0")) == STR("1");
        return on;
    }
    auto exe_stack(DWORD skip) -> StringType
    {
        void* frames[24]{};
        const USHORT n = CaptureStackBackTrace(skip + 1, 24, frames, nullptr);
        const HMODULE exe = GetModuleHandleW(nullptr);
        StringType out{};
        for (USHORT i = 0; i < n; ++i)
        {
            HMODULE m = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(frames[i]), &m) && m == exe)
                out += std::format(STR(" 0x{:x}"), reinterpret_cast<uintptr_t>(frames[i]) - exe_base() + 0x140000000);
        }
        return out.empty() ? StringType{STR(" (no exe frames)")} : out;
    }

    // The game thread's recent ProcessEvent calls (function, object class), skipping a function seen in the last 16
    // entries so per-frame events don't flood it. A leave can come from a Blueprint event (delegate broadcast -> BP ->
    // native -> VenueTravelMgr travel) that the native stack can't name; the ring's tail at the travel can.
    struct PeEntry
    {
        UFunction* fn;
        UClass* cls;
        uint64 ms;
    };
    constexpr size_t kPeRing = 512;
    PeEntry g_pe_ring[kPeRing]{};
    size_t g_pe_next = 0;
    auto pe_record(UObject* self, UFunction* fn) -> void
    {
        for (size_t k = 1; k <= 16; ++k)
            if (g_pe_ring[(g_pe_next + kPeRing - k) % kPeRing].fn == fn) return;
        g_pe_ring[g_pe_next % kPeRing] = {fn, self ? self->GetClassPrivate() : nullptr, GetTickCount64()};
        ++g_pe_next;
    }
    auto pe_dump(const CharType* why, size_t count) -> void
    {
        const size_t n = std::min(count, std::min(g_pe_next, kPeRing));
        const uint64 now = GetTickCount64();
        StringType out{};
        for (size_t k = n; k >= 1; --k)
        {
            const PeEntry& e = g_pe_ring[(g_pe_next - k) % kPeRing];
            out += std::format(STR("\n    -{} ms {} on {}"), now - e.ms, e.fn ? e.fn->GetFullName() : StringType{STR("?")},
                               e.cls ? e.cls->GetName() : StringType{STR("?")});
        }
        Output::send<LogLevel::Normal>(STR("[host] leave: last {} distinct ProcessEvent calls before {} (oldest first):{}\n"), n, why, out);
    }

    Native9 g_venue_travel_original = nullptr;
    auto venue_travel_detour(uint64 a, uint64 b, uint64 c, uint64 d, uint64 e, uint64 f, uint64 g, uint64 h, uint64 i) -> uint64
    {
        if (leave_trace_on() && GetCurrentThreadId() == g_game_thread)
        {
            Output::send<LogLevel::Normal>(STR("[host] leave: VenueTravelMgr travel (FUN_1407b2de0); stack{}\n"), exe_stack(0));
            pe_dump(STR("the VenueTravelMgr travel"), 60);
        }
        return g_venue_travel_original(a, b, c, d, e, f, g, h, i);
    }
    auto install_venue_travel_hook() -> void
    {
        static bool installed = false;
        if (installed || !verified_build()) return;
        installed = true;
        const auto [prep, inst] = install_hook(at(&VerifiedBuild::venue_travel), reinterpret_cast<void*>(&venue_travel_detour), // FUN_1407b2de0 (VenueTravelMgr travel)
                                               reinterpret_cast<void**>(&g_venue_travel_original), STR("venue travel"));
        Output::send<LogLevel::Normal>(STR("[host] leave trace: venue travel hook - prepare {} install {}\n"), prep, inst);
    }
    // The end of a venue: HypeGameStateVenue's EndPlay (FUN_14068bca0), which is the plain HypeGameState's EndPlay
    // too (a TransitionMap's). Only a multiplayer venue's end - a game state with a MultiplayerGameplayState - is
    // logged; read before the original runs.
    Native9 g_venue_endplay_original = nullptr;
    auto venue_endplay_detour(uint64 a, uint64 b, uint64 c, uint64 d, uint64 e, uint64 f, uint64 g, uint64 h, uint64 i) -> uint64
    {
        auto* gs = reinterpret_cast<UObject*>(a);
        auto** mps = gs && is_live(gs) ? gs->GetValuePtrByPropertyNameInChain<UObject*>(STR("MultiplayerGameplayState")) : nullptr;
        if (mps && *mps) Output::send<LogLevel::Normal>(STR("[host] leave: multiplayer venue end-play; stack{}\n"), exe_stack(0));
        return g_venue_endplay_original(a, b, c, d, e, f, g, h, i);
    }
    auto install_endplay_hook() -> void
    {
        static bool installed = false;
        if (installed || !verified_build()) return;
        installed = true;
        const auto [prep, inst] = install_hook(at(&VerifiedBuild::venue_end_play), reinterpret_cast<void*>(&venue_endplay_detour),
                                               reinterpret_cast<void**>(&g_venue_endplay_original), STR("venue end-play"));
        Output::send<LogLevel::Normal>(STR("[host] leave trace: venue end-play hook - prepare {} install {}\n"), prep, inst);
    }

    // The leave trace's other half (see leave_trace_on). FUN_140695990 (player state, FString reason) is the stock
    // kick: GameSession KickPlayer on the state's controller. Its callers: the empty-lobby timer (FUN_1407207f0,
    // "afk_dj"), the server's kick-everyone state (FUN_14071bc80), the crossplay and block-list checks, battle joins
    // and the join refusals. The AFK RPC's own kick (FUN_140696b40) skips it; ClientWasKicked shows that one.
    Native9 g_stock_kick_original = nullptr;
    auto stock_kick_detour(uint64 a, uint64 b, uint64 c, uint64 d, uint64 e, uint64 f, uint64 g, uint64 h, uint64 i) -> uint64
    {
        wchar_t reason[96]{};
        if (b) read_fstring(reinterpret_cast<const void*>(b), 0, reason, 96);
        auto* ps = reinterpret_cast<UObject*>(a);
        Output::send<LogLevel::Normal>(STR("[host] leave: stock kick of {} reason '{}'; stack{}\n"), ps && is_live(ps) ? player_line(ps) : StringType{STR("?")},
                                       StringType(reason), exe_stack(0));
        return g_stock_kick_original(a, b, c, d, e, f, g, h, i);
    }
    struct LeaveFn
    {
        const CharType* path;
        UFunction* fn;
    };
    LeaveFn g_leave_fns[] = {{STR("/Script/Engine.PlayerController:ClientWasKicked"), nullptr},
                             {STR("/Script/Engine.PlayerController:ClientReturnToMainMenu"), nullptr},
                             {STR("/Script/Engine.PlayerController:ClientReturnToMainMenuWithTextReason"), nullptr},
                             {STR("/Script/Engine.PlayerController:ClientTravelInternal"), nullptr},
                             {STR("/Script/Engine.GameInstance:HandleNetworkError"), nullptr},
                             {STR("/Script/Engine.GameInstance:HandleTravelError"), nullptr},
                             {STR("/Script/Hype.HypePlayerController:ServerUpdatePlayerAfkSeconds"), nullptr},
                             {STR("/Script/Hype.HypePlayerController:_OnDisconnectPopupCreated"), nullptr},
                             {STR("/Script/Hype.PauseSubsystem:_OnPopupCreatedNetworkDisconnect"), nullptr},
                             {STR("/Script/Hype.PopupManager:_OnNetworkDisconnectPopupCreated"), nullptr}};
    auto leave_event(UObject* self, UFunction* fn, void* parms) -> void
    {
        int which = -1;
        for (int k = 0; k < static_cast<int>(std::size(g_leave_fns)); ++k)
            if (g_leave_fns[k].fn == fn) which = k;
        if (which < 0) return;
        const auto* p = static_cast<const uint8*>(parms);
        StringType args{};
        if (which == 6) // ServerUpdatePlayerAfkSeconds(int32): about every second per idle DJ, so only some values
        {
            const int32 secs = p ? *reinterpret_cast<const int32*>(p) : -1;
            if (secs != 1 && secs % 15 != 0) return;
            args = std::format(STR(" afkSeconds {}"), secs);
        }
        else if (p && (which == 1 || which == 3)) // (FString ReturnReason) / (FString URL, TravelType, bSeamless, ...)
        {
            wchar_t s[160]{};
            read_fstring(p, 0, s, 160);
            args = which == 1 ? std::format(STR(" reason '{}'"), s) : std::format(STR(" url '{}' type {} seamless {}"), s, p[0x10], p[0x11]);
        }
        else if (p && which == 4) // (ENetworkFailure FailureType, bool bIsServer)
        {
            static const CharType* const failures[] = {STR("NetDriverAlreadyExists"), STR("NetDriverCreateFailure"), STR("NetDriverListenFailure"), STR("ConnectionLost"),
                                                       STR("ConnectionTimeout"), STR("FailureReceived"), STR("OutdatedClient"), STR("OutdatedServer"),
                                                       STR("PendingConnectionFailure"), STR("NetGuidMismatch"), STR("NetChecksumMismatch")};
            args = std::format(STR(" failure {} ({}) isServer {}"), p[0], p[0] < std::size(failures) ? failures[p[0]] : STR("?"), p[1]);
        }
        else if (p && which == 5)
            args = std::format(STR(" failure {}"), p[0]);
        StringType who = self ? self->GetName() : StringType{STR("?")};
        if (self && which == 6)
            if (auto** ps = self->GetValuePtrByPropertyNameInChain<UObject*>(STR("PlayerState")); ps && *ps) who = player_line(*ps);
        Output::send<LogLevel::Normal>(STR("[host] leave: {} on {}{}{}\n"), fn->GetName(), who, args, which == 6 ? StringType{} : STR("; stack") + exe_stack(0));
    }
    auto install_leave_trace() -> void
    {
        static bool done = false;
        if (done || !leave_trace_on() || !verified_build()) return;
        done = true;
        install_venue_travel_hook();
        install_endplay_hook();
        const auto [prep, inst] = install_hook(at(&VerifiedBuild::stock_kick), reinterpret_cast<void*>(&stock_kick_detour),
                                               reinterpret_cast<void**>(&g_stock_kick_original), STR("stock kick"));
        StringType missing{};
        for (auto& l : g_leave_fns)
            if (!(l.fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, l.path))) missing += StringType(STR(" ")) + l.path;
        Hook::RegisterProcessEventPreCallback([](Hook::TCallbackIterationData<void>&, UObject* self, UFunction* fn, void* parms) {
            if (!fn || GetCurrentThreadId() != g_game_thread) return;
            pe_record(self, fn);
            leave_event(self, fn, parms);
        });
        Output::send<LogLevel::Normal>(STR("[host] leave trace: stock kick hook - prepare {} install {}; functions missing:{}\n"), prep, inst,
                                       missing.empty() ? StringType{STR(" none")} : missing);
    }
#endif // FNL_DEBUG: trace hooks

    // ---- the game session id ---------------------------------------------------------------------------------------
    // Each player state's replicated _gameSessionId (+0x1050) is the server's id. A Development server's own id
    // ("local-dev-gamesession-<GUID>") is unknown to fuserd; song transfers then use the matchmaking join's id.
    constexpr std::wstring_view kDevSessionPrefix = L"local-dev-gamesession-";
    constexpr size_t kPsGameSessionId = 0x1050;

    // ---- the matchmaking join ---------------------------------------------------------------------------------------
    // Create Private Session, a public match or a session browser join ends, on every player, in UMatchmakingMgr's
    // join (FUN_140706890): after the multiplayer loading travel it builds ?playerSessionId=?gameSessionId=?hypeUserId=
    // ?buildConfig= and Browses to ip_address:Port. A P2P joiner waits for the tunnel first (drive_p2p_join);
    // JoinAddressOverride=<ip:port> sends a joiner there instead of the address fuserd gave (two machines behind one
    // NAT, where the public address only works with hairpin NAT). A player session id starting "psess-host-" (MM+0x90)
    // means fuserd made this player the session's host: see "player-hosted sessions".
    constexpr size_t kMmGameSession = 0x80, kMmPlayerSession = 0x90;
    constexpr uintptr_t kBackendUserId = 0x1b8;
    Native9 g_mm_join_original = nullptr;

    // ---- player-hosted sessions (stub) ------------------------------------------------------------------------------
    // fuserd makes a session's creating player its host when it runs no managed (FDS) servers. The plan: when an FDS
    // server executable is installed and configured on this machine, launch it for the session (its venue, time of day
    // and set length from the matchmaking data), join it like any FDS, and run the P2P tunnel's host end towards its
    // game and beacon ports (start_p2p(true, ...)) so players behind NAT reach it. Not written yet: this returns false,
    // and the caller lets the stock join run - to this player's own advertised address, which fails with the game's
    // own connection error rather than hanging.
    auto start_player_hosted_fds(uint64 mm, const StringType& gsid) -> bool
    {
        (void)mm;
        Output::send<LogLevel::Warning>(STR("[host] matchmaking: fuserd made this player the host of session '{}'; player-hosted sessions need a local FDS server, "
                                            "which is not supported yet - joining as stock (expect a connection error)\n"),
                                        gsid);
        return false;
    }

    auto mm_set_state(uint64 mm, uint8 state) -> void
    {
        using SetState = void (*)(uint64, uint8);
        reinterpret_cast<SetState>(at(&VerifiedBuild::mm_set_state))(mm, state);
    }

    auto own_user_id(UObject* gi) -> int32
    {
        void* backend = nullptr;
        int32 id = -1;
        auto** slot = gi ? gi->GetValuePtrByPropertyNameInChain<UObject*>(STR("theBackendServerMgr")) : nullptr;
        if (slot && *slot) backend = *slot;
        if (backend) read_i32(backend, kBackendUserId, &id);
        return id;
    }

    auto start_p2p(bool host, const StringType& gsid) -> bool; // P2P signaling, below

    // A joiner's matchmaking join waits for the tunnel, then opens its local end (drive_p2p_join). If the tunnel
    // finds no path within P2PJoinTimeoutMs, the stock join runs as it would have (JoinAddressOverride if set).
    struct PendingP2PJoin
    {
        bool active = false;
        uint64 args[9]{};
        StringType url_options;
        uint64 started = 0, deadline = 0, generation = 0;
    };
    PendingP2PJoin g_p2p_join{};
    std::atomic<uint64_t> g_p2p_join_failed{0}; // the generation of a joiner worker that gave up
    std::atomic<uint64_t> g_p2p_generation{0}; // bumped by each start_p2p (P2P signaling, below)

    // The fuserd game session id of this player's last matchmaking join: song transfers use it when the
    // replicated _gameSessionId is a Development server's local-dev-... id.
    StringType g_mm_joined_session{};

    auto mm_join_detour(uint64 a, uint64 b, uint64 c, uint64 d, uint64 e, uint64 f, uint64 g, uint64 h, uint64 i) -> uint64
    {
        wchar_t psid[128]{}, gsid[128]{};
        read_fstring(reinterpret_cast<void*>(a), kMmPlayerSession, psid, 128);
        read_fstring(reinterpret_cast<void*>(a), kMmGameSession, gsid, 128);
        g_mm_joined_session = gsid;
        const bool host = std::wstring_view(psid).starts_with(L"psess-host-");
        Output::send<LogLevel::Normal>(STR("[host] matchmaking: join of game session '{}' ({})\n"), gsid, host ? STR("we are its host") : STR("joining"));
        g_armed = true; // the state watch (DiagnosticLogs)
        if (host)
        {
            if (start_player_hosted_fds(a, gsid)) return 0;
            return g_mm_join_original(a, b, c, d, e, f, g, h, i);
        }
        const bool p2p = start_p2p(false, gsid); // P2P=1 only
        if (p2p)
        {
            const uint64 now = GetTickCount64();
            g_p2p_join = PendingP2PJoin{true, {a, b, c, d, e, f, g, h, i},
                                        std::format(STR("?playerSessionId={}?gameSessionId={}?hypeUserId={}?buildConfig=Shipping"), psid, gsid,
                                                    own_user_id(game_instance())),
                                        now, now + static_cast<uint64>(std::max(1000, _wtoi(ini_value(STR("P2PJoinTimeoutMs"), STR("10000")).c_str()))),
                                        g_p2p_generation.load()};
            Output::send<LogLevel::Normal>(STR("[p2p] join: waiting for the tunnel before joining (up to {} ms)\n"), g_p2p_join.deadline - now);
            return 0;
        }
        const StringType over = ini_value(STR("JoinAddressOverride"), STR(""));
        if (!over.empty())
        {
            mm_set_state(a, 0);
            open_url(std::format(STR("{}?playerSessionId={}?gameSessionId={}?hypeUserId={}?buildConfig=Shipping"), over, psid, gsid, own_user_id(game_instance())));
            return 0;
        }
        return g_mm_join_original(a, b, c, d, e, f, g, h, i);
    }

    // ---- stems this machine does not have ----------------------------------------------------------------------
    // A remote DJ's SpawnDisc whose cel this machine cannot resolve (FUN_14062a920: FUN_140591ea0 finds it neither in
    // CelMgr nor among the game state's crafted instruments) stalls that DJ's whole queue. The consumer
    // (FUN_14069bc90) sends ServerRequestCraftedInstrumentCel once (FUN_14062bcd0) and then returns at that item on
    // every tick. The request exists for crafted instruments: the server answers only from its crafted registry
    // (FUN_14069f770), so a song this player lacks - unowned DLC, a custom song not installed - never arrives, and
    // nothing else that DJ does plays here. A late joiner is stuck the same way: its sync (FUN_140699ac0) waits while
    // FUN_140621700 finds a missing cel in the snapshot.
    // After MissingCelGraceMs (default 1500; a crafted instrument arrives within that), the stuck SpawnDisc is marked
    // completed (+0xe8) so the queue moves on, and a snapshot item is turned into an Invalid action locally (type 0:
    // the applier FUN_140626920 and the readiness check both skip it). A cel found missing once is skipped at once
    // from then on. The stem is simply not heard here; nothing is spawned without its cel. MissingCelSkip=0 turns
    // it off.
    // While a store install is running (another player's crate song being fetched from fuserd), the
    // skip waits for it, up to StoreInstallWaitMs (default 30000) from when the cel was first missing: once the song
    // is registered the cel resolves, and the stuck SpawnDisc (or snapshot item) plays as it would have.
    auto song_installs_active() -> bool; // the store section, below
    using CelMissingFn = uint8 (*)(uint64, uint64);
    CelMissingFn g_cel_missing_original = nullptr;
    CelMissingFn g_snapshot_missing_original = nullptr;
    std::unordered_map<uint64, uint64> g_cel_missing_since{}; // cel FName (as 8 bytes) -> first seen missing
    std::unordered_set<uint64> g_cels_skipped{}, g_cels_waiting{};
    auto missing_cel_skip_enabled() -> bool
    {
        static const bool on = ini_value(STR("MissingCelSkip"), STR("1")) == STR("1");
        return on;
    }
    // Whether a missing cel has been missing long enough to skip (and remember it as skipped).
    auto give_up_on_cel(const uint8* item, const CharType* where) -> bool
    {
        uint64 cel = 0;
        std::memcpy(&cel, item + 0x18, 8);
        if (g_cels_skipped.contains(cel)) return true;
        static const uint64 grace = static_cast<uint64>(std::max(0, _wtoi(ini_value(STR("MissingCelGraceMs"), STR("1500")).c_str())));
        const uint64 now = GetTickCount64();
        const auto [since, fresh] = g_cel_missing_since.try_emplace(cel, now);
        if (now - since->second < grace) return false;
        static const uint64 store_wait = static_cast<uint64>(std::max(0, _wtoi(ini_value(STR("StoreInstallWaitMs"), STR("30000")).c_str())));
        if (now - since->second < store_wait && song_installs_active())
        {
            if (g_cels_waiting.insert(cel).second)
                Output::send<LogLevel::Normal>(STR("[host] missing cel: '{}' is not installed here; waiting for the store install ({}, track {})\n"),
                                               reinterpret_cast<const FName*>(item + 0x18)->ToString(), where, static_cast<int>(item[0x20]));
            return false;
        }
        g_cels_skipped.insert(cel);
        Output::send<LogLevel::Warning>(STR("[host] missing cel: '{}' is not installed here; skipping it ({}, track {})\n"),
                                        reinterpret_cast<const FName*>(item + 0x18)->ToString(), where, static_cast<int>(item[0x20]));
        return true;
    }
    auto cel_missing_detour(uint64 item, uint64 ps) -> uint8
    {
        const uint8 missing = g_cel_missing_original(item, ps);
        auto* it = reinterpret_cast<uint8*>(item);
        if (!missing && !g_cels_waiting.empty())
        {
            uint64 cel = 0;
            std::memcpy(&cel, it + 0x18, 8);
            if (g_cels_waiting.erase(cel))
                Output::send<LogLevel::Normal>(STR("[host] missing cel: '{}' resolves now; playing it ({} ms after it was first missing)\n"),
                                               reinterpret_cast<const FName*>(it + 0x18)->ToString(), GetTickCount64() - g_cel_missing_since[cel]);
        }
        if (!missing || !missing_cel_skip_enabled()) return missing;
        if (give_up_on_cel(it, STR("action queue"))) it[0xe8] = 1; // completed: the consumer skips it from the next tick
        return missing; // this tick the consumer still returns at it, without executing it
    }
    auto snapshot_missing_detour(uint64 category, uint64 ps) -> uint8
    {
        if (missing_cel_skip_enabled() && g_cel_missing_original)
        {
            auto* arr = reinterpret_cast<uint8*>(category);
            uint8* data = *reinterpret_cast<uint8**>(arr + 0x108);
            const int32 num = *reinterpret_cast<int32*>(arr + 0x110);
            for (int32 i = 0; data && i < num && i < 4096; ++i)
            {
                uint8* item = data + static_cast<size_t>(i) * 0x100; // FMultiplayerActionItemEntry
                if (item[0xc] == 1 && g_cel_missing_original(reinterpret_cast<uint64>(item), ps) && give_up_on_cel(item, STR("state of the world")))
                    item[0xc] = 0; // Invalid: not applied, and no longer holds up the sync
            }
        }
        return g_snapshot_missing_original(category, ps);
    }
    auto install_missing_cel_hooks() -> void
    {
        static bool installed = false;
        if (installed || !verified_build()) return;
        installed = true;
        const auto hook = [](uintptr_t address, void* detour, CelMissingFn* original, const CharType* what) {
            const auto [prep, inst] = install_hook(address, detour, reinterpret_cast<void**>(original), what);
            Output::send<LogLevel::Normal>(STR("[host] missing cel: {} hook - prepare {} install {}\n"), what, prep, inst);
        };
        hook(at(&VerifiedBuild::cel_missing_queue), reinterpret_cast<void*>(&cel_missing_detour), &g_cel_missing_original, STR("action queue (FUN_14062a920)"));
        hook(at(&VerifiedBuild::cel_missing_snapshot), reinterpret_cast<void*>(&snapshot_missing_detour), &g_snapshot_missing_original, STR("state-of-the-world sync (FUN_140621700)"));
    }

    auto install_mm_join_hook() -> void
    {
        static bool installed = false;
        if (installed || !verified_build()) return;
        installed = true;
        const auto [prep, inst] = install_hook(at(&VerifiedBuild::mm_join), reinterpret_cast<void*>(&mm_join_detour),
                                               reinterpret_cast<void**>(&g_mm_join_original), STR("matchmaking join"));
        Output::send<LogLevel::Normal>(STR("[host] matchmaking: join hook - prepare {} install {}\n"), prep, inst);
    }

#if FNL_DEBUG
    // From here to pressed(): the debug report (Ctrl+Shift+R), the trace (Ctrl+Shift+T) and the debug keys - debug
    // builds only.
    auto widget_shown(UObject* w) -> bool
    {
        if (!w || !is_live(w)) return false;
        Call in_viewport(STR("/Script/UMG.UserWidget:IsInViewport"));
        Call parent(STR("/Script/UMG.Widget:GetParent"));
        return (in_viewport.call(w) && in_viewport.get<bool>(STR("ReturnValue"))) || (parent.call(w) && parent.get<UObject*>(STR("ReturnValue")));
    }

    // The report's screen view: the current shell screen, the screen stack at ShellFlowMgr+0x1a0, its waiting-screen
    // widget and every top-level viewport widget. (The stock flow tears the waiting / synchronization screens down
    // when a join and the multiplayer synchronization finish.)
    auto report_screens() -> void
    {
        UObject* gi = game_instance();
        UObject* flow = shell_flow_mgr(gi);
        if (!flow)
        {
            Output::send<LogLevel::Normal>(STR("[host] screens: no ShellFlowMgr\n"));
            return;
        }
        Call current(STR("/Script/Hype.ShellFlowMgr:GetCurrentScreen"));
        const int cur = current.call(flow) ? current.get<uint8>(STR("ReturnValue")) : -1;
        StringType stack_list{};
        void* stack = nullptr;
        int32 n = 0;
        if (read_ptr(flow, 0x1a0, &stack) && read_i32(flow, 0x1a8, &n) && stack)
            for (int32 i = 0; i < n && i < 32; ++i)
            {
                uint8 k = 0;
                if (!read_u8(stack, static_cast<size_t>(i) * 2, &k)) break;
                stack_list += std::format(STR(" {}"), k);
            }
        auto** waiting_slot = flow->GetValuePtrByPropertyNameInChain<UObject*>(STR("_waitingScreenWidget"));
        UObject* waiting = waiting_slot ? *waiting_slot : nullptr;
        const bool waiting_shown = widget_shown(waiting);
        Output::send<LogLevel::Normal>(STR("[host] screens: current {}, stack [{} ], waiting screen {} ({})\n"), cur, stack_list,
                                       waiting ? waiting->GetName() : StringType{STR("none")}, waiting_shown ? STR("SHOWN") : STR("not shown"));

        Call all(STR("/Script/UMG.WidgetBlueprintLibrary:GetAllWidgetsOfClass"));
        auto* user_widget = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.UserWidget"));
        if (all.ok() && user_widget)
        {
            all.set<UObject*>(STR("WorldContextObject"), gi).set<UObject*>(STR("WidgetClass"), user_widget).set<bool>(STR("TopLevelOnly"), true);
            all.call(cdo(STR("/Script/UMG.Default__WidgetBlueprintLibrary")));
            if (auto* found = static_cast<TArray<UObject*>*>(all.raw(STR("FoundWidgets"))))
                for (int32 i = 0; i < found->Num(); ++i)
                {
                    UObject* w = (*found)[i];
                    if (!w || !is_live(w)) continue;
                    Call vis(STR("/Script/UMG.Widget:GetVisibility"));
                    const int v = vis.call(w) ? vis.get<uint8>(STR("ReturnValue")) : -1;
                    Output::send<LogLevel::Normal>(STR("[host] screens:   top-level {} ({}), visibility {}{}\n"), w->GetName(), w->GetClassPrivate()->GetName(), v,
                                                   widget_shown(w) ? STR("") : STR(", not in viewport"));
                }
        }
    }

    // Ctrl+Shift+T: the ProcessEvent trace window, and the lobby RPCs this machine sends, by name (with the deck
    // trace's drop hooks).
    auto install_rpc_hooks() -> void
    {
        install_drop_hooks();
        static bool done = false;
        if (done) return;
        done = true;
        static const CharType* const watched[] = {STR("ServerSetAsDJ"), STR("ServerSetAsSpectator"), STR("ServerSetIsReadyInLobby"), STR("ServerRequestMultiplayerTeamChange"),
                                                  STR("ServerMarkLobbyComplete"), STR("ServerSetCoopRoundNumTurnMeasures"), STR("ServerChangeLeaderTo"),
                                                  STR("ServerToggleAutoReadyCheck"), STR("ServerSetIsReadyInResults"), STR("ServerSetHypeUserId")};
        for (const CharType* n : watched)
        {
            if (auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, (StringType(STR("/Script/Hype.HypePlayerState:")) + n).c_str()))
                g_watched_rpcs.push_back(fn);
        }
        Hook::RegisterProcessEventPreCallback([](Hook::TCallbackIterationData<void>&, UObject* self, UFunction* fn, void*) {
            // ProcessEvent also runs on worker threads (e.g. animation Blueprint updates on TaskGraph threads):
            // everything below touches unsynchronised state, so only the game thread gets past here. Two workers and
            // the game thread growing g_traced at once corrupted the heap (crash, 2026-09-25 19:52).
            if (GetCurrentThreadId() != g_game_thread) return;
            if (!g_armed || !fn) return;
            if (g_trace_until && GetTickCount64() < g_trace_until && g_traced.size() < 1500 && std::find(g_traced.begin(), g_traced.end(), fn) == g_traced.end())
            {
                g_traced.push_back(fn);
                Output::send<LogLevel::Normal>(STR("[host] trace: {} on {}\n"), fn->GetPathName(), self ? self->GetName() : StringType{STR("?")});
            }
            if (std::find(g_watched_rpcs.begin(), g_watched_rpcs.end(), fn) == g_watched_rpcs.end()) return;
            Output::send<LogLevel::Normal>(STR("[host] rpc {} on {}\n"), fn->GetName(), self ? player_line(self) : StringType{STR("?")});
        });
        Output::send<LogLevel::Normal>(STR("[host] rpc: watching {} lobby RPCs\n"), g_watched_rpcs.size());
    }

    auto report_players() -> void
    {
        UObject* mps = mp_state(game_instance());
        if (!mps) return;
        UObject* gs = world_object(STR("/Script/Engine.GameplayStatics:GetGameState"), mps);
        auto* promoted = gs ? gs->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("_hypePlayerStates")) : nullptr;
        auto* incoming = gs ? gs->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("_incomingHypePlayerStates")) : nullptr;
        const auto configs = live_objects(STR("LevelLoadConfig"));
        Output::send<LogLevel::Normal>(STR("[host]   game state: _hypePlayerStates {}, _incomingHypePlayerStates {}; LevelLoadConfig._gameMode {}\n"), promoted ? promoted->Num() : -1,
                                       incoming ? incoming->Num() : -1, configs.empty() ? -1 : prop<uint8>(configs.front(), STR("_gameMode")));
        // the lobby entrance: MPS +0x3e0 set by the timeline-looped handler, +0x362 / +0x364 entrance state and start
        // time; MusicTimelineState's replicated position (+0xe8) and last-seen (+0xdc); HypeAudio's playing flag (+0x12d)
        uint8 allowed = 0xff, entr = 0xff, audio_on = 0xff;
        float entr_t = 0.0f;
        int32 pos = 0, seen = 0;
        read_u8(mps, 0x3e0, &allowed);
        read_u8(mps, 0x362, &entr);
        read_f32(mps, 0x364, &entr_t);
        UObject* mts = gs ? prop<UObject*>(gs, STR("MusicTimelineState")) : nullptr;
        if (mts) { read_i32(mts, 0xe8, &pos); read_i32(mts, 0xdc, &seen); }
        UObject* audio = prop<UObject*>(game_instance(), STR("theHypeAudio"));
        if (audio) read_u8(audio, 0x12d, &audio_on);
        UObject* desk = gs ? prop<UObject*>(gs, STR("DeskState")) : nullptr;
        void* desk_song = nullptr;
        if (desk) read_ptr(desk, 0xd0, &desk_song);
        auto* cached = mps->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("_cachedMultiplayerTransitions"));
        Output::send<LogLevel::Normal>(STR("[host]   lobby entrance: allowed {}, state {}, start {:.1f}; cached transitions {}; timeline pos {} (last seen {}); audio +0x12d {}; DeskState+0xd0 {}\n"),
                                       allowed, entr, entr_t, cached ? cached->Num() : -1, pos, seen, audio_on, hex(desk_song));
        auto* djs = mps->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("_djQueue"));
        auto* spec = mps->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("_spectatorList"));
        Output::send<LogLevel::Normal>(STR("[host]   _djQueue {}, _spectatorList {}, MaxNumDJsInSession {}, rounds {}, turn measures {}\n"), djs ? djs->Num() : -1,
                                       spec ? spec->Num() : -1, max_djs(), prop<int32>(mps, STR("_numCoopFreestyleRounds")), prop<int32>(mps, STR("_coopTurnLengthMeasures")));
        for (UObject* ps : players(mps))
        {
            uint8 decks = 0xff;
            read_u8(ps, 0x540, &decks); // _controllingDecksState (set by the client stage path's slot 133)
            // the state-of-the-world sync (FUN_140699ac0): synced tick / round +0x1040 / +0x1044 (-1 = not yet), the
            // replicated _stateOfTheWorld (+0x890 AtMasterTick, +0x894 InRound, GlobalAudioState items +0x9a8,
            // DeckSoloMuteStates items +0xac0), and the _multiplayerActions item count (+0x658)
            int32 synced_tick = 0, synced_round = 0, sotw_tick = 0, sotw_round = 0, n_global = 0, n_solo = 0, n_actions = 0;
            read_i32(ps, 0x1040, &synced_tick);
            read_i32(ps, 0x1044, &synced_round);
            read_i32(ps, 0x890, &sotw_tick);
            read_i32(ps, 0x894, &sotw_round);
            read_i32(ps, 0x9a8, &n_global);
            read_i32(ps, 0xac0, &n_solo);
            read_i32(ps, 0x658, &n_actions);
            Output::send<LogLevel::Normal>(STR("[host]   player {}, _controllingDecksState {}; synced {}/{}, world tick {} round {} (global {}, solo/mute {}), actions {}\n"), player_line(ps),
                                           static_cast<int>(decks), synced_tick, synced_round, sotw_tick, sotw_round, n_global, n_solo, n_actions);
        }
        Output::send<LogLevel::Normal>(STR("[host]   _currentDJ {} (local PS {})\n"), name_of(prop<UObject*>(mps, STR("_currentDJ"))), name_of(host_player_state(game_instance())));
        report_lobby_widgets();
    }

    // The debug keys (Ctrl+Shift+<key>, game window focused).
    auto game_has_focus() -> bool
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    auto pressed(int vk) -> bool
    {
        static bool was[256]{};
        const bool down = game_has_focus() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(vk) & 0x8000);
        const bool edge = down && !was[vk];
        was[vk] = down;
        return edge;
    }
#endif

    // DiagnosticLogs=1 (default off, release builds too): the per-frame logging - the clock samples during a set
    // (log_clock) and the state / connection watch after a join ("[host] changed: ..." and "[host] connection ...").
    auto diagnostic_logs() -> bool
    {
        static const bool on = ini_value(STR("DiagnosticLogs"), STR("0")) == STR("1");
        return on;
    }

    // Clock comparison across machines (every player logs it). Remote actions carry the performer's master-timeline
    // tick and run when the local clock reaches it, so a clock offset between machines shows up as a constant lag one
    // way and none the other. Every
    // 2 s during a set: the music clock's tick (FUN_1405928e0(clock, mode): 0 raw, 1 minus output latency, 2 the
    // other compensated variant; clock = FUN_140591640(ctx)) against the game state's server world time.
    auto clock_ticks(void* ctx, int32* out) -> bool
    {
        __try
        {
            using GetClock = void* (*)(void*);
            using Tick = int32 (*)(void*, char);
            void* clock = reinterpret_cast<GetClock>(at(&VerifiedBuild::audio_of))(ctx);
            if (!clock) return false;
            for (int m = 0; m < 3; ++m) out[m] = reinterpret_cast<Tick>(at(&VerifiedBuild::clock_tick))(clock, static_cast<char>(m));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
    auto log_clock() -> void
    {
        static uint64 last = 0;
        const uint64 now = GetTickCount64();
        if (now - last < 2000 || !verified_build() || !diagnostic_logs()) return;
        last = now;
        UObject* mps = mp_state(game_instance());
        if (!mps) return;
        int32 index = -1;
        read_i32(mps, kStageIndex, &index);
        const bool lobby = index <= 0;
        if (lobby && ini_value(STR("HostLobbyClockTrace"), STR("0")) != STR("1")) return;
        int32 t[3] = {-1, -1, -1};
        const bool has_clock = clock_ticks(mps, t);
        if (!has_clock && !lobby) return;
        UObject* gs = world_object(STR("/Script/Engine.GameplayStatics:GetGameState"), mps);
        float server = -1.0f;
        if (gs)
        {
            Call c(STR("/Script/Engine.GameStateBase:GetServerWorldTimeSeconds"));
            if (c.ok() && c.call(gs)) server = c.get<float>(STR("ReturnValue"));
        }
        UObject* mts = gs ? prop<UObject*>(gs, STR("MusicTimelineState")) : nullptr;
        int32 rep = -1;
        if (mts) read_i32(mts, 0xe8, &rep);
        if (lobby)
        {
            Output::send<LogLevel::Normal>(STR("[host] lobby clock: pid {} mono_ms {} server {:.3f} world {:.3f} stage {} raw {} latency {} mode2 {} timeline rep {} clock_ok {}\n"),
                                           GetCurrentProcessId(), now, server, world_seconds(mps), index, t[0], t[1], t[2], rep, has_clock ? 1 : 0);
            return;
        }
        Output::send<LogLevel::Normal>(STR("[host] clock: server {:.3f} world {:.3f} stage {} - tick raw {} / latency {} / mode2 {}; timeline rep {}\n"), server, world_seconds(mps),
                                       stage_label(mps, index), t[0], t[1], t[2], rep);
    }

    // ---- Route B: installing a catalogue song after launch ----------------------------------------------------
    // fuserd's community catalogue supplies a song's pak + .sig (api/community/resolve/, then /dl/). The loader saves
    // them as Content/Paks/custom_songs/<sn>_P.pak + .sig (the _P name keeps the pak-signing bypass, and the next
    // launch mounts them like any custom song), then does what the game's own DLC scan (FUN_1405e99a0) does with a new
    // pak: mounts it through the PakFile platform file (FUN_1417c5330, synchronous) and registers its song CelSongSet
    // with CelMgr (FUN_14059c740, mode 2 - which only runs while CelMgr +0x2a8 is 2, 4 or 6, so the loader waits for
    // that). The song then resolves like a base-game song (downloadState OnDisc). Network and hashing run on a worker
    // thread; mounting and registering on the game thread.
    // Test trigger: Ctrl+Shift+U installs StoreTestInstall=<shortname>. StoreURL= overrides fuserd's API base
    // (default: the Live environment's ClientURL, Default__BackendServerMgrSettings.EnvironmentSettings).
    constexpr size_t kBackendToken = 0x168; // BackendServerMgr: the session token FUN_1406e30d0 sends as "JWT <token>"
    constexpr size_t kCelMgrState = 0x2a8;  // CelMgr load state; FUN_14059c740 mode 2 runs in states 2, 4 and 6

    // The Live environment's ClientURL. EnvironmentSettings is a TMap<EBackendEnvironment, FBackendEnvironmentSettings>;
    // its elements are read through the map property's layout (FMapProperty +0x80: ValueOffset, then the set layout's
    // HashNextIdOffset, HashIndexOffset and element Size), and only an element whose key is Live (4) and whose
    // ClientURL starts with http is accepted.
    auto backend_client_url() -> StringType
    {
        UObject* settings = cdo(STR("/Script/Hype.Default__BackendServerMgrSettings"));
        FProperty* p = settings ? settings->GetPropertyByNameInChain(STR("EnvironmentSettings")) : nullptr;
        if (!p) return {};
        const uint8* map = p->ContainerPtrToValuePtr<uint8>(settings);
        int32 value_offset = 0, stride = 0;
        std::memcpy(&value_offset, reinterpret_cast<const uint8*>(p) + 0x80, 4);
        std::memcpy(&stride, reinterpret_cast<const uint8*>(p) + 0x8c, 4);
        if (value_offset < 1 || value_offset > 16 || stride < 0x40 || stride > 0x200) value_offset = 8, stride = 0x88;
        const uint8* data = nullptr;
        int32 num = 0;
        if (!read_ptr(map, 0, reinterpret_cast<void**>(const_cast<uint8**>(&data))) || !read_i32(map, 8, &num) || !data || num < 0 || num > 16)
            return {};
        for (int32 i = 0; i < num; ++i)
        {
            const uint8* e = data + static_cast<size_t>(i) * stride;
            if (e[0] != 4) continue; // EBackendEnvironment::Live
            wchar_t url[512]{};
            if (read_fstring(e, value_offset, url, 512) > 0 && std::wstring_view(url).starts_with(L"http")) return url;
        }
        return {};
    }

    auto store_api_base() -> StringType
    {
        StringType base = ini_value(STR("StoreURL"), STR(""));
        if (base.empty()) base = backend_client_url();
        if (!base.empty() && base.back() != L'/') base += L'/';
        return base;
    }

    // The game's own fuserd session token (empty before login).
    auto backend_token() -> StringType
    {
        UObject* gi = game_instance();
        auto** slot = gi ? gi->GetValuePtrByPropertyNameInChain<UObject*>(STR("theBackendServerMgr")) : nullptr;
        if (!slot || !*slot) return {};
        wchar_t tok[2048]{};
        if (read_fstring(*slot, kBackendToken, tok, 2048) <= 0) return {};
        return tok;
    }

    auto narrow(const StringType& w) -> std::string
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    auto widen(const std::string& s) -> StringType
    {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        StringType w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    // SHA-256 (CNG), hex.
    struct Sha256
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool ok = false;
        Sha256()
        {
            ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
                 BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
        }
        ~Sha256()
        {
            if (hash) BCryptDestroyHash(hash);
            if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        }
        void add(const void* p, size_t n)
        {
            if (ok) ok = BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(p)), static_cast<ULONG>(n), 0) >= 0;
        }
        auto hex() -> std::string
        {
            uint8 d[32]{};
            if (!ok || BCryptFinishHash(hash, d, 32, 0) < 0) return {};
            static const char* digits = "0123456789abcdef";
            std::string s;
            for (uint8 b : d) s += digits[b >> 4], s += digits[b & 15];
            return s;
        }
    };

    auto file_sha256(const std::filesystem::path& path, int64* size) -> std::string
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return {};
        Sha256 h;
        std::vector<char> buf(1 << 16);
        int64 total = 0;
        while (in)
        {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto n = in.gcount();
            if (n <= 0) break;
            h.add(buf.data(), static_cast<size_t>(n));
            total += n;
        }
        if (size) *size = total;
        return h.hex();
    }

    // HTTP through the real WinHTTP. This process's winhttp.dll is the loader's proxy, which exports only the stock
    // WinHTTP list, and WinINet fails inside it (error 127: it looks up a function the proxy doesn't export,
    // 2026-10-01). So the functions are taken from System32\winhttp.dll itself, which the proxy has already loaded by
    // full path. One request; the body goes to *body, or with a file sink to a file while being hashed.
    struct RealWinHttp
    {
        decltype(&WinHttpOpen) open = nullptr;
        decltype(&WinHttpConnect) connect = nullptr;
        decltype(&WinHttpOpenRequest) open_request = nullptr;
        decltype(&WinHttpSendRequest) send = nullptr;
        decltype(&WinHttpReceiveResponse) receive = nullptr;
        decltype(&WinHttpQueryHeaders) query = nullptr;
        decltype(&WinHttpReadData) read = nullptr;
        decltype(&WinHttpCloseHandle) close = nullptr;
        decltype(&WinHttpCrackUrl) crack = nullptr;
        decltype(&WinHttpSetTimeouts) timeouts = nullptr;
        bool ok = false;
    };
    auto real_winhttp() -> const RealWinHttp&
    {
        static const RealWinHttp w = [] {
            RealWinHttp r;
            wchar_t sys[MAX_PATH]{};
            GetSystemDirectoryW(sys, MAX_PATH);
            const std::wstring path = std::wstring(sys) + L"\\winhttp.dll";
            HMODULE m = GetModuleHandleW(path.c_str());
            if (!m) m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!m) return r;
            auto get = [m](auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name)); };
            get(r.open, "WinHttpOpen");
            get(r.connect, "WinHttpConnect");
            get(r.open_request, "WinHttpOpenRequest");
            get(r.send, "WinHttpSendRequest");
            get(r.receive, "WinHttpReceiveResponse");
            get(r.query, "WinHttpQueryHeaders");
            get(r.read, "WinHttpReadData");
            get(r.close, "WinHttpCloseHandle");
            get(r.crack, "WinHttpCrackUrl");
            get(r.timeouts, "WinHttpSetTimeouts");
            r.ok = r.open && r.connect && r.open_request && r.send && r.receive && r.query && r.read && r.close && r.crack && r.timeouts;
            return r;
        }();
        return w;
    }
    struct HttpResult
    {
        DWORD status = 0;
        std::string body, error, sha256;
        int64 bytes = 0;
    };
    auto http_request(const StringType& method, const StringType& url, const StringType& headers, const std::string& payload,
                      const std::filesystem::path* file_sink) -> HttpResult
    {
        HttpResult r;
        const RealWinHttp& w = real_winhttp();
        if (!w.ok)
        {
            r.error = "System32 winhttp.dll is not available";
            return r;
        }
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256]{}, path[2048]{};
        uc.lpszHostName = host, uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path, uc.dwUrlPathLength = 2048;
        if (!w.crack(url.c_str(), 0, 0, &uc))
        {
            r.error = "bad URL";
            return r;
        }
        HINTERNET session = w.open(L"FUSERNativeLoader", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session) w.timeouts(session, 10000, 10000, 30000, 60000);
        HINTERNET conn = session ? w.connect(session, host, uc.nPort, 0) : nullptr;
        const DWORD flags = WINHTTP_FLAG_REFRESH | (uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        HINTERNET req = conn ? w.open_request(conn, method.c_str(), path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags) : nullptr;
        if (!req || !w.send(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : static_cast<DWORD>(-1),
                            payload.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()),
                            static_cast<DWORD>(payload.size()), 0) ||
            !w.receive(req, nullptr))
        {
            r.error = "request failed (WinHTTP error " + std::to_string(GetLastError()) + ")";
        }
        else
        {
            DWORD len = sizeof(r.status);
            w.query(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &r.status, &len, WINHTTP_NO_HEADER_INDEX);
            std::ofstream out;
            Sha256 h;
            if (file_sink) out.open(*file_sink, std::ios::binary | std::ios::trunc);
            if (file_sink && !out)
                r.error = "cannot write " + file_sink->string();
            else
            {
                std::vector<char> buf(1 << 16);
                DWORD got = 0;
                while (w.read(req, buf.data(), static_cast<DWORD>(buf.size()), &got) && got > 0)
                {
                    r.bytes += got;
                    if (file_sink && r.status == 200)
                    {
                        out.write(buf.data(), got);
                        h.add(buf.data(), got);
                    }
                    else if (r.body.size() < (1 << 20))
                        r.body.append(buf.data(), got);
                }
                if (file_sink) r.sha256 = h.hex();
            }
        }
        if (req) w.close(req);
        if (conn) w.close(conn);
        if (session) w.close(session);
        return r;
    }

    // Minimal JSON field reads for the resolve answer (flat string / number / bool values with known keys). Only a
    // "key" followed by ':' counts: the first resolve test read "kind":"shortName" as the shortName key and saved the
    // song as true_P.pak (2026-10-01).
    auto json_raw(const std::string& s, const char* key) -> std::string
    {
        const std::string k = std::string("\"") + key + "\"";
        size_t i = 0;
        for (size_t from = 0;; from = i + 1)
        {
            i = s.find(k, from);
            if (i == std::string::npos) return {};
            size_t j = i + k.size();
            while (j < s.size() && (s[j] == ' ' || s[j] == '\n' || s[j] == '\r' || s[j] == '\t')) ++j;
            if (j < s.size() && s[j] == ':')
            {
                i = j;
                break;
            }
        }
        ++i;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
        if (i >= s.size()) return {};
        if (s[i] == '"')
        {
            std::string v;
            for (++i; i < s.size() && s[i] != '"'; ++i)
            {
                if (s[i] == '\\' && i + 1 < s.size()) ++i; // \" \\ \/ (no \u escapes in these fields)
                v += s[i];
            }
            return v;
        }
        size_t j = i;
        while (j < s.size() && s[j] != ',' && s[j] != '}' && s[j] != ']') ++j;
        return s.substr(i, j - i);
    }

    struct SongInstall
    {
        StringType shortname;
        enum class Stage { Resolving, Downloaded, Mounted, Registering, Unlocking, Done, Failed } stage = Stage::Resolving;
        StringType meta_path, pak_path, error;
        uint64 started_ms = 0, register_since = 0, last_request = 0;
        int requests = 0;
        bool reused = false;
        bool by_path = false;   // shortname holds a crate's Meta object path until the resolve answer names the song
        bool temporary = false; // Download Songs = Temporarily: kept in this run's session folder, not custom_songs
        StringType crate_path;  // the crate path it was asked for by (crate scan), for the retry after a failure
        bool p2p_next = false;  // fuserd had no way to the song that worked (no entry, or its community link failed): ask the players
    };
    std::mutex g_song_mutex;
    std::vector<std::shared_ptr<SongInstall>> g_song_installs;
    std::atomic<int> g_song_requests_active{0}; // songs being asked for from other players (the missing-cel wait counts them)
    std::atomic<int> g_sync_ok{0}, g_sync_failed{0}; // songs installed / given up on, for the sync notices (counted where they end)
    // The session-wide sync round (fuserd's p2p/song/poll/ "sync"): this machine's state goes up with each poll, and a
    // finished round comes back for the "all players" notice.
    std::atomic<bool> g_sync_active{false};       // this machine is fetching crate songs (drive_sync_notices)
    std::atomic<bool> g_sync_started{false};      // a batch started since the last poll (one shorter than a poll still counts)
    std::atomic<int> g_sync_batch_failed{0};      // songs its current / last batch couldn't get
    std::atomic<int> g_sync_batch_done{0}, g_sync_batch_total{0}; // its songs that have ended, and all of them
    std::atomic<int> g_round_done{0}, g_round_players{0}, g_round_failed{0}; // the last finished round (song worker)
    std::atomic<bool> g_round_active{false};      // a round is under way somewhere in the session (song worker)
    // Every polling player's own sync state, from fuserd's last poll answer ("sync" "states"), for the sync panel.
    struct PlayerSync
    {
        int64 player = 0; // fuserd player id (= _hypeUserId)
        bool syncing = false;
        int done = 0, total = 0, failed = 0;
    };
    std::mutex g_player_syncs_mu;
    std::vector<PlayerSync> g_player_syncs;

    auto song_dir() -> std::filesystem::path
    {
        std::error_code ec;
        auto dir = std::filesystem::weakly_canonical(std::filesystem::path(exe_dir()) / L".." / L".." / L"Content" / L"Paks" / L"custom_songs", ec);
        return dir;
    }

    // ---- Download Songs (No / Temporarily / Permanently) ---------------------------------------------------------
    // The settings screen (native/fusernative.cpp) saves DownloadSongs=0/1/2 to FuserNative.ini; it is read here at
    // most once a second, so a change applies mid-session. Without the key, the old StoreAutoInstall=0 reads as No;
    // the default is Permanently (Route B as before). It governs the songs this machine fetches because another
    // player's crate has them, the StoreTestInstall trigger, and owned Diamond Store songs restored at login; a
    // purchase just made is the player's own choice and always installs permanently.
    constexpr int kDownloadNo = 0, kDownloadTemporarily = 1, kDownloadPermanently = 2;
    auto download_mode() -> int
    {
        static std::mutex mu;
        static uint64 read_at = 0;
        static int mode = kDownloadPermanently;
        std::lock_guard lock(mu);
        const uint64 now = GetTickCount64();
        if (read_at && now - read_at < 1000) return mode;
        read_at = now;
        const StringType v = ini_value(STR("DownloadSongs"), STR(""));
        const int parsed = v.empty() ? -1 : _wtoi(v.c_str());
        mode = parsed >= kDownloadNo && parsed <= kDownloadPermanently ? parsed
               : ini_value(STR("StoreAutoInstall"), STR("1")) == STR("0") ? kDownloadNo
                                                                         : kDownloadPermanently;
        return mode;
    }
    auto download_mode_name(int mode) -> const CharType*
    {
        return mode == kDownloadNo ? STR("No") : mode == kDownloadTemporarily ? STR("Temporarily") : STR("Permanently");
    }

    // Temporarily: %LOCALAPPDATA%\Fuser\Saved\FuserNative\SessionSongs\<pid>\, outside Content/Paks so the next
    // launch doesn't mount it. The pak keeps its <sn>_P.pak name and its .sig beside it: the signature bypass
    // (dllmain.cpp) keys on "_P" in the file name, not on the folder. Per process, because two games can run from one
    // installation (a local multiplayer test); a mounted pak can't be deleted, so a run's folder is removed by a later
    // launch (clean_session_songs) once its process has exited.
    auto session_songs_root() -> std::filesystem::path
    {
        wchar_t buf[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        const std::filesystem::path base = n > 0 && n < MAX_PATH ? std::filesystem::path(buf) / L"Fuser" / L"Saved"
                                                                 : std::filesystem::path(exe_dir()) / L".." / L".." / L"Saved";
        return base / L"FuserNative" / L"SessionSongs";
    }
    auto session_song_dir() -> std::filesystem::path { return session_songs_root() / std::to_wstring(GetCurrentProcessId()); }
    // Community downloads are fetched and extracted in CommunityTmp\<pid>\<X>\ (community_download).
    auto community_tmp_root() -> std::filesystem::path { return session_songs_root().parent_path() / L"CommunityTmp"; }

    // Removes root's <pid> folders whose FUSER process is no longer running.
    auto clean_run_folders(const std::filesystem::path& root, const CharType* what) -> void
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) return;
        int removed = 0, kept = 0;
        for (const auto& entry : std::filesystem::directory_iterator(root, ec))
        {
            const std::wstring name = entry.path().filename().wstring();
            const DWORD pid = name.empty() || name.find_first_not_of(L"0123456789") != std::wstring::npos ? 0 : static_cast<DWORD>(_wtoi(name.c_str()));
            if (!entry.is_directory(ec) || pid == 0 || pid == GetCurrentProcessId()) continue;
            bool alive = false;
            if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
            {
                DWORD code = 0;
                wchar_t image[MAX_PATH]{};
                DWORD len = MAX_PATH;
                // still running, and still FUSER (a reused pid belongs to something else, so its folder is stale)
                // FuserEOS-Win64-Shipping.exe (Epic) or Fuser-Win64-Shipping.exe (Steam)
                alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE && QueryFullProcessImageNameW(h, 0, image, &len) &&
                        std::wstring_view(image, len).find(L"Fuser") != std::wstring_view::npos;
                CloseHandle(h);
            }
            if (alive)
            {
                ++kept;
                continue;
            }
            const auto count = std::filesystem::remove_all(entry.path(), ec);
            if (ec) Output::send<LogLevel::Warning>(STR("[store] {}: cannot remove {} ({})\n"), what, entry.path().wstring(), widen(ec.message()));
            else if (count) ++removed;
        }
        if (removed || kept)
            Output::send<LogLevel::Normal>(STR("[store] {}: removed {} folder(s) of earlier runs, kept {} of running ones ({})\n"), what, removed, kept, root.wstring());
    }

    // Once at startup: the Temporarily songs and community download leftovers of runs that have exited.
    auto clean_session_songs() -> void
    {
        clean_run_folders(session_songs_root(), STR("session songs"));
        clean_run_folders(community_tmp_root(), STR("community downloads"));
    }

    auto song_fail(const std::shared_ptr<SongInstall>& job, const StringType& why) -> void
    {
        std::lock_guard lock(g_song_mutex);
        job->error = why;
        job->stage = SongInstall::Stage::Failed;
    }

    // A catalogue entry from api/community/resolve/ ("songs"[i].song).
    struct ResolvedSong
    {
        std::string sn, meta_path, pak_url, sig_url, pak_sha, sig_sha, version, licence;
        int64 pak_size = 0;
    };

    // POST api/community/resolve/, retried on HTTP 429 (fuserd's per-address rate limit) and on transport errors, with
    // 2 / 4 / 8 / 16 s between tries: a crate's songs arrive in a burst (2026-10-04: 24 of 168 lookups refused).
    auto resolve_post(const StringType& base, const StringType& token, const std::string& body) -> HttpResult
    {
        StringType headers = L"Content-Type: application/json\r\n";
        if (!token.empty()) headers += L"Authorization: JWT " + token + L"\r\n";
        constexpr int kTries = 5;
        HttpResult res{};
        for (int attempt = 1; attempt <= kTries; ++attempt)
        {
            res = http_request(L"POST", base + L"community/resolve/", headers, body, nullptr);
            if (res.error.empty() && res.status != 429) break;
            if (attempt == kTries) break;
            Output::send<LogLevel::Warning>(STR("[store] resolve: HTTP {} {}; trying again ({} of {})\n"), res.status, widen(res.error), attempt + 1, kTries);
            Sleep(static_cast<DWORD>(1000u << attempt));
        }
        return res;
    }

    auto song_download(const std::shared_ptr<SongInstall>& job, const ResolvedSong& rs) -> void;
    auto community_download(const std::shared_ptr<SongInstall>& job, const jsonl::Value& c, const StringType& base, const StringType& token) -> void;

    // One resolve answer ("songs"[i]): the catalogue's entry, else fuserd's community link, else a miss
    // - which the crate scan hands to the players.
    auto song_answer_fetch(const std::shared_ptr<SongInstall>& job, const jsonl::Value& a, const StringType& base, const StringType& token) -> void
    {
        if (a["found"].boolean())
        {
            const jsonl::Value& s = a["song"];
            song_download(job, ResolvedSong{s["shortName"].str(), s["meta_path"].str(), s["pak_url"].str(), s["sig_url"].str(),
                                            s["pak_sha256"].str(), s["sig_sha256"].str(), std::to_string(s["version"].i64()),
                                            s["licence"].str(), s["pak_size"].i64()});
            return;
        }
        if (!a["community"].is_null())
        {
            community_download(job, a["community"], base, token);
            return;
        }
        {
            std::lock_guard lock(g_song_mutex);
            job->p2p_next = true;
        }
        song_fail(job, STR("not in fuserd's catalogue"));
    }

    // Worker thread: resolve one song, then download + verify (or reuse a file already there with the right hash).
    auto song_fetch(std::shared_ptr<SongInstall> job, StringType base, StringType token) -> void
    {
        const std::string body = std::string("{\"") + (job->by_path ? "paths" : "shortNames") + "\":[\"" + narrow(job->shortname) + "\"]}";
        HttpResult res = resolve_post(base, token, body);
        jsonl::Value root;
        if (!res.error.empty() || res.status != 200 || !jsonl::parse(res.body, &root) || root["results"]["songs"].size() != 1)
        {
            song_fail(job, std::format(STR("resolve failed: HTTP {} {} {}"), res.status, widen(res.error), widen(res.body.substr(0, 200))));
            return;
        }
        song_answer_fetch(job, root["results"]["songs"].at(0), base, token);
    }

    // Worker thread: a crate scan's missing songs, resolved together - one request per 64 paths (fuserd's maximum)
    // where each used to send its own - then each found one downloaded in turn.
    auto song_fetch_batch(std::vector<std::shared_ptr<SongInstall>> jobs, StringType base, StringType token) -> void
    {
        constexpr size_t kMaxQueries = 64; // fuserd's maxResolve
        for (size_t first = 0; first < jobs.size(); first += kMaxQueries)
        {
            const size_t last = (std::min)(jobs.size(), first + kMaxQueries);
            std::map<std::string, std::shared_ptr<SongInstall>> by_query;
            std::string body = "{\"paths\":[";
            for (size_t i = first; i < last; ++i)
            {
                const std::string path = narrow(jobs[i]->shortname);
                if (path.find_first_of("\"\\") != std::string::npos)
                {
                    song_fail(jobs[i], STR("not a crate song path"));
                    continue;
                }
                body += (by_query.empty() ? "\"" : ",\"") + path + "\"";
                by_query[path] = jobs[i];
            }
            body += "]}";
            if (by_query.empty()) continue;
            const HttpResult res = resolve_post(base, token, body);
            jsonl::Value root;
            if (!res.error.empty() || res.status != 200 || !jsonl::parse(res.body, &root))
            {
                const StringType why = std::format(STR("resolve failed: HTTP {} {} {}"), res.status, widen(res.error), widen(res.body.substr(0, 200)));
                for (const auto& [path, job] : by_query) song_fail(job, why);
                continue;
            }
            const jsonl::Value& songs = root["results"]["songs"];
            for (size_t i = 0; i < songs.size(); ++i)
            {
                const jsonl::Value& a = songs.at(i);
                const auto it = by_query.find(a["query"].str());
                if (it == by_query.end()) continue;
                const std::shared_ptr<SongInstall> job = it->second;
                by_query.erase(it);
                song_answer_fetch(job, a, base, token);
            }
            for (const auto& [path, job] : by_query) song_fail(job, STR("resolve answer left it out"));
        }
    }

    // Worker thread: download and verify a resolved song (or reuse files already there with the right hashes).
    auto song_download(const std::shared_ptr<SongInstall>& job, const ResolvedSong& rs) -> void
    {
        const std::string &pak_url = rs.pak_url, &sig_url = rs.sig_url, &pak_sha = rs.pak_sha, &sig_sha = rs.sig_sha, &sn = rs.sn;
        const int64 pak_size = rs.pak_size;
        if (pak_url.empty() || sig_url.empty() || pak_sha.size() != 64 || sig_sha.size() != 64 || sn.empty())
        {
            song_fail(job, STR("resolve answer is incomplete"));
            return;
        }
        if (rs.meta_path.find("/" + sn + "/") == std::string::npos || sn.find_first_of("/\\.:") != std::string::npos)
        {
            song_fail(job, std::format(STR("resolve answer is inconsistent (shortName '{}', meta_path '{}')"), widen(sn), widen(rs.meta_path)));
            return;
        }
        const std::filesystem::path dir = job->temporary ? session_song_dir() : song_dir();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path pak = dir / (widen(sn) + L"_P.pak"), sig = dir / (widen(sn) + L"_P.sig");
        {
            std::lock_guard lock(g_song_mutex);
            job->shortname = widen(sn);
            job->meta_path = widen(rs.meta_path);
            job->pak_path = pak.wstring();
        }
        Output::send<LogLevel::Normal>(STR("[store] {}: in the catalogue (version {}, licence '{}'); pak {} bytes; keeping it {} in {}\n"), widen(sn),
                                       widen(rs.version), widen(rs.licence), pak_size,
                                       job->temporary ? STR("for this run") : STR("permanently"), dir.wstring());
        bool reused = true;
        for (auto [url, path, sha] : {std::tuple{pak_url, pak, pak_sha}, std::tuple{sig_url, sig, sig_sha}})
        {
            int64 have = 0;
            if (std::filesystem::exists(path, ec) && file_sha256(path, &have) == sha) continue;
            reused = false;
            std::filesystem::path part = path;
            part += std::format(L".{}.part", GetCurrentProcessId()); // a second game on this machine may fetch the same song
            HttpResult dl = http_request(L"GET", widen(url), {}, {}, &part);
            if (!dl.error.empty() || dl.status != 200)
            {
                std::filesystem::remove(part, ec);
                song_fail(job, std::format(STR("download of {} failed: HTTP {} {}"), widen(url), dl.status, widen(dl.error)));
                return;
            }
            if (dl.sha256 != sha)
            {
                std::filesystem::remove(part, ec);
                song_fail(job, std::format(STR("{} has the wrong sha256 ({}, catalogue {})"), widen(url), widen(dl.sha256), widen(sha)));
                return;
            }
            if (!MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                const DWORD err = GetLastError();
                std::filesystem::remove(part, ec);
                if (file_sha256(path, &have) == sha) continue; // another process placed (and maybe mounted) it meanwhile
                SetLastError(err);
                song_fail(job, std::format(STR("cannot place {} (error {}): is it in use?"), path.wstring(), GetLastError()));
                return;
            }
            Output::send<LogLevel::Normal>(STR("[store] {}: downloaded {} ({} bytes, sha256 ok)\n"), widen(sn), path.filename().wstring(), dl.bytes);
        }
        std::string why;
        const p2p::SigVerdict v = p2p::check_song_sig(pak.wstring(), sig.wstring(), &why);
        if (v == p2p::SigVerdict::Bad)
        {
            song_fail(job, std::format(STR("its .sig would make FUSER quit at mount: {}"), widen(why)));
            return;
        }
        if (v == p2p::SigVerdict::Repaired) Output::send<LogLevel::Warning>(STR("[store] {}: {}\n"), widen(sn), widen(why));
        std::lock_guard lock(g_song_mutex);
        job->reused = reused;
        job->stage = SongInstall::Stage::Downloaded;
    }

    // ---- Community songs, from their creators' public Google Drive --------------------------------------------------
    // fuserd's community entry names the Drive file(s) and, for an archive, the pak and sig inside it. The files are
    // downloaded into CommunityTmp, extracted (libarchive, community_fetch.cpp), checked with the same layout rule as
    // a pak received from another player, then placed like a catalogue download. fuserd never has the bytes, so there
    // is no hash to check against; the layout check is the gate. Every outcome is reported to community/report/, and
    // a failure hands the song to the players.
    auto community_tmp_dir(const std::string& x) -> std::filesystem::path
    {
        return community_tmp_root() / std::to_wstring(GetCurrentProcessId()) / widen(x);
    }

    auto community_report(const StringType& base, const StringType& token, const std::string& x, const std::string& reason,
                          const std::string& detail, const std::string& sha, const std::string& sig_sha, int64 size, uint64 ms) -> void
    {
        auto esc = [](std::string s) {
            std::string o;
            for (char c : s.substr(0, 200))
                if (c == '"' || c == '\\') o += '\\', o += c;
                else if (static_cast<unsigned char>(c) >= 0x20) o += c;
            return o;
        };
        const std::string body = std::format(R"({{"x":"{}","ok":{},"reason":"{}","detail":"{}","sha256":"{}","sig_sha256":"{}","size":{},"ms":{}}})", esc(x),
                                             reason.empty() ? "true" : "false", reason, esc(detail), sha, sig_sha, size, ms);
        StringType headers = L"Content-Type: application/json\r\n";
        if (!token.empty()) headers += L"Authorization: JWT " + token + L"\r\n";
        const HttpResult res = http_request(L"POST", base + L"community/report/", headers, body, nullptr);
        if (!res.error.empty() || res.status != 200)
            Output::send<LogLevel::Warning>(STR("[store] {}: community report not sent (HTTP {} {})\n"), widen(x), res.status, widen(res.error));
    }

    // Worker thread: fetch a community entry ("songs"[i].community) and leave the job at Downloaded, or fail it.
    auto community_download(const std::shared_ptr<SongInstall>& job, const jsonl::Value& c, const StringType& base, const StringType& token) -> void
    {
        const uint64 start = GetTickCount64();
        const std::string x = c["x"].str(), kind = c["kind"].str(), archive = c["archive"].str();
        const jsonl::Value& files = c["files"];
        std::string reason, detail;
        auto fail = [&](const std::string& r, const std::string& d) {
            reason = r, detail = d;
        };
        if (x.empty() || x.find_first_of("/\\.:\"") != std::string::npos || (kind != "archive" && kind != "folder") ||
            files.size() != (kind == "archive" ? 1u : 2u))
        {
            Output::send<LogLevel::Error>(STR("[store] {}: fuserd's community entry is malformed\n"), job->shortname);
            std::lock_guard lock(g_song_mutex);
            job->p2p_next = true;
            job->error = STR("community entry is malformed");
            job->stage = SongInstall::Stage::Failed;
            return;
        }
        Output::send<LogLevel::Normal>(STR("[store] {}: not in the catalogue; community link: {} {}{} by {} ({} file(s))\n"), widen(x),
                                       widen(c["artist"].str()), widen(c["title"].str()), archive.empty() ? StringType{} : STR(", ") + widen(archive),
                                       widen(c["creator"].str()), files.size());
        std::error_code ec;
        const std::filesystem::path tmp = community_tmp_dir(x);
        std::filesystem::remove_all(tmp, ec);
        std::filesystem::create_directories(tmp, ec);
        const std::filesystem::path pak_tmp = tmp / (widen(x) + L"_P.pak"), sig_tmp = tmp / (widen(x) + L"_P.sig");
        int64 downloaded = 0;

        // Download each file. A Drive answer that is a page (quota, access denied, not found) or the wrong size fails.
        for (size_t i = 0; i < files.size() && reason.empty(); ++i)
        {
            const jsonl::Value& f = files.at(i);
            const int64 want = f["size"].i64();
            const std::filesystem::path dst = kind == "folder" ? (i == 0 ? pak_tmp : sig_tmp) : tmp / L"archive";
            const HttpResult dl = http_request(L"GET", widen(f["url"].str()), {}, {}, &dst);
            int64 have = 0;
            const bool exists = std::filesystem::exists(dst, ec);
            if (exists) have = static_cast<int64>(std::filesystem::file_size(dst, ec));
            std::string page = dl.body;
            std::ranges::transform(page, page.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (!dl.error.empty()) fail("http", dl.error);
            else if (dl.status != 200)
                fail(page.find("quota") != std::string::npos || page.find("too many users") != std::string::npos ? "quota" : "http",
                     std::format("HTTP {} for {}", dl.status, f["name"].str()));
            else if (community::looks_like_html(dst.wstring())) fail("html", "Drive answered with a page instead of " + f["name"].str());
            else if (have != want || want <= 0 || want > 128ll * 1024 * 1024) fail("size", std::format("{} is {} bytes, the entry says {}", f["name"].str(), have, want));
            downloaded += have;
        }
        // Extract, then check.
        std::string why;
        if (reason.empty() && kind == "archive" &&
            !community::extract_song((tmp / L"archive").wstring(), c["pak_entry"].str(), c["sig_entry"].str(), pak_tmp.wstring(), sig_tmp.wstring(),
                                     p2p::kMaxSongPakBytes, &why))
            fail("extract", why);
        if (reason.empty() && !community::sig_ok(sig_tmp.wstring(), &why)) fail("sig", why);
        if (reason.empty() && !p2p::check_song_pak(pak_tmp.wstring(), x, &why)) fail("layout", why);
        if (reason.empty())
        {
            const p2p::SigVerdict v = p2p::check_song_sig(pak_tmp.wstring(), sig_tmp.wstring(), &why);
            if (v == p2p::SigVerdict::Bad) fail("sig", why);
            else if (v == p2p::SigVerdict::Repaired)
            {
                Output::send<LogLevel::Warning>(STR("[store] {}: {}\n"), widen(x), widen(why));
                detail = why; // reported with the good install
            }
        }
        int64 pak_size = 0, sig_size = 0;
        const std::string pak_sha = reason.empty() ? file_sha256(pak_tmp, &pak_size) : std::string{};
        const std::string sig_sha = reason.empty() ? file_sha256(sig_tmp, &sig_size) : std::string{};

        // Place the pair like a catalogue download (song_download).
        const std::filesystem::path dir = job->temporary ? session_song_dir() : song_dir();
        const std::filesystem::path pak = dir / (widen(x) + L"_P.pak"), sig = dir / (widen(x) + L"_P.sig");
        if (reason.empty())
        {
            std::filesystem::create_directories(dir, ec);
            for (auto [from, to, sha] : {std::tuple{sig_tmp, sig, sig_sha}, std::tuple{pak_tmp, pak, pak_sha}})
            {
                int64 have = 0;
                if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) && file_sha256(to, &have) != sha)
                {
                    fail("install", std::format("cannot place {} (error {}): is it in use?", narrow(to.wstring()), GetLastError()));
                    break;
                }
            }
        }
        std::filesystem::remove_all(tmp, ec);
        std::filesystem::remove(tmp.parent_path(), ec); // this run's folder, once no other download is using it (fails while not empty)
        const uint64 ms = GetTickCount64() - start;
        community_report(base, token, x, reason, detail, pak_sha, sig_sha, reason.empty() ? pak_size : downloaded, ms);
        std::lock_guard lock(g_song_mutex);
        if (!reason.empty())
        {
            Output::send<LogLevel::Warning>(STR("[store] {}: community link failed ({}: {}) after {} ms; asking the players next\n"), widen(x), widen(reason), widen(detail), ms);
            job->p2p_next = true;
            job->error = std::format(STR("community link failed: {}"), widen(reason));
            job->stage = SongInstall::Stage::Failed;
            return;
        }
        // The Meta's object path, under the root the crate path named (community paks use DLC/Songs; a shortname
        // query - the test trigger - assumes it).
        const bool audio_root = job->by_path && job->shortname.find(L"/Game/Audio/Songs/") == 0;
        job->meta_path = widen(std::string(audio_root ? "/Game/Audio/Songs/" : "/Game/DLC/Songs/") + x + "/Meta_" + x + ".Meta_" + x);
        job->shortname = widen(x);
        job->pak_path = pak.wstring();
        job->reused = false;
        Output::send<LogLevel::Normal>(STR("[store] {}: community song fetched, extracted and checked ({} bytes downloaded, pak {} bytes, sha256 {}) in {} ms; "
                                           "keeping it {} in {}\n"),
                                       job->shortname, downloaded, pak_size, widen(pak_sha.substr(0, 12)), ms,
                                       job->temporary ? STR("for this run") : STR("permanently"), dir.wstring());
        job->stage = SongInstall::Stage::Downloaded;
    }

    auto song_installs_active() -> bool
    {
        std::lock_guard lock(g_song_mutex);
        return g_song_requests_active.load() > 0 ||
               std::ranges::any_of(g_song_installs, [](const auto& j) { return j->stage != SongInstall::Stage::Done && j->stage != SongInstall::Stage::Failed; });
    }

    // follow_setting: the fetch is Download Songs' to allow (No refuses it) and place (Temporarily or Permanently); a
    // Diamond Store purchase passes false and always installs permanently.
    auto request_song_install(const StringType& shortname, bool by_path = false, bool follow_setting = true) -> void
    {
        if (shortname.empty())
        {
            Output::send<LogLevel::Warning>(STR("[store] no song to install: set StoreTestInstall=<shortname> in FuserNative.ini\n"));
            return;
        }
        const int mode = follow_setting ? download_mode() : kDownloadPermanently;
        if (mode == kDownloadNo)
        {
            Output::send<LogLevel::Normal>(STR("[store] {}: not fetched (Download Songs is No)\n"), shortname);
            return;
        }
        const StringType base = store_api_base();
        if (base.empty())
        {
            Output::send<LogLevel::Error>(STR("[store] no fuserd URL: set StoreURL= or the Live ClientURL\n"));
            return;
        }
        const StringType token = backend_token();
        const size_t dots = std::count(token.begin(), token.end(), L'.');
        Output::send<LogLevel::Normal>(STR("[store] {}: installing from {} (session token {})\n"), shortname, base,
                                       token.empty() ? STR("none - log in first, or turn store.resolve_requires_login off")
                                                     : dots == 2 ? STR("found") : STR("found, but it does not look like a JWT"));
        auto job = std::make_shared<SongInstall>();
        job->shortname = shortname;
        job->by_path = by_path;
        job->temporary = mode == kDownloadTemporarily;
        job->started_ms = GetTickCount64();
        {
            std::lock_guard lock(g_song_mutex);
            g_song_installs.push_back(job);
        }
        std::thread(song_fetch, job, base, token).detach();
    }

    // The crate scan's missing songs (crate Meta object paths), resolved in one batch (song_fetch_batch).
    auto crate_fetch_failed(const StringType& path) -> void; // the crate scan's retry record, below
    auto request_crate_songs(const std::vector<StringType>& paths) -> void
    {
        const int mode = download_mode();
        const StringType base = store_api_base();
        if (paths.empty()) return;
        if (mode == kDownloadNo || base.empty())
        {
            if (base.empty()) Output::send<LogLevel::Error>(STR("[store] no fuserd URL: set StoreURL= or the Live ClientURL\n"));
            for (const StringType& path : paths) crate_fetch_failed(path); // not asked: the crate scan tries again later
            return;
        }
        const StringType token = backend_token();
        std::vector<std::shared_ptr<SongInstall>> jobs;
        for (const StringType& path : paths)
        {
            auto job = std::make_shared<SongInstall>();
            job->shortname = path;
            job->crate_path = path;
            job->by_path = true;
            job->temporary = mode == kDownloadTemporarily;
            job->started_ms = GetTickCount64();
            jobs.push_back(job);
        }
        Output::send<LogLevel::Normal>(STR("[store] {} crate song(s): asking {} in {} request(s) (session token {})\n"), paths.size(), base,
                                       (paths.size() + 63) / 64, token.empty() ? STR("none") : STR("found"));
        {
            std::lock_guard lock(g_song_mutex);
            g_song_installs.insert(g_song_installs.end(), jobs.begin(), jobs.end());
        }
        std::thread(song_fetch_batch, std::move(jobs), base, token).detach();
    }

    // The package file of a /Game/... object path, as the engine's file system sees it (pak contents included).
    auto package_file_exists(const StringType& object_path) -> bool
    {
        StringType pkg = object_path;
        if (const size_t dot = pkg.find(L'.', pkg.rfind(L'/') == StringType::npos ? 0 : pkg.rfind(L'/')); dot != StringType::npos) pkg.resize(dot);
        if (!pkg.starts_with(L"/Game/")) return false;
        const StringType file = L"../../../Fuser/Content/" + pkg.substr(6) + L".uasset";
        Call c(STR("/Script/Engine.BlueprintPathsLibrary:FileExists"));
        if (!c.ok()) return false;
        c.set_string(STR("InPath"), file.c_str());
        return c.call(cdo(STR("/Script/Engine.Default__BlueprintPathsLibrary"))) && c.get<bool>(STR("ReturnValue"));
    }

    auto mount_pak(const wchar_t* path, bool* mounted) -> bool
    {
        __try
        {
            using GetManager = void* (*)();
            using FindPlatformFile = void* (*)(void*, const wchar_t*);
            using Mount = bool (*)(void*, const wchar_t*, uint32, const wchar_t*, bool);
            void* manager = reinterpret_cast<GetManager>(at(&VerifiedBuild::platform_file_manager))();
            void* pak_file = manager ? reinterpret_cast<FindPlatformFile>(at(&VerifiedBuild::find_platform_file))(manager, L"PakFile") : nullptr;
            if (!pak_file) return false;
            *mounted = reinterpret_cast<Mount>(at(&VerifiedBuild::pak_mount))(pak_file, path, 0, nullptr, true);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    struct SoftPath // FSoftObjectPath: AssetPathName, then SubPathString (an FString, left empty)
    {
        FName asset;
        void* sub_data;
        int32 sub_num, sub_max;
    };
    auto cel_mgr(UObject* ctx, uint8* state) -> void*
    {
        __try
        {
            using GetCelMgr = void* (*)(void*);
            void* mgr = reinterpret_cast<GetCelMgr>(at(&VerifiedBuild::cel_mgr_of))(ctx);
            if (mgr && state) *state = *(static_cast<uint8*>(mgr) + kCelMgrState);
            return mgr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }
    auto register_song_meta(void* mgr, TArray<SoftPath>* paths) -> bool
    {
        __try
        {
            using Register = void (*)(void*, void*, int32);
            reinterpret_cast<Register>(at(&VerifiedBuild::cel_mgr_register))(mgr, paths, 2);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }


    // ---- P2P tunnel --------------------------------------------------------------------------------------------
    // The LAN feasibility test: FUSER's game traffic through the loader's UDP tunnel, endpoints from the ini.
    //   host (P2PTunnelHostHere=1: this process, in front of a server on this machine, e.g. an FDS):
    //     P2PTunnelListen=<udp port>; forwards to P2PTunnelGamePort (default: HostAddress's port, 7777) and
    //     HostBeaconPort (7877).
    //   joiner: P2PTunnelPeer=<host ip:port>, P2PLocalPort (default 17777), and JoinAddressOverride=127.0.0.1:<port>
    //     so the matchmaking join opens the tunnel's local end.
    auto drive_p2p_tunnel() -> void
    {
        static bool done = false;
        if (done || !verified_build()) return;
        done = true;
        const StringType listen = ini_value(STR("P2PTunnelListen"), STR(""));
        const StringType peer = ini_value(STR("P2PTunnelPeer"), STR(""));
        if (!listen.empty() && ini_value(STR("P2PTunnelHostHere"), STR("0")) == STR("1"))
        {
            const StringType address = ini_value(STR("HostAddress"), STR("127.0.0.1:7777"));
            const size_t colon = address.rfind(L':');
            const int default_port = colon == StringType::npos ? 7777 : _wtoi(address.c_str() + colon + 1);
            p2p::HostConfig cfg{};
            cfg.listen_port = static_cast<uint16_t>(_wtoi(listen.c_str()));
            cfg.game_port = static_cast<uint16_t>(_wtoi(ini_value(STR("P2PTunnelGamePort"), std::to_wstring(default_port).c_str()).c_str()));
            cfg.beacon_port = static_cast<uint16_t>(_wtoi(ini_value(STR("HostBeaconPort"), STR("7877")).c_str()));
            p2p::start_host(cfg);
        }
        else if (!peer.empty())
        {
            p2p::JoinConfig cfg{};
            cfg.host = peer;
            cfg.local_port = static_cast<uint16_t>(_wtoi(ini_value(STR("P2PLocalPort"), STR("17777")).c_str()));
            p2p::start_join(cfg);
            if (ini_value(STR("JoinAddressOverride"), STR("")).empty())
                Output::send<LogLevel::Warning>(STR("[p2p] join: JoinAddressOverride is not set; matchmaking joins won't use the tunnel\n"));
        }
    }

    // ---- P2P signaling -------------------------------------------------------------------------------------------
    // With P2P=1, a player-hosted session's host (the creator's game, whose tunnel end forwards to the server on its
    // machine - see "player-hosted sessions"; nothing starts the host role until that is written) and each joiner run
    // a worker: fuserd's p2p/config/ for the STUN ports, STUN from the tunnel
    // socket, the interface + STUN candidates to p2p/host/ or p2p/join/, then p2p/poll/ (the other side's candidates,
    // new joiners) and p2p/report/ (what the agent found). The worker only connects the tunnel and reports; the game
    // still joins the advertised address (drive_p2p_join sends it through the tunnel). Ctrl+Shift+N (or P2PProbe=1
    // at startup) runs the STUN part alone.
    struct P2PApi
    {
        StringType base, token;
    };

    // status: the HTTP status (0 when the request never got an answer).
    auto p2p_call(const P2PApi& api, const CharType* path, const std::string& body, jsonl::Value* results, StringType* err, int* status = nullptr) -> bool
    {
        StringType headers = L"Content-Type: application/json\r\n";
        if (!api.token.empty()) headers += L"Authorization: JWT " + api.token + L"\r\n";
        const HttpResult res = http_request(body.empty() ? L"GET" : L"POST", api.base + path, headers, body, nullptr);
        if (status) *status = res.error.empty() ? static_cast<int>(res.status) : 0;
        jsonl::Value root;
        const bool parsed = jsonl::parse(res.body, &root);
        if (!res.error.empty() || res.status != 200 || !parsed)
        {
            const std::string msg = parsed ? root["error"]["message"].str() : res.body.substr(0, 120);
            *err = std::format(STR("HTTP {} {}{}"), res.status, widen(res.error), widen(msg));
            return false;
        }
        *results = root["results"];
        return true;
    }

    auto p2p_candidates_json(const std::vector<p2p::Candidate>& cs) -> std::string
    {
        std::string out = "[";
        for (size_t i = 0; i < cs.size(); ++i)
            out += std::format("{}{{\"type\":\"{}\",\"ip\":\"{}\",\"port\":{}}}", i ? "," : "", cs[i].type, cs[i].ip, cs[i].port);
        return out + "]";
    }
    auto p2p_candidates(const jsonl::Value& arr) -> std::vector<p2p::Candidate>
    {
        std::vector<p2p::Candidate> out;
        for (size_t i = 0; i < arr.size() && i < 8; ++i)
        {
            const jsonl::Value& c = arr.at(i);
            const int64_t port = c["port"].i64();
            if (port > 0 && port < 65536) out.push_back(p2p::Candidate{c["type"].str(), c["ip"].str(), static_cast<uint16_t>(port)});
        }
        return out;
    }
    // The pair key from its hex text (64 digits). Anything malformed gives an all-zero key, which the other side won't
    // share, so the pair simply never connects (no exceptions on the worker thread).
    auto p2p_key(const std::string& hex) -> p2p::Key
    {
        p2p::Key k{};
        const auto digit = [](char c) -> int {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        };
        if (hex.size() != 2 * k.size()) return k;
        for (size_t i = 0; i < k.size(); ++i)
        {
            const int hi = digit(hex[2 * i]), lo = digit(hex[2 * i + 1]);
            if (hi < 0 || lo < 0) return p2p::Key{};
            k[i] = static_cast<uint8_t>(hi << 4 | lo);
        }
        return k;
    }
    auto p2p_cands_text(const std::vector<p2p::Candidate>& cs) -> StringType
    {
        StringType out;
        for (const p2p::Candidate& c : cs) out += std::format(STR(" {} {}:{}"), widen(c.type), widen(c.ip), c.port);
        return out;
    }

    // p2p/config/ + STUN from the tunnel socket: the candidates to register, and the mapping. False when P2P is off.
    auto p2p_gather(const P2PApi& api, std::vector<p2p::Candidate>* cands, std::string* mapping) -> bool
    {
        jsonl::Value cfg;
        StringType err;
        if (!p2p_call(api, STR("p2p/config/"), {}, &cfg, &err))
        {
            Output::send<LogLevel::Warning>(STR("[p2p] p2p/config/ failed: {}\n"), err);
            return false;
        }
        if (!cfg["enabled"].boolean())
        {
            Output::send<LogLevel::Normal>(STR("[p2p] fuserd has P2P off (p2p.enabled)\n"));
            return false;
        }
        std::vector<std::pair<std::wstring, uint16_t>> servers;
        for (size_t i = 0; i < cfg["stun"].size(); ++i)
            servers.emplace_back(widen(cfg["stun"].at(i)["host"].str()), static_cast<uint16_t>(cfg["stun"].at(i)["port"].i64()));
        if (!p2p::open(static_cast<uint16_t>(_wtoi(ini_value(STR("P2PTunnelPort"), STR("0")).c_str())))) return false;
        const p2p::StunResult stun = p2p::stun(servers);
        // P2PSkipLan=1 leaves the interface addresses out, to test the public (punched) path between two machines
        // behind one router (which then needs hairpinning).
        // Adapters without a default gateway (host-only virtual ones) are left out unless P2PAllAdapters=1.
        std::vector<std::wstring> skipped;
        *cands = ini_value(STR("P2PSkipLan"), STR("0")) == STR("1")
                     ? std::vector<p2p::Candidate>{}
                     : p2p::interface_candidates(ini_value(STR("P2PAllAdapters"), STR("0")) == STR("1"), &skipped);
        if (!skipped.empty())
        {
            StringType names;
            for (const std::wstring& n : skipped) names += (names.empty() ? STR("") : STR(", ")) + n;
            Output::send<LogLevel::Normal>(STR("[p2p] adapters without a default gateway left out: {}\n"), names);
        }
        cands->insert(cands->end(), stun.srflx.begin(), stun.srflx.end());
        *mapping = stun.mapping;
        Output::send<LogLevel::Normal>(STR("[p2p] STUN:{} mapping {}; candidates:{}\n"), stun.detail, widen(stun.mapping), p2p_cands_text(*cands));
        return !cands->empty();
    }

    auto p2p_send_reports(const P2PApi& api, const StringType& gsid) -> void
    {
        for (const p2p::Report& r : p2p::take_reports())
        {
            jsonl::Value ignored;
            StringType err;
            const std::string body = std::format("{{\"game_session_id\":\"{}\",\"peer_id\":{},\"state\":\"{}\",\"path\":\"{}\",\"rtt_ms\":{}}}", narrow(gsid),
                                                 r.peer_id, r.state, r.path, r.rtt_ms);
            if (!p2p_call(api, STR("p2p/report/"), body, &ignored, &err))
                Output::send<LogLevel::Warning>(STR("[p2p] p2p/report/ failed: {}\n"), err);
        }
    }

    struct P2PRole
    {
        bool host = false;
        StringType gsid;
        uint16_t game_port = 7777, beacon_port = 7877, local_port = 17777;
    };

    // The worker: runs until the session ends (fuserd says it's unknown) or a newer worker starts.
    //
    // Recovery: a poll that gets no answer, a 429 or a server error is retried - the session isn't over because the
    // network blinked or fuserd restarted - until kP2PGiveUpMs of failures; a registration fuserd has forgotten (its
    // polls failed for longer than the two-minute TTL) is made again. Every kP2PNetCheckMs the worker also compares
    // this machine's adapter addresses with the ones it registered: on a change (Wi-Fi to cable, a new DHCP lease, a
    // VPN) it gathers again from the same tunnel socket and registers again. A joiner keeps its peer id and key, and
    // allocates a new relay (the old allocation belongs to the old address). The other side's poll brings the new
    // candidates, and its agent probes them.
    constexpr uint64 kP2PGiveUpMs = 120000, kP2PNetCheckMs = 5000;
    auto p2p_lan_signature() -> std::string
    {
        std::string out;
        for (const p2p::Candidate& c : p2p::interface_candidates(true)) out += c.ip + ",";
        return out;
    }

    auto p2p_worker(P2PApi api, P2PRole role, uint64_t generation) -> void
    {
        // A joiner that gives up lets its pending join fall back at once (drive_p2p_join).
        struct FailFlag
        {
            bool joiner;
            uint64_t generation;
            bool armed = true;
            ~FailFlag()
            {
                if (joiner && armed) g_p2p_join_failed.store(generation);
            }
        } fail{!role.host, generation};
        const CharType* side = role.host ? STR("host") : STR("join");
        std::vector<p2p::Candidate> cands;
        std::string mapping;
        if (!p2p_gather(api, &cands, &mapping)) return;
        std::string lan = p2p_lan_signature();
        jsonl::Value res;
        StringType err;
        p2p::JoinRole jr{};
        jr.local_port = role.local_port;
        jr.relay_only = ini_value(STR("P2PForceRelay"), STR("0")) == STR("1");
        std::optional<p2p::Candidate> relay_cand;
        const auto body_of = [&](const std::vector<p2p::Candidate>& cs) {
            return std::format("{{\"game_session_id\":\"{}\",\"candidates\":{},\"mapping\":\"{}\"}}", narrow(role.gsid), p2p_candidates_json(cs), mapping);
        };
        // Registers this side; the first time it also takes the tunnel role. new_relay: allocate the relay again.
        const auto register_side = [&](bool first, bool new_relay) -> bool {
            if (role.host)
            {
                if (first) p2p::become_host(p2p::HostRole{role.game_port, role.beacon_port, false});
                if (!p2p_call(api, STR("p2p/host/"), body_of(cands), &res, &err))
                {
                    Output::send<LogLevel::Error>(STR("[p2p] host: p2p/host/ failed: {}\n"), err);
                    return false;
                }
                Output::send<LogLevel::Normal>(STR("[p2p] host: registered {}for session {}\n"), first ? STR("") : STR("again "), role.gsid);
                return true;
            }
            if (!p2p_call(api, STR("p2p/join/"), body_of(cands), &res, &err))
            {
                Output::send<LogLevel::Error>(STR("[p2p] join: p2p/join/ failed: {}\n"), err);
                return false;
            }
            if (first)
            {
                jr.peer_id = static_cast<uint32_t>(res["peer_id"].i64());
                jr.key = p2p_key(res["key"].str());
                if (!p2p::become_joiner(jr)) return false;
            }
            Output::send<LogLevel::Normal>(STR("[p2p] join: registered {}for session {} as peer {:08x}{}\n"), first ? STR("") : STR("again "), role.gsid,
                                           jr.peer_id, res["host"].is_null() ? STR("; waiting for the host") : STR(""));
            // The relay: allocate an address on fuserd's TURN server and register it as one more candidate; the
            // host sends to it like any other. P2PRelay=0 leaves it out; P2PForceRelay=1 registers nothing else.
            const jsonl::Value relay = res["relay"];
            if (relay.is_null())
            {
                if (jr.relay_only && first) Output::send<LogLevel::Warning>(STR("[p2p] join: P2PForceRelay is set but fuserd offers no relay\n"));
                return true;
            }
            if (ini_value(STR("P2PRelay"), STR("1")) != STR("1") && !jr.relay_only) return true;
            if (new_relay || !relay_cand)
            {
                const p2p::RelayInfo info{widen(relay["host"].str()), static_cast<uint16_t>(relay["port"].i64()), relay["realm"].str(),
                                          relay["username"].str(), relay["password"].str()};
                relay_cand = p2p::relay_allocate(info); // releases a previous allocation
            }
            if (!relay_cand) return true;
            std::vector<p2p::Candidate> with = jr.relay_only ? std::vector<p2p::Candidate>{} : cands;
            with.push_back(*relay_cand);
            jsonl::Value again;
            if (p2p_call(api, STR("p2p/join/"), body_of(with), &again, &err))
            {
                res = again;
                Output::send<LogLevel::Normal>(STR("[p2p] join: registered the relay candidate {}:{}{}\n"), widen(relay_cand->ip), relay_cand->port,
                                               jr.relay_only ? STR(" (relay only)") : STR(""));
            }
            else
                Output::send<LogLevel::Warning>(STR("[p2p] join: re-registering with the relay candidate failed: {}\n"), err);
            return true;
        };
        if (!register_side(true, true)) return;
        fail.armed = false; // registered: from here a missing path is the join's timeout to judge
        const uint64_t started = GetTickCount64();
        uint64 failing_since = 0, last_net_check = started;
        for (;;)
        {
            if (role.host)
            {
                std::vector<p2p::PeerInfo> peers;
                const jsonl::Value& list = res["peers"];
                for (size_t i = 0; i < list.size() && i < 24; ++i)
                    peers.push_back(p2p::PeerInfo{static_cast<uint32_t>(list.at(i)["peer_id"].i64()), p2p_key(list.at(i)["key"].str()),
                                                  p2p_candidates(list.at(i)["candidates"])});
                p2p::set_peers(peers);
            }
            else if (!res["host"].is_null())
                p2p::set_host(p2p_candidates(res["host"]["candidates"]));
            p2p_send_reports(api, role.gsid);
            // The host keeps polling every second (new joiners must be authorised before their probes count); a
            // joiner slows down once connected (its registration lives two minutes).
            const bool fast = role.host || !p2p::joiner_connected() || GetTickCount64() - started < 30000 || failing_since;
            for (int waited = 0; waited < (fast ? 1000 : 15000); waited += 250)
            {
                if (g_p2p_generation.load() != generation) return;
                Sleep(250);
            }
            p2p_send_reports(api, role.gsid);
            uint64 now = GetTickCount64();
            if (now - last_net_check >= kP2PNetCheckMs)
            {
                last_net_check = now;
                const std::string now_lan = p2p_lan_signature();
                if (now_lan != lan)
                {
                    Output::send<LogLevel::Normal>(STR("[p2p] {}: this machine's addresses changed ({} -> {}); gathering and registering again\n"), side,
                                                   widen(lan), widen(now_lan));
                    std::vector<p2p::Candidate> fresh;
                    std::string fresh_mapping;
                    if (p2p_gather(api, &fresh, &fresh_mapping))
                    {
                        cands = fresh;
                        mapping = fresh_mapping;
                        if (register_side(false, true)) lan = now_lan; // otherwise tried again at the next check
                    }
                }
            }
            const std::string poll = std::format("{{\"game_session_id\":\"{}\"}}", narrow(role.gsid));
            int status = 0;
            if (p2p_call(api, STR("p2p/poll/"), poll, &res, &err, &status))
            {
                if (failing_since)
                    Output::send<LogLevel::Normal>(STR("[p2p] {}: fuserd answers again after {} s\n"), side, (GetTickCount64() - failing_since) / 1000);
                failing_since = 0;
                continue;
            }
            now = GetTickCount64();
            const bool forgotten = status == 400 && err.find(STR("register with")) != StringType::npos;
            if (forgotten)
            {
                Output::send<LogLevel::Normal>(STR("[p2p] {}: fuserd forgot this side's registration; registering again\n"), side);
                if (register_side(false, false))
                {
                    failing_since = 0;
                    continue;
                }
            }
            if (forgotten || status == 0 || status == 429 || status >= 500)
            {
                if (!failing_since)
                {
                    failing_since = now;
                    Output::send<LogLevel::Warning>(STR("[p2p] {}: p2p/poll/ failed ({}); retrying for up to {} s\n"), side, err, kP2PGiveUpMs / 1000);
                }
                if (now - failing_since < kP2PGiveUpMs) continue;
            }
            Output::send<LogLevel::Normal>(STR("[p2p] {}: session {} over ({}); tunnel role ended\n"), side, role.gsid, err);
            if (g_p2p_generation.load() == generation) p2p::stop();
            return;
        }
    }

    // Game thread (it reads the backend's token and URL): start the worker for a session. True when it started.
    auto start_p2p(bool host, const StringType& gsid) -> bool
    {
        if (ini_value(STR("P2P"), STR("0")) != STR("1") || gsid.empty()) return false;
        P2PApi api{store_api_base(), backend_token()};
        if (api.base.empty())
        {
            Output::send<LogLevel::Warning>(STR("[p2p] no fuserd URL (StoreURL / Live ClientURL); P2P not started\n"));
            return false;
        }
        P2PRole role{};
        role.host = host;
        role.gsid = gsid;
        const StringType address = ini_value(STR("HostAddress"), STR("127.0.0.1:7777"));
        const size_t colon = address.rfind(L':');
        role.game_port = static_cast<uint16_t>(colon == StringType::npos ? 7777 : _wtoi(address.c_str() + colon + 1));
        role.beacon_port = static_cast<uint16_t>(_wtoi(ini_value(STR("HostBeaconPort"), STR("7877")).c_str()));
        role.local_port = static_cast<uint16_t>(_wtoi(ini_value(STR("P2PLocalPort"), STR("17777")).c_str()));
        const uint64_t generation = ++g_p2p_generation;
        Output::send<LogLevel::Normal>(STR("[p2p] {} session {}: starting the tunnel agent\n"), host ? STR("hosting") : STR("joining"), gsid);
        std::thread(p2p_worker, api, role, generation).detach();
        return true;
    }

    // Game thread, each tick: a pending P2P join opens the tunnel's local end once a path is up, or falls back to
    // the stock join (the advertised address, or JoinAddressOverride) when the agent gave up or the time ran out.
    auto drive_p2p_join() -> void
    {
        PendingP2PJoin& j = g_p2p_join;
        if (!j.active) return;
        const uint64 now = GetTickCount64();
        if (j.generation != g_p2p_generation.load())
        {
            j.active = false; // another session started meanwhile
            return;
        }
        if (p2p::joiner_connected())
        {
            j.active = false;
            const StringType port = ini_value(STR("P2PLocalPort"), STR("17777"));
            Output::send<LogLevel::Normal>(STR("[p2p] join: tunnel up after {} ms; joining through it (127.0.0.1:{})\n"), now - j.started, port);
            mm_set_state(j.args[0], 0);
            open_url(STR("127.0.0.1:") + port + j.url_options);
            return;
        }
        const bool gave_up = g_p2p_join_failed.load() == j.generation;
        if (!gave_up && now < j.deadline) return;
        j.active = false;
        const StringType over = ini_value(STR("JoinAddressOverride"), STR(""));
        Output::send<LogLevel::Warning>(STR("[p2p] join: {} after {} ms; joining {} instead\n"), gave_up ? STR("the tunnel agent gave up") : STR("no tunnel path"),
                                        now - j.started, over.empty() ? StringType(STR("the advertised address")) : over);
        if (!over.empty())
        {
            mm_set_state(j.args[0], 0);
            open_url(over + j.url_options);
            return;
        }
        g_mm_join_original(j.args[0], j.args[1], j.args[2], j.args[3], j.args[4], j.args[5], j.args[6], j.args[7], j.args[8]);
    }

#if FNL_DEBUG
    // Ctrl+Shift+N / P2PProbe=1 (debug builds): the STUN part alone, for checking a network.
    auto p2p_probe() -> void
    {
        P2PApi api{store_api_base(), backend_token()};
        if (api.base.empty()) return;
        std::thread([api] {
            std::vector<p2p::Candidate> cands;
            std::string mapping;
            p2p_gather(api, &cands, &mapping);
        }).detach();
    }

    // ---- The song transfer test mode (debug builds) -------------------------------------------------------------
    // Ctrl+Shift+X on two machines moves one song over a transfer pair, with no fuserd. Both set P2PTunnelPort and
    // XferTestPeer=<the other machine's ip:port>; the sender also sets XferTestSend=<path to <X>_P.pak> (its .sig
    // beside it) and optionally SendSongsKBps (2048). The receiver takes the sizes and hashes from the sender's
    // manifest, checks them and the pak layout, and writes the files into its session folder's xfer-test
    // subfolder; nothing is installed (the song transfers below do that). A fixed pair id and an all-zero key: a test only.
    constexpr uint32_t kXferTestPair = 0x58464552; // 'XFER'
    auto xfer_test() -> void
    {
        const StringType peer = ini_value(STR("XferTestPeer"), STR(""));
        const size_t colon = peer.rfind(L':');
        if (peer.empty() || colon == StringType::npos)
        {
            Output::send<LogLevel::Warning>(STR("[p2p] xfer test: set XferTestPeer=<the other machine's ip:port> (and P2PTunnelPort) in FuserNative.ini\n"));
            return;
        }
        const StringType send = ini_value(STR("XferTestSend"), STR(""));
        const uint16 port = static_cast<uint16>(_wtoi(ini_value(STR("P2PTunnelPort"), STR("0")).c_str()));
        const uint32_t cap = static_cast<uint32_t>(_wtoi(ini_value(STR("SendSongsKBps"), STR("2048")).c_str()));
        const std::filesystem::path dir = session_song_dir() / L"xfer-test";
        std::thread([peer, colon, send, port, cap, dir] {
            if (!p2p::open(port)) return;
            p2p::xfer_cancel(kXferTestPair); // a repeat replaces the last test
            const std::vector<p2p::Candidate> cands{{"host", narrow(peer.substr(0, colon)), static_cast<uint16>(_wtoi(peer.c_str() + colon + 1))}};
            if (!send.empty())
            {
                std::filesystem::path sig = send;
                sig.replace_extension(L".sig");
                p2p::XferSend s{};
                s.pair_id = kXferTestPair;
                s.peer = cands;
                s.pak = send;
                s.sig = sig.wstring();
                s.name = narrow(std::filesystem::path(send).stem().wstring());
                s.cap_kbps = cap ? cap : 2048;
                Output::send<LogLevel::Normal>(STR("[p2p] xfer test: sending {} to {}\n"), send, peer);
                p2p::xfer_send(s);
            }
            else
            {
                p2p::XferRecv r{};
                r.pair_id = kXferTestPair;
                r.peer = cands;
                r.dir = dir.wstring();
                Output::send<LogLevel::Normal>(STR("[p2p] xfer test: receiving from {} into {}\n"), peer, r.dir);
                p2p::xfer_recv(r);
            }
        }).detach();
    }
#endif

    // ---- Other players' crate songs this machine lacks ----------------------------------------------------------
    // Each player state's DJPlayerState (HypePlayerState._djPlayerState, a separate object: the player state itself
    // has no _crateData) replicates its crate (_crateData, FCrateData; songs +0x38: TArray<TSoftObjectPtr<
    // UCelSongSet>>, 0x28 per element, the FSoftObjectPath's AssetPathName at +0x10) - at a join and on every crate
    // edit. Twice a second in a multiplayer game, every song path not seen before is checked: loaded, or its package
    // file present (pak contents included), or else it is not on this machine and fuserd is asked for it by path
    // (resolve normalises the path). Official DLC never gets here: its Meta packages ship in dlcmanifest.pak, and the
    // game's own DLC path handles an unowned one. A scan's missing songs go to fuserd in one batch (request_crate_songs).
    // Download Songs = No (the old StoreAutoInstall=0 reads the same) pauses it without marking paths, so turning
    // it back on mid-session picks up what is still missing.
    // Only performers' crates are fetched from (HypePlayerState._team DJ): a viewer never plays its
    // crate, so its songs are not needed here, and on a Main Stage of 50 viewers every machine would otherwise fetch
    // all of theirs. A viewer still counts as an owner who can send a song a performer's crate holds. A viewer who
    // becomes a performer (or a joiner whose team arrives later) is fetched from at the next scan: nothing is
    // recorded for the crate songs passed over.
    // Each path is asked for once while its fetch is under way, and never again once it is here. A fetch that ends
    // without the song is tried again after kCrateRetryMs (a player who has it may join, or turn Send Songs on), up
    // to kCrateMaxTries times in a session. The record is cleared when the session ends (drive_song_transfers): it
    // used to last the whole run, so a song that failed once was never asked for again (2026-10-04).
    struct CrateTry
    {
        uint64 retry_at = 0; // 0: its fetch is under way, or it is on this machine
        int tries = 0;
    };
    constexpr uint64 kCrateRetryMs = 120000;
    constexpr int kCrateMaxTries = 3;
    std::unordered_map<uint64, CrateTry> g_crate_tries{};  // by crate path FName key (game thread)
    std::mutex g_crate_failed_mu;
    std::vector<StringType> g_crate_failed{};                // crate paths whose fetch ended without the song (any thread)
    auto crate_fetch_failed(const StringType& path) -> void
    {
        std::lock_guard lock(g_crate_failed_mu);
        g_crate_failed.push_back(path);
    }
    auto crate_path_key(const StringType& path) -> uint64 // game thread
    {
        const FName name(path.c_str(), FNAME_Add);
        uint64 key = 0;
        std::memcpy(&key, &name, sizeof(key));
        return key;
    }
    std::unordered_map<UObject*, uint8> g_crate_teams_logged{}; // each player's team as last logged by the crate scan
    auto crate_tries_reset() -> void // game thread: the session ended
    {
        g_crate_tries.clear();
        g_crate_teams_logged.clear();
        std::lock_guard lock(g_crate_failed_mu);
        g_crate_failed.clear();
    }
    std::unordered_map<uint64, std::vector<int32>> g_crate_owners; // crate path (FName key) -> the hype user ids holding it
    std::unordered_map<UObject*, int32> g_crate_sizes_logged{};
    constexpr uint8 kTeamDJ = 1; // ETeam: 0 unknown, 1 DJ (performer), 2 Spectator (viewer)
    auto drive_crate_scan() -> void
    {
        static uint64 last = 0;
        const uint64 now = GetTickCount64();
        if (now - last < 500 || !verified_build() || download_mode() == kDownloadNo) return;
        last = now;
        UObject* mps = mp_state(game_instance());
        if (!mps) return;
        UObject* local = host_player_state(game_instance());
        // fetches that ended without their song: try again later, or give up for this session
        {
            std::vector<StringType> failed;
            {
                std::lock_guard lock(g_crate_failed_mu);
                failed.swap(g_crate_failed);
            }
            for (const StringType& path : failed)
            {
                auto it = g_crate_tries.find(crate_path_key(path));
                if (it == g_crate_tries.end() || it->second.retry_at != 0) continue;
                const bool again = it->second.tries < kCrateMaxTries;
                it->second.retry_at = again ? now + kCrateRetryMs : UINT64_MAX;
                Output::send<LogLevel::Normal>(STR("[store] {}: not fetched (try {} of {}); {}\n"), path, it->second.tries, kCrateMaxTries,
                                               again ? STR("asking again in 2 minutes") : STR("giving up for this session"));
            }
        }
        std::vector<StringType> missing;
        for (UObject* ps : players(mps))
        {
            UObject* dj = prop<UObject*>(ps, STR("_djPlayerState"));
            auto* crate = dj ? dj->GetValuePtrByPropertyNameInChain<uint8>(STR("_crateData")) : nullptr;
            if (!crate) continue;
            const uint8* data = *reinterpret_cast<uint8* const*>(crate + 0x38);
            const int32 num = *reinterpret_cast<const int32*>(crate + 0x40);
            if (auto [it, fresh] = g_crate_sizes_logged.try_emplace(ps, -1); it->second != num)
            {
                it->second = num;
                Output::send<LogLevel::Verbose>(STR("[store] crate scan: {} has {} crate song(s)\n"), ps->GetName(), num);
            }
            const uint8 team = prop<uint8>(ps, STR("_team"));
            const bool performer = team == kTeamDJ;
            if (auto [it, fresh] = g_crate_teams_logged.try_emplace(ps, 0xff); it->second != team)
            {
                it->second = team;
                if (ps != local && num > 0)
                    Output::send<LogLevel::Normal>(STR("[store] crate scan: {} is {}: {}\n"), ps->GetName(),
                                                   performer ? STR("a performer") : team == 2 ? STR("a viewer") : STR("not on a team yet"),
                                                   performer ? STR("fetching its crate songs") : STR("its crate songs are not fetched"));
            }
            for (int32 i = 0; data && i < num && i < 256; ++i)
            {
                const FName& path = *reinterpret_cast<const FName*>(data + static_cast<size_t>(i) * 0x28 + 0x10);
                uint64 key = 0;
                std::memcpy(&key, &path, sizeof(key));
                if (key && ps != local) // who could send this song (fuserd ids only: -1 = none, 900001+ = lobby placeholders)
                    if (const int32 id = prop<int32>(ps, STR("_hypeUserId")); id > 0 && id < 900001)
                        if (auto& owners = g_crate_owners[key]; std::ranges::find(owners, id) == owners.end()) owners.push_back(id);
                if (key == 0 || !performer) continue; // a viewer's crate: owners only, nothing fetched (see above)
                auto [entry, fresh] = g_crate_tries.try_emplace(key);
                CrateTry& t = entry->second;
                if (!fresh && (t.retry_at == 0 || now < t.retry_at)) continue;
                const StringType s = path.ToString();
                if (s.empty() || s == STR("None") || UObjectGlobals::StaticFindObject(nullptr, nullptr, s.c_str()) || package_file_exists(s))
                {
                    t = CrateTry{}; // here: not looked at again
                    continue;
                }
                ++t.tries;
                t.retry_at = 0;
                Output::send<LogLevel::Normal>(STR("[store] {}'s crate has {}, which is not on this machine: asking fuserd{}\n"), ps->GetName(), s,
                                               t.tries > 1 ? std::format(STR(" (try {} of {})"), t.tries, kCrateMaxTries) : StringType{});
                missing.push_back(s);
            }
        }
        request_crate_songs(missing);
    }

    // UnlockMgr's name map (+0x128, TMap<FName, FUnlockable*>) is built once, at its init (FUN_1407427e0 per unlock
    // type: GetAllRows into the type's row array, then one map entry per row). A row added later must be put in
    // both, the way that loop does it: FUN_140341bf0 allocates the set element; it holds {name, row, HashNextId -1};
    // FUN_140358bc0 links it at GetTypeHash(name) = Number + FUN_1409eab10(ComparisonIndex). Without it,
    // GetUnlockState returns LockedHidden for the song, and the song lists leave it out.
    constexpr size_t kUnlockNameMap = 0x128, kUnlockSongRows = 0x98;
    auto unlock_map_has(void* unlock_mgr, FName name, bool* has) -> bool
    {
        __try
        {
            using Find = int32* (*)(void*, int32*, FName);
            int32 out = -1;
            *has = *reinterpret_cast<Find>(at(&VerifiedBuild::unlock_map_find))(static_cast<uint8*>(unlock_mgr) + kUnlockNameMap, &out, name) != -1;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
    struct SetAlloc // FSparseArrayAllocationInfo
    {
        int32 index, pad;
        uint64* element;
    };
    auto unlock_map_add(void* unlock_mgr, uint64 key, uint8* row) -> bool
    {
        __try
        {
            using AllocFn = void (*)(void*, SetAlloc*);
            using Hash = int32 (*)(uint32);
            using Link = void (*)(void*, int32*, uint32, void*, int32, void*);
            uint8* map = static_cast<uint8*>(unlock_mgr) + kUnlockNameMap;
            SetAlloc slot{};
            reinterpret_cast<AllocFn>(at(&VerifiedBuild::unlock_map_alloc))(map, &slot);
            if (!slot.element) return false;
            slot.element[0] = key;
            slot.element[1] = reinterpret_cast<uint64>(row);
            *reinterpret_cast<int32*>(slot.element + 2) = -1;
            const uint32 hash = static_cast<uint32>(key >> 32) + static_cast<uint32>(reinterpret_cast<Hash>(at(&VerifiedBuild::fname_hash))(static_cast<uint32>(key)));
            int32 id = -1;
            reinterpret_cast<Link>(at(&VerifiedBuild::unlock_map_link))(map, &id, hash, slot.element, slot.index, nullptr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
    // The song's unlock row: in DT_UnlocksSongs (a copy of the template row, which is unlocked) and in UnlockMgr's
    // name map and song rows. *had says whether UnlockMgr already had it.
    auto ensure_unlock_row(const StringType& shortname, UObject** mgr_out, bool* had) -> StringType
    {
        uint8* row = unlocks_add_song_row(shortname.c_str());
        if (!row) return STR("could not add a DT_UnlocksSongs row");
        UObject* gi = game_instance();
        auto** slot = gi ? gi->GetValuePtrByPropertyNameInChain<UObject*>(STR("theUnlockMgr")) : nullptr;
        UObject* mgr = slot ? *slot : nullptr;
        if (!mgr) return STR("no UnlockMgr");
        *mgr_out = mgr;
        const FName name(shortname.c_str(), FNAME_Add);
        if (!unlock_map_has(mgr, name, had)) return STR("UnlockMgr name lookup FAULTED");
        if (!*had)
        {
            uint64 key = 0;
            std::memcpy(&key, &name, sizeof(key));
            if (!unlock_map_add(mgr, key, row)) return STR("UnlockMgr name map insert FAULTED");
            reinterpret_cast<TArray<uint8*>*>(reinterpret_cast<uint8*>(mgr) + kUnlockSongRows)->Add(row);
        }
        return {};
    }
    auto unlock_song(const StringType& shortname) -> StringType
    {
        UObject* mgr = nullptr;
        bool has = false;
        if (StringType err = ensure_unlock_row(shortname, &mgr, &has); !err.empty()) return err;
        const FName name(shortname.c_str(), FNAME_Add);
        Call c(STR("/Script/Hype.UnlockMgr:GetUnlockState"));
        c.set<FName>(STR("unlockableName"), name).set<uint8>(STR("unlockType"), 1 /*Song*/);
        const int state = c.call(mgr) ? c.get<uint8>(STR("ReturnValue")) : -1;
        Output::send<LogLevel::Normal>(STR("[store] {}: unlock row {}; UnlockMgr GetUnlockState = {} (3 = Unlocked)\n"), shortname,
                                       has ? STR("was already in UnlockMgr") : STR("added to DT_UnlocksSongs and UnlockMgr"), state);
        return state == 3 ? StringType{} : std::format(STR("unlock state is {}, not Unlocked"), state);
    }

    auto song_p2p_request(const StringType& path) -> void; // song transfers, below

    // Game thread, each tick: mount and register the downloaded songs.
    auto drive_song_installs() -> void
    {
        std::vector<std::shared_ptr<SongInstall>> jobs;
        {
            std::lock_guard lock(g_song_mutex);
            jobs = g_song_installs;
        }
        for (const auto& job : jobs)
        {
            using Stage = SongInstall::Stage;
            Stage stage;
            {
                std::lock_guard lock(g_song_mutex);
                stage = job->stage;
            }
            if (stage == Stage::Failed)
            {
                // fuserd's sources came first: its catalogue, then the community link.
                const bool p2p = job->by_path && job->p2p_next;
                Output::send<LogLevel::Error>(STR("[store] {}: FAILED - {}\n"), job->shortname, job->error);
                job->stage = Stage::Done;
                if (p2p) song_p2p_request(job->shortname); // another player has it - ask them
                else
                {
                    ++g_sync_failed;
                    if (!job->crate_path.empty()) crate_fetch_failed(job->crate_path);
                }
                continue;
            }
            if (stage == Stage::Downloaded)
            {
                if (package_file_exists(job->meta_path))
                {
                    Output::send<LogLevel::Normal>(STR("[store] {}: {} is already mounted{}\n"), job->shortname, job->meta_path,
                                                   job->reused ? STR(" (files were already up to date)") : STR(" - the new files apply at the next launch"));
                }
                else
                {
                    bool mounted = false;
                    const bool called = mount_pak(job->pak_path.c_str(), &mounted);
                    const bool visible = package_file_exists(job->meta_path);
                    Output::send<LogLevel::Normal>(STR("[store] {}: mount {} -> {}; Meta package {}\n"), job->shortname, job->pak_path,
                                                   called ? (mounted ? STR("mounted") : STR("REFUSED")) : STR("FAULTED"),
                                                   visible ? STR("now visible") : STR("NOT visible"));
                    if (!called || !mounted || !visible)
                    {
                        job->error = STR("the pak did not mount");
                        job->stage = Stage::Failed;
                        continue;
                    }
                }
                job->stage = Stage::Registering;
                job->register_since = GetTickCount64();
                continue;
            }
            if (stage == Stage::Unlocking)
            {
                const StringType err = unlock_song(job->shortname);
                if (err.empty())
                {
                    Output::send<LogLevel::Normal>(STR("[store] {}: installed - in the song lists from their next refresh (crate edit: type in the search box)\n"), job->shortname);
                    job->stage = Stage::Done;
                    ++g_sync_ok;
                }
                else
                {
                    job->error = err;
                    job->stage = Stage::Failed;
                }
                continue;
            }
            if (stage == Stage::Registering)
            {
                // Registered = CelMgr's pass (FUN_1405adbd0) has run: it sets a non-DLC song's downloadState to OnDisc
                // (1), and only OnDisc / DownloadedOwned songs go into its visible-song map (+0x148), which the song
                // lists read. The object alone is loaded before that pass (the first test saw downloadState 0).
                uint8 state = 0xff;
                void* mgr = cel_mgr(game_instance(), &state);
                if (UObject* existing = UObjectGlobals::StaticFindObject(nullptr, nullptr, job->meta_path.c_str()))
                {
                    uint8 dl = 0xff;
                    std::memcpy(&dl, reinterpret_cast<const uint8*>(existing) + 0x140, 1);
                    if (dl == 1 || dl == 4)
                    {
                        Output::send<LogLevel::Normal>(STR("[store] {}: registered - {} loaded, downloadState {} ({} ms after the request)\n"),
                                                       job->shortname, existing->GetName(), static_cast<int>(dl), GetTickCount64() - job->started_ms);
                        job->stage = Stage::Unlocking;
                        continue;
                    }
                    if (state == 5) continue; // CelMgr's async load is still running
                }
                if (GetTickCount64() - job->register_since > 30000)
                {
                    job->error = std::format(STR("CelMgr never registered it (state {})"), static_cast<int>(state));
                    job->stage = Stage::Failed;
                    continue;
                }
                // Mode 2 only runs in states 2, 4 and 6 (it sets 5 while its async load runs): wait for one, and ask
                // again a little later if the song still hasn't appeared (at most 3 requests).
                const uint64 now = GetTickCount64();
                if (!mgr || (state != 2 && state != 4 && state != 6) || job->requests >= 3 ||
                    (job->requests > 0 && now - job->last_request < 2000))
                    continue;
                TArray<SoftPath> paths{};
                paths.Add(SoftPath{FName(job->meta_path.c_str(), FNAME_Add), nullptr, 0, 0});
                const bool ok = register_song_meta(mgr, &paths);
                FMemory::Free(paths.Data);
                ++job->requests;
                job->last_request = now;
                Output::send<LogLevel::Normal>(STR("[store] {}: CelMgr registration {} (mode 2, CelMgr state {}, request {})\n"), job->shortname,
                                               ok ? STR("requested") : STR("FAULTED"), static_cast<int>(state), job->requests);
                if (!ok)
                {
                    job->error = STR("CelMgr registration faulted");
                    job->stage = Stage::Failed;
                }
            }
        }
        std::lock_guard lock(g_song_mutex);
        std::erase_if(g_song_installs, [](const auto& j) { return j->stage == SongInstall::Stage::Done; });
    }

    // ---- Song transfers between players --------------------------------------------------------------------------
    // In any multiplayer session, a worker polls fuserd's p2p/song/poll/ every 2 s, saying whether
    // this machine sends (SendSongs). As an owner it answers the requests waiting on it: it finds <X>_P.pak in
    // custom_songs (or this run's session folder: songs received Temporarily are sent on), checks its layout and
    // offers it with this machine's tunnel candidates, or declines (not_found, not_custom, busy). As a requester -
    // a crate song that fuserd's catalogue doesn't have (Route B answered "not in fuserd's catalogue"), asked for
    // from every player whose crate holds it - it receives into the session folder's "incoming", accepts with its
    // candidates, and when the agent has the files checked (sha256 and layout) it moves them where Download Songs
    // says and hands them to the Route B install at its Downloaded stage. Every outcome is reported, so fuserd moves
    // on to the next owner after a failure. The transfers themselves are the agent's (p2p_tunnel.cpp).
    struct SongXfer
    {
        std::string id, role, path, song, state;
        uint32_t pair = 0;   // the agent pair running for the current attempt (0: none)
        bool told_transferring = false;
        uint64_t started = 0;
        std::string reason;          // fuserd's reason with a final state
        std::vector<int64> owners;   // requester: who was asked, for a retry
        int tries = 0;               // requester: retries so far (owners busy or not answering)
    };
    // A song to ask other players for. At most kMaxOpenSongRequests are open at once (fuserd's MaxOpenPerRequester):
    // the rest wait here in order, where every request used to go at once and all past the 8th were refused
    // (2026-10-04: 16 lost to "too many open song requests").
    struct SongRequest
    {
        std::string path;
        std::vector<int64> owners;
        int tries = 0;
        uint64 not_before = 0; // a retry waits until then
    };
    constexpr int kMaxOpenSongRequests = 8;
    constexpr int kSongRequestRetries = 3;
    std::mutex g_song_req_mu;
    std::vector<SongRequest> g_song_req_queue; // songs to request, in order
    std::atomic<uint64_t> g_song_gen{0};                                       // the worker's session; a new one ends the old
    std::atomic<uint64_t> g_song_worker_gen{0};                                // the running worker's generation (0: none)

    auto json_escape(const std::string& s) -> std::string
    {
        std::string out;
        for (char c : s)
        {
            if (c == '"' || c == '\\') out += '\\', out += c;
            else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
            else out += c;
        }
        return out;
    }

    // The crate path rule fuserd applies (songxfer.ShortName): /Game/DLC/Songs/<X>/Meta_<X>.Meta_<X> -> <X>.
    auto crate_song_name(const std::string& path) -> std::string
    {
        constexpr std::string_view prefix = "/Game/DLC/Songs/";
        if (!path.starts_with(prefix)) return {};
        const std::string rest = path.substr(prefix.size());
        const size_t slash = rest.find('/');
        if (slash == std::string::npos) return {};
        const std::string x = rest.substr(0, slash);
        if (x.empty() || x.size() > 96 || x.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos ||
            rest.substr(slash + 1) != "Meta_" + x + ".Meta_" + x)
            return {};
        return x;
    }

    // Where this machine has <X>_P.pak to send: custom_songs (any depth), else this run's session folder.
    auto find_song_pak(const std::string& x) -> std::filesystem::path
    {
        const std::wstring want = widen(x) + L"_P.pak";
        std::error_code ec;
        for (const std::filesystem::path& root : {song_dir(), session_song_dir()})
        {
            if (!std::filesystem::is_directory(root, ec)) continue;
            for (auto it = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            {
                if (ec) break;
                if (it->is_regular_file(ec) && _wcsicmp(it->path().filename().c_str(), want.c_str()) == 0) return it->path();
            }
        }
        return {};
    }

    // Game thread: a crate song fuserd doesn't have; ask the players whose crates hold it.
    auto song_p2p_request(const StringType& path) -> void
    {
        // Queued even before the session's worker is up (it drains the queue first): the crate scan usually finds a
        // missing song right after joining, before it starts (a request could be lost that way).
        if (download_mode() == kDownloadNo)
        {
            Output::send<LogLevel::Normal>(STR("[songs] {}: not asking other players (Download Songs is No)\n"), path);
            return;
        }
        if (crate_song_name(narrow(path)).empty())
        {
            Output::send<LogLevel::Normal>(STR("[songs] {}: not a custom song path; not asking other players\n"), path);
            return;
        }
        const FName name(path.c_str(), FNAME_Add);
        uint64 key = 0;
        std::memcpy(&key, &name, sizeof(key));
        std::vector<int64> owners;
        if (auto it = g_crate_owners.find(key); it != g_crate_owners.end())
            for (int32 id : it->second) owners.push_back(id);
        if (owners.empty())
        {
            Output::send<LogLevel::Normal>(STR("[songs] {}: no other player with a fuserd id holds it; skipped\n"), path);
            ++g_sync_failed;
            crate_fetch_failed(path); // one may join
            return;
        }
        Output::send<LogLevel::Normal>(STR("[songs] {}: not in fuserd's catalogue; asking {} player(s) who have it\n"), path, owners.size());
        std::lock_guard lock(g_song_req_mu);
        g_song_req_queue.push_back(SongRequest{narrow(path), std::move(owners)});
        g_song_requests_active.fetch_add(1); // the missing-cel wait counts it at once (the worker recounts every poll)
    }

    // Any thread: a request ended without the song (a received song's install counts the success).
    auto song_request_failed(const std::string& path) -> void
    {
        ++g_sync_failed;
        crate_fetch_failed(widen(path));
    }

    struct SongWorkerState
    {
        P2PApi api;
        StringType gsid;
        std::vector<p2p::Candidate> cands;
        std::string mapping, lan;
        std::map<std::string, SongXfer> xfers; // by transfer id
        std::set<std::string> finished;        // transfers already ended here; fuserd lists them for another minute
    };

    auto song_post(SongWorkerState& w, const CharType* path, const std::string& body, jsonl::Value* res, int* status = nullptr) -> bool
    {
        StringType err;
        if (p2p_call(w.api, path, body, res, &err, status)) return true;
        Output::send<LogLevel::Warning>(STR("[songs] {} failed: {}\n"), path, err);
        return false;
    }
    auto song_body(const SongWorkerState& w, const std::string& rest) -> std::string
    {
        return std::format("{{\"game_session_id\":\"{}\"{}{}}}", narrow(w.gsid), rest.empty() ? "" : ",", rest);
    }
    auto song_report(SongWorkerState& w, const SongXfer& x, const std::string& state, const std::string& reason, const p2p::XferStatus* st = nullptr) -> void
    {
        jsonl::Value ignored;
        song_post(w, STR("p2p/song/report/"),
                  song_body(w, std::format("\"transfer_id\":\"{}\",\"state\":\"{}\",\"reason\":\"{}\",\"bytes\":{},\"ms\":{},\"path\":\"{}\"", x.id, state,
                                           json_escape(reason.substr(0, 150)), st ? st->bytes : 0, st ? st->ms : 0, st ? st->path : std::string("none"))),
                  &ignored);
    }
    auto song_report_state(const std::string& error) -> std::string // the agent's failure -> fuserd's report state
    {
        if (error.starts_with("no path")) return "no_path";
        if (error.starts_with("stalled")) return "stalled";
        if (error.starts_with("rejected")) return "rejected";
        return "failed";
    }

    // The requester's files are checked: move them where Download Songs says and start the install.
    auto song_install_received(const SongXfer& x, const p2p::XferStatus& st) -> std::string
    {
        const int mode = download_mode();
        if (mode == kDownloadNo) return "Download Songs was turned off";
        std::error_code ec;
        const std::filesystem::path dir = mode == kDownloadTemporarily ? session_song_dir() : song_dir();
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path pak = dir / (widen(x.song) + L"_P.pak"), sig = dir / (widen(x.song) + L"_P.sig");
        if (!MoveFileExW(st.sig_out.c_str(), sig.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) ||
            !MoveFileExW(st.pak_out.c_str(), pak.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
            return std::format("cannot place the files in {} (error {})", narrow(dir.wstring()), GetLastError());
        auto job = std::make_shared<SongInstall>();
        job->shortname = widen(x.song);
        job->meta_path = widen(x.path);
        job->pak_path = pak.wstring();
        job->temporary = mode == kDownloadTemporarily;
        job->started_ms = GetTickCount64();
        job->stage = SongInstall::Stage::Downloaded;
        Output::send<LogLevel::Normal>(STR("[songs] {}: received from another player ({} bytes in {} ms via {}); keeping it {} in {}\n"), job->shortname, st.bytes, st.ms,
                                       widen(st.path), job->temporary ? STR("for this run") : STR("permanently"), dir.wstring());
        std::lock_guard lock(g_song_mutex);
        g_song_installs.push_back(job);
        return {};
    }

    // An owner asked for a song: offer it, or decline.
    auto song_answer(SongWorkerState& w, SongXfer& x, int uploads) -> void
    {
        std::string decline;
        std::filesystem::path pak;
        uint64_t pak_size = 0, sig_size = 0;
        std::string pak_sha, sig_sha, why;
        // Simultaneous Uploads (settings screen, SendSongsMax 1-4, default 2), read per request: a change applies to
        // the next request, never to transfers under way.
        const int max_uploads = std::clamp(_wtoi(ini_value(STR("SendSongsMax"), STR("2")).c_str()), 1, 4);
        if (uploads >= max_uploads) decline = "busy";
        else if ((pak = find_song_pak(x.song)).empty()) decline = "not_found";
        else if (!p2p::check_song_pak(pak.wstring(), x.song, &why)) decline = "not_custom";
        std::filesystem::path sig = pak;
        sig.replace_extension(L".sig");
        // A stale chunk table is repaired here, before the digests: a receiver (on any loader) then mounts a .sig that
        // matches. One that can't be repaired would make the receiver quit, so it isn't sent.
        if (decline.empty())
        {
            const p2p::SigVerdict v = p2p::check_song_sig(pak.wstring(), sig.wstring(), &why);
            if (v == p2p::SigVerdict::Bad) decline = "bad_sig";
            else if (v == p2p::SigVerdict::Repaired)
            {
                Output::send<LogLevel::Warning>(STR("[songs] {}: before sending it: {}\n"), widen(x.song), widen(why));
                why.clear();
            }
        }
        if (decline.empty() && (!p2p::file_digest(pak.wstring(), &pak_size, &pak_sha) || !p2p::file_digest(sig.wstring(), &sig_size, &sig_sha) || !sig_size))
            decline = "not_found";
        if (decline.empty() && w.cands.empty()) decline = "busy"; // no candidates to offer (STUN failed): let the next owner try
        jsonl::Value res;
        if (!decline.empty())
        {
            Output::send<LogLevel::Normal>(STR("[songs] {}: asked for it; declining ({}{})\n"), widen(x.song), widen(decline), why.empty() ? StringType{} : STR(": ") + widen(why));
            song_post(w, STR("p2p/song/decline/"), song_body(w, std::format("\"transfer_id\":\"{}\",\"reason\":\"{}\"", x.id, decline)), &res);
            return;
        }
        const std::string body = song_body(w, std::format("\"transfer_id\":\"{}\",\"size\":{},\"sha256\":\"{}\",\"sig_size\":{},\"sig_sha256\":\"{}\",\"candidates\":{},\"mapping\":\"{}\"",
                                                          x.id, pak_size, pak_sha, sig_size, sig_sha, p2p_candidates_json(w.cands), w.mapping));
        if (!song_post(w, STR("p2p/song/offer/"), body, &res)) return;
        const jsonl::Value& t = res["transfer"];
        p2p::XferSend s{};
        s.pair_id = static_cast<uint32_t>(t["pair_id"].i64());
        s.key = p2p_key(t["key"].str());
        s.pak = pak.wstring();
        s.sig = sig.wstring();
        s.name = x.song + "_P";
        s.cap_kbps = static_cast<uint32_t>((std::max)(64, _wtoi(ini_value(STR("SendSongsKBps"), STR("2048")).c_str())));
        Output::send<LogLevel::Normal>(STR("[songs] {}: asked for it; offering {} ({} bytes)\n"), widen(x.song), pak.wstring(), pak_size);
        if (p2p::xfer_send(s)) x.pair = s.pair_id, x.started = GetTickCount64();
    }

    auto song_worker(P2PApi api, StringType gsid, uint64_t gen) -> void
    {
        SongWorkerState w{api, gsid, {}, {}, {}, {}};
        g_song_worker_gen.store(gen);
        Output::send<LogLevel::Normal>(STR("[songs] session {}: song transfers on (Send Songs {}, Download Songs {})\n"), gsid,
                                       ini_value(STR("SendSongs"), STR("0")) == STR("1") ? STR("Yes") : STR("No"), download_mode_name(download_mode()));
        const auto gather = [&] {
            std::vector<p2p::Candidate> cands;
            std::string mapping;
            if (p2p_gather(w.api, &cands, &mapping)) w.cands = cands, w.mapping = mapping;
            w.lan = p2p_lan_signature();
        };
        gather();
        uint64 last_lan_check = GetTickCount64();
        bool disabled_logged = false;
        int round_seen = -1; // the last finished sync round this worker knows of (-1: not polled yet)
        while (g_song_gen.load() == gen)
        {
            // re-gather after a network change
            if (GetTickCount64() - last_lan_check > 5000)
            {
                last_lan_check = GetTickCount64();
                if (p2p_lan_signature() != w.lan) gather();
            }
            // new requests, up to kMaxOpenSongRequests open (retries wait for their time)
            std::vector<SongRequest> batch;
            {
                int open = 0;
                for (const auto& [id, x] : w.xfers)
                    if (x.role == "requester") ++open;
                const uint64 now = GetTickCount64();
                std::lock_guard lock(g_song_req_mu);
                for (auto it = g_song_req_queue.begin(); it != g_song_req_queue.end() && open + static_cast<int>(batch.size()) < kMaxOpenSongRequests;)
                {
                    if (it->not_before > now)
                    {
                        ++it;
                        continue;
                    }
                    batch.push_back(std::move(*it));
                    it = g_song_req_queue.erase(it);
                }
            }
            for (size_t i = 0; i < batch.size(); ++i)
            {
                SongRequest& r = batch[i];
                std::string list;
                for (int64 o : r.owners) list += (list.empty() ? "" : ",") + std::to_string(o);
                jsonl::Value res;
                StringType err;
                int status = 0;
                if (!p2p_call(w.api, STR("p2p/song/request/"), song_body(w, std::format("\"song_path\":\"{}\",\"owners\":[{}]", json_escape(r.path), list)), &res, &err, &status))
                {
                    if (err.find(STR("too many open song requests")) != StringType::npos)
                    {
                        // fuserd still counts some as open: this one and the rest of the batch go back to the front
                        std::lock_guard lock(g_song_req_mu);
                        g_song_req_queue.insert(g_song_req_queue.begin(), std::make_move_iterator(batch.begin() + static_cast<ptrdiff_t>(i)),
                                                std::make_move_iterator(batch.end()));
                        break;
                    }
                    if (err.find(STR("no other member of the session holds the song")) != StringType::npos)
                    {
                        // every owner has Send Songs off, or has left; the crate scan asks again later
                        Output::send<LogLevel::Normal>(STR("[songs] {}: no player who has it is sending songs (Send Songs off, or they left)\n"), widen(r.path));
                        song_request_failed(r.path);
                        continue;
                    }
                    if (++r.tries <= kSongRequestRetries)
                    {
                        Output::send<LogLevel::Warning>(STR("[songs] {}: p2p/song/request/ failed ({}); trying again in 10 s\n"), widen(r.path), err);
                        r.not_before = GetTickCount64() + 10000;
                        std::lock_guard lock(g_song_req_mu);
                        g_song_req_queue.push_back(std::move(r));
                    }
                    else
                    {
                        Output::send<LogLevel::Warning>(STR("[songs] {}: p2p/song/request/ failed ({}); giving up\n"), widen(r.path), err);
                        song_request_failed(r.path);
                    }
                    continue;
                }
                const jsonl::Value& t = res["transfer"];
                SongXfer& x = w.xfers[t["transfer_id"].str()];
                x.id = t["transfer_id"].str();
                x.role = "requester";
                x.path = r.path;
                x.song = t["song"].str();
                x.state = t["state"].str();
                x.owners = r.owners;
                x.tries = r.tries;
            }
            // poll
            const bool sending = ini_value(STR("SendSongs"), STR("0")) == STR("1");
            jsonl::Value res;
            int status = 0;
            StringType err;
            const bool syncing = g_sync_started.exchange(false) || g_sync_active.load();
            const std::string poll_body = std::format("\"sending\":{},\"syncing\":{},\"sync_done\":{},\"sync_total\":{},\"sync_failed\":{}", sending ? "true" : "false",
                                                      syncing ? "true" : "false", g_sync_batch_done.load(), g_sync_batch_total.load(), g_sync_batch_failed.load());
            if (!p2p_call(w.api, STR("p2p/song/poll/"), song_body(w, poll_body), &res, &err, &status))
            {
                if (status == 404 && !disabled_logged)
                    Output::send<LogLevel::Warning>(STR("[songs] fuserd has no song transfers ({}); requests will be skipped\n"), err);
                disabled_logged = disabled_logged || status == 404;
                if (status == 404)
                    for (auto& [id, x] : w.xfers) x.state = "failed";
            }
            else
            {
                // The session's sync round: a round finishing after this worker's first poll is announced (one that
                // finished before this player arrived isn't). drive_sync_notices shows it on the game thread.
                if (const jsonl::Value& sync = res["sync"]; !sync.is_null())
                {
                    // every polling player's own state (absent from an older fuserd: the panel shows only this machine's)
                    g_round_active.store(sync["active"].boolean());
                    std::vector<PlayerSync> states;
                    const jsonl::Value& list = sync["states"];
                    for (size_t i = 0; i < list.size(); ++i)
                    {
                        const jsonl::Value& p = list.at(i);
                        states.push_back(PlayerSync{p["player_id"].i64(), p["syncing"].boolean(), static_cast<int>(p["done"].i64()),
                                                    static_cast<int>(p["total"].i64()), static_cast<int>(p["failed"].i64())});
                    }
                    {
                        std::lock_guard lock(g_player_syncs_mu);
                        g_player_syncs = std::move(states);
                    }
                    const int done = static_cast<int>(sync["done_round"].i64());
                    if (round_seen < 0) round_seen = done;
                    else if (done > round_seen)
                    {
                        round_seen = done;
                        g_round_players.store(static_cast<int>(sync["players"].i64()));
                        g_round_failed.store(static_cast<int>(sync["failed"].i64()));
                        g_round_done.fetch_add(1);
                        Output::send<LogLevel::Normal>(STR("[songs] sync round {} finished for all players ({} fetched songs, {} couldn't be fetched)\n"), done,
                                                       sync["players"].i64(), sync["failed"].i64());
                    }
                }
                std::set<std::string> listed;
                int uploads = 0;
                for (const auto& [id, x] : w.xfers)
                    if (x.role == "owner" && x.pair) ++uploads;
                const jsonl::Value& list = res["transfers"];
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const jsonl::Value& t = list.at(i);
                    const std::string id = t["transfer_id"].str();
                    listed.insert(id);
                    if (w.finished.contains(id)) continue; // ended here already: not a new transfer
                    SongXfer& x = w.xfers[id];
                    const bool fresh = x.id.empty();
                    x.id = id;
                    x.role = t["role"].str();
                    x.path = t["song_path"].str();
                    x.song = t["song"].str();
                    const std::string state = t["state"].str();
                    if (state != x.state && x.role == "requester")
                        Output::send<LogLevel::Normal>(STR("[songs] {}: transfer {} {}{}\n"), widen(x.song), widen(x.id), widen(state),
                                                       t["reason"].str().empty() ? StringType{} : STR(" - ") + widen(t["reason"].str()));
                    x.state = state;
                    x.reason = t["reason"].str();
                    const uint32_t pair = t["pair_id"].is_null() ? 0 : static_cast<uint32_t>(t["pair_id"].i64());
                    const std::vector<p2p::Candidate> peer = t["peer"].is_null() ? std::vector<p2p::Candidate>{} : p2p_candidates(t["peer"]["candidates"]);
                    if (state == "done" || state == "failed" || state == "cancelled")
                    {
                        if (x.pair) p2p::xfer_cancel(x.pair), x.pair = 0;
                        continue;
                    }
                    if (x.role == "owner")
                    {
                        if (state == "asked" && (fresh || !x.pair)) song_answer(w, x, uploads), uploads += x.pair ? 1 : 0;
                        else if (x.pair && !peer.empty()) p2p::xfer_set_peer(x.pair, peer);
                        continue;
                    }
                    // requester: a new offer (the first owner's, or the next one's after a failure)
                    if (pair && pair != x.pair && !t["file"].is_null())
                    {
                        if (x.pair) p2p::xfer_cancel(x.pair);
                        p2p::XferRecv r{};
                        r.pair_id = pair;
                        r.key = p2p_key(t["key"].str());
                        r.peer = peer;
                        r.dir = (session_song_dir() / L"incoming").wstring();
                        r.name = x.song + "_P";
                        r.pak_size = static_cast<uint64_t>(t["file"]["size"].i64());
                        r.sig_size = static_cast<uint64_t>(t["file"]["sig_size"].i64());
                        r.pak_sha256 = t["file"]["sha256"].str();
                        r.sig_sha256 = t["file"]["sig_sha256"].str();
                        x.pair = p2p::xfer_recv(r) ? pair : 0;
                        x.told_transferring = false;
                        x.started = GetTickCount64();
                        jsonl::Value ignored;
                        if (x.pair && !w.cands.empty())
                            song_post(w, STR("p2p/song/accept/"),
                                      song_body(w, std::format("\"transfer_id\":\"{}\",\"candidates\":{},\"mapping\":\"{}\"", x.id, p2p_candidates_json(w.cands), w.mapping)), &ignored);
                        else
                            song_report(w, x, "failed", w.cands.empty() ? "no tunnel candidates (STUN failed)" : "could not start receiving");
                    }
                    else if (x.pair && !peer.empty())
                        p2p::xfer_set_peer(x.pair, peer);
                }
                // a transfer that left the poll is over for this machine
                for (auto& [id, x] : w.xfers)
                    if (!listed.contains(id) && x.state != "done" && x.state != "failed" && x.state != "cancelled")
                    {
                        if (x.pair) p2p::xfer_cancel(x.pair), x.pair = 0;
                        x.state = "over";
                    }
            }
            // the agent's progress
            for (const p2p::XferStatus& st : p2p::xfer_status())
            {
                auto it = std::ranges::find_if(w.xfers, [&](const auto& kv) { return kv.second.pair == st.pair_id; });
                if (it == w.xfers.end()) continue;
                SongXfer& x = it->second;
                if (st.state == "transferring" && st.bytes > 0 && !x.told_transferring)
                {
                    x.told_transferring = true;
                    song_report(w, x, "transferring", "", &st);
                }
                else if (st.state == "done" && x.role == "requester")
                {
                    const std::string why = song_install_received(x, st);
                    song_report(w, x, why.empty() ? "done" : "failed", why, &st);
                    p2p::xfer_cancel(x.pair);
                    x.pair = 0;
                    x.state = why.empty() ? "done" : "failed";
                }
                else if (st.state == "done") // the owner: the requester reports done
                {
                    Output::send<LogLevel::Normal>(STR("[songs] {}: sent to another player ({} bytes in {} ms via {})\n"), widen(x.song), st.bytes, st.ms, widen(st.path));
                    p2p::xfer_cancel(x.pair);
                    x.pair = 0;
                }
                else if (st.state == "failed")
                {
                    Output::send<LogLevel::Warning>(STR("[songs] {}: transfer {} failed here ({}): {}\n"), widen(x.song), widen(x.id), widen(x.role), widen(st.error));
                    song_report(w, x, song_report_state(st.error), st.error, &st);
                    p2p::xfer_cancel(x.pair);
                    x.pair = 0;
                }
            }
            // Transfers that ended: remembered by id, as fuserd lists them for another minute (they used to come back as
            // new ones and were logged and counted again on every poll: one failure 30 times, 2026-10-04). A request
            // whose owners were busy or didn't answer is asked for again in 15 s, up to kSongRequestRetries times;
            // any other ending is final for now (a received song's install counts the success).
            for (auto it = w.xfers.begin(); it != w.xfers.end();)
            {
                const SongXfer& x = it->second;
                if (x.state != "done" && x.state != "failed" && x.state != "cancelled" && x.state != "over")
                {
                    ++it;
                    continue;
                }
                if (x.role == "requester" && x.state != "done")
                {
                    const bool owners_busy = x.state == "failed" && (x.reason.find("busy") != std::string::npos || x.reason.find("no answer") != std::string::npos);
                    if (owners_busy && x.tries < kSongRequestRetries)
                    {
                        Output::send<LogLevel::Normal>(STR("[songs] {}: its owner(s) were busy or didn't answer; asking again in 15 s (retry {} of {})\n"),
                                                       widen(x.song), x.tries + 1, kSongRequestRetries);
                        std::lock_guard lock(g_song_req_mu);
                        g_song_req_queue.push_back(SongRequest{x.path, x.owners, x.tries + 1, GetTickCount64() + 15000});
                    }
                    else
                        song_request_failed(x.path);
                }
                w.finished.insert(it->first);
                it = w.xfers.erase(it);
            }
            int active = 0;
            for (const auto& [id, x] : w.xfers)
                if (x.role == "requester") ++active;
            {
                std::lock_guard lock(g_song_req_mu);
                active += static_cast<int>(g_song_req_queue.size());
            }
            g_song_requests_active.store(active);
            for (int waited = 0; waited < 2000 && g_song_gen.load() == gen; waited += 100) Sleep(100);
        }
        for (auto& [id, x] : w.xfers)
            if (x.pair) p2p::xfer_cancel(x.pair);
        g_song_requests_active.store(0);
        g_round_active.store(false);
        {
            std::lock_guard lock(g_player_syncs_mu); // the session's players: gone with it
            g_player_syncs.clear();
        }
        uint64_t mine = gen;
        g_song_worker_gen.compare_exchange_strong(mine, 0); // unless a newer worker already took over
        Output::send<LogLevel::Normal>(STR("[songs] session {}: song transfers off\n"), gsid);
    }

    // Game thread, once a second: run the worker while this machine is in a multiplayer session with a fuserd id.
    auto drive_song_transfers() -> void
    {
        static uint64 last = 0;
        static StringType current;
        const uint64 now = GetTickCount64();
        if (now - last < 1000 || !verified_build()) return;
        last = now;
        // The session: the replicated _gameSessionId when it is fuserd's, else this player's matchmaking join's id
        // (2026-10-03: a server that kept its local-dev-... id made song transfers silently never start). Nothing outside a multiplayer venue.
        UObject* gi = game_instance();
        UObject* ps = gi && mp_state(gi) ? host_player_state(gi) : nullptr;
        wchar_t id[160]{};
        StringType gsid = ps && read_fstring(ps, kPsGameSessionId, id, 160) > 0 ? StringType(id) : StringType{};
        if (ps && (gsid.empty() || gsid.starts_with(kDevSessionPrefix)))
        {
            static StringType noted;
            const StringType replicated = gsid;
            gsid = g_mm_joined_session.starts_with(L"gsess-") ? g_mm_joined_session : StringType{};
            if (noted != replicated + gsid)
            {
                noted = replicated + gsid;
                Output::send<LogLevel::Normal>(STR("[songs] the server's session id is '{}'; {}\n"), replicated.empty() ? StringType(STR("(none)")) : replicated,
                                               gsid.empty() ? StringType(STR("no matchmaking session id either: song transfers off"))
                                                            : STR("using this player's matchmaking session '") + gsid + STR("'"));
            }
        }
        if (gsid == current) return;
        // a session that ended (or was replaced) takes its crate songs' tries with it: the next session asks afresh.
        // Not when one starts: its first crate scan may already have asked.
        if (!current.empty()) crate_tries_reset();
        current = gsid;
        const uint64_t gen = ++g_song_gen;
        if (gsid.empty())
        {
            g_crate_owners.clear();
            std::lock_guard lock(g_song_req_mu); // requests of the session that ended
            g_song_req_queue.clear();
            g_song_requests_active.store(0);
            return; // the worker sees the new generation and stops
        }
        P2PApi api{store_api_base(), backend_token()};
        if (api.base.empty() || api.token.empty())
        {
            Output::send<LogLevel::Normal>(STR("[songs] session {}: no fuserd login; song transfers off\n"), gsid);
            return;
        }
        std::thread(song_worker, api, gsid, gen).detach();
    }

    // ---- custom-song sync notices on the event highway -----------------------------------------------------------
    // The multiplayer event highway (UMultiplayerEventHighwayWidget, the "<player> joined the festival" feed of the
    // lobby and gameplay screens) shows our own lines: "Synchronising N custom songs..." when this machine starts
    // fetching songs other players' crates hold (Route B or song transfers), and "Custom songs synchronised" (or how
    // many couldn't be fetched) when nothing is left. The widget's own machinery shows them (2026-10-03, Ghidra +
    // Blueprints):
    //   - an event is a UMultiplayerEventHighwayType {EventHighwayType Join/Leave, hypeUserId, HypeUsername, IsDone};
    //     the widget keeps free ones in _allObjectsList, makes more with FUN_140771540 (NewObject), and queues them
    //     in _eventsAwaitingShowing;
    //   - its tick (FUN_14077fee0 -> FUN_140797430) moves the first queued event to _inUseList while under
    //     MaxEventsNum and calls _OnAddEvent(event); the Blueprint's AddEvent takes a pooled
    //     WBP_ListItem_MultiplayerEventHighwayItem and Setup(event): "{0} joined/left the festival" with the
    //     username into its WBP_ScrollableTextWidget, the avatar hidden for hypeUserId -1, then fade in, 15 s, fade
    //     out, IsDone - after which the widget recycles it.
    // So a notice is a free event (hypeUserId -1) queued on each live highway; when _OnAddEvent shows one of ours, its
    // item's text is replaced through "Set Text". A screen that isn't ticking never shows it, so notices still queued
    // after 10 s (or when a newer one comes) are withdrawn. SyncNotices=0 turns this off.
    // new_highway_event (FUN_140771540): NewObject<UMultiplayerEventHighwayType>(outer)
    struct Notice
    {
        StringType text;
        uint64 queued = 0;
        FWeakObjectPtr widget;
    };
    std::unordered_map<UObject*, Notice> g_notices; // event object -> its text (game thread)
    // Shown notices, for the safety timeout: the item only times itself out (fade, 15 s, fade, IsDone) when the
    // event's hypeUserId isn't -1 - for -1 its Setup stops after the text (2026-10-03: notices with -1 stayed up).
    std::vector<std::pair<FWeakObjectPtr, uint64>> g_notices_shown;
    constexpr uint64 kNoticeTimeoutMs = 18000; // the item's own 0.25 + 15 + 0.25 s, with room

    auto new_highway_event(UObject* widget) -> UObject*
    {
        __try
        {
            using Fn = UObject* (*)(UObject*);
            return reinterpret_cast<Fn>(at(&VerifiedBuild::new_highway_event))(widget);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    // Withdraw our notices still waiting in a highway's queue: all of them, or only those older than max_age.
    auto withdraw_notices(uint64 now, uint64 max_age) -> void
    {
        for (UObject* hw : live_objects(STR("WBP_MultiplayerEventHighway_C")))
        {
            auto* waiting = hw->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("_eventsAwaitingShowing"));
            for (int32 i = waiting ? waiting->Num() - 1 : -1; i >= 0; --i)
            {
                auto it = g_notices.find((*waiting)[i]);
                if (it == g_notices.end() || now - it->second.queued < max_age) continue;
                waiting->RemoveAt(i);
                g_notices.erase(it);
            }
        }
    }

    auto highway_notice(const StringType& text) -> void
    {
        static const bool on = ini_value(STR("SyncNotices"), STR("1")) == STR("1");
        if (!on || !verified_build()) return;
        const uint64 now = GetTickCount64();
        withdraw_notices(now, 0); // a newer notice replaces any still waiting
        int32 local_id = -1;
        if (UObject* ps = host_player_state(game_instance()))
            if (const int32 id = prop<int32>(ps, STR("_hypeUserId")); id > 0 && id < 900001) local_id = id; // not -1 / a lobby placeholder
        int queued = 0;
        for (UObject* hw : live_objects(STR("WBP_MultiplayerEventHighway_C")))
        {
            auto* pool = hw->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("_allObjectsList"));
            auto* waiting = hw->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("_eventsAwaitingShowing"));
            if (!waiting) continue;
            UObject* ev = nullptr;
            if (pool && pool->Num() > 0)
            {
                ev = (*pool)[pool->Num() - 1];
                pool->RemoveAt(pool->Num() - 1);
            }
            else ev = new_highway_event(hw);
            if (!ev || ev->GetClassPrivate()->GetName() != STR("MultiplayerEventHighwayType")) continue;
            if (auto* type = ev->GetValuePtrByPropertyNameInChain<uint8>(STR("EventHighwayType"))) *type = 0;
            // This player's own fuserd id: the item shows their avatar and runs its stock fade / 15 s / fade / IsDone.
            // (-1, no avatar, made the item stop after the text and stay up.)
            if (auto* id = ev->GetValuePtrByPropertyNameInChain<int32>(STR("hypeUserId"))) *id = local_id;
            if (auto* done = ev->GetValuePtrByPropertyNameInChain<bool>(STR("IsDone"))) *done = false;
            if (FProperty* name = ev->GetPropertyByNameInChain(STR("HypeUsername")))
            {
                const FString empty(STR(""));
                name->CopyCompleteValue(name->ContainerPtrToValuePtr<void>(ev), &empty);
            }
            waiting->Add(ev);
            g_notices[ev] = Notice{text, now, FWeakObjectPtr(hw)};
            ++queued;
        }
        Output::send<LogLevel::Normal>(STR("[songs] notice '{}' queued on {} event highway(s)\n"), text, queued);
    }

    // ProcessEvent post-callback: the highway showed an event; if it is one of ours, put our text in its item.
    auto on_highway_event(UObject* self, UFunction* fn, void* parms) -> void
    {
        static const FName add(STR("_OnAddEvent"), FNAME_Add);
        if (g_notices.empty() || !parms || fn->GetFName() != add) return;
        UObject* ev = *static_cast<UObject**>(parms);
        auto it = g_notices.find(ev);
        if (it == g_notices.end()) return;
        const StringType text = it->second.text;
        g_notices.erase(it);
        g_notices_shown.emplace_back(FWeakObjectPtr(ev), GetTickCount64());
        for (UObject* item : live_objects(STR("WBP_ListItem_MultiplayerEventHighwayItem_C")))
        {
            if (prop<UObject*>(item, STR("EventHighwayData")) != ev) continue;
            UObject* label = prop<UObject*>(item, STR("WBP_ScrollableTextWidget"));
            Call set(STR("/Game/UI/Shared/WBP_ScrollableTextWidget.WBP_ScrollableTextWidget_C:Set Text"));
            const FText value(text.c_str());
            const bool ok = label && set.ok() && set.copy_in(STR("InText"), &value).call(label);
            Output::send<LogLevel::Normal>(STR("[songs] notice '{}' shown on {}{}\n"), text, self->GetName(), ok ? STR("") : STR(" (its text could NOT be set)"));
            return;
        }
        Output::send<LogLevel::Warning>(STR("[songs] notice '{}': {} showed it, but no highway item holds it\n"), text, self->GetName());
    }

    // ---- The custom songs sync panel (lobby only) ---------------------------------------------------------------------
    // While any player in the session is fetching custom songs, in the lobby stage, a panel built like the multiplayer
    // queue's rounds panel (WBP_MultiplayerQueue PlayerListPanel: a MI_DarkUI_DarkPanel box, an AttenRound 16 heading in
    // a 46-unit strip, rows below) shows "Custom Songs Sync" and a row per player: their profile picture, name, status
    // and a bar. This machine's row comes from its own batch; the others' from fuserd's poll answer ("sync" "states",
    // which every polling loader reports into), so a player without the loader shows no status.
    // The panel is assembled at run time in a stock WBP_Component_ProgressBar_MPTurn (the queue's round progress): its
    // root canvas takes the background, the texts, the pictures (WBP_ProfileAvatar_Circle, as the lobby and the queue
    // show them) and the further bars; its UProgressBar (ProgressBar_43) is the first row's bar. It goes in the lobby
    // screen's right-hand column, just above the Performers panel, with the event feed raised to make room. Once nobody
    // is fetching it stays up for kPanelLingerMs. Other stages (gameplay) only get the highway notices.
    // SyncProgressBar=0 turns it off.
    constexpr const CharType* kPanelHostClass = STR("/Game/UI/Shared/Widgets/WBP_Component_ProgressBar_MPTurn.WBP_Component_ProgressBar_MPTurn_C");
    constexpr const CharType* kPanelAvatarClass = STR("/Game/Shell/WBP_ProfileAvatar_Circle.WBP_ProfileAvatar_Circle_C");
    constexpr const CharType* kPanelBackground = STR("/Game/UI/Shared/Materials/MI_DarkUI_DarkPanel.MI_DarkUI_DarkPanel");
    constexpr const CharType* kPanelFont = STR("/Game/UI/Fonts/AttenRound.AttenRound");
    // slate units (1080p pixels); the heading strip and inset are the rounds panel's, the picture size the queue's, the
    // width and the gap the lobby column's (its Performers panel's SizeBox, and the event feed's 38 above the column
    // less the panel background's 18)
    constexpr float kPanelWidth = 354.0f, kPanelHeadingHeight = 46.0f, kPanelRowHeight = 44.0f, kPanelBottom = 12.0f;
    constexpr float kPanelColumnGap = 20.0f;
    constexpr float kPanelInset = 18.0f, kPanelHeadingTop = 6.0f, kPanelHeadingTextHeight = 27.0f, kPanelRowTextHeight = 20.0f;
    constexpr float kPanelAvatarSize = 38.0f, kPanelAvatarTop = 3.0f, kPanelTextLeft = kPanelInset + kPanelAvatarSize + 10.0f, kPanelNameWidth = 150.0f;
    constexpr float kPanelBarTop = 24.0f, kPanelBarHeight = 12.0f;
    constexpr int32 kPanelHeadingFontSize = 16, kPanelRowFontSize = 14, kPanelZOrder = 50;
    constexpr size_t kPanelMaxRows = 6; // with the event feed raised above it, more would push the feed off a 1080p screen
    constexpr uint64 kPanelLingerMs = 5000, kPanelRetryMs = 2000; // retry: while no lobby screen on show has the column
    constexpr uint8 kHitTestInvisible = 3; // ESlateVisibility: never takes the lobby's mouse or focus
    constexpr uint8 kDrawAsBox = 1;        // ESlateBrushDrawType::Box
    constexpr uint8 kJustifyRight = 2;     // ETextJustify::Right
    constexpr uint8 kClipToBounds = 1;     // EWidgetClipping::ClipToBounds
    struct Vec2
    {
        float x, y;
    };
    struct Anchors // FAnchors {FVector2D Minimum, Maximum}
    {
        Vec2 min, max;
    };
    struct Margin // FMargin
    {
        float left, top, right, bottom;
    };
    struct PanelRow
    {
        int64 player = 0;                          // fuserd player id
        FWeakObjectPtr state, avatar, status, bar; // state: the player's HypePlayerState, for the picture
        bool picture_asked = false;
        StringType shown{};
        float percent = -1.0f;
    };
    struct SyncPanel
    {
        FWeakObjectPtr host, screen; // screen: the lobby screen holding it
        FWeakObjectPtr slot;         // its slot in the lobby's right-hand column
        FWeakObjectPtr feed_slot;    // the event feed's slot there, raised above the panel
        Margin feed_offsets{};       // the feed's own offsets, put back when the panel goes
        UObject* tree = nullptr;     // the host's WidgetTree and root canvas, for adding rows (valid while host is)
        UObject* canvas = nullptr;
        std::vector<PanelRow> rows;  // one per player, in session_players' order
    };
    SyncPanel g_panel{};

    // An asset by object path, loaded through the asset registry when nothing has loaded it yet; rooted, as nothing of
    // ours holds it between panels.
    auto load_asset(const CharType* path) -> UObject*
    {
        UObject* o = UObjectGlobals::StaticFindObject(nullptr, nullptr, path);
        if (!o)
        {
            Call get(STR("/Script/AssetRegistry.AssetRegistryHelpers:GetAsset"));
            auto* data = get.raw(STR("InAssetData"));
            auto* sp = CastField<FStructProperty>(get.param(STR("InAssetData")));
            FProperty* object_path = sp && sp->GetStruct().Get() ? sp->GetStruct().Get()->FindProperty(FName(STR("ObjectPath"), FNAME_Add)) : nullptr;
            if (!data || !object_path) return nullptr;
            *object_path->ContainerPtrToValuePtr<FName>(data) = FName(path, FNAME_Add);
            get.call(cdo(STR("/Script/AssetRegistry.Default__AssetRegistryHelpers")));
            o = UObjectGlobals::StaticFindObject(nullptr, nullptr, path);
        }
        if (o && !o->IsRootSet()) o->SetRootSet();
        return o;
    }

    // A field of a struct value held in `owner`'s property `prop`, e.g. an Image's Brush.ImageSize.
    template <typename T>
    auto struct_field(UObject* owner, const CharType* prop_name, const CharType* field) -> T*
    {
        auto* sp = owner ? CastField<FStructProperty>(owner->GetPropertyByNameInChain(prop_name)) : nullptr;
        UScriptStruct* st = sp ? sp->GetStruct().Get() : nullptr;
        FProperty* f = st ? st->FindProperty(FName(field, FNAME_Add)) : nullptr;
        return f ? f->ContainerPtrToValuePtr<T>(sp->ContainerPtrToValuePtr<void>(owner)) : nullptr;
    }

    auto parent_of(UObject* w) -> UObject*
    {
        Call c(STR("/Script/UMG.Widget:GetParent"));
        return w && c.call(w) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    auto in_lobby_stage() -> bool
    {
        UObject* mps = mp_state(game_instance());
        Call c(STR("/Script/Hype.MultiplayerGameplayState:GetMultiplayerGameplayStage"));
        return mps && c.call(mps) && c.get<uint8>(STR("ReturnValue")) == 1; // EMultiplayerGameplayStage::Lobby
    }

    // A session player, for a panel row.
    struct SessionPlayer
    {
        UObject* state = nullptr; // its HypePlayerState
        int64 id = 0;             // fuserd player id (_hypeUserId)
        StringType name{};
        bool local = false;
    };

    auto player_name(UObject* ps) -> StringType
    {
        // _localUserName: each machine sets it for every player (it isn't replicated)
        if (auto* s = ps->GetValuePtrByPropertyNameInChain<FString>(STR("_localUserName")); s && ***s) return **s;
        Call c(STR("/Script/Engine.PlayerState:GetPlayerName"));
        return c.call(ps) ? c.get_string(STR("ReturnValue")) : StringType{};
    }

    // The venue's players with a fuserd id, this machine's first, at most kPanelMaxRows.
    auto session_players() -> std::vector<SessionPlayer>
    {
        std::vector<SessionPlayer> out{};
        UObject* gi = game_instance();
        UObject* mps = mp_state(gi);
        UObject* me = host_player_state(gi);
        for (UObject* ps : mps ? players(mps) : std::vector<UObject*>{})
        {
            const int32 id = prop<int32>(ps, STR("_hypeUserId"));
            if (id <= 0 || id >= 900001) continue; // not known yet, or a lobby placeholder
            SessionPlayer p{ps, id, player_name(ps), ps == me};
            if (p.local) out.insert(out.begin(), std::move(p));
            else out.push_back(std::move(p));
        }
        if (out.size() > kPanelMaxRows) out.resize(kPanelMaxRows);
        return out;
    }

    // A UMG widget made at run time in a widget tree (what UWidgetTree::ConstructWidget does for a native widget).
    auto new_widget(UObject* tree, const CharType* class_path) -> UObject*
    {
        auto* cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, class_path);
        return cls && tree ? UObjectGlobals::NewObject<UObject>(tree, cls) : nullptr;
    }

    // A widget Blueprint instance (CreateWidget), owned by this machine's player controller.
    auto new_user_widget(const CharType* class_path) -> UObject*
    {
        auto* cls = static_cast<UClass*>(load_asset(class_path));
        UObject* gi = game_instance();
        Call create(STR("/Script/UMG.WidgetBlueprintLibrary:Create"));
        create.set<UObject*>(STR("WorldContextObject"), gi).set<UObject*>(STR("WidgetType"), cls);
        Call pc(STR("/Script/Engine.GameplayStatics:GetPlayerController"));
        pc.set<UObject*>(STR("WorldContextObject"), gi).set<int32>(STR("PlayerIndex"), 0);
        if (gi && pc.call(cdo(STR("/Script/Engine.Default__GameplayStatics")))) create.set<UObject*>(STR("OwningPlayer"), pc.get<UObject*>(STR("ReturnValue")));
        return gi && cls && create.call(cdo(STR("/Script/UMG.Default__WidgetBlueprintLibrary"))) ? create.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    // A canvas slot's layout. Offsets: left / top, then right / bottom - the size along a point anchor, the inset along a
    // stretched one.
    auto layout_slot(UObject* slot, Anchors anchors, Margin offsets, Vec2 alignment = {0.0f, 0.0f}, int32 z = 0) -> void
    {
        Call a(STR("/Script/UMG.CanvasPanelSlot:SetAnchors"));
        a.set<Anchors>(STR("InAnchors"), anchors).call(slot);
        Call o(STR("/Script/UMG.CanvasPanelSlot:SetOffsets"));
        o.set<Margin>(STR("InOffset"), offsets).call(slot);
        Call al(STR("/Script/UMG.CanvasPanelSlot:SetAlignment"));
        al.set<Vec2>(STR("InAlignment"), alignment).call(slot);
        Call zo(STR("/Script/UMG.CanvasPanelSlot:SetZOrder"));
        zo.set<int32>(STR("InZOrder"), z).call(slot);
    }

    auto place(UObject* canvas, UObject* child, Anchors anchors, Margin offsets, Vec2 alignment = {0.0f, 0.0f}, int32 z = 0) -> UObject*
    {
        Call add(STR("/Script/UMG.PanelWidget:AddChild"));
        add.set<UObject*>(STR("Content"), child);
        UObject* slot = canvas && child && add.call(canvas) ? add.get<UObject*>(STR("ReturnValue")) : nullptr;
        if (slot) layout_slot(slot, anchors, offsets, alignment, z);
        return slot;
    }

    auto set_text(UObject* text, const StringType& s) -> void
    {
        Call set(STR("/Script/UMG.TextBlock:SetText"));
        const FText value(s.c_str());
        set.copy_in(STR("InText"), &value).call(text);
    }

    constexpr Anchors kPanelAcross{{0.0f, 0.0f}, {1.0f, 0.0f}}; // stretched across the panel, from its top
    constexpr Anchors kPanelTopLeft{{0.0f, 0.0f}, {0.0f, 0.0f}};

    // A text block in the rounds panel's font, set up before the panel is built.
    auto panel_text(int32 font_size, Anchors anchors, Margin offsets, uint8 justify = 0, bool clip = false) -> UObject*
    {
        UObject* text = new_widget(g_panel.tree, STR("/Script/UMG.TextBlock"));
        if (!text) return nullptr;
        if (UObject* font = load_asset(kPanelFont))
        {
            if (auto** object = struct_field<UObject*>(text, STR("Font"), STR("FontObject"))) *object = font;
            if (auto* face = struct_field<FName>(text, STR("Font"), STR("TypefaceFontName"))) *face = FName(STR("Regular"), FNAME_Add);
        }
        if (auto* size = struct_field<int32>(text, STR("Font"), STR("Size"))) *size = font_size;
        if (auto* j = text->GetValuePtrByPropertyNameInChain<uint8>(STR("Justification"))) *j = justify;
        if (auto* c = text->GetValuePtrByPropertyNameInChain<uint8>(STR("Clipping")); c && clip) *c = kClipToBounds;
        place(g_panel.canvas, text, anchors, offsets, {0.0f, 0.0f}, 1);
        return text;
    }

    // A bar's WidgetStyle with its background and fill brushes exchanged: MPTurn's is a dark fill over a light track
    // (a turn running out); a sync fills light over dark.
    auto swap_bar_brushes(UObject* bar) -> void
    {
        auto* sp = CastField<FStructProperty>(bar->GetPropertyByNameInChain(STR("WidgetStyle")));
        UScriptStruct* st = sp ? sp->GetStruct().Get() : nullptr;
        FProperty* back = st ? st->FindProperty(FName(STR("BackgroundImage"), FNAME_Add)) : nullptr;
        FProperty* fill = st ? st->FindProperty(FName(STR("FillImage"), FNAME_Add)) : nullptr;
        if (!back || !fill) return;
        void* style = sp->ContainerPtrToValuePtr<void>(bar);
        std::vector<uint64> held(static_cast<size_t>(back->GetSize()) / 8 + 1, 0);
        back->InitializeValue(held.data());
        back->CopyCompleteValue(held.data(), back->ContainerPtrToValuePtr<void>(style));
        back->CopyCompleteValue(back->ContainerPtrToValuePtr<void>(style), fill->ContainerPtrToValuePtr<void>(style));
        fill->CopyCompleteValue(fill->ContainerPtrToValuePtr<void>(style), held.data());
        back->DestroyValue(held.data());
    }

    auto panel_height() -> float { return kPanelHeadingHeight + kPanelRowHeight * static_cast<float>(g_panel.rows.size()) + kPanelBottom; }

    // A player's row: the profile picture, the name, the status (right-aligned) and under them the bar, filling left
    // to right. `bar` is MPTurn's own (the first row) or null for a new UProgressBar styled like the first row's.
    auto add_panel_row(const SessionPlayer& player, UObject* bar) -> void
    {
        const float top = kPanelHeadingHeight + kPanelRowHeight * static_cast<float>(g_panel.rows.size());
        PanelRow row{};
        row.player = player.id;
        row.state = FWeakObjectPtr(player.state);
        if (UObject* avatar = new_user_widget(kPanelAvatarClass))
        {
            place(g_panel.canvas, avatar, kPanelTopLeft, {kPanelInset, top + kPanelAvatarTop, kPanelAvatarSize, kPanelAvatarSize}, {0.0f, 0.0f}, 1);
            row.avatar = FWeakObjectPtr(avatar);
        }
        if (UObject* name = panel_text(kPanelRowFontSize, kPanelTopLeft, {kPanelTextLeft, top, kPanelNameWidth, kPanelRowTextHeight}, 0, true))
            set_text(name, player.name.empty() ? StringType(STR("Player")) : player.name);
        row.status = FWeakObjectPtr(panel_text(kPanelRowFontSize, kPanelAcross, {kPanelTextLeft + kPanelNameWidth, top, kPanelInset, kPanelRowTextHeight}, kJustifyRight));
        const Margin where{kPanelTextLeft, top + kPanelBarTop, kPanelInset, kPanelBarHeight};
        if (bar)
        {
            swap_bar_brushes(bar);
            if (UObject* slot = prop<UObject*>(bar, STR("Slot"))) layout_slot(slot, kPanelAcross, where, {0.0f, 0.0f}, 1);
        }
        else if (UObject* first = g_panel.rows.empty() ? nullptr : g_panel.rows.front().bar.Get(); first && (bar = new_widget(g_panel.tree, STR("/Script/UMG.ProgressBar"))))
        {
            if (FProperty* style = first->GetPropertyByNameInChain(STR("WidgetStyle")))
                style->CopyCompleteValue(style->ContainerPtrToValuePtr<void>(bar), style->ContainerPtrToValuePtr<void>(first));
            place(g_panel.canvas, bar, kPanelAcross, where, {0.0f, 0.0f}, 1);
        }
        if (bar)
        {
            if (auto* type = bar->GetValuePtrByPropertyNameInChain<uint8>(STR("BarFillType"))) *type = 0; // EProgressBarFillType::LeftToRight
            if (auto* percent = bar->GetValuePtrByPropertyNameInChain<float>(STR("Percent"))) *percent = 0.0f;
        }
        row.bar = FWeakObjectPtr(bar);
        g_panel.rows.push_back(std::move(row));
    }

    // The panel, built but not yet shown: MPTurn's root canvas gets the background, the heading and a row per player.
    auto build_sync_panel(const std::vector<SessionPlayer>& roster) -> UObject*
    {
        UObject* host = new_user_widget(kPanelHostClass);
        g_panel = SyncPanel{};
        g_panel.tree = prop<UObject*>(host, STR("WidgetTree"));
        g_panel.canvas = prop<UObject*>(g_panel.tree, STR("RootWidget"));
        UObject* bar = prop<UObject*>(host, STR("ProgressBar_43"));
        if (!g_panel.canvas || g_panel.canvas->GetClassPrivate()->GetName() != STR("CanvasPanel") || !bar)
        {
            Output::send<LogLevel::Warning>(STR("[songs] sync panel: {} {} (root {}, ProgressBar_43 {})\n"), kPanelHostClass, host ? STR("has an unexpected layout") : STR("could not be created"),
                                            g_panel.canvas ? g_panel.canvas->GetName() : StringType(STR("none")), bar ? STR("found") : STR("MISSING"));
            g_panel = SyncPanel{};
            return nullptr;
        }
        // the background, under everything else (z -1); stretched over the panel
        UObject* back = new_widget(g_panel.tree, STR("/Script/UMG.Image"));
        UObject* material = load_asset(kPanelBackground);
        if (back && material)
        {
            if (auto** resource = struct_field<UObject*>(back, STR("Brush"), STR("ResourceObject"))) *resource = material;
            if (auto* size = struct_field<Vec2>(back, STR("Brush"), STR("ImageSize"))) *size = Vec2{14.0f, 14.0f};
            if (auto* margin = struct_field<Margin>(back, STR("Brush"), STR("Margin"))) *margin = Margin{0.5f, 0.5f, 0.5f, 0.5f};
            if (auto* draw = struct_field<uint8>(back, STR("Brush"), STR("DrawAs"))) *draw = kDrawAsBox;
            place(g_panel.canvas, back, {{0.0f, 0.0f}, {1.0f, 1.0f}}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, -1);
        }
        if (UObject* heading = panel_text(kPanelHeadingFontSize, kPanelAcross, {kPanelInset, kPanelHeadingTop, kPanelInset, kPanelHeadingTextHeight}))
            set_text(heading, STR("Custom Songs Sync"));
        for (const SessionPlayer& p : roster)
            add_panel_row(p, g_panel.rows.empty() ? bar : nullptr);
        Output::send<LogLevel::Normal>(STR("[songs] sync panel built in {}: background {}, {} player row(s)\n"), host->GetName(),
                                       back && material ? StringType(STR("MI_DarkUI_DarkPanel")) : StringType(STR("MISSING")), g_panel.rows.size());
        return host;
    }

    // The rows' profile pictures, asked for once the panel is shown (the picture widget's Construct has run then).
    auto request_pictures() -> void
    {
        for (PanelRow& row : g_panel.rows)
        {
            UObject* avatar = row.avatar.Get();
            UObject* ps = row.state.Get();
            if (row.picture_asked || !avatar || !ps) continue;
            row.picture_asked = true;
            Call fetch(STR("/Game/Shell/WBP_ProfileAvatar_Circle.WBP_ProfileAvatar_Circle_C:FetchProfilePictureFromPlayerState"));
            fetch.set<UObject*>(STR("PlayerState"), ps).set<bool>(STR("registerForVoiceChat"), false).set<bool>(STR("isClickable"), false).call(avatar);
        }
    }

    // A widget by name under `w` in a live widget tree (depth first through the panels' children). The screen's own
    // variables aren't used: "Is Variable" references can be null on a live screen.
    auto find_widget(UObject* w, const StringType& name) -> UObject*
    {
        static auto* panel_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.PanelWidget"));
        if (!w) return nullptr;
        if (w->GetName() == name) return w;
        if (!panel_class || !w->IsA(panel_class)) return nullptr;
        Call count(STR("/Script/UMG.PanelWidget:GetChildrenCount"));
        const int32 n = count.call(w) ? count.get<int32>(STR("ReturnValue")) : 0;
        for (int32 i = 0; i < n; ++i)
        {
            Call at(STR("/Script/UMG.PanelWidget:GetChildAt"));
            at.set<int32>(STR("Index"), i);
            if (UObject* found = at.call(w) ? find_widget(at.get<UObject*>(STR("ReturnValue")), name) : nullptr) return found;
        }
        return nullptr;
    }

    // Whether a shell screen is on show: its Slate widget exists and is visible (UWidget::IsVisible). A shell screen is
    // neither in the viewport nor in a UMG parent, so IsInViewport / GetParent can't tell.
    auto widget_visible(UObject* w) -> bool
    {
        Call visible(STR("/Script/UMG.Widget:IsVisible"));
        return w && visible.call(w) && visible.get<bool>(STR("ReturnValue"));
    }

    // The lobby screen on show, and in it the right-hand column (RelativePosition_EventHighway, a canvas anchored at the
    // screen's bottom right and sized by the Performers panel, DetailsAndActions) and the event feed's slot in it
    // (PositioningWithoutAddedHeight: anchored at the column's top, its bottom 38 above it).
    auto lobby_column(UObject** screen, UObject** feed_slot) -> UObject*
    {
        for (UObject* s : live_objects(STR("WBP_Screen_MultiplayerLobby_C")))
        {
            if (!widget_visible(s)) continue;
            UObject* root = prop<UObject*>(prop<UObject*>(s, STR("WidgetTree")), STR("RootWidget"));
            UObject* column = find_widget(root, STR("RelativePosition_EventHighway"));
            UObject* slot = prop<UObject*>(find_widget(column, STR("PositioningWithoutAddedHeight")), STR("Slot"));
            if (!column || column->GetClassPrivate()->GetName() != STR("CanvasPanel") || !slot) continue;
            *screen = s;
            *feed_slot = slot;
            return column;
        }
        return nullptr;
    }

    // In the column: the panel's bottom kPanelColumnGap above the Performers panel's background (which reaches 18 above
    // the column's top), right-aligned with it and as wide; the event feed goes up by the panel's height and the gap.
    auto layout_in_column() -> void
    {
        UObject* slot = g_panel.slot.Get();
        UObject* feed = g_panel.feed_slot.Get();
        if (!slot || !feed) return;
        const float height = panel_height();
        layout_slot(slot, {{1.0f, 0.0f}, {1.0f, 0.0f}}, {0.0f, g_panel.feed_offsets.top, kPanelWidth, height}, {1.0f, 1.0f}, kPanelZOrder);
        Margin raised = g_panel.feed_offsets;
        raised.top -= height + kPanelColumnGap;
        Call o(STR("/Script/UMG.CanvasPanelSlot:SetOffsets"));
        o.set<Margin>(STR("InOffset"), raised).call(feed);
    }

    // Show a built panel in the lobby column found by lobby_column.
    auto attach_sync_panel(UObject* host, UObject* screen, UObject* column, UObject* feed) -> bool
    {
        Call vis(STR("/Script/UMG.Widget:SetVisibility"));
        vis.set<uint8>(STR("InVisibility"), kHitTestInvisible).call(host);
        auto* feed_offsets = struct_field<Margin>(feed, STR("LayoutData"), STR("Offsets"));
        UObject* slot = feed_offsets ? place(column, host, {{1.0f, 0.0f}, {1.0f, 0.0f}}, {0.0f, 0.0f, kPanelWidth, panel_height()}, {1.0f, 1.0f}, kPanelZOrder) : nullptr;
        if (!slot) return false;
        g_panel.screen = FWeakObjectPtr(screen);
        g_panel.slot = FWeakObjectPtr(slot);
        g_panel.feed_slot = FWeakObjectPtr(feed);
        g_panel.feed_offsets = *feed_offsets; // put back when the panel goes
        layout_in_column();
        Output::send<LogLevel::Normal>(STR("[songs] sync panel shown above {}'s Performers panel; event feed raised by {:.0f} (from {:.0f})\n"), screen->GetName(),
                                       panel_height() + kPanelColumnGap, g_panel.feed_offsets.top);
        return true;
    }

    // Still on show: in its column, with its lobby screen on show.
    auto sync_panel_attached() -> bool
    {
        UObject* host = g_panel.host.Get();
        return host && parent_of(host) && widget_visible(g_panel.screen.Get());
    }

    auto hide_sync_panel() -> void
    {
        UObject* host = g_panel.host.Get();
        Call remove(STR("/Script/UMG.Widget:RemoveFromParent"));
        const bool removed = host && remove.call(host);
        if (UObject* feed = g_panel.feed_slot.Get()) // the event feed back where the lobby had it
        {
            Call o(STR("/Script/UMG.CanvasPanelSlot:SetOffsets"));
            o.set<Margin>(STR("InOffset"), g_panel.feed_offsets).call(feed);
        }
        g_panel = SyncPanel{};
        if (removed) Output::send<LogLevel::Normal>(STR("[songs] sync panel removed\n"));
    }

    // A row's status and bar for a player's sync state (null: no state - a player without the loader, or an older
    // fuserd). The counts are the player's current batch, or its last one.
    auto row_status(const PlayerSync* s, StringType* text) -> float
    {
        if (!s)
        {
            *text = STR("No sync status");
            return 0.0f;
        }
        if (s->syncing)
        {
            if (s->total <= 0)
            {
                *text = STR("Checking songs...");
                return 0.0f;
            }
            *text = std::format(STR("{} / {} songs"), s->done, s->total);
            return static_cast<float>(s->done) / static_cast<float>(s->total);
        }
        if (s->failed <= 0)
        {
            *text = STR("All songs present");
            return 1.0f;
        }
        *text = std::format(STR("{} song{} missing"), s->failed, s->failed == 1 ? STR("") : STR("s"));
        return s->total > 0 ? static_cast<float>(s->total - s->failed) / static_cast<float>(s->total) : 0.0f;
    }

    auto set_row(PanelRow& row, const StringType& text, float percent) -> void
    {
        if (UObject* t = row.status.Get(); t && text != row.shown)
        {
            row.shown = text;
            set_text(t, text);
        }
        if (UObject* bar = row.bar.Get(); bar && percent != row.percent)
        {
            row.percent = percent;
            Call set(STR("/Script/UMG.ProgressBar:SetPercent"));
            set.set<float>(STR("InPercent"), percent).call(bar);
        }
    }

    // Game thread, with the sync notices (twice a second): show, update or remove the panel. active / done / total /
    // failed: this machine's batch (its current one, or its last one once that is over).
    auto drive_sync_panel(bool active, int done, int total, int failed) -> void
    {
        static const bool on = ini_value(STR("SyncProgressBar"), STR("1")) == STR("1");
        static bool was_active = false, gave_up = false, noted = false;
        static uint64 ended = 0, retry_at = 0;
        const uint64 now = GetTickCount64();
        std::vector<PlayerSync> remote{};
        {
            std::lock_guard lock(g_player_syncs_mu);
            remote = g_player_syncs;
        }
        // anyone fetching: this machine, or another player in fuserd's round
        const bool anyone = active || g_round_active.load() || std::ranges::any_of(remote, [](const PlayerSync& p) { return p.syncing; });
        if (was_active && !anyone) ended = now;
        if (anyone) ended = 0;
        was_active = anyone;
        const bool wanted = on && !gave_up && (anyone || (ended && now - ended < kPanelLingerMs)) && verified_build();
        if (!wanted || !in_lobby_stage())
        {
            if (g_panel.host.Get()) hide_sync_panel();
            return;
        }
        const std::vector<SessionPlayer> roster = session_players();
        bool same = roster.size() == g_panel.rows.size();
        for (size_t i = 0; same && i < roster.size(); ++i)
            same = roster[i].id == g_panel.rows[i].player;
        if (!sync_panel_attached() || !same)
        {
            // new, its screen went away (a travel, the lobby closing) or the players changed: start over once a lobby
            // screen is on show
            hide_sync_panel();
            if (roster.empty() || now < retry_at) return;
            UObject* screen = nullptr;
            UObject* feed = nullptr;
            UObject* column = lobby_column(&screen, &feed);
            if (!column)
            {
                retry_at = now + kPanelRetryMs;
                if (!noted) Output::send<LogLevel::Normal>(STR("[songs] sync panel: no lobby screen with the Performers column on show yet; trying again every {} s\n"), kPanelRetryMs / 1000);
                noted = true;
                return;
            }
            noted = false;
            UObject* host = build_sync_panel(roster);
            g_panel.host = FWeakObjectPtr(host);
            if (!host || !attach_sync_panel(host, screen, column, feed))
            {
                gave_up = true; // not every half second for the rest of the run
                g_panel = SyncPanel{};
                Output::send<LogLevel::Warning>(STR("[songs] sync panel could not be shown; not tried again this run\n"));
                return;
            }
        }
        request_pictures();
        const PlayerSync mine{0, active, done, total, failed};
        for (size_t i = 0; i < roster.size() && i < g_panel.rows.size(); ++i)
        {
            const PlayerSync* state = &mine;
            if (!roster[i].local)
            {
                auto it = std::ranges::find_if(remote, [&](const PlayerSync& p) { return p.player == roster[i].id; });
                state = it == remote.end() ? nullptr : &*it;
            }
            StringType text{};
            const float percent = row_status(state, &text);
            set_row(g_panel.rows[i], text, percent);
        }
    }

    // Game thread, twice a second: a batch starts when this machine begins fetching crate songs in a session, and ends
    // when nothing is left.
    auto drive_sync_notices() -> void
    {
        static bool installed = false;
        if (!installed && verified_build())
        {
            installed = true;
            Hook::RegisterProcessEventPostCallback([](Hook::TCallbackIterationData<void>&, UObject* self, UFunction* fn, void* parms) {
                if (fn && self && GetCurrentThreadId() == g_game_thread) on_highway_event(self, fn, parms);
            });
        }
        static uint64 last = 0;
        static bool active = false, batched = false; // batched: a batch has started in this venue
        static int ok0 = 0, failed0 = 0;
        const uint64 now = GetTickCount64();
        if (now - last < 500) return;
        last = now;
        if (!g_notices.empty()) withdraw_notices(now, 10000);
        // The safety timeout: a shown notice still not done after kNoticeTimeoutMs is marked done; the widget's tick then
        // removes it (FUN_14077fee0: an in-use event with IsDone goes to _OnRemoveEvent).
        std::erase_if(g_notices_shown, [now](const auto& shown) {
            UObject* ev = shown.first.Get();
            auto* done = ev ? ev->GetValuePtrByPropertyNameInChain<bool>(STR("IsDone")) : nullptr;
            if (!done || *done) return true;
            if (now - shown.second < kNoticeTimeoutMs) return false;
            *done = true;
            Output::send<LogLevel::Normal>(STR("[songs] notice timed out after {} s; marked done\n"), kNoticeTimeoutMs / 1000);
            return true;
        });
        UObject* gi = game_instance();
        if (!gi || !mp_state(gi))
        {
            active = batched = false; // outside a multiplayer venue: no batch
            drive_sync_panel(false, 0, 0, 0);
            return;
        }
        int pending = g_song_requests_active.load();
        {
            std::lock_guard lock(g_song_mutex);
            for (const auto& j : g_song_installs)
                if (j->stage != SongInstall::Stage::Done && j->stage != SongInstall::Stage::Failed) ++pending;
        }
        if (!active && pending > 0)
        {
            active = batched = true;
            ok0 = g_sync_ok.load();
            failed0 = g_sync_failed.load();
            g_sync_started.store(true);
            highway_notice(pending == 1 ? StringType(STR("Synchronising a custom song...")) : std::format(STR("Synchronising {} custom songs..."), pending));
        }
        else if (active && pending == 0)
        {
            active = false;
            const int failed = g_sync_failed.load() - failed0, ok = g_sync_ok.load() - ok0;
            highway_notice(failed == 0 ? StringType(STR("Custom songs synchronised"))
                                       : ok == 0 ? StringType(STR("Custom songs could not be synchronised"))
                                                 : std::format(STR("Custom songs synchronised - {} couldn't be fetched"), failed));
        }
        // this machine's part of the session-wide round (the song worker sends it with each poll)
        // and its counts, the current batch's or the last one's final ones (all zero before its first batch here)
        const int batch_failed = batched ? g_sync_failed.load() - failed0 : 0;
        const int batch_ended = batched ? g_sync_ok.load() - ok0 + batch_failed : 0;
        g_sync_active.store(active);
        g_sync_batch_failed.store(batch_failed);
        g_sync_batch_done.store(batch_ended);
        g_sync_batch_total.store(batch_ended + pending);
        drive_sync_panel(active, batch_ended, batch_ended + pending, batch_failed);
        // every player has finished: once per round the song worker saw finish
        static int round_shown = 0;
        if (const int round = g_round_done.load(); round != round_shown)
        {
            round_shown = round;
            const int failed = g_round_failed.load();
            highway_notice(failed == 0 ? StringType(STR("All players' custom songs are synchronised"))
                                       : std::format(STR("All players have synchronised - {} custom song{} couldn't be fetched"), failed, failed == 1 ? STR("") : STR("s")));
        }
    }

    // ---- The Elder Credit ("Diamond") store --------------------------------------------------------------------
    // fuserd offers catalogue songs there. The game only unlocks content already on disk: the purchase handler
    // (_OnItemPurchasedWithElderCredits, FUN_140791730) and the login pass over claimed assets
    // (_OnGetClaimedAssetsComplete, FUN_1407901c0) both skip an asset with no unlock row. So the loader installs a
    // bought song that isn't here, with the same install as Route B. A tile also takes its title, artist and art
    // from the local CelSongSet (UnlockMgr.TryGetSongSetByName, a CelMgr lookup), which a song that isn't installed
    // doesn't have, so the loader fills those in from fuserd's catalogue.
    using NetJobDoneFn = void (*)(uint64, uint64*);
    NetJobDoneFn g_ec_list_done_original = nullptr, g_ec_purchase_done_original = nullptr, g_claimed_done_original = nullptr;
    using PurchaseRequestFn = void (*)(uint64, void*, int32);
    PurchaseRequestFn g_ec_purchase_request_original = nullptr;

    // The Elder Credit balance the game checks a price against: the local profile's (set from players/xp/).
    auto local_elder_credits() -> int
    {
        UObject* gi = game_instance();
        auto** slot = gi ? gi->GetValuePtrByPropertyNameInChain<UObject*>(STR("thePlayerProfileMgr")) : nullptr;
        if (!slot || !*slot) return -1;
        Call c(STR("/Script/Hype.PlayerProfileMgr:GetCurrentProfileElderCredits"));
        return c.call(*slot) ? c.get<int32>(STR("ReturnValue")) : -1;
    }

    // The CelSongSet the game has for a song shortname, or null when the song isn't on this machine.
    auto song_set_by_name(const StringType& sn) -> UObject*
    {
        UObject* gi = game_instance();
        auto** slot = gi ? gi->GetValuePtrByPropertyNameInChain<UObject*>(STR("theUnlockMgr")) : nullptr;
        if (!slot || !*slot) return nullptr;
        Call c(STR("/Script/Hype.UnlockMgr:TryGetSongSetByName"));
        c.set<FName>(STR("unlockableName"), FName(sn.c_str(), FNAME_Add));
        return c.call(*slot) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    // The Song assets of a UElderCreditStoreItemObject (assets: TArray<FStoreAssetDetails {FName unlockName;
    // EUnlockableType unlockType}>, 0xc each; Song = 1).
    auto song_assets(UObject* item) -> std::vector<StringType>
    {
        std::vector<StringType> out{};
        const uint8* arr = item ? item->GetValuePtrByPropertyNameInChain<uint8>(STR("assets")) : nullptr;
        if (!arr) return out;
        const uint8* data = *reinterpret_cast<const uint8* const*>(arr);
        const int32 num = *reinterpret_cast<const int32*>(arr + 8);
        for (int32 i = 0; data && i < num && i < 16; ++i)
        {
            const uint8* e = data + static_cast<size_t>(i) * 0xc;
            if (e[8] == 1) out.push_back(reinterpret_cast<const FName*>(e)->ToString());
        }
        return out;
    }

    auto fstring_at(const uint8* p) -> StringType // an FString {TCHAR* data; int32 num, max}
    {
        const auto* data = *reinterpret_cast<const CharType* const*>(p);
        const int32 num = *reinterpret_cast<const int32*>(p + 8);
        return data && num > 1 ? StringType(data, static_cast<size_t>(num - 1)) : StringType{};
    }

    // follow_setting: false for a purchase just made (always permanent); true for an owned song restored at login,
    // which follows Download Songs like any fetched song (No: not restored; Temporarily: kept for this run).
    auto install_if_missing(const StringType& sn, const CharType* why, bool follow_setting) -> void
    {
        if (song_set_by_name(sn))
        {
            Output::send<LogLevel::Normal>(STR("[store] {}: {}; already installed here\n"), sn, why);
            return;
        }
        Output::send<LogLevel::Normal>(STR("[store] {}: {}, not installed here: installing\n"), sn, why);
        request_song_install(sn, false, follow_setting);
    }

    // A tile's catalogue details, fetched once per shortname (api/community/resolve/).
    struct StoreMeta
    {
        bool done = false;
        StringType title, artist, art_url, error;
    };
    std::unordered_map<StringType, std::shared_ptr<StoreMeta>> g_store_meta; // lower-case shortname; g_song_mutex
    uint64 g_tile_scan_until = 0;                                            // tiles are looked at until then

    auto store_meta_fetch(StringType sn, std::shared_ptr<StoreMeta> meta, StringType base, StringType token) -> void
    {
        const std::string body = "{\"shortNames\":[\"" + narrow(sn) + "\"]}";
        StringType headers = L"Content-Type: application/json\r\n";
        if (!token.empty()) headers += L"Authorization: JWT " + token + L"\r\n";
        const HttpResult res = base.empty() ? HttpResult{0, {}, "no fuserd URL"} : http_request(L"POST", base + L"community/resolve/", headers, body, nullptr);
        const size_t at = res.body.find("\"song\"");
        std::lock_guard lock(g_song_mutex);
        meta->done = true;
        g_tile_scan_until = std::max(g_tile_scan_until, GetTickCount64() + 3000); // fill the tiles waiting for it
        if (!res.error.empty() || res.status != 200 || json_raw(res.body, "found") != "true" || at == std::string::npos)
        {
            meta->error = std::format(STR("HTTP {} {}{}"), res.status, widen(res.error), res.status == 200 ? STR(" (not in the catalogue)") : STR(""));
            return;
        }
        const std::string song = res.body.substr(at);
        meta->title = widen(json_raw(song, "song_name"));
        if (meta->title.empty()) meta->title = widen(json_raw(song, "name"));
        meta->artist = widen(json_raw(song, "artist"));
        meta->art_url = widen(json_raw(song, "art_url"));
    }

    // Call a function whose first parameter is an FText (WBP_Scrollable_*'s "Set Text", UTextBlock::SetText).
    auto call_with_text(UObject* target, const CharType* fn_name, const StringType& text) -> bool
    {
        UFunction* fn = target ? target->GetFunctionByNameInChain(fn_name) : nullptr;
        if (!fn || text.empty()) return false;
        Call c(fn);
        const FText value(text.c_str());
        c.copy_in_first(&value);
        return c.call(target);
    }

    // Fill a Diamond Store tile (WBP_CollectionGridItem_Store_ElderCreditItem) the way "Setup Elder Credit Store
    // Item" would from a CelSongSet: title, artist, the featured title, and (for a standard tile: a featured one
    // already loads featured_image_url itself) the album art through the widget's own SetImageAsync.
    auto fill_store_tile(UObject* tile, UObject* item, const StoreMeta& meta) -> StringType
    {
        UObject* w = prop<UObject*>(tile, STR("_CachedItemWidget"));
        if (!w)
            if (UObject* grid = prop<UObject*>(tile, STR("CollectionGridItemWidget"))) w = prop<UObject*>(grid, STR("ItemWidget"));
        if (!w) return STR("no song widget");
        const bool title = call_with_text(prop<UObject*>(w, STR("WBP_Scrollable_SongTitleField")), STR("Set Text"), meta.title);
        const bool artist = call_with_text(prop<UObject*>(w, STR("WBP_Scrollable_ArtistTitle")), STR("Set Text"), meta.artist);
        call_with_text(prop<UObject*>(w, STR("AlternateFeaturedText")), STR("SetText"), meta.title);
        bool art = false;
        if (!prop<bool>(item, STR("isFeatured")) && !meta.art_url.empty())
            if (UFunction* fn = w->GetFunctionByNameInChain(STR("SetImageAsync")))
            {
                Call c(fn);
                art = c.set_string(STR("ImageURL"), meta.art_url.c_str()).call(w);
            }
        return std::format(STR("title {}, artist {}, art {}"), title ? STR("set") : STR("NOT set"), artist ? STR("set") : STR("NOT set"),
                           art ? STR("requested") : meta.art_url.empty() ? STR("none in the catalogue") : STR("from the featured image"));
    }

    // Game thread: while the Diamond Store's tiles are being built (a few seconds after its list arrives, or after
    // a catalogue lookup finishes), fill those of songs that aren't installed. Each tile is filled once per song.
    std::set<std::pair<UObject*, StringType>> g_tiles_filled{};
    auto drive_store_tiles() -> void
    {
        static uint64 last = 0;
        const uint64 now = GetTickCount64();
        uint64 until = 0;
        {
            std::lock_guard lock(g_song_mutex);
            until = g_tile_scan_until;
        }
        if (now >= until || now - last < 500) return;
        last = now;
        for (UObject* tile : live_objects(STR("WBP_CollectionGridItem_Store_ElderCreditItem_C")))
        {
            UObject* item = prop<UObject*>(tile, STR("_CachedElderCreditStoreItem"));
            const auto songs = song_assets(item);
            if (songs.empty() || g_tiles_filled.contains({tile, songs[0]}) || song_set_by_name(songs[0])) continue;
            const StringType& sn = songs[0];
            StringType key = sn;
            std::ranges::transform(key, key.begin(), ::towlower);
            std::shared_ptr<StoreMeta> meta;
            bool start = false;
            StoreMeta copy{};
            {
                std::lock_guard lock(g_song_mutex);
                auto& slot = g_store_meta[key];
                if (!slot) slot = std::make_shared<StoreMeta>(), start = true;
                meta = slot;
                copy = *meta;
            }
            if (start)
            {
                std::thread(store_meta_fetch, sn, meta, store_api_base(), backend_token()).detach();
                continue;
            }
            if (!copy.done) continue;
            g_tiles_filled.insert({tile, sn});
            if (!copy.error.empty())
            {
                Output::send<LogLevel::Warning>(STR("[store] {}: Diamond Store tile left blank - catalogue lookup failed: {}\n"), sn, copy.error);
                continue;
            }
            Output::send<LogLevel::Normal>(STR("[store] {}: Diamond Store tile filled from the catalogue ('{}' by '{}'): {}\n"), sn, copy.title, copy.artist,
                                           fill_store_tile(tile, item, copy));
        }
    }

    // FUN_140790530: the Diamond Store list arrived; its tiles are built next.
    auto ec_list_done_detour(uint64 screen, uint64* done) -> void
    {
        g_ec_list_done_original(screen, done);
        Output::send<LogLevel::Normal>(STR("[store] Diamond Store list {}; Elder Credit balance (local profile) {}\n"),
                                       done && reinterpret_cast<const uint8*>(done)[0x10] ? STR("received") : STR("request FAILED"), local_elder_credits());
        std::lock_guard lock(g_song_mutex);
        g_tile_scan_until = std::max(g_tile_scan_until, GetTickCount64() + 5000);
    }

    // The screen's offer object with this uuid (its _featuredItems / _standardItems).
    auto find_offer(UObject* screen, const StringType& uuid) -> UObject*
    {
        for (const CharType* list : {STR("_featuredItems"), STR("_standardItems")})
        {
            auto* items = screen ? screen->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(list) : nullptr;
            for (int32 i = 0; items && i < items->Num(); ++i)
            {
                UObject* item = (*items)[i];
                auto* id = item ? item->GetValuePtrByPropertyNameInChain<uint8>(STR("UUID")) : nullptr;
                if (id && _wcsicmp(fstring_at(id).c_str(), uuid.c_str()) == 0) return item;
            }
        }
        return nullptr;
    }

    // FUN_140789280 (UHypeStoreScreen::TryPurchaseItemWithElderCredits(uuid, price)): before sending the purchase,
    // the game requires an unlock row for every asset (UnlockMgr FUN_140733640) and otherwise reports
    // NotPurchasable without asking the server. A song that isn't installed has no
    // row, so add it first. The template row is unlocked, and a song that isn't installed isn't in any list anyway.
    auto ec_purchase_request_detour(uint64 screen, void* uuid_fstring, int32 price) -> void
    {
        const StringType uuid = uuid_fstring ? fstring_at(static_cast<const uint8*>(uuid_fstring)) : StringType{};
        UObject* item = find_offer(reinterpret_cast<UObject*>(screen), uuid);
        Output::send<LogLevel::Normal>(STR("[store] Diamond Store purchase requested: offer {} ({}), price {}, local balance {}\n"), uuid,
                                       item ? STR("found") : STR("NOT on the screen"), price, local_elder_credits());
        for (const StringType& sn : song_assets(item))
        {
            UObject* mgr = nullptr;
            bool had = false;
            const StringType err = ensure_unlock_row(sn, &mgr, &had);
            if (!had || !err.empty())
                Output::send<LogLevel::Normal>(STR("[store] {}: unlock row for the purchase {}\n"), sn, !err.empty() ? err : StringType(STR("added")));
        }
        g_ec_purchase_request_original(screen, uuid_fstring, price);
    }

    // FUN_140791730 (UHypeStoreScreen::_OnItemPurchasedWithElderCredits): after the game's own unlock pass, install
    // the bought songs that aren't here. FCompletedNetJob: job at +0, success at +0x10; the job's request uuid
    // FString at +0x28; the screen's offers are its _featuredItems / _standardItems.
    auto ec_purchase_done_detour(uint64 screen, uint64* done) -> void
    {
        g_ec_purchase_done_original(screen, done);
        if (!done || !done[0]) return;
        const uint8* job = reinterpret_cast<const uint8*>(done[0]);
        const StringType uuid = fstring_at(job + 0x28);
        if (!reinterpret_cast<const uint8*>(done)[0x10])
        {
            // The job's EElderCreditPurchaseError (+0x48, from the error code; FUN_14070fcf0).
            static const CharType* const kErrors[] = {STR("Success"), STR("NotEnoughCredits"), STR("NotPurchasable"), STR("PriceChanged")};
            const uint8 e = job[0x48];
            Output::send<LogLevel::Warning>(STR("[store] Diamond Store purchase {} refused by fuserd: {}\n"), uuid, e < 4 ? kErrors[e] : STR("?"));
            return;
        }
        Output::send<LogLevel::Normal>(STR("[store] Diamond Store purchase {} accepted; balance now {}\n"), uuid, *reinterpret_cast<const int32*>(job + 0x40));
        if (UObject* item = find_offer(reinterpret_cast<UObject*>(screen), uuid))
        {
            for (const StringType& sn : song_assets(item)) install_if_missing(sn, STR("bought in the Diamond Store"), false);
            return;
        }
        Output::send<LogLevel::Warning>(STR("[store] Diamond Store purchase {}: offer not found on the screen\n"), uuid);
    }

    // FUN_1407901c0 (UHypeTitleScreen::_OnGetClaimedAssetsComplete), at login: the songs this player bought. Each
    // FAssetDetails (0x28: asset_id +0, asset_name FName +4, asset_type FString +0x10, asset_type_val +0x20) of type
    // "Song" is installed if missing, once CelMgr has finished its own registration.
    std::vector<StringType> g_claimed_pending{};
    auto claimed_done_detour(uint64 title, uint64* done) -> void
    {
        g_claimed_done_original(title, done);
        if (!done || !reinterpret_cast<const uint8*>(done)[0x10] || !done[0]) return;
        const uint8* job = reinterpret_cast<const uint8*>(done[0]);
        const uint8* data = *reinterpret_cast<const uint8* const*>(job + 0x30);
        const int32 num = *reinterpret_cast<const int32*>(job + 0x38);
        for (int32 i = 0; data && i < num && i < 1024; ++i)
        {
            const uint8* e = data + static_cast<size_t>(i) * 0x28;
            if (_wcsicmp(fstring_at(e + 0x10).c_str(), L"Song") != 0) continue;
            FName name{};
            std::memcpy(&name, e + 4, sizeof(name));
            g_claimed_pending.push_back(name.ToString());
        }
        if (!g_claimed_pending.empty())
            Output::send<LogLevel::Normal>(STR("[store] claimed assets: {} song(s) bought in the Diamond Store\n"), g_claimed_pending.size());
    }
    // UDLCManager's scan (EDlcScanningState Finished = 3): custom songs are registered with CelMgr by then. CelMgr
    // being idle isn't enough - at login it is idle with only the 145 base songs (an installed custom
    // song was "installed" again, harmlessly).
    auto dlc_scan_finished() -> bool
    {
        UObject* gi = game_instance();
        UObject* cls = cdo(STR("/Script/Hype.DLCManager"));
        Call sub(STR("/Script/Engine.SubsystemBlueprintLibrary:GetGameInstanceSubsystem"));
        if (!gi || !cls || !sub.ok()) return false;
        sub.set<UObject*>(STR("ContextObject"), gi).set<UObject*>(STR("Class"), cls);
        UObject* mgr = sub.call(cdo(STR("/Script/Engine.Default__SubsystemBlueprintLibrary"))) ? sub.get<UObject*>(STR("ReturnValue")) : nullptr;
        if (!mgr) return false;
        Call c(STR("/Script/Hype.DLCManager:GetDlcScanningStatus"));
        return c.call(mgr) && c.get<uint8>(STR("ReturnValue")) == 3;
    }
    auto drive_claimed_installs() -> void
    {
        if (g_claimed_pending.empty()) return;
        static uint64 last = 0;
        if (GetTickCount64() - last < 500) return;
        last = GetTickCount64();
        uint8 state = 0xff;
        if (!cel_mgr(game_instance(), &state) || (state != 2 && state != 4 && state != 6) || !dlc_scan_finished()) return;
        for (const StringType& sn : std::exchange(g_claimed_pending, {})) install_if_missing(sn, STR("owned (Diamond Store)"), true);
    }

    // ---- The multiplayer crate song limit (fuserd multiplayer.max_crate_songs) -------------------------------------
    // A server may cap the songs in the crate a player enters multiplayer or the Main Stage with, so a session's custom
    // songs can sync in time. fuserd hands the limit out at multiplayer/limits/ (no login: the check below runs before
    // the game logs in); 0 is no limit, and anything else is at least kMinCrateSongLimit, the stock crate size (stock
    // players have no loader, so a lower limit would bind only modded ones).
    // The game already refuses a crate: UOnlineModeFlow (spawned by the WBP_OnlineModeFlow popup) runs the stage
    // CheckCrateConstraints (2) for modes SynchronousMultiplayer and MainStage only (FUN_140786ce0). The stage
    // (FUN_14078a9b0) runs UCrate::CheckCrateConstraints (FUN_1405f3080) on CrateMgr.CurrentCrate under the crate's own
    // CrateData.MissionConstraint, and any error fails the flow with CrateDoesntMeetConstraints. One of the check's
    // errors is CategoryFull: 0 < MissionConstraint.NumSongSlots < songs.Num() (unset, -1, on a player's crate). The
    // popup's ShowCrateDoesntMeetConstraintsError then runs the check AGAIN (and shows nothing if it finds no error),
    // and queues "{error} Edit your Crate and then try re-entering multiplayer." in a WBP_HypePopup_TextOnly.
    // So when the flow enters that stage (the popup's OnOnlineModeFlowStateChanged_Event, broadcast before the stage
    // runs) with more songs than the limit, MissionConstraint.NumSongSlots is set to the limit until the next engine
    // tick: both checks then find CategoryFull, all synchronously, in the same frame (flow -> error broadcast -> the
    // popup's re-check -> QueuePopup). The QueuePopup post-hook replaces that popup's text with one naming the limit.
    constexpr int32 kMinCrateSongLimit = 24;
    constexpr uint8 kFlowCheckCrateConstraints = 2; // EOnlineModeFlowState
    std::atomic<int32> g_crate_limit{-1};           // fuserd's max_crate_songs: -1 not known (no limit), 0 no limit
    std::atomic<bool> g_crate_limit_busy{false};
    uint64 g_crate_limit_next = 0;                  // game thread: when to fetch next (0: at once)
    uint64 g_crate_frame = 0;                       // game thread: engine ticks seen by drive_crate_limit
    struct CrateLimitOverride
    {
        bool active = false, popup_done = false;
        FWeakObjectPtr crate{};
        int32* slot = nullptr; // the crate's CrateData.MissionConstraint.NumSongSlots
        int32 saved = 0, limit = 0, songs = 0;
        uint64 frame = 0;
    };
    CrateLimitOverride g_crate_override{};

    // fuserd's multiplayer/limits/ on a worker. A failure keeps the last value; an older fuserd (404) means no limit.
    // Logged in, the same call reports this loader ({"loader": FNL_VERSION, "build": the exe build}): fuserd keeps
    // players without the loader (stock FUSER) apart from loader users in public matchmaking, the session browser and
    // the Main Stage. Reported at login (drive_crate_limit: a new session token) and on each
    // entry to multiplayer, so fuserd knows before any ticket. False when no fetch was started.
    auto fetch_crate_limit(const CharType* why) -> bool
    {
        if (g_crate_limit_busy.exchange(true)) return false;
        const StringType base = store_api_base();
        if (base.empty())
        {
            g_crate_limit_busy = false;
            return false;
        }
        const StringType token = backend_token();
        const std::string build = narrow(build_name() ? StringType(build_name()) : StringType{});
        std::thread([base, why, token, build] {
            StringType headers = L"Content-Type: application/json\r\n";
            if (!token.empty()) headers += L"Authorization: JWT " + token + L"\r\n";
            const std::string body = token.empty() ? std::string("{}") : std::format("{{\"loader\":\"{}\",\"build\":\"{}\"}}", FNL_VERSION, build);
            const HttpResult res = http_request(L"POST", base + L"multiplayer/limits/", headers, body, nullptr);
            if (!token.empty() && res.error.empty() && res.status == 200)
                Output::send<LogLevel::Normal>(STR("[crate] reported loader {} ({}) to fuserd ({})\n"), widen(FNL_VERSION), widen(build), why);
            jsonl::Value root;
            const jsonl::Value* v = nullptr;
            if (res.error.empty() && res.status == 200 && jsonl::parse(res.body, &root)) v = &root["results"]["max_crate_songs"];
            if (v && v->t == jsonl::Value::T::Num)
            {
                int32 n = static_cast<int32>(v->i64());
                if (n < 0) n = 0;
                if (n > 0 && n < kMinCrateSongLimit) n = kMinCrateSongLimit;
                if (const int32 old = g_crate_limit.exchange(n); old != n)
                    Output::send<LogLevel::Normal>(STR("[crate] crate song limit from fuserd ({}): {}\n"), why,
                                                   n ? std::format(STR("{} songs"), n) : StringType(STR("none")));
            }
            else if (res.error.empty() && res.status == 404 && g_crate_limit < 0)
            {
                g_crate_limit = 0; // a fuserd without the endpoint: no limit (and no retries)
                Output::send<LogLevel::Normal>(STR("[crate] crate song limit ({}): fuserd has none (HTTP 404, an older fuserd)\n"), why);
            }
            else
            {
                Output::send<LogLevel::Warning>(STR("[crate] crate song limit not fetched ({}): {}; {}\n"), why,
                                                res.error.empty() ? std::format(STR("HTTP {}"), res.status) : widen(res.error),
                                                g_crate_limit > 0 ? std::format(STR("keeping {} songs"), g_crate_limit.load()) : StringType(STR("no limit")));
            }
            g_crate_limit_busy = false;
        }).detach();
        return true;
    }

    // A reflected struct's member property, by name, under a struct property.
    auto struct_member(FProperty* p, const CharType* name) -> FProperty*
    {
        auto* sp = CastField<FStructProperty>(p);
        UScriptStruct* st = sp ? sp->GetStruct().Get() : nullptr;
        return st ? st->FindProperty(FName(name, FNAME_Add)) : nullptr;
    }

    auto crate_limit_restore(const CharType* why) -> void
    {
        CrateLimitOverride& o = g_crate_override;
        if (!o.active) return;
        if (o.crate.Get() && o.slot) *o.slot = o.saved;
        Output::send<LogLevel::Verbose>(STR("[crate] crate check limit lifted ({}){}\n"), why, o.popup_done ? STR("") : STR("; no Crate Error popup was queued"));
        o = CrateLimitOverride{};
    }

    // The flow enters CheckCrateConstraints: over the limit, the check gets it as the crate's NumSongSlots.
    auto crate_limit_on_flow_state(uint8 state) -> void
    {
        if (state != kFlowCheckCrateConstraints) return;
        const int32 limit = g_crate_limit.load();
        if (limit <= 0)
        {
            Output::send<LogLevel::Verbose>(STR("[crate] entering multiplayer: {}\n"), limit < 0 ? STR("crate song limit not known yet; not checked") : STR("no crate song limit"));
            return;
        }
        UObject* crate = prop<UObject*>(prop<UObject*>(game_instance(), STR("theCrateMgr")), STR("CurrentCrate"));
        FProperty* data = crate ? crate->GetPropertyByNameInChain(STR("CrateData")) : nullptr;
        FProperty* songs = struct_member(data, STR("songs"));
        FProperty* mission = struct_member(data, STR("MissionConstraint"));
        FProperty* slots = struct_member(mission, STR("NumSongSlots"));
        if (!songs || !slots)
        {
            if (crate) Output::send<LogLevel::Warning>(STR("[crate] entering multiplayer: the crate's songs / MissionConstraint not found; not checked\n"));
            return; // no crate: the stage refuses it itself (NoCrateSelected)
        }
        uint8* d = data->ContainerPtrToValuePtr<uint8>(crate);
        const int32 n = songs->ContainerPtrToValuePtr<TArray<uint8>>(d)->Num();
        if (n <= limit)
        {
            Output::send<LogLevel::Normal>(STR("[crate] entering multiplayer: {} crate songs, within the server's limit of {}\n"), n, limit);
            return;
        }
        crate_limit_restore(STR("a new check"));
        int32* slot = slots->ContainerPtrToValuePtr<int32>(mission->ContainerPtrToValuePtr<uint8>(d));
        g_crate_override = CrateLimitOverride{.active = true, .crate = FWeakObjectPtr(crate), .slot = slot, .saved = *slot, .limit = limit, .songs = n,
                                              .frame = g_crate_frame};
        *slot = limit;
        Output::send<LogLevel::Normal>(STR("[crate] entering multiplayer: {} crate songs, over the server's limit of {}: the game's crate check refuses it\n"), n, limit);
    }

    // PopupManager.QueuePopup (native) while the limit is applied: the crate check's popup is the WBP_HypePopup_TextOnly
    // it adds - the last of _queuedPopups (the online-mode popup is still open), or else the new _openPopup.
    struct PopupState
    {
        int32 queued = -1;
        UObject* open = nullptr;
    };
    PopupState g_popup_before{};
    auto popup_state(UObject* mgr) -> PopupState
    {
        auto* q = mgr ? mgr->GetValuePtrByPropertyNameInChain<TArray<uint8>>(STR("_queuedPopups")) : nullptr;
        return {q ? q->Num() : -1, prop<UObject*>(mgr, STR("_openPopup"))};
    }

    auto crate_limit_popup(UObject* mgr) -> void
    {
        CrateLimitOverride& o = g_crate_override;
        const PopupState now = popup_state(mgr);
        auto* queued = CastField<FArrayProperty>(mgr->GetPropertyByNameInChain(STR("_queuedPopups")));
        auto* inner = queued ? CastField<FStructProperty>(queued->GetInner()) : nullptr;
        uint8* data = nullptr;
        UObject* shown = nullptr;
        FProperty* desc = nullptr;
        FProperty* widget = nullptr;
        if (inner && now.queued > g_popup_before.queued && now.queued > 0)
        {
            auto* arr = queued->ContainerPtrToValuePtr<TArray<uint8>>(mgr);
            data = arr->Data + static_cast<size_t>(now.queued - 1) * static_cast<size_t>(inner->GetElementSize());
            desc = struct_member(inner, STR("DescriptionText"));
            widget = struct_member(inner, STR("PopupWidget"));
        }
        else if (now.open && now.open != g_popup_before.open)
        {
            shown = now.open;
            FProperty* pd = shown->GetPropertyByNameInChain(STR("PopupData"));
            data = pd ? pd->ContainerPtrToValuePtr<uint8>(shown) : nullptr;
            desc = struct_member(pd, STR("DescriptionText"));
            widget = struct_member(pd, STR("PopupWidget"));
        }
        if (!data || !desc || !widget) return; // not a popup this call added
        UObject* cls = *widget->ContainerPtrToValuePtr<UObject*>(data);
        if (!cls || cls->GetName() != STR("WBP_HypePopup_TextOnly_C")) return;
        o.popup_done = true;
        const StringType text = std::format(STR("This server allows at most {} songs in a multiplayer Crate, and this Crate has {}. "
                                                "Edit your Crate and then try re-entering multiplayer."), o.limit, o.songs);
        const FText value(text.c_str());
        desc->CopyCompleteValue(desc->ContainerPtrToValuePtr<void>(data), &value);
        if (shown)
            if (UFunction* refresh = shown->GetFunctionByNameInChain(STR("RefreshValues")))
            {
                Call c(refresh);
                c.call(shown);
            }
        Output::send<LogLevel::Normal>(STR("[crate] Crate Error popup {}: the server's limit of {} songs (crate has {})\n"),
                                       shown ? STR("shown; text replaced") : STR("queued; text replaced"), o.limit, o.songs);
    }

    // Every engine tick: the fetch (at startup, retried every 30 s until fuserd answers, and again whenever the online
    // flow starts, at most once a minute), the hooks, and lifting the limit the tick after the check.
    auto drive_crate_limit() -> void
    {
        ++g_crate_frame;
        if (g_crate_override.active && g_crate_override.frame != g_crate_frame) crate_limit_restore(STR("the check is over"));
        const uint64 now = GetTickCount64();
        if (g_crate_limit_next != UINT64_MAX && now >= g_crate_limit_next && verified_build())
        {
            g_crate_limit_next = now + 30000;
            fetch_crate_limit(STR("startup"));
        }
        if (g_crate_limit_next != UINT64_MAX && g_crate_limit >= 0) g_crate_limit_next = UINT64_MAX; // fetched: from now on per flow start
        // A new fuserd session token (a login): report the loader at once, before the player reaches matchmaking.
        static StringType reported_token{};
        static uint64 token_check = 0;
        if (now >= token_check && verified_build())
        {
            token_check = now + 2000;
            if (const StringType token = backend_token(); !token.empty() && token != reported_token && fetch_crate_limit(STR("login")))
                reported_token = token;
        }
        static bool installed = false;
        if (installed || !verified_build()) return;
        UFunction* queue = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Hype.PopupManager:QueuePopup"));
        if (!queue) return;
        installed = true;
        UObjectGlobals::RegisterHook(
            queue,
            [](UnrealScriptFunctionCallableContext& ctx, void*) {
                if (g_crate_override.active && !g_crate_override.popup_done) g_popup_before = popup_state(ctx.Context);
            },
            [](UnrealScriptFunctionCallableContext& ctx, void*) {
                if (g_crate_override.active && !g_crate_override.popup_done && ctx.Context && GetCurrentThreadId() == g_game_thread) crate_limit_popup(ctx.Context);
            },
            nullptr);
        static const FName state_changed(STR("OnOnlineModeFlowStateChanged_Event"), FNAME_Add);
        Hook::RegisterProcessEventPreCallback([](Hook::TCallbackIterationData<void>&, UObject* self, UFunction* fn, void* parms) {
            if (!fn || !self || !parms || fn->GetFName() != state_changed || GetCurrentThreadId() != g_game_thread) return;
            const UClass* cls = self->GetClassPrivate();
            if (!cls || cls->GetName() != STR("WBP_OnlineModeFlow_C")) return;
            const uint8 state = *static_cast<const uint8*>(parms);
            static uint64 last_fetch = 0;
            if (state == 1 || state == kFlowCheckCrateConstraints) // the flow's first stages: refresh for the next attempt
                if (const uint64 t = GetTickCount64(); t - last_fetch > 60000)
                {
                    last_fetch = t;
                    fetch_crate_limit(STR("entering multiplayer"));
                }
            crate_limit_on_flow_state(state);
        });
        Output::send<LogLevel::Verbose>(STR("[crate] crate song limit hooks installed (online flow state, PopupManager.QueuePopup)\n"));
    }

    auto install_store_hooks() -> void
    {
        static bool installed = false;
        if (installed || !verified_build()) return;
        installed = true;
        const auto hook = [](uintptr_t address, void* detour, NetJobDoneFn* original, const CharType* what) {
            const auto [prep, inst] = install_hook(address, detour, reinterpret_cast<void**>(original), what);
            Output::send<LogLevel::Verbose>(STR("[store] {} hook - prepare {} install {}\n"), what, prep, inst);
        };
        hook(at(&VerifiedBuild::ec_list_done), reinterpret_cast<void*>(&ec_list_done_detour), &g_ec_list_done_original, STR("Diamond Store list (FUN_140790530)"));
        hook(at(&VerifiedBuild::ec_purchase_done), reinterpret_cast<void*>(&ec_purchase_done_detour), &g_ec_purchase_done_original, STR("Diamond Store purchase (FUN_140791730)"));
        hook(at(&VerifiedBuild::claimed_done), reinterpret_cast<void*>(&claimed_done_detour), &g_claimed_done_original, STR("claimed assets (FUN_1407901c0)"));
        {
            const auto [prep, inst] = install_hook(at(&VerifiedBuild::ec_purchase_request), reinterpret_cast<void*>(&ec_purchase_request_detour),
                                                   reinterpret_cast<void**>(&g_ec_purchase_request_original), STR("Diamond Store purchase request"));
            Output::send<LogLevel::Verbose>(STR("[store] Diamond Store purchase request (FUN_140789280) hook - prepare {} install {}\n"), prep, inst);
        }
    }

    // Every hook's funchook page, at host_hook_start (see prepare_hook): the same function / detour pairs the
    // install_* functions above install, whether or not they ever do (the leave trace's are off by default). The
    // trace hooks exist in debug builds only.
    auto prepare_all_hooks() -> void
    {
        if (!verified_build()) return;
#if FNL_DEBUG
        for (size_t n = 0; n < std::size(g_drop_hooks); ++n) prepare_hook(at(g_drop_hooks[n].field), g_drop_detours[n]);
        prepare_hook(at(&VerifiedBuild::venue_travel), reinterpret_cast<void*>(&venue_travel_detour));
        prepare_hook(at(&VerifiedBuild::venue_end_play), reinterpret_cast<void*>(&venue_endplay_detour));
        prepare_hook(at(&VerifiedBuild::stock_kick), reinterpret_cast<void*>(&stock_kick_detour));
#endif
        prepare_hook(at(&VerifiedBuild::cel_missing_queue), reinterpret_cast<void*>(&cel_missing_detour));
        prepare_hook(at(&VerifiedBuild::cel_missing_snapshot), reinterpret_cast<void*>(&snapshot_missing_detour));
        prepare_hook(at(&VerifiedBuild::mm_join), reinterpret_cast<void*>(&mm_join_detour));
        prepare_hook(at(&VerifiedBuild::ec_list_done), reinterpret_cast<void*>(&ec_list_done_detour));
        prepare_hook(at(&VerifiedBuild::ec_purchase_done), reinterpret_cast<void*>(&ec_purchase_done_detour));
        prepare_hook(at(&VerifiedBuild::claimed_done), reinterpret_cast<void*>(&claimed_done_detour));
        prepare_hook(at(&VerifiedBuild::ec_purchase_request), reinterpret_cast<void*>(&ec_purchase_request_detour));
        int prepared = 0, in_range = 0;
        for (const auto& [address, p] : g_prepared_hooks)
        {
            prepared += p.prepare == 0;
            in_range += p.prepare == 0 && hook_in_range(reinterpret_cast<void*>(address), p.trampoline);
        }
        if (in_range == prepared)
            Output::send<LogLevel::Normal>(STR("[host] hooks: {} of {} prepared, all in jump range\n"), prepared, g_prepared_hooks.size());
        else
            Output::send<LogLevel::Error>(STR("[host] hooks: {} of {} prepared, only {} in jump range - the others stay off\n"), prepared,
                                          g_prepared_hooks.size(), in_range);
    }
} // namespace

auto host_hook_start() -> void
{
    const std::string_view built = __DATE__ " " __TIME__;
    Output::send<LogLevel::Normal>(STR("[host] FUSERMultiplayer built {}\n"), StringType(built.begin(), built.end()));
    clean_session_songs(); // Download Songs = Temporarily: songs of runs that have exited
    prepare_all_hooks();   // now, while the memory above the exe is still free (Proton); installed later, as before
    Hook::RegisterEngineTickPreCallback([](Hook::TCallbackIterationData<void>&, UEngine*, float, bool) {
        static uint64 tick = 0;
        g_game_thread = GetCurrentThreadId();
        ++tick;
        install_mm_join_hook();
        install_missing_cel_hooks();
        static bool key_watch = false; // debug builds: Ctrl+Shift+T / R turn the state watch on without DiagnosticLogs
#if FNL_DEBUG
        install_leave_trace();
        // a direct join: the lobby music asset must be loaded before a multiplayer venue's state begins play (see
        // preload_mp_transition), and the game mode must be multiplayer freestyle (configure_mp_freestyle)
        if (pressed('J')) { preload_mp_transition(); configure_mp_freestyle(); open_join(); }
        if (pressed('U')) request_song_install(ini_value(STR("StoreTestInstall"), STR(""))); // test trigger
        if (pressed('N')) p2p_probe(); // STUN from the tunnel socket
        if (pressed('X')) xfer_test(); // a song transfer between two machines, no fuserd
        {
            static bool probed = false; // P2PProbe=1: the same once, after login (the backend token is needed)
            if (!probed && tick % 120 == 0 && ini_value(STR("P2PProbe"), STR("0")) == STR("1") && !backend_token().empty())
            {
                probed = true;
                p2p_probe();
            }
        }
#endif
        install_store_hooks();
        drive_p2p_tunnel();
        drive_p2p_join();
#if FNL_DEBUG
        audio_trace(); // DeckTrace: the audio tracks after a local drop
#endif
        drive_crate_limit(); // the multiplayer crate song limit (fuserd multiplayer.max_crate_songs)
        drive_crate_scan();
        drive_song_installs();
        drive_claimed_installs();
        drive_song_transfers(); // song transfers between players
        drive_sync_notices();   // custom-song sync notices on the event highway
        drive_store_tiles();
#if FNL_DEBUG
        if (pressed('T'))
        {
            // any machine: 8 s of every distinct function run through ProcessEvent (e.g. while clicking the lobby's ready)
            g_armed = true;
            key_watch = true;
            install_rpc_hooks();
            g_traced.clear();
            g_trace_until = GetTickCount64() + 8000;
            Output::send<LogLevel::Normal>(STR("[host] trace: 8 s from now\n"));
        }
        if (tick % 15 == 0 && verified_build() && mp_state(game_instance()))
            install_drop_hooks(); // log-only: where a stem drop stops (song item, desk, spawn, ServerSpawnDisc)
#endif
        log_clock();
#if FNL_DEBUG
        if (pressed('R'))
        {
            Output::send<LogLevel::Normal>(STR("[host] Ctrl+Shift+R\n"));
            g_armed = true;
            key_watch = true;
            g_last = capture();
            log_state(STR("report"), g_last, true);
            report_players();
            report_screens();
        }
#endif
        // the state / connection watch (only logs): after a join, with DiagnosticLogs=1 or a debug key
        const bool watch = g_armed && (key_watch || diagnostic_logs());
        if (watch) watch_connections();
        if (watch && tick % 60 == 0)
        {
            refresh_watch();
            const State now = capture();
            if (!(now == g_last))
            {
                g_last = now;
                log_state(STR("changed"), now, now.beacon_hosts != 0 || now.started == 1);
            }
        }
    });
#if FNL_DEBUG
    const CharType* keys = STR("Ctrl+Shift+J join HostAddress, U store test install, N STUN probe, X transfer test, T trace, R report");
#else
    const CharType* keys = STR("no debug keys in a release build");
#endif
    Output::send<LogLevel::Verbose>(STR("[host] armed ({}); diagnostic logs {}\n"), keys, diagnostic_logs() ? STR("ON (DiagnosticLogs=1)") : STR("off"));
}

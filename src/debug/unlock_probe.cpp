// Debug builds only: a read-only probe of the song unlock state (it located UnlockMgr's caches for unlocks.cpp, and
// now checks the patched table was adopted). Debug builds only. It answers what static analysis can't:
//   - timing: when DT_UnlocksSongs, UnlockMgr, its caches (row-pointer arrays at +0x98.., the TMap at +0x128) and the
//     save's _songUnlockProgression appear, relative to fue's unreal-init (first ProcessEvent) and the first tick;
//   - what the +0x128 map holds (GetUnlockState answers LockedHidden for any name missing from it);
//   - the song registry's keys for custom songs (GameInstance+0x180 -> TMap<FName, UCelSongSet*> at +0x148) vs their
//     pak names, and their IsDlc / ownership bytes;
//   - save entries for custom / event songs (they override the table);
//   - which UnlockMgr UFUNCTIONs the UI calls, and from where.
// Snapshots go to FuserNative.log at each milestone and on F10.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "fue/fue.hpp"

#include <filesystem>
#include <map>
#include <set>

using namespace fue;

auto unlocks_table() -> UObject*; // unlocks.cpp

namespace
{
    // ---------------------------------------------------------------- raw engine containers --------
    // 4.24 TSet / TMap: Elements = TSparseArray { TArray Data @0 (Num @8), TBitArray AllocationFlags (inline @0x10,
    // secondary @0x20), FirstFreeIndex @0x30, NumFreeIndices @0x34 }, then the hash. Element = pair + 8 bytes of hash links.
    struct RawSet
    {
        const uint8* p;
        auto data() const -> const uint8* { return *reinterpret_cast<uint8* const*>(p); }
        auto slots() const -> int32 { return *reinterpret_cast<const int32*>(p + 8); }
        auto num() const -> int32 { return slots() - *reinterpret_cast<const int32*>(p + 0x34); }
        auto used(int32 i) const -> bool
        {
            const uint32* bits = *reinterpret_cast<uint32* const*>(p + 0x20);
            if (!bits) bits = reinterpret_cast<const uint32*>(p + 0x10);
            return (bits[i >> 5] >> (i & 31)) & 1;
        }
        template <typename F>
        auto each(size_t stride, F&& f) const -> void
        {
            for (int32 i = 0; i < slots(); ++i)
                if (used(i)) f(data() + static_cast<size_t>(i) * stride);
        }
    };

    template <typename T>
    auto at(const void* base, size_t off) -> T { return *reinterpret_cast<const T*>(static_cast<const uint8*>(base) + off); }

    uint8* g_exe = nullptr;
    auto rva(const void* p) -> uintptr_t { return reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(g_exe); }

    // A pointer that may or may not be a UObject (the +0x128 map's values): check its class chain without faulting.
    // (No C++ objects in here: __try can't unwind them.)
    auto looks_like_object(const void* p) -> bool
    {
        __try
        {
            const void* cls = *reinterpret_cast<void* const*>(static_cast<const uint8*>(p) + 0x10);
            return cls && *reinterpret_cast<void* const*>(static_cast<const uint8*>(cls) + 0x10) != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto safe_object_name(const void* p) -> StringType
    {
        if (!p) return STR("null");
        if (!looks_like_object(p)) return STR("not an object");
        auto* o = static_cast<const UObject*>(p);
        return o->GetName() + STR(" : ") + o->GetClassPrivate()->GetName();
    }

    // ---------------------------------------------------------------- state -------------------------
    const CharType* const kSamples[] = {STR("bornthisway"), STR("cancionporferrer"), STR("justabouttosnap"), STR("barcoded"), STR("dokidoki"), STR("mantra")};
    uint64 g_tick = 0, g_t0 = 0;
    FWeakObjectPtr g_mgr{};
    int32 g_last_map = -2, g_last_rows = -2;
    bool g_have_table = false;
    std::map<StringType, std::pair<int, StringType>> g_calls{}; // UnlockMgr UFUNCTION -> (count, first caller)

    // A live instance: not a class default object or a template / archetype (e.g. the UnlockMgr default subobject
    // Default__HypeGameInstance:UnlockMgr, whose _gameInstance is null - calling into it crashed the first probe run).
    constexpr uint32 kRF_ClassDefaultObject = 0x10, kRF_ArchetypeObject = 0x20; // EObjectFlags @0x8
    auto is_live(const UObject* o) -> bool
    {
        if (at<uint32>(o, 0x8) & (kRF_ClassDefaultObject | kRF_ArchetypeObject)) return false;
        for (const UObject* x = o; x; x = x->GetOuterPrivate())
            if (x->GetName().rfind(STR("Default__"), 0) == 0) return false;
        return true;
    }

    auto find_one(const CharType* cls) -> UObject*
    {
        std::vector<UObject*> v{};
        UObjectGlobals::FindAllOf(cls, v);
        for (UObject* o : v)
            if (is_live(o)) return o;
        return nullptr;
    }

    auto unlock_mgr() -> UObject*
    {
        if (UObject* m = g_mgr.Get()) return m;
        UObject* m = find_one(STR("UnlockMgr"));
        if (m) g_mgr = FWeakObjectPtr(m);
        return m;
    }

    auto row_map(UObject* table) -> RawSet { return RawSet{reinterpret_cast<const uint8*>(table) + 0x30}; } // UDataTable::RowMap

    auto find_row(UObject* table, const FName& n) -> const uint8*
    {
        const uint8* found = nullptr;
        row_map(table).each(0x18, [&](const uint8* e) {
            if (at<FName>(e, 0) == n) found = at<const uint8*>(e, 8);
        });
        return found;
    }

    // FUnlockable (0xA0): vptr, unlockName @8, unlockType @0x10, unlockCategory @0x11, audioCreditsCost @0x14,
    // levelReq @0x1c, isHiddenWhenLocked @0x50 (decompiled row -> state, FUN_140725800) - checked against reflection below
    auto row_text(const uint8* r) -> StringType
    {
        if (!r) return STR("no row");
        return std::format(STR("name {} type {} cat {} cost {} level {} hidden {}"), at<FName>(r, 8).ToString(), at<uint8>(r, 0x10), at<uint8>(r, 0x11), at<int32>(r, 0x14),
                           at<int32>(r, 0x1c), at<uint8>(r, 0x50));
    }

    auto call_state(UObject* mgr, const CharType* fn_name, const FName& n) -> int
    {
        UFunction* fn = mgr->GetFunctionByNameInChain(fn_name);
        if (!fn) return -1;
        std::vector<uint8> buf(static_cast<size_t>(fn->GetPropertiesSize()) + 16, 0);
        FProperty* pn = fn->FindProperty(FName(STR("unlockableName")));
        if (!pn) pn = fn->FindProperty(FName(STR("unlockName")));
        FProperty* pt = fn->FindProperty(FName(STR("unlockType")));
        FProperty* pr = fn->FindProperty(FName(STR("ReturnValue")));
        if (!pn || !pt || !pr) return -2;
        *pn->ContainerPtrToValuePtr<FName>(buf.data()) = n;
        *pt->ContainerPtrToValuePtr<uint8>(buf.data()) = 1; // EUnlockableType::Song
        mgr->ProcessEvent(fn, buf.data());
        if (pr->GetElementSize() == 1) return *pr->ContainerPtrToValuePtr<uint8>(buf.data());
        return *(pr->ContainerPtrToValuePtr<uint8>(buf.data()) + 0xA); // FUnlockProgressionState::unlockState
    }

    // the old loader's naming: every *_P pak under Content/Paks/custom_songs, minus "_P"
    auto pak_songs() -> std::vector<StringType>
    {
        static std::vector<StringType> names{};
        static bool done = false;
        if (done) return names;
        done = true;
        std::error_code ec{};
        const auto dir = std::filesystem::path(fue::exe_dir()).parent_path().parent_path() / L"Content" / L"Paks" / L"custom_songs";
        for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (!it->is_regular_file() || it->path().extension() != L".pak") continue;
            const auto stem = it->path().stem().wstring();
            if (stem.size() > 2 && stem.ends_with(L"_P")) names.push_back(stem.substr(0, stem.size() - 2));
        }
        return names;
    }

    auto snapshot(const CharType* when, bool detail) -> void
    {
        const auto t = GetTickCount64() - g_t0;
        Output::send<LogLevel::Verbose>(STR("[unlock] ===== {} (tick {}, +{} ms, {} objects) =====\n"), when, g_tick, t, UObjectGlobals::object_count());

        // the table(s)
        std::vector<UObject*> tables{};
        UObjectGlobals::FindAllOf(STR("DataTable"), tables);
        UObject* table = nullptr;
        for (UObject* o : tables)
        {
            if (o->GetName() != STR("DT_UnlocksSongs")) continue;
            table = o;
            Output::send<LogLevel::Verbose>(STR("[unlock] table {} @{} vtable RVA 0x{:X} (UDataTable 0x2F0AB28), {} rows\n"), o->GetPathName(), static_cast<void*>(o),
                                            rva(*reinterpret_cast<void* const*>(o)), row_map(o).num());
        }

        UObject* mgr = unlock_mgr();
        if (!mgr)
        {
            Output::send<LogLevel::Verbose>(STR("[unlock] no UnlockMgr yet\n"));
            return;
        }
        auto* songs_prop = mgr->GetValuePtrByPropertyNameInChain<UObject*>(STR("_songUnlockData"));
        UObject* songs = songs_prop ? *songs_prop : nullptr;
        const RawSet map{reinterpret_cast<const uint8*>(mgr) + 0x128};
        StringType arrays{};
        for (size_t off = 0x98; off < 0x128; off += 0x10) arrays += std::format(STR(" +0x{:X}:{}"), off, at<int32>(mgr, off + 8));
        Output::send<LogLevel::Verbose>(STR("[unlock] UnlockMgr {} ({}) _songUnlockData {} (field offset {}; raw +0x58 {}), arrays{}, +0x128 map {} entries\n"),
                                        mgr->GetFullName(), static_cast<void*>(mgr), songs ? songs->GetPathName() : StringType{STR("null")},
                                        songs_prop ? reinterpret_cast<uint8*>(songs_prop) - reinterpret_cast<uint8*>(mgr) : -1, at<void*>(mgr, 0x58), arrays, map.num());
        if (songs) table = songs;
        if (songs) Output::send<LogLevel::Verbose>(STR("[unlock] manager's table {} the one unlocks.cpp patched\n"), songs == unlocks_table() ? STR("IS") : STR("is NOT"));

        // what the +0x128 map is: its first entries, and how many keys are song-table rows
        {
            int shown = 0, song_rows = 0;
            StringType first{};
            map.each(0x18, [&](const uint8* e) {
                const FName k = at<FName>(e, 0);
                if (table && find_row(table, k)) ++song_rows;
                if (shown++ < 6) first += std::format(STR(" [{} -> {} ({})]"), k.ToString(), at<void*>(e, 8), safe_object_name(at<void*>(e, 8)));
            });
            Output::send<LogLevel::Verbose>(STR("[unlock] +0x128: {} of {} keys are song rows; first:{}\n"), song_rows, map.num(), first);
        }

        // the song registry: GameInstance(+0x48) -> +0x180 -> TMap<FName, UCelSongSet*> @+0x148
        UObject* gi = at<UObject*>(mgr, 0x48);
        UObject* reg = gi ? at<UObject*>(gi, 0x180) : nullptr;
        std::vector<std::pair<FName, UObject*>> custom{};
        if (reg)
        {
            const RawSet songs_map{reinterpret_cast<const uint8*>(reg) + 0x148};
            int dlc = 0, total = 0;
            songs_map.each(0x18, [&](const uint8* e) {
                ++total;
                UObject* s = at<UObject*>(e, 8);
                if (s && at<uint8>(s, 0xd1)) ++dlc;
                if (table && !find_row(table, at<FName>(e, 0))) custom.emplace_back(at<FName>(e, 0), s);
            });
            FProperty* is_dlc = nullptr;
            if (!custom.empty() && custom.front().second) is_dlc = custom.front().second->GetClassPrivate()->FindProperty(FName(STR("IsDlc")));
            Output::send<LogLevel::Verbose>(STR("[unlock] registry {} ({}): {} songs, {} IsDlc (byte +0xD1; reflected IsDlc offset {}), {} without a song row\n"), reg->GetName(),
                                            reg->GetClassPrivate()->GetName(), total, dlc, is_dlc ? is_dlc->GetOffset_Internal() : -1, custom.size());
        }
        else
            Output::send<LogLevel::Verbose>(STR("[unlock] no song registry yet (GameInstance {})\n"), static_cast<void*>(gi));

        // the save's song progression
        UObject* save = find_one(STR("HypeSaveData"));
        auto* prog_prop = save ? save->GetValuePtrByPropertyNameInChain<uint8>(STR("_songUnlockProgression")) : nullptr;
        std::map<FName, uint8, bool (*)(const FName&, const FName&)> saved([](const FName& a, const FName& b) {
            return a.ComparisonIndex != b.ComparisonIndex ? a.ComparisonIndex < b.ComparisonIndex : a.Number < b.Number;
        });
        if (prog_prop)
        {
            int by_state[4]{};
            RawSet{prog_prop}.each(0x20, [&](const uint8* e) {
                const uint8 st = at<uint8>(e, 8 + 0xA);
                saved[at<FName>(e, 0)] = st;
                if (st < 4) ++by_state[st];
            });
            Output::send<LogLevel::Verbose>(STR("[unlock] save {}: _songUnlockProgression {} entries (Locked {}, LockedHidden {}, Purchasable {}, Unlocked {})\n"), save->GetName(),
                                            saved.size(), by_state[0], by_state[1], by_state[2], by_state[3]);
        }
        else
            Output::send<LogLevel::Verbose>(STR("[unlock] no save data yet\n"));

        const bool can_call = gi && reg; // GetUnlockState dereferences _gameInstance->+0x180 for songs
        auto line = [&](const FName& n, UObject* set) {
            const uint8* r = table ? find_row(table, n) : nullptr;
            bool in_map = false;
            map.each(0x18, [&](const uint8* e) { in_map |= at<FName>(e, 0) == n; });
            const auto s = saved.find(n);
            Output::send<LogLevel::Verbose>(STR("[unlock]   {:40} row [{}] +0x128 {} save {} | GetUnlockState {} ProgressState {}{}\n"), n.ToString(), row_text(r), in_map ? STR("yes") : STR("no"),
                                            s == saved.end() ? StringType{STR("-")} : std::format(STR("{}"), s->second), can_call ? call_state(mgr, STR("GetUnlockState"), n) : -9,
                                            can_call ? call_state(mgr, STR("GetUnlockProgressState"), n) : -9,
                                            set ? std::format(STR(" | set {} IsDlc {} own 0x{:X}"), set->GetName(), at<uint8>(set, 0xd1), at<uint8>(set, 0x140)) : StringType{});
        };
        for (const CharType* n : kSamples) line(FName(n), nullptr);
        size_t shown = 0;
        for (auto& [n, s] : custom)
            if (shown++ < 2) line(n, s); // official DLC (IsDlc, no row) - a couple for reference
        std::map<StringType, UObject*> reg_by_name{};
        if (reg)
            RawSet{reinterpret_cast<const uint8*>(reg) + 0x148}.each(0x18, [&](const uint8* e) { reg_by_name[at<FName>(e, 0).ToString()] = at<UObject*>(e, 8); });
        int customs = 0, unregistered = 0;
        for (const auto& stem : pak_songs())
        {
            const auto it = reg_by_name.find(stem);
            if (it == reg_by_name.end()) ++unregistered;
            ++customs;
            if (detail || customs <= 3) line(FName(stem.c_str()), it == reg_by_name.end() ? nullptr : it->second);
        }
        Output::send<LogLevel::Verbose>(STR("[unlock] custom_songs paks: {} ({} with no registry entry under that name)\n"), customs, unregistered);

        if (detail)
        {
            StringType calls{};
            for (auto& [n, c] : g_calls) calls += std::format(STR("\n[unlock]   {} x{} (first from {})"), n, c.first, c.second);
            Output::send<LogLevel::Verbose>(STR("[unlock] UnlockMgr UFUNCTIONs called so far:{}\n"), calls);
        }
    }

    // count every native UnlockMgr UFUNCTION call and remember its first caller (script frame Node)
    auto hook_natives() -> void
    {
        const CharType* const names[] = {STR("GetUnlockState"), STR("IsItemUnlocked"), STR("GetSongsWithUnlockState"), STR("GetNonHiddenSongs"), STR("GetUnlockProgressState"),
                                         STR("SetUnlockState"), STR("TryGetSongSetByName"), STR("GetItemCost"), STR("GetUnlockableRowCopy"), STR("IsUnlockNew"), STR("TryPurchaseItem")};
        int n = 0;
        for (const CharType* name : names)
        {
            auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, (StringType(STR("/Script/Hype.UnlockMgr:")) + name).c_str());
            if (!fn) continue;
            const StringType key = name;
            UObjectGlobals::RegisterHook(
                fn,
                [key, fn](UnrealScriptFunctionCallableContext& ctx, void*) {
                    if (ctx.TheStack.Node() == fn) return; // a ProcessEvent call (the probe's own), not a script caller
                    auto& c = g_calls[key];
                    if (c.first++ == 0)
                    {
                        UFunction* node = ctx.TheStack.Node();
                        c.second = node ? node->GetFullName() : StringType{STR("?")};
                        Output::send<LogLevel::Verbose>(STR("[unlock] first call: UnlockMgr::{} from {} (tick {})\n"), key, c.second, g_tick);
                    }
                },
                {}, nullptr);
            ++n;
        }
        Output::send<LogLevel::Verbose>(STR("[unlock] counting {} UnlockMgr natives\n"), n);
    }
} // namespace

auto unlock_probe_start() -> void
{
    g_exe = reinterpret_cast<uint8*>(GetModuleHandleW(nullptr));
    g_t0 = GetTickCount64();
    fue::on_unreal_init([] {
        snapshot(STR("unreal-init (first ProcessEvent)"), false);
        hook_natives();
    });
    Hook::RegisterProcessEventPreCallback([](Hook::TCallbackIterationData<void>&, UObject* self, UFunction* fn, void*) {
        if (!fn || !self) return;
        const StringType n = fn->GetName();
        static std::set<StringType> seen{};
        if ((n == STR("_OnSaveDataLoaded") || n == STR("_OnDlcScanningFinished")) && seen.insert(n).second) snapshot((STR("before ") + n).c_str(), false);
    });
    Hook::RegisterProcessEventPostCallback([](Hook::TCallbackIterationData<void>&, UObject* self, UFunction* fn, void*) {
        if (!fn || !self) return;
        const StringType n = fn->GetName();
        static std::set<StringType> seen{};
        if ((n == STR("_OnSaveDataLoaded") || n == STR("_OnDlcScanningFinished")) && seen.insert(n).second) snapshot((STR("after ") + n).c_str(), false);
    });
    Hook::RegisterEngineTickPreCallback([](Hook::TCallbackIterationData<void>&, UEngine*, float, bool) {
        ++g_tick;
        if (g_tick == 1) snapshot(STR("first tick"), false);
        // cheap milestone polling: the manager's appearance, its table, and changes of the +0x128 map / row count
        if (!g_mgr.Get() && g_tick % 30 == 0 && unlock_mgr()) snapshot(STR("UnlockMgr appeared"), false);
        if (UObject* m = g_mgr.Get())
        {
            auto* songs = m->GetValuePtrByPropertyNameInChain<UObject*>(STR("_songUnlockData"));
            const int32 map_n = RawSet{reinterpret_cast<const uint8*>(m) + 0x128}.num();
            const int32 rows = songs && *songs ? row_map(*songs).num() : -1;
            if ((songs && *songs && !g_have_table) || map_n != g_last_map || rows != g_last_rows)
            {
                g_have_table = songs && *songs;
                g_last_map = map_n;
                g_last_rows = rows;
                snapshot(std::format(STR("change: +0x128 {} entries, {} song rows"), map_n, rows).c_str(), false);
            }
        }
        static bool was = false;
        const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (down && !was) snapshot(STR("F10"), true);
        was = down;
    });
    Output::send<LogLevel::Verbose>(STR("[unlock] probe armed (F10 = full snapshot)\n"));
}

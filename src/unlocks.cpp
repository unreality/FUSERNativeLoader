// Free unlocks for custom songs and event songs, applied at runtime.
//
// Replaces the song loader's generated customSongsUnlocked_P.pak, which overrode all of DT_UnlocksSongs with a stale
// embedded copy plus cloned rows. Here the live table is patched instead:
//   - at fue's unreal-init (the first ProcessEvent, well before UnlockMgr's init) DT_UnlocksSongs is loaded through
//     KismetSystemLibrary::LoadAsset_Blocking - TSoftObjectPtr::LoadSynchronous, the call UnlockMgr's table loader
//     makes later - and rooted;
//   - event-reward song rows become progression rewards (EventReward is LockedHidden once the event is gone; the rows
//     already cost 0), edited in place;
//   - every Content/Paks/custom_songs/**/<name>_P.pak without a row gets one: a copy of bornthisway (free, no
//     requirements) named <name>, added through UDataTable::AddRow. The pak stem is the song's unlock name (checked:
//     64 of 64 songs).
// UnlockMgr's own load then returns this object, so its caches (row arrays, the name -> row map GetUnlockState uses)
// and the save's first-unlock entries are built from the patched rows (verified live).
// Engine layout used besides reflection, shared by every 4.24 build: UDataTable::RowStruct @0x28, RowMap @0x30
// (TMap<FName, uint8*>), AddRow at vtable 0x288.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "fue/fue.hpp"

#include <filesystem>

using namespace fue;

namespace
{
    constexpr const CharType* kTablePath = STR("/Game/Gameplay/Meta/DT_UnlocksSongs.DT_UnlocksSongs");
    constexpr const CharType* kTemplateRow = STR("bornthisway");
    constexpr uint8 kSong = 1;                                             // EUnlockableType::Song
    constexpr uint8 kProgressionReward = 0, kEventReward = 1;             // EUnlockCategory
    constexpr size_t kRowStruct = 0x28, kRowMap = 0x30, kVAddRow = 0x288; // UDataTable (4.24)

    UObject* g_table = nullptr;

    // TMap<FName, uint8*> RowMap: a TSet whose elements (0x18: key, value, hash links) live in a sparse array -
    // TArray @0 (Num @8), allocation bits inline @0x10 or at the pointer @0x20
    template <typename F>
    auto each_row(UObject* table, F&& f) -> void
    {
        const uint8* set = reinterpret_cast<const uint8*>(table) + kRowMap;
        uint8* data = *reinterpret_cast<uint8* const*>(set);
        const int32 slots = *reinterpret_cast<const int32*>(set + 8);
        const uint32* bits = *reinterpret_cast<uint32* const*>(set + 0x20);
        if (!bits) bits = reinterpret_cast<const uint32*>(set + 0x10);
        for (int32 i = 0; i < slots; ++i)
            if ((bits[i >> 5] >> (i & 31)) & 1)
            {
                uint8* e = data + static_cast<size_t>(i) * 0x18;
                f(*reinterpret_cast<FName*>(e), *reinterpret_cast<uint8**>(e + 8));
            }
    }

    auto find_row(UObject* table, const FName& name) -> uint8*
    {
        uint8* found = nullptr;
        each_row(table, [&](const FName& n, uint8* row) {
            if (n == name) found = row;
        });
        return found;
    }

    auto row_count(UObject* table) -> int32
    {
        const uint8* set = reinterpret_cast<const uint8*>(table) + kRowMap;
        return *reinterpret_cast<const int32*>(set + 8) - *reinterpret_cast<const int32*>(set + 0x34);
    }

    // Content/Paks/custom_songs/**/<name>_P.pak -> <name>, the naming the song loader has always used
    auto custom_song_names() -> std::vector<StringType>
    {
        std::vector<StringType> names{};
        std::error_code ec{};
        const auto dir = std::filesystem::path(exe_dir()).parent_path().parent_path() / L"Content" / L"Paks" / L"custom_songs";
        for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (!it->is_regular_file(ec) || it->path().extension() != L".pak") continue;
            const auto stem = it->path().stem().wstring();
            if (stem.size() > 2 && stem.ends_with(L"_P")) names.push_back(stem.substr(0, stem.size() - 2));
        }
        return names;
    }

    // A ProcessEvent parameter buffer, its parameters constructed and destroyed through their properties
    struct Call
    {
        UFunction* fn;
        std::vector<uint8> buf;
        explicit Call(UFunction* f) : fn(f), buf(static_cast<size_t>(f->GetPropertiesSize()) + 16, 0)
        {
            each([&](FProperty* p) { p->InitializeValue_InContainer(buf.data()); });
        }
        ~Call()
        {
            each([&](FProperty* p) { p->DestroyValue_InContainer(buf.data()); });
        }
        template <typename F>
        void each(F&& f)
        {
            for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
            {
                if (!p->HasAnyPropertyFlags(CPF_Parm)) break;
                f(p);
            }
        }
        auto prop(const CharType* n) -> FProperty* { return fn->FindProperty(FName(n)); }
        auto addr(const CharType* n) -> uint8* { return prop(n)->ContainerPtrToValuePtr<uint8>(buf.data()); }
    };

    // path -> FSoftObjectPath -> TSoftObjectPtr -> LoadSynchronous, all through KismetSystemLibrary
    auto load_blocking(const CharType* path) -> UObject*
    {
        UObject* ksl = UObjectGlobals::StaticFindObject(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        auto* make = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary:MakeSoftObjectPath"));
        auto* conv = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary:Conv_SoftObjPathToSoftObjRef"));
        auto* load = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary:LoadAsset_Blocking"));
        if (!ksl || !make || !conv || !load) return nullptr;
        Call c1(make), c2(conv), c3(load);
        if (!c1.prop(STR("PathString")) || !c2.prop(STR("SoftObjectPath")) || !c3.prop(STR("Asset"))) return nullptr;
        {
            const FString s(path);
            c1.prop(STR("PathString"))->CopyCompleteValue(c1.addr(STR("PathString")), &s);
        }
        ksl->ProcessEvent(make, c1.buf.data());
        c2.prop(STR("SoftObjectPath"))->CopyCompleteValue(c2.addr(STR("SoftObjectPath")), c1.addr(STR("ReturnValue")));
        ksl->ProcessEvent(conv, c2.buf.data());
        c3.prop(STR("Asset"))->CopyCompleteValue(c3.addr(STR("Asset")), c2.addr(STR("ReturnValue")));
        ksl->ProcessEvent(load, c3.buf.data());
        return *reinterpret_cast<UObject**>(c3.addr(STR("ReturnValue")));
    }

    auto in_exe(const void* p) -> bool
    {
        HMODULE mod = nullptr;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(p), &mod) &&
               mod == GetModuleHandleW(nullptr);
    }

    auto apply() -> void
    {
        const auto t0 = GetTickCount64();
        UObject* loaded_before = UObjectGlobals::StaticFindObject(nullptr, nullptr, kTablePath);
        UObject* table = loaded_before ? loaded_before : load_blocking(kTablePath);
        auto fail = [](const CharType* why) { Output::send<LogLevel::Error>(STR("[unlocks] {} - custom and event songs are NOT unlocked\n"), why); };
        // a plain UDataTable (UCompositeDataTable's AddRow is a no-op) of FUnlockable rows
        if (!table || table->GetClassPrivate()->GetName() != STR("DataTable")) return fail(STR("DT_UnlocksSongs did not load as a DataTable"));
        auto* row_struct = *reinterpret_cast<UStruct**>(reinterpret_cast<uint8*>(table) + kRowStruct);
        FProperty* p_name = row_struct ? row_struct->FindProperty(FName(STR("unlockName"))) : nullptr;
        FProperty* p_type = row_struct ? row_struct->FindProperty(FName(STR("unlockType"))) : nullptr;
        FProperty* p_cat = row_struct ? row_struct->FindProperty(FName(STR("unlockCategory"))) : nullptr;
        void* add_row_fn = (*reinterpret_cast<void* const* const*>(table))[kVAddRow / sizeof(void*)];
        if (!row_struct || row_struct->GetName() != STR("Unlockable") || !p_name || !p_type || !p_cat) return fail(STR("DT_UnlocksSongs rows are not FUnlockable"));
        if (!in_exe(add_row_fn)) return fail(STR("UDataTable::AddRow is not where 4.24 puts it"));
        table->SetRootSet(); // alive until UnlockMgr holds it (and after: it never unloads)
        g_table = table;
        const int32 o_name = p_name->GetOffset_Internal(), o_type = p_type->GetOffset_Internal(), o_cat = p_cat->GetOffset_Internal();
        const int32 stock = row_count(table);

        int events = 0;
        each_row(table, [&](const FName&, uint8* row) {
            if (row[o_type] == kSong && row[o_cat] == kEventReward)
            {
                row[o_cat] = kProgressionReward;
                ++events;
            }
        });

        const uint8* tmpl = find_row(table, FName(kTemplateRow));
        using AddRowFn = void (*)(UObject*, FName, const void*); // UDataTable::AddRow(FName, const FTableRowBase&)
        auto add_row = reinterpret_cast<AddRowFn>(add_row_fn);
        int added = 0, present = 0;
        for (const auto& name : custom_song_names())
        {
            const FName n(name.c_str());
            if (find_row(table, n))
            {
                ++present;
                continue;
            }
            if (!tmpl) break;
            add_row(table, n, tmpl); // copies the template row into a new allocation under this name
            if (uint8* row = find_row(table, n))
            {
                *reinterpret_cast<FName*>(row + o_name) = n;
                ++added;
            }
        }
        if (!tmpl) Output::send<LogLevel::Error>(STR("[unlocks] template row '{}' is missing - custom songs are NOT unlocked\n"), kTemplateRow);
        if (loaded_before)
            Output::send<LogLevel::Warning>(STR("[unlocks] DT_UnlocksSongs was already loaded at unreal-init; if UnlockMgr had cached it, added rows may not show until restart\n"));
        Output::send<LogLevel::Verbose>(STR("[unlocks] DT_UnlocksSongs: {} event songs made progression rewards, {} custom songs added ({} already had rows), {} -> {} rows ({} ms)\n"),
                                        events, added, present, stock, row_count(table), GetTickCount64() - t0);
    }
} // namespace

// Called by fnl_start after fue::init, before FuserNative (so this runs first at unreal-init).
auto unlocks_start() -> void { on_unreal_init(&apply); }

// The patched table (the unlock probe checks UnlockMgr adopted it)
auto unlocks_table() -> UObject* { return g_table; }

// A row for a custom song installed after launch (Route B, host_hook.cpp): the same free bornthisway copy
// as at startup, added to the live table. Returns the row (an existing one if the name has a row already), or nullptr.
// UnlockMgr built its name map from the table once, at its init, so the caller must also add the row there.
auto unlocks_add_song_row(const CharType* name) -> uint8*
{
    UObject* table = g_table;
    if (!table) return nullptr;
    const FName n(name);
    if (uint8* row = find_row(table, n)) return row;
    auto* row_struct = *reinterpret_cast<UStruct**>(reinterpret_cast<uint8*>(table) + kRowStruct);
    FProperty* p_name = row_struct ? row_struct->FindProperty(FName(STR("unlockName"))) : nullptr;
    const uint8* tmpl = find_row(table, FName(kTemplateRow));
    void* add_row_fn = (*reinterpret_cast<void* const* const*>(table))[kVAddRow / sizeof(void*)];
    if (!p_name || !tmpl || !in_exe(add_row_fn)) return nullptr;
    reinterpret_cast<void (*)(UObject*, FName, const void*)>(add_row_fn)(table, n, tmpl);
    uint8* row = find_row(table, n);
    if (row) *reinterpret_cast<FName*>(row + p_name->GetOffset_Internal()) = n;
    return row;
}

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
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "data_table.hpp"
#include "unlocks.hpp"

#include <filesystem>

using namespace fue;
using namespace data_table;

namespace
{
    constexpr const CharType* kTablePath = STR("/Game/Gameplay/Meta/DT_UnlocksSongs.DT_UnlocksSongs");
    constexpr const CharType* kTemplateRow = STR("bornthisway");
    constexpr uint8 kSong = 1;                                             // EUnlockableType::Song
    constexpr uint8 kProgressionReward = 0, kEventReward = 1;             // EUnlockCategory
    UObject* g_table = nullptr;

    // Content/Paks/custom_songs/**/<name>_P.pak -> <name>, the naming the song loader has always used
    auto custom_song_names() -> std::vector<StringType>
    {
        std::vector<StringType> names{};
        std::error_code ec{};
        const auto dir = std::filesystem::path(module_dir()).parent_path().parent_path() / L"Content" / L"Paks" / L"custom_songs";
        for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (!it->is_regular_file(ec) || it->path().extension() != L".pak") continue;
            const auto stem = it->path().stem().wstring();
            if (stem.size() > 2 && stem.ends_with(L"_P")) names.push_back(stem.substr(0, stem.size() - 2));
        }
        return names;
    }

    auto apply() -> void
    {
        const auto t0 = GetTickCount64();
        UObject* loaded_before = UObjectGlobals::StaticFindObject(nullptr, nullptr, kTablePath);
        UObject* table = loaded_before ? loaded_before : load_blocking(kTablePath);
        auto fail = [](const CharType* why) { Output::send<LogLevel::Error>(STR("[unlocks] {} - custom and event songs are NOT unlocked\n"), why); };
        // a plain UDataTable of FUnlockable rows
        UStruct* rs = row_struct(table);
        if (!rs) return fail(STR("DT_UnlocksSongs did not load as a DataTable"));
        FProperty* p_name = rs->FindProperty(FName(STR("unlockName")));
        FProperty* p_type = rs->FindProperty(FName(STR("unlockType")));
        FProperty* p_cat = rs->FindProperty(FName(STR("unlockCategory")));
        if (rs->GetName() != STR("Unlockable") || !p_name || !p_type || !p_cat) return fail(STR("DT_UnlocksSongs rows are not FUnlockable"));
        if (!can_add_rows(table)) return fail(STR("UDataTable::AddRow is not where 4.24 puts it"));
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
            if (uint8* row = add_row(table, n, tmpl)) // a copy of the template row under this name
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
    UStruct* rs = row_struct(table);
    FProperty* p_name = rs ? rs->FindProperty(FName(STR("unlockName"))) : nullptr;
    if (!p_name) return nullptr;
    uint8* row = add_row(table, n, find_row(table, FName(kTemplateRow)));
    if (row) *reinterpret_cast<FName*>(row + p_name->GetOffset_Internal()) = n;
    return row;
}

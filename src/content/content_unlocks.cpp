// Unlock rows for content items, for every EUnlockableType: a row in the type's DT_Unlocks* table (before UnlockMgr
// reads it, or later), and - for a row added after UnlockMgr's init - the row inserted into UnlockMgr's lists.
//
// UnlockMgr (EGS v1.4.2 FUN_140739370 / FUN_1407427e0) reads each type's table once: GetAllRows into a per-type
// array of row pointers (FUN_1407335b0: Mission +0xa8, Song +0x98, Venue +0xb8, Instrument +0xc8, AudioEffects +0xd8,
// OutfitPartVariant +0xe8, BodyTextureAsset +0xf8, Emote +0x108, VenueItem +0x118), then one entry per row in a name
// map shared by every type (+0x128, TMap<FName, FUnlockable*>). GetUnlockState finds a row through that map; the
// item lists walk the arrays.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "content/content_types.hpp"

#include <array>

using namespace fue;
using namespace data_table;

namespace content
{
    namespace
    {
        constexpr std::array<const CharType*, static_cast<size_t>(UnlockType::Count)> kTables = {
            STR("/Game/Gameplay/Meta/DT_UnlocksMissions.DT_UnlocksMissions"),
            STR("/Game/Gameplay/Meta/DT_UnlocksSongs.DT_UnlocksSongs"),
            STR("/Game/Gameplay/Meta/DT_UnlocksVenues.DT_UnlocksVenues"),
            STR("/Game/Gameplay/Meta/DT_UnlocksInstruments.DT_UnlocksInstruments"),
            STR("/Game/Gameplay/Meta/DT_UnlocksAudioEffects.DT_UnlocksAudioEffects"),
            STR("/Game/Gameplay/Meta/DT_UnlocksOutfitPartVariants.DT_UnlocksOutfitPartVariants"),
            STR("/Game/Gameplay/Meta/DT_UnlocksBodyTextureAssets.DT_UnlocksBodyTextureAssets"),
            STR("/Game/Gameplay/Meta/DT_UnlocksEmotes.DT_UnlocksEmotes"),
            STR("/Game/Gameplay/Meta/DT_UnlocksVenueItems.DT_UnlocksVenueItems"),
        };
        constexpr std::array<size_t, static_cast<size_t>(UnlockType::Count)> kRowArrays = {0xa8, 0x98, 0xb8, 0xc8, 0xd8, 0xe8, 0xf8, 0x108, 0x118};
        constexpr size_t kNameMap = 0x128;
        constexpr size_t kNameMapStride = fname_map_stride(sizeof(void*)); // TPair<FName, FUnlockable*> + hash links

        template <typename T>
        auto field(UStruct* rs, uint8* row, const CharType* name) -> T*
        {
            FProperty* p = rs->FindProperty(FName(name));
            return p ? p->ContainerPtrToValuePtr<T>(row) : nullptr;
        }

        // An unset platform id string: empty, or "0" (what the stock tables hold for an item that isn't DLC)
        auto unset_id(const TArray<TCHAR>* s) -> bool
        {
            return !s || s->Num() <= 1 || (s->Num() == 2 && (*s)[0] == STR('0'));
        }

        // A row UnlockMgr computes as unlocked from the start (FUN_140725800): a progression reward with no level,
        // venue, mission, instrument or effect requirement and no cost - and, to copy cleanly, no platform ids.
        auto is_free_template(UStruct* rs, uint8* row) -> bool
        {
            auto* category = field<uint8>(rs, row, STR("unlockCategory"));
            auto* audio = field<int32>(rs, row, STR("audioCreditsCost"));
            auto* visual = field<int32>(rs, row, STR("visualCreditsCost"));
            auto* level = field<int32>(rs, row, STR("levelReq"));
            auto* venue = field<FName>(rs, row, STR("venueReq"));
            auto* missions = field<TArray<void*>>(rs, row, STR("missionCompletionReqs"));
            auto* instrument = field<void*>(rs, row, STR("InstrumentUseReq"));
            auto* effect = field<void*>(rs, row, STR("AudioEffectUseReq"));
            auto* steam = field<int32>(rs, row, STR("SteamAppId"));
            auto* epic = field<TArray<TCHAR>>(rs, row, STR("EpicItemId"));
            if (!category || !audio || !visual || !level || !venue || !missions || !instrument || !effect) return false;
            return *category == 0 && *audio < 1 && *visual < 1 && *level == 0 && venue->IsNone() && missions->Num() == 0 && !*instrument &&
                   !*effect && (!steam || *steam == 0) && unset_id(epic);
        }

        auto unlock_mgr() -> UObject* { return find_live_object(STR("UnlockMgr")); }
    } // namespace

    auto unlock_table(UnlockType type) -> UObject*
    {
        const size_t t = static_cast<size_t>(type);
        if (t >= kTables.size()) return nullptr;
        UObject* table = UObjectGlobals::StaticFindObject(nullptr, nullptr, kTables[t]);
        if (!table) table = load_blocking(kTables[t]);
        UStruct* rs = row_struct(table);
        if (!rs || rs->GetName() != STR("Unlockable") || !can_add_rows(table)) return nullptr;
        table->SetRootSet(); // UnlockMgr loads it later through the same soft path and gets this object
        return table;
    }

    auto add_unlock_row(UnlockType type, const FName& id, UnlockCategory category, StringType* why) -> uint8*
    {
        UObject* table = unlock_table(type);
        if (!table) return *why = std::format(STR("the {} unlock table is not a table of FUnlockable"), unlock_type_name(static_cast<int>(type))), nullptr;
        UStruct* rs = row_struct(table);
        if (find_row(table, id)) return *why = STR("an unlock row of that name already exists"), nullptr;
        uint8* tmpl = nullptr;
        each_row(table, [&](const FName&, uint8* row) {
            if (!tmpl && is_free_template(rs, row)) tmpl = row;
        });
        if (!tmpl) return *why = std::format(STR("the {} unlock table has no free row to copy"), unlock_type_name(static_cast<int>(type))), nullptr;
        uint8* row = add_row(table, id, tmpl);
        if (!row) return *why = STR("UDataTable::AddRow did not add the row"), nullptr;
        *field<FName>(rs, row, STR("unlockName")) = id;
        *field<uint8>(rs, row, STR("unlockType")) = static_cast<uint8>(type);
        *field<uint8>(rs, row, STR("unlockCategory")) = static_cast<uint8>(category);
        if (auto* hidden = CastField<FBoolProperty>(rs->FindProperty(FName(STR("isHiddenWhenLocked"))))) hidden->SetPropertyValue(hidden->ContainerPtrToValuePtr<void>(row), false);
        return row;
    }

    auto register_unlock_live(UnlockType type, const FName& id, uint8* row, StringType* why) -> bool
    {
        UObject* mgr = unlock_mgr();
        if (!mgr) return *why = STR("no UnlockMgr"), false;
        uint8* base = reinterpret_cast<uint8*>(mgr);
        void* map = base + kNameMap;
        if (uint8* e = fname_map_find(map, kNameMapStride, id))
        {
            if (*reinterpret_cast<uint8**>(e + 8) == row) return true; // already registered
            return *why = STR("UnlockMgr already has an unlock row of that name (another unlock type's)"), false;
        }
        StringType err{};
        uint8* value = fname_map_add(map, kNameMapStride, id, [&](uint8* v) { *reinterpret_cast<uint8**>(v) = row; }, &err);
        if (!value) return *why = STR("UnlockMgr's name map: ") + err, false;
        reinterpret_cast<TArray<uint8*>*>(base + kRowArrays[static_cast<size_t>(type)])->Add(row);
        return true;
    }

    auto unlock_type_index(const StringType& name) -> int
    {
        for (int i = 0; i < static_cast<int>(UnlockType::Count); ++i)
            if (_wcsicmp(unlock_type_name(i).c_str(), name.c_str()) == 0) return i;
        return -1;
    }

    auto unlock_type_name(int index) -> StringType
    {
        static const CharType* const kNames[] = {STR("Mission"), STR("Song"), STR("Venue"), STR("Instrument"), STR("AudioEffects"),
                                                 STR("OutfitPartVariant"), STR("BodyTextureAsset"), STR("Emote"), STR("VenueItem")};
        return index >= 0 && index < static_cast<int>(std::size(kNames)) ? kNames[index] : StringType{};
    }
} // namespace content

#pragma once
// Content types: what each content.json "type" adds to the game. A type is a TypeHandler in content_types.cpp's
// table: the EUnlockableType its unlock rows use, a manifest check, the rows it adds to the game's tables, and how it
// inserts an item into managers that built their lists at boot (nullptr: such items wait for the next launch).
// Unlock rows are shared by every type (content_unlocks.cpp): one per item in the type's DT_Unlocks* table.
//
// Adding a type: write its handler (content_projector.cpp is the model) and add it to type_handlers().
#include "custom_content.hpp"
#include "data_table.hpp"
#include "multiplayer/json_lite.hpp"

#include <string>
#include <vector>

namespace content
{
    // EUnlockableType
    enum class UnlockType : uint8 { Mission, Song, Venue, Instrument, AudioEffects, OutfitPartVariant, BodyTextureAsset, Emote, VenueItem, Count };

    // An item read from a manifest
    struct Item
    {
        ItemInfo info;
        Source source = Source::Custom;
        StringType pak;                 // the pak's file name, for the log
        std::string folder;             // "CustomContent/<id>/", as in the pak
        jsonl::Value manifest;          // content.json
        std::vector<std::string> files; // the pak's files in that folder, relative to it
    };

    struct TypeHandler
    {
        const char* type;       // content.json "type"
        UnlockType unlock_type; // the unlock table its items' unlock rows go to
        // The manifest checked against the item's files: an error, or empty.
        StringType (*validate)(const Item& item);
        // The item's rows in the game's tables (before the managers read them, or later): an error, or empty.
        StringType (*add_rows)(const Item& item);
        // The item inserted into the managers that built their lists at boot: an error, or empty. nullptr: the
        // type can't, so an item added after boot is used from the next launch.
        StringType (*register_live)(const Item& item);
    };

    auto type_handlers() -> const std::vector<TypeHandler>&;
    auto find_handler(const std::string& type) -> const TypeHandler*;

    // ---- preview icons (content_types.cpp) -----------------------------------------------------------------
    // content.json "icon_image" (any type): a PNG in the item's folder. The check: an error, or empty (also when
    // the item has none).
    auto validate_icon_image(const Item& item) -> StringType;
    // The icon_image as a rooted runtime UTexture2D (KismetRenderingLibrary::ImportFileAsTexture2D, read through the
    // pak file system), set as the row's PreviewImage, SmallRewardImage and LargePreviewImage soft pointers (the
    // fields of FVenueCustomItemBase and UCharacterAssetBase). Those point at the texture in memory, so the menus'
    // loads resolve it without a package. Empty when the item has no icon_image or it was set; otherwise an error,
    // and the row keeps the images it had.
    auto set_icon_images(const Item& item, UStruct* rs, uint8* row) -> StringType;

    // ---- unlock rows (content_unlocks.cpp) -----------------------------------------------------------------
    // ProgressionReward rows copied from a free template are unlocked from the start; EventReward rows stay
    // LockedHidden until something sets them Unlocked (UnlockMgr's recheck never writes an EventReward row's state).
    enum class UnlockCategory : uint8 { ProgressionReward = 0, EventReward = 1 };

    // The type's unlock table, loaded and rooted the way UnlockMgr will load it.
    auto unlock_table(UnlockType type) -> UObject*;

    // A new unlock row `id` in the type's table: a copy of the table's first row that is free (a progression reward
    // with no requirement and no cost), named and typed for the item, in `category`. nullptr with why set when the
    // table or a template is missing, or the name is taken.
    auto add_unlock_row(UnlockType type, const FName& id, UnlockCategory category, StringType* why) -> uint8*;

    // The row inserted into UnlockMgr's lists after its init (its per-type row array and its name map), so
    // GetUnlockState and the item lists find it. False with why set when UnlockMgr or its map can't take it.
    auto register_unlock_live(UnlockType type, const FName& id, uint8* row, StringType* why) -> bool;

    // ---- shared helpers (custom_content.cpp) ------------------------------------------------------------------
    auto utf8_to_wide(const std::string& s) -> StringType;
    auto narrow_id(const StringType& s) -> std::string; // ids and type names are ASCII
    auto lower(std::string s) -> std::string;
    auto find_live_object(const CharType* class_name) -> UObject*; // the first non-default instance
} // namespace content

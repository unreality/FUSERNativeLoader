#pragma once
// Content types: what each content.json "type" adds to the game. A type is a TypeHandler: the EUnlockableType its
// unlock rows use, a manifest check, the rows it adds to the game's tables, and how it inserts an item into managers
// that built their lists at boot (nullptr: such items wait for the next launch). Unlock rows are shared by every type
// (add_unlock_row): one per item in the type's DT_Unlocks* table. The loader's own types are projector screens and
// outfit parts; a mod adds one with register_type (the loader's src/content/content_projector.cpp is the model).
//
// A content.json, its files and their names come from content paks, server content and other players: a handler
// treats them as untrusted, checking everything in validate before add_rows uses it.
#include "custom_content.hpp"
#include "data_table.hpp"
#include "json_lite.hpp"

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

    FNL_API auto type_handlers() -> const std::vector<TypeHandler>&;
    FNL_API auto find_handler(const std::string& type) -> const TypeHandler*;

    // A content type added by a mod: from fnl_mod_init, before the startup registration (unreal-init) reads the
    // content paks. `type` must point at static storage. False for a name already registered or a handler without a
    // name, validate, add_rows or a valid unlock type.
    FNL_API auto register_type(const TypeHandler& handler) -> bool;

    // ---- preview icons (content_types.cpp) -----------------------------------------------------------------
    // content.json "icon_image" (any type): a PNG in the item's folder. The check: an error, or empty (also when
    // the item has none).
    FNL_API auto validate_icon_image(const Item& item) -> StringType;
    // The icon_image as a rooted runtime UTexture2D (KismetRenderingLibrary::ImportFileAsTexture2D, read through the
    // pak file system), set as the row's PreviewImage, SmallRewardImage and LargePreviewImage soft pointers (the
    // fields of FVenueCustomItemBase and UCharacterAssetBase). Those point at the texture in memory, so the menus'
    // loads resolve it without a package. Empty when the item has no icon_image or it was set; otherwise an error,
    // and the row keeps the images it had.
    FNL_API auto set_icon_images(const Item& item, UStruct* rs, uint8* row) -> StringType;

    // ---- unlock rows (content_unlocks.cpp) -----------------------------------------------------------------
    // ProgressionReward rows copied from a free template are unlocked from the start; EventReward rows stay
    // LockedHidden until something sets them Unlocked (UnlockMgr's recheck never writes an EventReward row's state).
    enum class UnlockCategory : uint8 { ProgressionReward = 0, EventReward = 1 };

    // The type's unlock table, loaded and rooted the way UnlockMgr will load it.
    FNL_API auto unlock_table(UnlockType type) -> UObject*;

    // A new unlock row `id` in the type's table: a copy of the table's first row that is free (a progression reward
    // with no requirement and no cost), named and typed for the item, in `category`. nullptr with why set when the
    // table or a template is missing, or the name is taken.
    FNL_API auto add_unlock_row(UnlockType type, const FName& id, UnlockCategory category, StringType* why) -> uint8*;

    // The row inserted into UnlockMgr's lists after its init (its per-type row array and its name map), so
    // GetUnlockState and the item lists find it. False with why set when UnlockMgr or its map can't take it.
    FNL_API auto register_unlock_live(UnlockType type, const FName& id, uint8* row, StringType* why) -> bool;

    // ---- shared helpers (custom_content.cpp) ------------------------------------------------------------------
    FNL_API auto utf8_to_wide(const std::string& s) -> StringType;
    FNL_API auto narrow_id(const StringType& s) -> std::string; // ids and type names are ASCII
    FNL_API auto lower(std::string s) -> std::string;
    FNL_API auto find_live_object(const CharType* class_name) -> UObject*; // the first non-default instance
} // namespace content

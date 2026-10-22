#pragma once
// Custom and server content: stage items (and, as their types are added, every other unlockable) registered from
// content paks. Each item is a CustomContent/<id>/ folder in a pak mounted at ../../../Fuser/Content/ with a
// content.json naming its type; the type's handler (content_types.hpp) adds the game rows it needs.
//
// Two sources:
//   - custom_content/: Content/Paks/custom_content/**/*.pak, mounted by the engine at startup. Its items are free
//     (a free progression unlock row) and win any id conflict.
//   - server content: paks the multiplayer extension keeps for fuserd outside Content/Paks and mounts itself. Its
//     items are locked (an EventReward unlock row) until the game unlocks them: a purchase, a claimed reward, or
//     fuserd's claimed assets at login.
//
// At unreal-init (before UnlockMgr and UVenueMgr read their tables) both sources are registered, custom_content/
// first. An item that arrives later (a login sync) is registered while the game runs: its rows, then the live
// managers when its type can do that, otherwise at the next launch.
#include "fue/fue.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace content
{
    using namespace fue;

    enum class Source : uint8 { Custom, Server };

    // The items a content pak declares.
    struct ItemInfo
    {
        StringType id;          // its row / unlock name
        StringType type;        // content.json "type"
        StringType unlock_type; // its EUnlockableType name ("VenueItem")
        StringType name;        // display name
    };

    // Reads a pak's index and manifests from its file (no engine needed): the items it holds, checked as the
    // registration checks them - one CustomContent/<id>/ folder per item, nothing outside those folders, a known
    // type and the files its manifest names. Empty, with why set, when the pak can't be used.
    auto read_items(const std::filesystem::path& pak, StringType* why) -> std::vector<ItemInfo>;

    // The multiplayer extension's server paks for the startup registration: called once, at unreal-init, with the
    // ids custom_content/ registered (lower case). It mounts the server paks it keeps whose ids none of those
    // override, and returns them; they are registered after custom_content/.
    using ServerPaks = std::function<std::vector<std::filesystem::path>(const std::vector<StringType>& custom_ids)>;
    auto set_server_paks(ServerPaks provider) -> void;

    // The ids registered from custom_content/ (lower case). Valid after unreal-init.
    auto custom_ids() -> std::vector<StringType>;

    // A registered item, or nothing.
    struct Registered
    {
        ItemInfo info;
        Source source;
        bool live; // inserted into the live managers (false: its rows wait for the next launch)
    };
    auto registered(const StringType& id) -> std::optional<Registered>;

    // Registers a mounted server pak's items while the game runs (game thread): each item's rows, then the live
    // managers. One line per item: what happened.
    struct LiveResult
    {
        StringType id;
        bool ok = false;   // usable now
        StringType note;   // what was done, or why not
    };
    auto register_live(const std::filesystem::path& pak) -> std::vector<LiveResult>;

    // EUnlockableType: its index (-1 for an unknown name) and its name.
    auto unlock_type_index(const StringType& name) -> int;
    auto unlock_type_name(int index) -> StringType;
} // namespace content

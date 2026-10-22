#pragma once
// What host_hook.cpp shares with the multiplayer DLL's other features (server_content.cpp), and what those features
// give host_hook.cpp's hooks and engine tick.
#include "fue/fue.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace mp
{
    using namespace fue;

    // ---- host_hook.cpp ---------------------------------------------------------------------------------------
    auto supported() -> bool;                // a verified build: the exe addresses below are known
    auto on_game_thread() -> bool;
    auto api_base() -> StringType;           // fuserd's API base (".../api/"), "" when unknown
    auto session_token() -> StringType;      // the game's fuserd session token, "" before login
    auto ini(const CharType* key, const CharType* fallback) -> StringType; // FuserNative.ini
    struct Http
    {
        unsigned long status = 0;
        std::string body, error, sha256; // sha256: of the file written to the sink
        int64 bytes = 0;
    };
    auto http(const StringType& method, const StringType& url, const StringType& headers, const std::string& payload,
              const std::filesystem::path* file_sink) -> Http;
    auto file_sha256(const std::filesystem::path& path, int64* size) -> std::string;
    auto mount_pak(const wchar_t* path, bool* mounted) -> bool; // FPakPlatformFile::Mount, synchronous
    auto unlock_mgr() -> UObject*;
    // The .sig check every pak gets before a mount mid-session (p2p::check_song_sig): "" when it can be mounted
    // (a repaired chunk table included), else why not.
    auto check_sig(const std::filesystem::path& pak, const std::filesystem::path& sig, std::string* note) -> std::string;

    // ---- server_content.cpp ----------------------------------------------------------------------------------
    auto server_content_start() -> void; // host_hook_start, before unreal-init: the startup provider
    auto drive_server_content() -> void; // each engine tick
    // A claimed asset at login (store/elder_credit_store/get_claimed_assets/): unlocked again once its item is here.
    auto server_content_claimed(const FName& name, uint8 unlock_type) -> void;
    // Entering multiplayer: check fuserd for items uploaded since the login.
    auto server_content_recheck(const CharType* why) -> void;
    // A Diamond Store offer's asset the game can show: false for a non-song asset the game has no unlock row for.
    auto server_content_offer_ok(const FName& name, uint8 unlock_type) -> bool;
    // UHypeTitleScreen's state setter (title_set_state): holds the move to the main menu while the sync runs.
    using TitleSetStateFn = void (*)(void* title, uint8 state);
    auto title_set_state_detour(void* title, uint8 state) -> void;
    extern TitleSetStateFn g_title_set_state_original;
} // namespace mp

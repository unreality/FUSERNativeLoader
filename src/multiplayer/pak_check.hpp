#pragma once

// The custom-song pak layout check. A *_P pak overrides base game content, so a pak
// received from another player is mounted only when it is shaped like every community song pak:
//   - UE4 pak version 8, unencrypted index, no encrypted entries;
//   - mount point ../../../Fuser/Content/;
//   - every entry under Audio/Songs/<X>/, DLC/Songs/<X>/ or UI/AlbumArt/T_<X>* (no deeper path there);
//   - at most 64 MB.
// The sender checks its own file with the same rule before offering it.

#include <string>

namespace p2p
{
    constexpr unsigned long long kMaxSongPakBytes = 64ull * 1024 * 1024;

    // True when the pak at path has the layout above for song <X>; otherwise why says what is wrong.
    auto check_song_pak(const std::wstring& path, const std::string& x, std::string* why) -> bool;

    // The .sig check for a song pak, run before a pak is mounted mid-session or sent to another player.
    // A .sig (FPakSignatureFile) is: magic 0x73832DAA, version 1, a 512-byte signature block, then a CRC32 per 64 KB
    // chunk of the pak. The engine checks every chunk it reads synchronously against that table (the index chunk at
    // mount among them). FUSER's handler for a mismatch requests exit, so a pak changed after its .sig was written
    // quits the game when it is mounted mid-session. (At startup the handler isn't bound yet, which is why such a pak
    // works for a player who already has it installed.)
    //   Ok       - the standard signature block, and a chunk table that matches the pak;
    //   Repaired - the chunk table was stale (wrong hashes or chunk count), but the pak is exactly what its packer
    //              wrote (the footer's index SHA-1, every entry's data SHA-1 and header copy, every entry in bounds):
    //              the .sig is rewritten with the pak's real chunk hashes;
    //   Bad      - anything else (unreadable .sig, another signature block, a stale table on an inconsistent pak):
    //              refuse the song.
    // note says what was repaired, or why the song is Bad. max_pak_bytes: the largest pak read (server content paks
    // may be larger than songs).
    enum class SigVerdict { Ok, Repaired, Bad };
    auto check_song_sig(const std::wstring& pak, const std::wstring& sig, std::string* note,
                        unsigned long long max_pak_bytes = kMaxSongPakBytes) -> SigVerdict;
    constexpr unsigned long long kMaxContentPakBytes = 2048ull * 1024 * 1024;
} // namespace p2p

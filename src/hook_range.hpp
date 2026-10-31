#pragma once
// Whether a funchook hook can be installed safely (code_patch.cpp, for every hook made through the loader; C++17).
//
// funchook joins a hooked function to its trampoline page with a jmp rel32, which reaches +/-2 GB. It reserves that
// page by searching upward from the function for free memory, and range-checks only a page it reuses, never one it
// has just reserved. On Windows the memory above the exe is normally free. Under Wine/Proton the game's own
// allocations fill it, so a hook prepared once the game is running can get a page more than 2 GB away: the jump is
// truncated and lands 4 GB too low (Steam Deck crash, 2026-10-08: a drop hook prepared at venue entry got a page
// 2.8 GB above its function, and the first disc spawn jumped to 0xf46f1043).
//
// funchook_prepare replaces the target pointer with the trampoline, which sits on that page a few dozen bytes from the
// transit code the jump goes to. Install only when this holds; otherwise destroy the handle and leave the hook off.
#include <cstdint>

inline bool hook_in_range(const void* function, const void* trampoline)
{
    const int64_t distance = static_cast<int64_t>(reinterpret_cast<uintptr_t>(trampoline)) -
                             static_cast<int64_t>(reinterpret_cast<uintptr_t>(function) + 5);
    return distance > -0x7FFF0000LL && distance < 0x7FFF0000LL; // a 64 KB margin covers the rest of the page
}

// Code patches, the patch journal, change owners and hook sets (code_patch.hpp).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "loader/code_patch.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <funchook.h>

#include "fue/fue.hpp"
#include "hook_range.hpp"

using namespace fue;

namespace
{
    // Compared at each hooked function: funchook's jump, with room to spare
    constexpr size_t kHookBytes = 16;
    constexpr size_t kChunk = 1 << 20;

    std::mutex g_mutex{};
    std::map<uintptr_t, uint8_t> g_original{}; // a changed byte's address -> its value as shipped

    // One change: the bytes a hook's jump overwrote, a patch, or an import slot
    struct Change
    {
        uintptr_t at;
        size_t size;
        std::string owner;
    };
    std::vector<Change> g_changes{};

    // The shipped bytes of [at, at + count), from shipped; a byte journalled already keeps its first value
    auto record(const void* at, const uint8_t* shipped, size_t count) -> void
    {
        const auto a = reinterpret_cast<uintptr_t>(at);
        for (size_t i = 0; i < count; ++i) g_original.emplace(a + i, shipped[i]);
    }

    // "exe+0x..." in the game exe, else the address
    auto where(const void* p) -> StringType
    {
        const auto a = reinterpret_cast<uintptr_t>(p), base = reinterpret_cast<uintptr_t>(code_exe_base());
        return a >= base && a < base + code_exe_size() ? std::format(STR("exe+0x{:X}"), a - base) : std::format(STR("{}"), p);
    }

    // The owners of the changes touching [a, a + size), skip left out: each once, in the order they changed it
    auto overlapping(uintptr_t a, size_t size, const char* skip, std::vector<std::string>& owners) -> size_t
    {
        for (const Change& c : g_changes)
        {
            if (c.at >= a + size || a >= c.at + c.size || (skip && c.owner == skip)) continue;
            if (std::find(owners.begin(), owners.end(), c.owner) == owners.end()) owners.push_back(c.owner);
        }
        return owners.size();
    }

    auto joined(const std::vector<std::string>& names) -> std::string
    {
        std::string out{};
        for (const std::string& n : names) out += (out.empty() ? "" : ", ") + n;
        return out;
    }

    // The change kept, with a warning when someone else changed one of its bytes already (named at shown: a hook's
    // function rather than its first changed byte)
    auto add_change(const void* at, size_t size, const char* owner, const StringType& what, const void* shown = nullptr) -> void
    {
        const auto a = reinterpret_cast<uintptr_t>(at);
        std::vector<std::string> others{};
        if (overlapping(a, size, owner, others))
            Output::send<LogLevel::Warning>(STR("[fnl] {} at {}: changed by {} too, {} first\n"), what, where(shown ? shown : at), to_wstring(owner),
                                            to_wstring(joined(others)));
        const bool again = std::any_of(g_changes.begin(), g_changes.end(), [&](const Change& c) { return c.at == a && c.size == size && c.owner == owner; });
        if (!again) g_changes.push_back({a, size, owner});
    }

    auto hex_digit(char c) -> int
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // "48 8B ?? 05" (spaces optional, ? or ?? any byte) as bytes and a mask of the fixed ones
    auto parse_pattern(const char* pattern, std::vector<uint8_t>& bytes, std::vector<uint8_t>& fixed) -> bool
    {
        for (const char* c = pattern; *c;)
        {
            if (*c == ' ')
            {
                ++c;
                continue;
            }
            if (*c == '?')
            {
                c += c[1] == '?' ? 2 : 1;
                bytes.push_back(0);
                fixed.push_back(0);
                continue;
            }
            const int hi = hex_digit(c[0]), lo = c[1] ? hex_digit(c[1]) : -1;
            if (hi < 0 || lo < 0) return false;
            bytes.push_back(static_cast<uint8_t>(hi << 4 | lo));
            fixed.push_back(1);
            c += 2;
        }
        return !bytes.empty();
    }

    auto readable(const MEMORY_BASIC_INFORMATION& m) -> bool
    {
        return m.State == MEM_COMMIT && m.Protect && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    }

    // ---------------------------------------------------------------- hooks ---------------------------------------
    // What a gate's code reads, on a writable page
    struct GateData
    {
        void* detour;
        void* trampoline;
        volatile char on;
    };

    struct HookEntry
    {
        std::string name;
        void** target = nullptr;        // the caller's variable: the function, then the trampoline
        void* function = nullptr;
        void* detour = nullptr;
        uint8_t* gate = nullptr;        // nullptr: funchook jumps to the detour itself (code_hook)
        GateData* data = nullptr;
        void* trampoline = nullptr;
        uint8_t prepared[kHookBytes]{}; // the function's bytes when funchook copied its prologue
    };
} // namespace

struct CodeHookSet
{
    std::string owner;
    std::vector<HookEntry> hooks;
    funchook_t* handle = nullptr;
    bool gated = false;
    bool installed = false;
    bool broken = false; // its jumps couldn't be written: never tried again
};

namespace
{
    std::vector<std::unique_ptr<CodeHookSet>> g_sets{}; // every prepared set; never freed, its gates stay reachable

    // Gates: kGateCode bytes of code each in the first half of a block (executable), their GateData in the second
    constexpr size_t kGateBlock = 64 * 1024, kGateCode = 32, kGateSlots = kGateBlock / 2 / kGateCode;
    static_assert(kGateSlots * sizeof(GateData) <= kGateBlock / 2);
    uint8_t* g_gate_block = nullptr;
    size_t g_gates_used = kGateSlots;

    auto put32(uint8_t* at, int64_t v) -> void
    {
        const auto d = static_cast<int32_t>(v);
        std::memcpy(at, &d, 4);
    }

    // A new gate for e, off. Its code: cmp byte [rip+on], 0 / je +6 / jmp [rip+detour] / jmp [rip+trampoline]. It
    // touches no register but the flags, which are volatile at a function's entry.
    auto new_gate(HookEntry& e) -> bool
    {
        DWORD old = 0;
        if (g_gates_used == kGateSlots)
        {
            auto* block = static_cast<uint8_t*>(VirtualAlloc(nullptr, kGateBlock, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!block) return false;
            if (!VirtualProtect(block, kGateBlock / 2, PAGE_EXECUTE_READ, &old))
            {
                VirtualFree(block, 0, MEM_RELEASE);
                return false;
            }
            g_gate_block = block;
            g_gates_used = 0;
        }
        const size_t i = g_gates_used++;
        uint8_t* code = g_gate_block + i * kGateCode;
        auto* data = reinterpret_cast<GateData*>(g_gate_block + kGateBlock / 2 + i * sizeof(GateData));
        data->detour = e.detour;
        data->trampoline = nullptr;
        data->on = 0;

        const auto rel = [code](size_t next, const volatile void* p) {
            return static_cast<int64_t>(reinterpret_cast<uintptr_t>(p)) - static_cast<int64_t>(reinterpret_cast<uintptr_t>(code + next));
        };
        uint8_t c[kGateCode];
        std::memset(c, 0xCC, sizeof(c));
        c[0] = 0x80, c[1] = 0x3D, put32(c + 2, rel(7, &data->on)), c[6] = 0x00;  // cmp byte [rip+on], 0
        c[7] = 0x74, c[8] = 0x06;                                                 // je +6 (off: the trampoline)
        c[9] = 0xFF, c[10] = 0x25, put32(c + 11, rel(15, &data->detour));         // jmp [rip+detour]
        c[15] = 0xFF, c[16] = 0x25, put32(c + 17, rel(21, &data->trampoline));    // jmp [rip+trampoline]
        // still executable while it is written: the block's other gates may be running
        if (!VirtualProtect(code, kGateCode, PAGE_EXECUTE_READWRITE, &old)) return false;
        std::memcpy(code, c, kGateCode);
        VirtualProtect(code, kGateCode, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), code, kGateCode);
        e.gate = code;
        e.data = data;
        return true;
    }

    // The set's entries from hooks[], each with a gate when the set is gated
    auto fill(CodeHookSet& set, const FnlHook* hooks, size_t count, StringType& why) -> bool
    {
        for (size_t i = 0; i < count; ++i)
        {
            const FnlHook& h = hooks[i];
            HookEntry e{};
            e.name = h.name ? h.name : "?";
            if (!h.target || !*h.target || !h.detour)
            {
                why = std::format(STR("hook {}: no function or no detour"), to_wstring(e.name));
                return false;
            }
            e.target = h.target;
            e.function = *h.target;
            e.detour = h.detour;
            if (set.gated && !new_gate(e))
            {
                why = std::format(STR("hook {}: no memory for its gate (Win32 {})"), to_wstring(e.name), GetLastError());
                return false;
            }
            set.hooks.push_back(std::move(e));
        }
        return true;
    }

    // funchook_prepare for each of the set's hooks on a new handle, each trampoline range-checked
    auto prepare_all(CodeHookSet& set, StringType& why) -> bool
    {
        set.handle = funchook_create();
        if (!set.handle)
        {
            why = STR("funchook_create failed");
            return false;
        }
        for (HookEntry& e : set.hooks)
        {
            std::memcpy(e.prepared, e.function, kHookBytes);
            void* trampoline = e.function;
            const int prep = funchook_prepare(set.handle, &trampoline, e.gate ? static_cast<void*>(e.gate) : e.detour);
            if (prep != 0)
                why = std::format(STR("hook {} at {}: funchook_prepare {} ({})"), to_wstring(e.name), where(e.function), prep, to_wstring(funchook_error_message(set.handle)));
            else if (!hook_in_range(e.function, trampoline))
                why = std::format(STR("hook {} at {}: funchook's trampoline {} is out of jump range"), to_wstring(e.name), where(e.function), trampoline);
            if (!why.empty())
            {
                funchook_destroy(set.handle);
                set.handle = nullptr;
                return false;
            }
            e.trampoline = trampoline;
        }
        return true;
    }

    // A written jump journalled and owned: the bytes that differ from the ones it was prepared over
    auto journal_hook(const HookEntry& e, const std::string& owner) -> void
    {
        const auto* now = static_cast<const uint8_t*>(e.function);
        size_t lo = 0, hi = 0;
        for (size_t i = 0; i < kHookBytes; ++i)
        {
            if (now[i] == e.prepared[i]) continue;
            if (hi == 0) lo = i;
            hi = i + 1;
        }
        if (hi == 0) hi = 5; // a jmp rel32, which can't leave all its bytes as they were
        record(now + lo, e.prepared + lo, hi - lo);
        add_change(now + lo, hi - lo, owner.c_str(), std::format(STR("hook {}"), to_wstring(e.name)), e.function);
    }

    // The set's jumps written: prepared again first if one of its functions changed since (its trampoline would
    // otherwise run the old bytes, dropping that change)
    auto install(CodeHookSet& set) -> bool
    {
        const StringType owner = to_wstring(set.owner);
        bool moved = false;
        for (const HookEntry& e : set.hooks) moved |= std::memcmp(e.prepared, e.function, kHookBytes) != 0;
        if (moved)
        {
            Output::send<LogLevel::Normal>(STR("[fnl] {}: a function changed since its hooks were prepared - preparing them again\n"), owner);
            funchook_destroy(set.handle);
            set.handle = nullptr;
            StringType why{};
            if (!prepare_all(set, why))
            {
                Output::send<LogLevel::Error>(STR("[fnl] {}: {} - none installed\n"), owner, why);
                return false;
            }
        }
        for (HookEntry& e : set.hooks)
        {
            if (e.data) e.data->trampoline = e.trampoline;
            else *e.target = e.trampoline; // before the jump: a detour can run at once and calls through it
        }
        const int inst = funchook_install(set.handle, 0);
        if (inst != 0)
        {
            // Not destroyed: functions it patched already jump to its trampolines (or to a gate, which is off)
            Output::send<LogLevel::Error>(STR("[fnl] {}: funchook_install {} ({}) - some of its {} hooks may be in place\n"), owner, inst,
                                          to_wstring(funchook_error_message(set.handle)), set.hooks.size());
            return false;
        }
        for (HookEntry& e : set.hooks)
        {
            *e.target = e.trampoline;
            journal_hook(e, set.owner);
            Output::send<LogLevel::Verbose>(STR("[fnl] hook {} at {} ({})\n"), to_wstring(e.name), where(e.function), owner);
        }
        set.installed = true;
        return true;
    }
} // namespace

uint8_t* code_exe_base()
{
    return reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
}

size_t code_exe_size()
{
    static const size_t size = [] {
        uint8_t* base = code_exe_base();
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        return static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
    }();
    return size;
}

// The range is read a chunk at a time into a buffer, with the journal's bytes laid over it. Regions that can't be
// read (between sections, guard pages) are skipped.
uint8_t* code_find_pattern(const char* pattern, const uint8_t* start, size_t size)
{
    std::vector<uint8_t> want{}, fixed{};
    if (!pattern || !parse_pattern(pattern, want, fixed)) return nullptr;
    const auto base = reinterpret_cast<uintptr_t>(code_exe_base()), image_end = base + code_exe_size();
    uintptr_t from = start ? reinterpret_cast<uintptr_t>(start) : base;
    const uintptr_t end = start ? (size > image_end - std::min(from, image_end) ? image_end : from + size) : image_end;
    from = std::max(from, base);
    const size_t n = want.size();
    if (from >= end || end - from < n) return nullptr;

    std::lock_guard lock(g_mutex);
    std::vector<uint8_t> buf(kChunk + n - 1);
    for (uintptr_t at = from; at + n <= end;)
    {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &m, sizeof(m))) break;
        const uintptr_t region_end = std::min(end, reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize);
        if (readable(m))
        {
            while (at + n <= region_end)
            {
                const size_t len = static_cast<size_t>(std::min<uintptr_t>(kChunk + n - 1, region_end - at));
                std::memcpy(buf.data(), reinterpret_cast<const void*>(at), len);
                for (auto it = g_original.lower_bound(at); it != g_original.end() && it->first < at + len; ++it) buf[it->first - at] = it->second;
                const size_t last = len - n;
                for (size_t i = 0; i <= last; ++i)
                {
                    if (fixed[0])
                    {
                        const void* hit = std::memchr(buf.data() + i, want[0], last - i + 1);
                        if (!hit) break;
                        i = static_cast<const uint8_t*>(hit) - buf.data();
                    }
                    size_t j = 1;
                    while (j < n && (!fixed[j] || buf[i + j] == want[j])) ++j;
                    if (j == n && (!fixed[0] || buf[i] == want[0])) return reinterpret_cast<uint8_t*>(at + i);
                }
                at += last + 1;
            }
        }
        at = region_end;
    }
    return nullptr;
}

bool code_hook(const char* owner, const FnlHook* hooks, size_t count)
{
    if (!owner || !hooks || !count) return false;
    std::lock_guard lock(g_mutex);
    CodeHookSet set{};
    set.owner = owner;
    StringType why{};
    if (!fill(set, hooks, count, why) || !prepare_all(set, why))
    {
        Output::send<LogLevel::Error>(STR("[fnl] {}: {} - none hooked\n"), to_wstring(owner), why);
        return false;
    }
    return install(set); // the handle stays allocated: its trampolines are in use
}

CodeHookSet* code_hook_prepare(const char* owner, const FnlHook* hooks, size_t count, uint32_t flags)
{
    if (!owner || !hooks || !count) return nullptr;
    if (flags & ~static_cast<uint32_t>(FNL_HOOK_INSTALL_LATER))
    {
        // a flag from a newer SDK: refused, so the mod knows it didn't get what it asked for
        Output::send<LogLevel::Error>(STR("[fnl] {}: hook_prepare with unknown flags 0x{:X} - none prepared\n"), to_wstring(owner), flags);
        return nullptr;
    }
    std::lock_guard lock(g_mutex);
    auto set = std::make_unique<CodeHookSet>();
    set->owner = owner;
    set->gated = true;
    StringType why{};
    if (!fill(*set, hooks, count, why) || !prepare_all(*set, why))
    {
        Output::send<LogLevel::Error>(STR("[fnl] {}: {} - none prepared\n"), to_wstring(owner), why);
        return nullptr;
    }
    const bool later = (flags & FNL_HOOK_INSTALL_LATER) != 0;
    CodeHookSet* out = set.get();
    if (!later && !install(*set))
    {
        set->broken = true;
        out = nullptr;
    }
    if (out)
        Output::send<LogLevel::Verbose>(STR("[fnl] {}: {} hook(s) prepared from {}, off{}\n"), to_wstring(owner), set->hooks.size(), to_wstring(set->hooks[0].name),
                                        later ? STR(", installed at the first enable") : STR(""));
    g_sets.push_back(std::move(set)); // kept even when broken: jumps already written lead to its gates
    return out;
}

bool code_hook_enable(const char* owner, CodeHookSet* set, bool enabled)
{
    if (!owner || !set) return false;
    std::lock_guard lock(g_mutex);
    const bool known = std::any_of(g_sets.begin(), g_sets.end(), [set](const auto& s) { return s.get() == set; });
    if (!known || set->owner != owner)
    {
        Output::send<LogLevel::Error>(STR("[fnl] {}: hook_enable on a set it didn't prepare\n"), to_wstring(owner));
        return false;
    }
    if (enabled && !set->installed)
    {
        if (set->broken) return false;
        if (!install(*set))
        {
            set->broken = true;
            return false;
        }
    }
    if (!set->installed) return true; // off, and never written
    for (HookEntry& e : set->hooks) InterlockedExchange8(&e.data->on, enabled ? 1 : 0);
    Output::send<LogLevel::Verbose>(STR("[fnl] {}: {} hook(s) from {} {}\n"), to_wstring(owner), set->hooks.size(), to_wstring(set->hooks[0].name),
                                    enabled ? STR("on") : STR("off"));
    return true;
}

size_t code_changed(const char* skip_owner, const void* at, size_t size, std::string* owners)
{
    if (!at || !size) return 0;
    std::lock_guard lock(g_mutex);
    std::vector<std::string> names{};
    const size_t n = overlapping(reinterpret_cast<uintptr_t>(at), size, skip_owner, names);
    if (owners) *owners = joined(names);
    return n;
}

bool code_patch(const char* owner, void* at, const void* bytes, size_t count, bool strict)
{
    if (!owner || !at || !bytes || !count) return false;
    std::lock_guard lock(g_mutex);
    if (strict)
    {
        std::vector<std::string> others{};
        if (overlapping(reinterpret_cast<uintptr_t>(at), count, owner, others))
        {
            Output::send<LogLevel::Warning>(STR("[fnl] {}: strict patch of {} byte(s) at {} refused - changed by {} already\n"), to_wstring(owner), count, where(at),
                                            to_wstring(joined(others)));
            return false;
        }
    }
    DWORD old = 0;
    if (!VirtualProtect(at, count, PAGE_EXECUTE_READWRITE, &old))
    {
        Output::send<LogLevel::Error>(STR("[fnl] {}: patch at {} not writable (Win32 {})\n"), to_wstring(owner), where(at), GetLastError());
        return false;
    }
    record(at, static_cast<const uint8_t*>(at), count);
    std::memcpy(at, bytes, count);
    VirtualProtect(at, count, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, count);
    add_change(at, count, owner, std::format(STR("patch of {} byte(s)"), count));
    Output::send<LogLevel::Verbose>(STR("[fnl] patch {} byte(s) at {} ({})\n"), count, where(at), to_wstring(owner));
    return true;
}

bool code_redirect_import(const char* owner, const char* dll, const char* function, void* replacement, void** original)
{
    if (!owner || !function || !replacement || !original) return false;
    uint8_t* exe = code_exe_base();
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe + reinterpret_cast<const IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    void** slot = nullptr;
    const char* from = nullptr;
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(exe + imports.VirtualAddress); imports.VirtualAddress && d->Name && !slot; ++d)
    {
        const char* name = reinterpret_cast<const char*>(exe + d->Name);
        if ((dll && _strnicmp(name, dll, std::strlen(dll)) != 0) || !d->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(exe + d->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(exe + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(exe + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function) != 0) continue;
            slot = reinterpret_cast<void**>(&slots->u1.Function);
            from = name;
            break;
        }
    }
    const StringType what = std::format(STR("import {}!{}"), to_wstring(from ? from : dll ? dll : "*"), to_wstring(function));
    if (!slot)
    {
        Output::send<LogLevel::Verbose>(STR("[fnl] {}: {} not found\n"), to_wstring(owner), what);
        return false;
    }
    std::lock_guard lock(g_mutex);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
    {
        Output::send<LogLevel::Error>(STR("[fnl] {}: {} slot not writable (Win32 {})\n"), to_wstring(owner), what, GetLastError());
        return false;
    }
    *original = *slot; // set before the slot changes: the replacement may run at once, on another thread
    InterlockedExchangePointer(slot, replacement);
    VirtualProtect(slot, sizeof(void*), old, &old);
    add_change(slot, sizeof(void*), owner, what);
    Output::send<LogLevel::Verbose>(STR("[fnl] {} redirected ({})\n"), what, to_wstring(owner));
    return true;
}

// Code patches, the patch journal and hook owners (code_patch.hpp).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "loader/code_patch.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <funchook.h>

#include "fue/fue.hpp"
#include "hook_range.hpp"

using namespace fue;

namespace
{
    // Journalled at each hooked function: funchook's jump, with room to spare
    constexpr size_t kHookBytes = 16;
    constexpr size_t kChunk = 1 << 20;

    std::mutex g_mutex{};
    std::map<uintptr_t, uint8_t> g_original{};          // a changed byte's address -> its value as shipped
    std::map<uintptr_t, std::string> g_owners{};        // a hooked function or import slot -> who changed it

    auto record(const void* at, size_t count) -> void
    {
        const auto a = reinterpret_cast<uintptr_t>(at);
        for (size_t i = 0; i < count; ++i) g_original.emplace(a + i, static_cast<const uint8_t*>(at)[i]); // keeps the first
    }

    // "exe+0x..." in the game exe, else the address
    auto where(const void* p) -> StringType
    {
        const auto a = reinterpret_cast<uintptr_t>(p), base = reinterpret_cast<uintptr_t>(code_exe_base());
        return a >= base && a < base + code_exe_size() ? std::format(STR("exe+0x{:X}"), a - base) : std::format(STR("{}"), p);
    }

    // The owner added to what changed `at`, with a warning when someone else changed it already
    auto add_owner(const void* at, const char* owner, const StringType& what) -> void
    {
        std::string& owners = g_owners[reinterpret_cast<uintptr_t>(at)];
        if (!owners.empty())
            Output::send<LogLevel::Warning>(STR("[fnl] {} at {}: changed by {} too, {} first\n"), what, where(at), to_wstring(owner),
                                            to_wstring(owners));
        owners += (owners.empty() ? "" : ", ") + std::string(owner);
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
    std::vector<void*> functions(count, nullptr);
    funchook_t* handle = funchook_create();
    auto fail = [&](size_t prepared, const StringType& why) {
        Output::send<LogLevel::Error>(STR("[fnl] {}: {} - none hooked\n"), to_wstring(owner), why);
        if (handle) funchook_destroy(handle);
        for (size_t i = 0; i < prepared; ++i) *hooks[i].target = functions[i];
        return false;
    };
    if (!handle) return fail(0, STR("funchook_create failed"));
    for (size_t i = 0; i < count; ++i)
    {
        const FnlHook& h = hooks[i];
        const StringType name = to_wstring(h.name ? h.name : "?");
        if (!h.target || !*h.target || !h.detour) return fail(i, std::format(STR("hook {}: no function or no detour"), name));
        functions[i] = *h.target;
        record(functions[i], kHookBytes);
        const int prep = funchook_prepare(handle, h.target, h.detour);
        if (prep != 0) return fail(i + 1, std::format(STR("hook {} at {}: funchook_prepare {} ({})"), name, where(functions[i]), prep, to_wstring(funchook_error_message(handle))));
        if (!hook_in_range(functions[i], *h.target))
            return fail(i + 1, std::format(STR("hook {} at {}: funchook's trampoline {} is out of jump range"), name, where(functions[i]), *h.target));
    }
    const int inst = funchook_install(handle, 0);
    if (inst != 0)
    {
        // Not destroyed: functions it patched already jump to its trampolines
        Output::send<LogLevel::Error>(STR("[fnl] {}: funchook_install {} ({}) - some of its {} hooks may be in place\n"), to_wstring(owner), inst,
                                      to_wstring(funchook_error_message(handle)), count);
        return false;
    }
    for (size_t i = 0; i < count; ++i)
    {
        const StringType name = to_wstring(hooks[i].name ? hooks[i].name : "?");
        add_owner(functions[i], owner, std::format(STR("hook {}"), name));
        Output::send<LogLevel::Verbose>(STR("[fnl] hook {} at {} ({})\n"), name, where(functions[i]), to_wstring(owner));
    }
    return true;
}

bool code_patch(const char* owner, void* at, const void* bytes, size_t count)
{
    if (!owner || !at || !bytes || !count) return false;
    std::lock_guard lock(g_mutex);
    DWORD old = 0;
    if (!VirtualProtect(at, count, PAGE_EXECUTE_READWRITE, &old))
    {
        Output::send<LogLevel::Error>(STR("[fnl] {}: patch at {} not writable (Win32 {})\n"), to_wstring(owner), where(at), GetLastError());
        return false;
    }
    record(at, count);
    std::memcpy(at, bytes, count);
    VirtualProtect(at, count, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, count);
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
    add_owner(slot, owner, what);
    Output::send<LogLevel::Verbose>(STR("[fnl] {} redirected ({})\n"), what, to_wstring(owner));
    return true;
}

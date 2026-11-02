// Pak diagnostics, in every build (pak_diag.hpp).
//
// FUSER binds one handler to the engine's pak master-signature and chunk-signature failure delegates. It formats
// "Signature check failed for pak file: %s" and requests a clean engine exit: the game closes, with no crash dump and
// no log. The loader's signature bypass covers only *_P paks' master hash and the async-read chunk check, so a
// failure can still come from a pak mounted at runtime (the DLC scan mounts the official DLC paks and the DLC
// manifest) or from a synchronous read of a pak whose .sig chunk table is stale.
//
// The handlers are found through that string's references, not by RVA: three functions on the EGS builds
// (0x68c160, 0x68c350, 0x69eae0), one on Steam (0x6d46c0). Each takes the pak's FString in rdx; 0x68c350 returns a
// bool. The detours log the pak and pass rcx, rdx, r8, r9 and rax through, so the game still exits as before.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <string_view>
#include <vector>

#include "data_table.hpp"
#include "fue/fue.hpp"
#include "loader/code_patch.hpp"
#include "pak_diag.hpp"

using namespace fue;

namespace
{
    constexpr std::wstring_view kHandlerText = L"Signature check failed for pak file";
    constexpr int kMaxHandlers = 4;
    constexpr uint64 kPollMs = 500, kScanMs = 2000, kProgressMs = 5000;

    using HandlerFn = uint64 (*)(void* self, const FString* pak, void* r8, void* r9);
    struct Handler
    {
        uint8* function = nullptr;
        HandlerFn original = nullptr;
    };
    Handler g_handlers[kMaxHandlers]{};
    uint8* g_exe = nullptr;

    // Paks opened once the engine is running, and the DLC scan as last polled (read from any thread)
    std::atomic<bool> g_running{false};
    std::atomic<DWORD> g_game_thread{0};
    std::atomic<int> g_opened{0};
    std::atomic<int> g_scan_state{-1}, g_scan_done{0}, g_scan_total{0};
    std::mutex g_last_mu{};
    StringType g_last_opened{};

    auto scan_state_name(int state) -> const CharType*
    {
        switch (state)
        {
        case 0: return STR("NotStarted");
        case 1: return STR("InitialChecks");
        case 2: return STR("Enumerating");
        case 3: return STR("Finished");
        default: return STR("unknown");
        }
    }

    auto on_failure(int slot, const FString* pak) -> void
    {
        StringType last{};
        {
            std::lock_guard lock(g_last_mu);
            last = g_last_opened;
        }
        const DWORD thread = GetCurrentThreadId();
        Output::send<LogLevel::Error>(STR("[pak] signature check failed: {} (handler exe+0x{:X}, {} thread {}) - FUSER now closes the game. "
                                          "DLC scan {} {}/{}; {} paks opened since startup, the last {}\n"),
                                      pak ? **pak : STR("(none)"), static_cast<size_t>(g_handlers[slot].function - g_exe),
                                      thread == g_game_thread.load() ? STR("game") : STR("worker"), thread, scan_state_name(g_scan_state.load()),
                                      g_scan_done.load(), g_scan_total.load(), g_opened.load(), last.empty() ? STR("(none)") : last);
    }

    template <int N>
    auto handler_detour(void* self, const FString* pak, void* r8, void* r9) -> uint64
    {
        on_failure(N, pak);
        return g_handlers[N].original(self, pak, r8, r9);
    }
    constexpr HandlerFn kDetours[kMaxHandlers] = {&handler_detour<0>, &handler_detour<1>, &handler_detour<2>, &handler_detour<3>};

    auto nt_headers() -> IMAGE_NT_HEADERS64*
    {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_exe);
        return reinterpret_cast<IMAGE_NT_HEADERS64*>(g_exe + dos->e_lfanew);
    }

    auto for_each_section(const std::function<void(uint8* start, size_t size, DWORD flags)>& f) -> void
    {
        IMAGE_NT_HEADERS64* nt = nt_headers();
        IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
            f(g_exe + s->VirtualAddress, std::min<size_t>(s->Misc.VirtualSize, s->SizeOfRawData), s->Characteristics);
    }

    // The function holding `pc`: its exception-directory entry, followed through chained unwind info to the primary
    auto function_start(uint8* pc) -> uint8*
    {
        DWORD64 image = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(pc), &image, nullptr);
        for (int depth = 0; rf && depth < 8; ++depth)
        {
            const uint8* unwind = reinterpret_cast<const uint8*>(image + rf->UnwindData);
            if (!((unwind[0] >> 3) & UNW_FLAG_CHAININFO)) return reinterpret_cast<uint8*>(image + rf->BeginAddress);
            const uint8 codes = unwind[2];
            rf = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8*>(unwind + 4 + ((codes + 1) & ~1) * 2));
        }
        return nullptr;
    }

    // Every function with a `lea reg, [rip+disp32]` to the handler's message
    auto find_handlers() -> std::vector<uint8*>
    {
        const auto* needle = reinterpret_cast<const uint8*>(kHandlerText.data());
        const size_t needle_size = kHandlerText.size() * sizeof(wchar_t);
        std::vector<uint8*> strings{};
        for_each_section([&](uint8* start, size_t size, DWORD flags) {
            if (flags & IMAGE_SCN_MEM_EXECUTE) return;
            for (uint8* p = start; (p = std::search(p, start + size, needle, needle + needle_size)) != start + size; ++p)
                strings.push_back(p);
        });
        std::vector<uint8*> functions{};
        if (strings.empty()) return functions;
        for_each_section([&](uint8* start, size_t size, DWORD flags) {
            if (!(flags & IMAGE_SCN_MEM_EXECUTE) || size < 7) return;
            for (uint8* p = start; p + 7 <= start + size; ++p)
            {
                if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) continue;
                uint8* target = p + 7 + *reinterpret_cast<const int32*>(p + 3);
                if (std::find(strings.begin(), strings.end(), target) == strings.end()) continue;
                uint8* f = function_start(p);
                if (f && std::find(functions.begin(), functions.end(), f) == functions.end()) functions.push_back(f);
            }
        });
        return functions;
    }

    auto install_handler_hooks() -> void
    {
        const std::vector<uint8*> found = find_handlers();
        if (found.empty())
        {
            Output::send<LogLevel::Warning>(STR("[pak] signature failure handlers: none found - pak signature failures will not be logged\n"));
            return;
        }
        FnlHook requests[kMaxHandlers]{};
        StringType rvas{};
        size_t count = 0;
        for (uint8* f : found)
        {
            if (count == kMaxHandlers) break;
            Handler& h = g_handlers[count];
            h.function = f;
            h.original = reinterpret_cast<HandlerFn>(f);
            requests[count] = {"pak signature failure handler", reinterpret_cast<void**>(&h.original), reinterpret_cast<void*>(kDetours[count])};
            rvas += std::format(STR("{}0x{:X}"), rvas.empty() ? STR("") : STR(", "), static_cast<size_t>(f - g_exe));
            ++count;
        }
        const bool hooked = code_hook("loader", requests, count);
        Output::send<LogLevel::Normal>(STR("[pak] signature failure handlers: {} of {} {} ({})\n"), count, found.size(), hooked ? STR("hooked") : STR("NOT hooked"), rvas);
    }

    auto is_live(UObject* o) -> bool
    {
        for (UObject* x = o; x; x = x->GetOuterPrivate())
            if (x->GetName().starts_with(STR("Default__"))) return false;
        return true;
    }

    auto dlc_manager() -> UObject*
    {
        static FWeakObjectPtr seen{};
        static uint64 next_scan = 0;
        if (UObject* o = seen.Get()) return o;
        const uint64 now = GetTickCount64();
        if (now < next_scan) return nullptr;
        next_scan = now + kScanMs;
        std::vector<UObject*> all{};
        UObjectGlobals::FindAllOf(STR("DLCManager"), all);
        for (UObject* o : all)
            if (is_live(o))
            {
                seen = FWeakObjectPtr(o);
                return o;
            }
        return nullptr;
    }

    // UDLCManager::GetDlcScanningStatus, polled: every change of state, and the progress while it scans
    auto watch_dlc_scan() -> void
    {
        static uint64 next_poll = 0, started = 0, last_progress = 0;
        const uint64 now = GetTickCount64();
        if (now < next_poll) return;
        next_poll = now + kPollMs;
        UObject* mgr = dlc_manager();
        UFunction* fn = mgr ? mgr->GetFunctionByNameInChain(STR("GetDlcScanningStatus")) : nullptr;
        if (!fn) return;
        data_table::Call c(fn);
        if (!c.prop(STR("ReturnValue")) || !c.prop(STR("numItemsFinished")) || !c.prop(STR("numItemsTotal"))) return;
        mgr->ProcessEvent(fn, c.buf.data());
        const int state = *c.addr(STR("ReturnValue"));
        const int done = *reinterpret_cast<int32*>(c.addr(STR("numItemsFinished")));
        const int total = *reinterpret_cast<int32*>(c.addr(STR("numItemsTotal")));
        g_scan_done = done;
        g_scan_total = total;
        const int was = g_scan_state.exchange(state);
        const bool scanning = state == 1 || state == 2;
        if (state != was)
        {
            if (scanning && !(was == 1 || was == 2)) started = now;
            const StringType took = state == 3 && started ? std::format(STR(" after {} ms"), now - started) : StringType{};
            Output::send<LogLevel::Normal>(STR("[pak] DLC scan: {} -> {}{} ({}/{} items, {} paks opened since startup)\n"), scan_state_name(was),
                                           scan_state_name(state), took, done, total, g_opened.load());
            last_progress = now;
        }
        else if (scanning && now - last_progress >= kProgressMs)
        {
            Output::send<LogLevel::Normal>(STR("[pak] DLC scan: still {} after {} ms ({}/{} items, {} paks opened since startup)\n"), scan_state_name(state),
                                           now - started, done, total, g_opened.load());
            last_progress = now;
        }
    }
} // namespace

auto pak_diag_start() -> void
{
    g_exe = reinterpret_cast<uint8*>(GetModuleHandleW(nullptr));
    install_handler_hooks();
    Hook::RegisterEngineTickPreCallback([](Hook::TCallbackIterationData<void>&, UEngine*, float, bool) {
        if (!g_running.exchange(true))
        {
            g_game_thread = GetCurrentThreadId();
            Output::send<LogLevel::Normal>(STR("[pak] first engine tick on thread {}\n"), g_game_thread.load());
        }
        watch_dlc_scan();
    });
}

void pak_diag_opened(const wchar_t* file_name, bool is_signed)
{
    if (!g_running.load() || !file_name) return;
    const int n = ++g_opened;
    {
        std::lock_guard lock(g_last_mu);
        g_last_opened = file_name;
    }
    Output::send<LogLevel::Normal>(STR("[pak] opened #{}: {} ({})\n"), n, file_name, is_signed ? STR("signed") : STR("not signed"));
}

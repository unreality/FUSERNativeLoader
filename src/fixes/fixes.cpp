// FUSERFixes: fixes to the game's engine and SDKs. A game-stage mod on the C tier (mod.ini beside it).
//
// EOS_Shutdown exit guard: the EGS 1.4.2 EOS SDK can throw a C++ exception from EOS_Shutdown during normal game exit
// (the same stack in several crash dumps). The SDK's normal cleanup path is kept, but an exit-only failure no longer
// invokes UE's crash reporter and disrupts the still-running player process. The SDK DLL may load after this mod, so
// the hook is tried again every tick until it is there.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>

#include "fnl_mod.h"

namespace
{
    const FnlApi* g_api = nullptr;
    FnlMod* g_self = nullptr;

    using EosShutdownFn = int32_t (*)();
    EosShutdownFn g_eos_shutdown = nullptr;
    bool g_eos_attempted = false;

    auto eos_shutdown_detour() -> int32_t
    {
        try
        {
            return g_eos_shutdown();
        }
        catch (...)
        {
            g_api->log(g_self, FNL_LOG_WARNING, L"EOS_Shutdown threw during process exit; continuing shutdown");
            return 0; // EOS_Success: the process is already exiting
        }
    }

    auto install_eos_guard() -> void
    {
        if (g_eos_attempted) return;
        HMODULE sdk = GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll");
        if (!sdk) return;
        g_eos_attempted = true;
        g_eos_shutdown = reinterpret_cast<EosShutdownFn>(GetProcAddress(sdk, "EOS_Shutdown"));
        if (!g_eos_shutdown)
        {
            g_api->log(g_self, FNL_LOG_WARNING, L"EOS_Shutdown exit guard: the SDK has no EOS_Shutdown");
            return;
        }
        const FnlHook hook{"EOS_Shutdown", reinterpret_cast<void**>(&g_eos_shutdown), reinterpret_cast<void*>(&eos_shutdown_detour)};
        const bool ok = g_api->hook(g_self, &hook, 1) != 0;
        g_api->log(g_self, ok ? FNL_LOG_NORMAL : FNL_LOG_ERROR, ok ? L"EOS_Shutdown exit guard installed" : L"EOS_Shutdown exit guard NOT installed");
    }
} // namespace

extern "C" FNL_MOD_EXPORT int fnl_mod_init(const FnlApi* api, FnlMod* self)
{
    if (!api || api->abi < FNL_MOD_ABI || !FNL_API_HAS(api, on)) return FNL_FAILED;
    g_api = api;
    g_self = self;
    install_eos_guard();
    if (!g_eos_attempted) api->on(self, FNL_EV_TICK, [](void*, const void*) { install_eos_guard(); }, nullptr);
    return FNL_OK;
}

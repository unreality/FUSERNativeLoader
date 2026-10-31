// The mod manager (mod_manager.hpp).
//
// mods/<folder>/mod.ini, keys (case-insensitive; ; or # starts a comment):
//   name=        shown in the log (default: the folder name)
//   dll=         the DLL in the folder, exporting fnl_mod_init (sdk/fnl_mod.h)
//   version=     shown in the log
//   stage=       early | game (default)
//   api=         the lowest FNL_MOD_ABI it needs (default 1)
//   sdk=         C++ tier only: must equal FNL_SDK_VERSION; C++ tier mods are game-stage only
//   requires=    folder names that must load first; one missing, disabled or not loaded skips this mod
//   after=       folder names to load after when they load (order only)
//   before=      folder names to load before when they load (order only; an early mod can't wait for a game-stage one)
//   supersedes=  files, relative to the game root, renamed to <file>.disabled before any pak is mounted
//   enabled=     0 leaves the mod out (so does a folder name ending in .disabled)
// Lists are comma-separated. Mods load in dependency order, then by folder name.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "loader/mod_manager.hpp"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "fnl_mod.h"
#include "fue/fue.hpp"
#include "loader/code_patch.hpp"
#include "loader/settings.hpp"

#ifndef FNL_DEBUG
#define FNL_DEBUG 0
#endif

using namespace fue;
namespace fs = std::filesystem;

struct FnlMod
{
    enum class State { Pending, Loaded, Unsupported, Failed, Skipped };

    StringType id;       // the folder name
    std::string owner;   // id in UTF-8, for code_patch's log
    fs::path dir;
    StringType dir_text; // FnlApi::mod_dir
    StringType name, version, dll;
    bool early = false;
    bool cpp = false;    // sdk= given: the C++ tier
    std::vector<StringType> needs, after, before, supersedes;
    StringType problem;  // why it won't load; empty when it will
    State state = State::Pending;
};

namespace
{
    std::vector<std::unique_ptr<FnlMod>> g_mods{}; // every folder with a mod.ini
    std::vector<FnlMod*> g_order{};                // the ones that load, in load order
    FnlGameInfo g_game{};
    FnlApi g_api{};

    auto lower(StringType s) -> StringType
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return s;
    }

    auto trim(const StringType& s) -> StringType
    {
        const size_t a = s.find_first_not_of(L" \t\r\n");
        if (a == StringType::npos) return {};
        return s.substr(a, s.find_last_not_of(L" \t\r\n") - a + 1);
    }

    auto split_list(const StringType& s) -> std::vector<StringType>
    {
        std::vector<StringType> out{};
        size_t from = 0;
        while (from <= s.size())
        {
            const size_t comma = std::min(s.find(L',', from), s.size());
            if (StringType item = trim(s.substr(from, comma - from)); !item.empty()) out.push_back(std::move(item));
            from = comma + 1;
        }
        return out;
    }

    auto find_mod(const StringType& id) -> FnlMod*
    {
        const StringType key = lower(id);
        for (auto& m : g_mods)
            if (lower(m->id) == key) return m.get();
        return nullptr;
    }

    auto stage_name(const FnlMod& m) -> const CharType* { return m.early ? STR("early") : STR("game"); }
    auto tier_name(const FnlMod& m) -> const CharType* { return m.cpp ? STR("C++") : STR("C"); }

    // ---------------------------------------------------------------- the API table -------------------------------
    auto api_log(FnlMod* self, int level, const wchar_t* text) -> void
    {
        const LogLevel l = static_cast<LogLevel>(std::clamp(level, FNL_LOG_VERBOSE, FNL_LOG_ERROR));
        Output::write(l, std::format(STR("[{}] {}"), self ? self->id : StringType(STR("?")), text ? text : STR("")));
    }

    auto api_find_pattern(FnlMod*, const char*, const char* pattern, const uint8_t* start, size_t size) -> uint8_t*
    {
        return code_find_pattern(pattern, start, size);
    }

    auto api_hook(FnlMod* self, const FnlHook* hooks, size_t count) -> int
    {
        return self && code_hook(self->owner.c_str(), hooks, count) ? 1 : 0;
    }

    auto api_patch(FnlMod* self, void* at, const void* bytes, size_t count) -> int
    {
        return self && code_patch(self->owner.c_str(), at, bytes, count) ? 1 : 0;
    }

    auto api_redirect_import(FnlMod* self, const char* dll, const char* function, void* replacement, void** original) -> int
    {
        return self && code_redirect_import(self->owner.c_str(), dll, function, replacement, original) ? 1 : 0;
    }

    auto api_hook_prepare(FnlMod* self, const FnlHook* hooks, size_t count, uint32_t flags) -> FnlHookSet*
    {
        return self ? reinterpret_cast<FnlHookSet*>(code_hook_prepare(self->owner.c_str(), hooks, count, flags)) : nullptr;
    }

    auto api_hook_enable(FnlMod* self, FnlHookSet* set, int enabled) -> int
    {
        return self && code_hook_enable(self->owner.c_str(), reinterpret_cast<CodeHookSet*>(set), enabled != 0) ? 1 : 0;
    }

    auto api_changed(FnlMod* self, const void* at, size_t size, uint32_t flags, char* owners, size_t cap) -> size_t
    {
        if (flags & ~static_cast<uint32_t>(FNL_CHANGED_BY_OTHERS))
        {
            // a flag from a newer SDK: refused, so the mod can't mistake the answer for one that honoured it
            api_log(self, FNL_LOG_ERROR, std::format(STR("changed: unknown flags 0x{:X}"), flags).c_str());
            if (owners && cap) owners[0] = '\0';
            return FNL_CHANGED_ERROR;
        }
        std::string names{};
        const char* skip = self && (flags & FNL_CHANGED_BY_OTHERS) ? self->owner.c_str() : nullptr;
        const size_t n = code_changed(skip, at, size, owners ? &names : nullptr);
        if (owners && cap)
        {
            const size_t k = std::min(names.size(), cap - 1);
            std::copy_n(names.begin(), k, owners);
            owners[k] = '\0';
        }
        return n;
    }

    auto api_patch_strict(FnlMod* self, void* at, const void* bytes, size_t count) -> int
    {
        return self && code_patch(self->owner.c_str(), at, bytes, count, true) ? 1 : 0;
    }

    auto api_on(FnlMod* self, FnlEvent event, void (*cb)(void* ctx, const void* arg), void* ctx) -> void
    {
        if (!cb)
        {
            api_log(self, FNL_LOG_ERROR, STR("on: no callback"));
            return;
        }
        switch (event)
        {
        case FNL_EV_UNREAL_INIT:
            on_unreal_init([cb, ctx] { cb(ctx, nullptr); });
            break;
        case FNL_EV_FIRST_TICK:
            Hook::RegisterEngineTickPreCallback([cb, ctx, done = false](Hook::TCallbackIterationData<void>&, UEngine*, float delta, bool) mutable {
                if (done) return;
                done = true;
                cb(ctx, &delta);
            });
            break;
        case FNL_EV_TICK:
            Hook::RegisterEngineTickPreCallback([cb, ctx](Hook::TCallbackIterationData<void>&, UEngine*, float delta, bool) { cb(ctx, &delta); });
            break;
        default:
            api_log(self, FNL_LOG_ERROR, std::format(STR("on: unknown event {}"), static_cast<int>(event)).c_str());
        }
    }

    auto api_mod_dir(FnlMod* self) -> const wchar_t* { return self ? self->dir_text.c_str() : STR(""); }

    auto api_ini_get(FnlMod*, const wchar_t* key, wchar_t* out, size_t cap) -> size_t
    {
        if (!key) return FNL_INI_ABSENT;
        const StringType prefix = StringType(key) + STR("=");
        std::wifstream in(fs::path(module_dir()) / L"FuserNative.ini");
        StringType line{};
        while (std::getline(in, line))
        {
            if (line.rfind(prefix, 0) != 0) continue;
            const StringType value = line.substr(prefix.size());
            if (out && cap)
            {
                const size_t n = std::min(value.size(), cap - 1);
                std::copy_n(value.begin(), n, out);
                out[n] = L'\0';
            }
            return value.size();
        }
        return FNL_INI_ABSENT;
    }

    auto api_add_setting(FnlMod* self, const FnlSetting* row) -> int { return self && row && settings_add(self, nullptr, *row) ? 1 : 0; }
    auto api_setting(FnlMod*, const char* key) -> int32_t { return settings_value(key); }
    auto api_set_setting(FnlMod*, const char* key, int32_t value) -> void { settings_set(key, value); }
    auto api_enable_setting(FnlMod*, const char* key, int enabled) -> void { settings_enable(key, enabled != 0); }

    // ---------------------------------------------------------------- interfaces between mods ---------------
    struct Interface
    {
        std::string name;
        uint32_t version = 0;
        const void* table = nullptr;
        FnlMod* owner = nullptr;
    };
    std::vector<Interface> g_interfaces{};

    auto api_provide_interface(FnlMod* self, const char* name, uint32_t version, const void* table) -> int
    {
        const StringType who = self ? self->id : StringType(STR("?"));
        if (!name || !*name || !table)
        {
            Output::send<LogLevel::Error>(STR("[mods] {}: provide_interface without a name or a table\n"), who);
            return 0;
        }
        for (const Interface& i : g_interfaces)
        {
            if (i.name != name) continue;
            Output::send<LogLevel::Error>(STR("[mods] {}: interface {} is provided by {} already - refused\n"), who, to_wstring(name),
                                          i.owner ? i.owner->id : StringType(STR("?")));
            return 0;
        }
        g_interfaces.push_back({name, version, table, self});
        Output::send<LogLevel::Normal>(STR("[mods] {}: provides {} v{}\n"), who, to_wstring(name), version);
        return 1;
    }

    auto api_find_interface(FnlMod* self, const char* name, uint32_t min_version) -> const void*
    {
        for (const Interface& i : g_interfaces)
        {
            if (!name || i.name != name) continue;
            if (i.version >= min_version) return i.table;
            Output::send<LogLevel::Warning>(STR("[mods] {}: interface {} is v{}, it needs v{}\n"), self ? self->id : StringType(STR("?")), to_wstring(name),
                                            i.version, min_version);
            return nullptr;
        }
        return nullptr;
    }

    auto fill_api() -> void
    {
        uint8_t* base = code_exe_base();
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        g_game.size = sizeof(FnlGameInfo);
        g_game.base = base;
        g_game.image_size = code_exe_size();
        g_game.timestamp = nt->FileHeader.TimeDateStamp;
        g_game.build = build_known() ? build_name() : nullptr;
        g_game.debug = FNL_DEBUG;

        g_api.size = sizeof(FnlApi);
        g_api.abi = FNL_MOD_ABI;
        g_api.loader_version = FNL_VERSION;
        g_api.game = &g_game;
        g_api.log = &api_log;
        g_api.find_pattern = &api_find_pattern;
        g_api.hook = &api_hook;
        g_api.patch = &api_patch;
        g_api.redirect_import = &api_redirect_import;
        g_api.on = &api_on;
        g_api.mod_dir = &api_mod_dir;
        g_api.ini_get = &api_ini_get;
        g_api.add_setting = &api_add_setting;
        g_api.setting = &api_setting;
        g_api.set_setting = &api_set_setting;
        g_api.enable_setting = &api_enable_setting;
        g_api.provide_interface = &api_provide_interface;
        g_api.find_interface = &api_find_interface;
        g_api.hook_prepare = &api_hook_prepare;
        g_api.hook_enable = &api_hook_enable;
        g_api.changed = &api_changed;
        g_api.patch_strict = &api_patch_strict;
    }

    // ---------------------------------------------------------------- manifests -----------------------------------
    auto parse_int(const StringType& s, int fallback) -> int
    {
        if (s.empty()) return fallback;
        wchar_t* end = nullptr;
        const long v = std::wcstol(s.c_str(), &end, 10);
        return end && *end == L'\0' ? static_cast<int>(v) : fallback;
    }

    // One mod.ini (UTF-8) as lower-case keys and trimmed values
    auto read_ini(const fs::path& path) -> std::map<StringType, StringType>
    {
        std::map<StringType, StringType> keys{};
        std::ifstream in(path, std::ios::binary);
        std::string raw{};
        bool first = true;
        while (std::getline(in, raw))
        {
            if (first && raw.rfind("\xEF\xBB\xBF", 0) == 0) raw.erase(0, 3);
            first = false;
            StringType line = to_wstring(raw);
            for (size_t i = 0; i < line.size(); ++i)
            {
                if ((line[i] == L';' || line[i] == L'#') && (i == 0 || line[i - 1] == L' ' || line[i - 1] == L'\t'))
                {
                    line.resize(i);
                    break;
                }
            }
            const size_t eq = line.find(L'=');
            if (eq == StringType::npos) continue;
            keys[lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
        }
        return keys;
    }

    auto read_mod(const fs::path& dir, bool fue_ok) -> std::unique_ptr<FnlMod>
    {
        auto m = std::make_unique<FnlMod>();
        m->id = dir.filename().wstring();
        m->dir = dir;
        m->dir_text = dir.wstring();
        int size = WideCharToMultiByte(CP_UTF8, 0, m->id.c_str(), -1, nullptr, 0, nullptr, nullptr);
        m->owner.assign(size > 0 ? size - 1 : 0, '\0');
        if (size > 1) WideCharToMultiByte(CP_UTF8, 0, m->id.c_str(), -1, m->owner.data(), size, nullptr, nullptr);

        auto keys = read_ini(dir / L"mod.ini");
        auto get = [&](const CharType* k) { auto it = keys.find(k); return it == keys.end() ? StringType{} : it->second; };
        m->name = get(STR("name")).empty() ? m->id : get(STR("name"));
        m->version = get(STR("version"));
        m->dll = get(STR("dll"));
        m->needs = split_list(get(STR("requires")));
        m->after = split_list(get(STR("after")));
        m->before = split_list(get(STR("before")));
        m->supersedes = split_list(get(STR("supersedes")));
        const StringType stage = lower(get(STR("stage")));
        m->early = stage == STR("early");
        const int api = parse_int(get(STR("api")), 1);
        const StringType sdk = get(STR("sdk"));
        m->cpp = !sdk.empty();

        if (get(STR("enabled")) == STR("0"))
            m->problem = STR("disabled (enabled=0)");
        else if (!stage.empty() && stage != STR("early") && stage != STR("game"))
            m->problem = std::format(STR("unknown stage '{}'"), stage);
        else if (m->dll.empty() || m->dll.find_first_of(L"\\/:") != StringType::npos)
            m->problem = STR("dll= must name a DLL in its folder");
        else if (!fs::is_regular_file(dir / m->dll))
            m->problem = std::format(STR("{} is missing"), m->dll);
        else if (api > FNL_MOD_ABI)
            m->problem = std::format(STR("needs API {} (this loader has {})"), api, FNL_MOD_ABI);
        else if (m->cpp && parse_int(sdk, -1) != FNL_SDK_VERSION)
            m->problem = std::format(STR("built for SDK {} (this loader has {})"), sdk, FNL_SDK_VERSION);
        else if (m->cpp && m->early)
            m->problem = STR("C++ tier mods (sdk=) can't be early-stage");
        else if (!m->early && !fue_ok)
            m->problem = STR("the game stage needs a game build the loader knows");
        return m;
    }

    auto discover(bool fue_ok) -> void
    {
        const fs::path root = fs::path(module_dir()) / L"mods";
        std::error_code ec;
        if (!fs::is_directory(root, ec)) return;
        std::vector<fs::path> dirs{};
        for (const auto& e : fs::directory_iterator(root, ec))
            if (e.is_directory(ec)) dirs.push_back(e.path());
        std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) { return lower(a.filename().wstring()) < lower(b.filename().wstring()); });
        for (const fs::path& dir : dirs)
        {
            const StringType id = dir.filename().wstring();
            if (lower(id).ends_with(STR(".disabled"))) continue;
            if (!fs::is_regular_file(dir / L"mod.ini", ec))
            {
                Output::send<LogLevel::Warning>(STR("[mods] {}: no mod.ini - ignored\n"), id);
                continue;
            }
            g_mods.push_back(read_mod(dir, fue_ok));
        }
    }

    // The mods without a problem, in dependency order (requires=, after= and before=, then folder name). A mod whose requires=
    // can't be met, an early mod requiring a game-stage one, or a cycle gets a problem instead.
    auto resolve_order() -> void
    {
        for (bool changed = true; changed;)
        {
            changed = false;
            for (auto& m : g_mods)
            {
                if (!m->problem.empty()) continue;
                for (const StringType& need : m->needs)
                {
                    FnlMod* dep = find_mod(need);
                    if (!dep)
                        m->problem = std::format(STR("requires {}, which isn't installed"), need);
                    else if (!dep->problem.empty())
                        m->problem = std::format(STR("requires {}, which won't load"), dep->id);
                    else if (m->early && !dep->early)
                        m->problem = std::format(STR("is early-stage but requires {}, a game-stage mod"), dep->id);
                    if (!m->problem.empty())
                    {
                        changed = true;
                        break;
                    }
                }
            }
        }

        std::map<FnlMod*, std::set<FnlMod*>> waits{}; // a mod -> the mods it loads after
        for (auto& m : g_mods)
        {
            if (!m->problem.empty()) continue;
            waits[m.get()];
            for (const StringType& id : m->needs) waits[m.get()].insert(find_mod(id));
            for (const StringType& id : m->after)
            {
                FnlMod* dep = find_mod(id);
                if (dep && dep->problem.empty() && dep != m.get() && !(m->early && !dep->early)) waits[m.get()].insert(dep); // a game-stage one loads later anyway
            }
            for (const StringType& id : m->before)
            {
                FnlMod* later = find_mod(id);
                if (!later || !later->problem.empty() || later == m.get()) continue;
                if (m->early == later->early)
                    waits[later].insert(m.get());
                else if (later->early) // an early-stage mod loads before every game-stage one
                    Output::send<LogLevel::Warning>(STR("[mods] {}: before={} ignored - {} is early-stage, this mod game-stage\n"), m->id, id, later->id);
            }
        }
        while (!waits.empty())
        {
            FnlMod* next = nullptr;
            for (auto& [m, deps] : waits)
                if (deps.empty() && (!next || lower(m->id) < lower(next->id))) next = m;
            if (!next) break;
            g_order.push_back(next);
            waits.erase(next);
            for (auto& [m, deps] : waits) deps.erase(next);
        }
        for (auto& [m, deps] : waits) m->problem = STR("its requires= / after= / before= form a cycle");
        // early mods first: they load inside DllMain, the rest at the first engine callback
        std::stable_partition(g_order.begin(), g_order.end(), [](FnlMod* m) { return m->early; });
    }

    // supersedes=: paths relative to the game root (the folder holding Fuser/), never outside it
    auto apply_supersedes(FnlMod& m) -> void
    {
        std::error_code ec;
        const fs::path root = fs::weakly_canonical(fs::path(module_dir()) / L".." / L".." / L"..", ec);
        for (const StringType& item : m.supersedes)
        {
            const fs::path rel = fs::path(item).lexically_normal();
            const bool inside = !rel.empty() && !rel.has_root_path() && *rel.begin() != L"..";
            if (!inside)
            {
                Output::send<LogLevel::Warning>(STR("[mods] {}: supersedes={} is not a path inside the game folder - ignored\n"), m.id, item);
                continue;
            }
            const fs::path from = root / rel;
            if (!fs::is_regular_file(from, ec)) continue;
            fs::path to = from;
            to += L".disabled";
            fs::rename(from, to, ec); // replaces an older .disabled copy
            Output::send<LogLevel::Normal>(STR("[mods] {}: superseded {} -> {}.disabled ({})\n"), m.id, rel.wstring(), rel.filename().wstring(),
                                           ec ? to_wstring(ec.message()) : StringType(STR("ok")));
        }
    }

    auto load(FnlMod& m) -> void
    {
        for (const StringType& need : m.needs)
        {
            FnlMod* dep = find_mod(need);
            if (dep && dep->state == FnlMod::State::Loaded) continue;
            m.state = FnlMod::State::Skipped;
            Output::send<LogLevel::Warning>(STR("[mods] {}: not loaded - it requires {}, which didn't load\n"), m.id, need);
            return;
        }
        Output::send<LogLevel::Normal>(STR("[mods] {}: init ({}{}{}, {} stage, {} tier)\n"), m.id, m.name, m.version.empty() ? STR("") : STR(" "), m.version,
                                       stage_name(m), tier_name(m));
        const fs::path path = m.dir / m.dll;
        HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll)
        {
            const DWORD err = GetLastError();
            m.state = FnlMod::State::Failed;
            // 126: a DLL it imports is missing; 127: an import is missing (a C++ tier mod built for another loader)
            Output::send<LogLevel::Error>(STR("[mods] {}: {} didn't load (Win32 {})\n"), m.id, m.dll, err);
            return;
        }
        auto init = reinterpret_cast<FnlModInitFn>(GetProcAddress(dll, "fnl_mod_init"));
        if (!init)
        {
            m.state = FnlMod::State::Failed;
            Output::send<LogLevel::Error>(STR("[mods] {}: {} has no fnl_mod_init export\n"), m.id, m.dll);
            return;
        }
        const int result = init(&g_api, &m);
        if (result == FNL_OK)
        {
            m.state = FnlMod::State::Loaded;
            Output::send<LogLevel::Normal>(STR("[mods] {}: loaded\n"), m.id);
        }
        else if (result == FNL_UNSUPPORTED)
        {
            m.state = FnlMod::State::Unsupported;
            Output::send<LogLevel::Normal>(STR("[mods] {}: nothing to do on this game build\n"), m.id);
        }
        else
        {
            m.state = FnlMod::State::Failed;
            Output::send<LogLevel::Error>(STR("[mods] {}: init failed ({})\n"), m.id, result);
        }
    }
} // namespace

auto fnl_mods_early(bool fue_ok) -> void
{
    static bool done = false;
    if (done) return;
    done = true;
    fill_api();
    try
    {
        discover(fue_ok);
    }
    catch (const std::exception& e)
    {
        Output::send<LogLevel::Error>(STR("[mods] reading mods/ failed: {}\n"), to_wstring(e.what()));
    }
    resolve_order();
    Output::send<LogLevel::Normal>(STR("[mods] {} found, {} to load (loader API {}, SDK {})\n"), g_mods.size(), g_order.size(), FNL_MOD_ABI, FNL_SDK_VERSION);
    for (auto& m : g_mods)
        if (!m->problem.empty()) Output::send<LogLevel::Warning>(STR("[mods] {}: not loaded - {}\n"), m->id, m->problem);
    for (FnlMod* m : g_order)
    {
        try
        {
            apply_supersedes(*m);
        }
        catch (const std::exception& e)
        {
            Output::send<LogLevel::Error>(STR("[mods] {}: supersedes= failed: {}\n"), m->id, to_wstring(e.what()));
        }
    }
    for (FnlMod* m : g_order)
        if (m->early) load(*m);
}

auto fnl_mods_game_stage(bool fue_ready) -> void
{
    static bool done = false;
    if (done) return;
    done = true;
    for (FnlMod* m : g_order)
    {
        if (m->early || m->state != FnlMod::State::Pending) continue;
        if (m->cpp && !fue_ready)
        {
            m->state = FnlMod::State::Skipped;
            Output::send<LogLevel::Warning>(STR("[mods] {}: not loaded - fue's self-check failed, and it is a C++ tier mod\n"), m->id);
            continue;
        }
        load(*m);
    }
}

auto fnl_mods_loaded() -> std::vector<FnlMod*>
{
    std::vector<FnlMod*> out{};
    for (FnlMod* m : g_order)
        if (m->state == FnlMod::State::Loaded) out.push_back(m);
    return out;
}

auto fnl_mod_heading(const FnlMod* mod) -> std::wstring
{
    if (!mod) return {};
    return mod->version.empty() ? mod->name : mod->name + STR(" ") + mod->version;
}

auto fnl_mods_about() -> std::vector<std::wstring>
{
    auto line = [](const FnlMod& m) -> std::wstring {
        const std::wstring heading = fnl_mod_heading(&m);
        switch (m.state)
        {
        case FnlMod::State::Loaded: return heading;
        case FnlMod::State::Unsupported: return heading + STR(" - not for this game build");
        case FnlMod::State::Failed: return heading + STR(" - failed to start");
        default: return heading + (m.problem.starts_with(STR("disabled")) ? STR(" - disabled") : STR(" - not loaded"));
        }
    };
    std::vector<std::wstring> out{};
    for (FnlMod* m : g_order) out.push_back(line(*m));
    for (auto& m : g_mods)
        if (std::find(g_order.begin(), g_order.end(), m.get()) == g_order.end()) out.push_back(line(*m));
    return out;
}

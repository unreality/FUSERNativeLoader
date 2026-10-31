// Server content: the items fuserd offers (api/content/list/), kept in Fuser/Content/ServerContent/ and registered
// locked (custom_content.hpp) until the game unlocks them: a Diamond Store purchase, a claimed event reward, or
// fuserd's claimed assets at login.
//
// Storage. Fuser/Content/ServerContent/ is outside Content/Paks, so the engine never mounts it; the loader mounts what
// it registers. index.json records every downloaded, checked pak: {"items": {"<id>": {version, sha256, size,
// sig_sha256, next?}}}. A pak is never deleted, whatever the list says: an item fuserd deleted stays on this machine.
//   - startup (the provider, at unreal-init): leftover *.part files are removed, a staged update (<id>_P.pak.next +
//     .sig.next) replaces its pak, and every indexed pak whose id custom_content/ doesn't override is mounted and
//     registered with the rest of the content;
//   - login (a new fuserd session token): the list is fetched and only new or changed items (by sha256) download,
//     on a worker thread, to .part files that are checked (sha256, the .sig's chunk table, the pak's item layout)
//     before they are renamed. A new item is mounted and registered at once (custom_content's register_live); a
//     changed item already mounted this session is staged as .next for the next launch.
//   - entering multiplayer, and a remote player's stage kit naming an unknown item: the list is checked again.
//
// The title screen's hold. The title screen moves to the main menu through UHypeTitleScreen's state setter
// (title_set_state, EGS v1.4.2 FUN_14078d190; state at +0x3f0). While the sync runs, its move from
// WaitingForClaimedAssets (7) to WaitingToGoToMainMenu (8) is held, at most ServerContentWaitSeconds (FuserNative.ini,
// default 30; 0 = never hold). The game's DLC scan popup (WBP_HypePopup_DlcScanning) stays open through state 7 and
// shows the progress; the loader opens one itself when the game didn't (the scan was already finished). Downloads
// that miss the limit go on in the background and register when they finish.
//
// Unlocks. The game's own claimed-assets pass (FUN_1407901c0) skips an asset without an unlock row, so the claimed
// assets are kept and SetUnlockState(Unlocked) is called for each once its item is registered.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "custom_content.hpp"
#include "data_table.hpp"
#include "json_lite.hpp"
#include "multiplayer/mp_shared.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

using namespace fue;

namespace mp
{
    TitleSetStateFn g_title_set_state_original = nullptr;

    namespace
    {
        constexpr size_t kTitleState = 0x3f0; // UHypeTitleScreen: its ETitleScreenState
        constexpr uint8 kWaitingForClaimedAssets = 7, kWaitingToGoToMainMenu = 8;
        constexpr uint8 kUnlockSong = 1, kUnlockStateUnlocked = 3;
        constexpr size_t kUnlockNameMap = 0x128;
        constexpr size_t kUnlockNameMapStride = data_table::fname_map_stride(sizeof(void*));

        auto lower(std::string s) -> std::string
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }
        auto widen(const std::string& s) -> StringType { return StringType(s.begin(), s.end()); } // ids, urls, hashes: ASCII
        auto narrow(const StringType& s) -> std::string
        {
            std::string out{};
            for (wchar_t c : s) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
            return out;
        }

        auto store_dir() -> std::filesystem::path
        {
            return std::filesystem::path(module_dir()).parent_path().parent_path() / L"Content" / L"ServerContent";
        }
        auto pak_path(const std::string& id) -> std::filesystem::path { return store_dir() / (widen(id) + L"_P.pak"); }
        auto sig_path(const std::string& id) -> std::filesystem::path { return store_dir() / (widen(id) + L"_P.sig"); }
        auto with_suffix(std::filesystem::path p, const wchar_t* suffix) -> std::filesystem::path
        {
            p += suffix;
            return p;
        }

        // ------------------------------------------------------------------ index.json -------------------
        struct Version
        {
            int64 version = 0, size = 0;
            std::string sha256, sig_sha256;
        };
        struct IndexEntry
        {
            std::string id; // canonical spelling
            Version cur;
            bool has_next = false;
            Version next;
        };
        std::map<std::string, IndexEntry> g_index{}; // by lower-case id
        std::mutex g_index_mu;

        auto read_version(const jsonl::Value& v) -> Version
        {
            return {v["version"].i64(), v["size"].i64(), v["sha256"].str(), v["sig_sha256"].str()};
        }
        auto json_string(const std::string& s) -> std::string
        {
            std::string out = "\"";
            for (char c : s)
            {
                if (c == '"' || c == '\\') out += '\\';
                if (static_cast<unsigned char>(c) >= 0x20) out += c;
            }
            return out + "\"";
        }
        auto version_json(const Version& v) -> std::string
        {
            return std::format("{{\"version\": {}, \"size\": {}, \"sha256\": {}, \"sig_sha256\": {}}}", v.version, v.size, json_string(v.sha256),
                               json_string(v.sig_sha256));
        }

        auto load_index() -> void
        {
            std::ifstream f(store_dir() / L"index.json", std::ios::binary);
            if (!f) return;
            std::stringstream ss;
            ss << f.rdbuf();
            jsonl::Value root{};
            if (!jsonl::parse(ss.str(), &root))
            {
                Output::send<LogLevel::Error>(STR("[server-content] index.json is not JSON - starting a new one (the paks are kept)\n"));
                return;
            }
            std::lock_guard lock(g_index_mu);
            for (const auto& [id, v] : root["items"].o)
            {
                IndexEntry e{id, read_version(v)};
                if (!v["next"].is_null()) e.has_next = true, e.next = read_version(v["next"]);
                g_index[lower(id)] = e;
            }
        }

        auto save_index() -> void // g_index_mu held
        {
            std::string out = "{\n  \"items\": {";
            bool first = true;
            for (const auto& [key, e] : g_index)
            {
                out += std::format("{}\n    {}: {}", first ? "" : ",", json_string(e.id), version_json(e.cur));
                if (e.has_next) out.insert(out.size() - 1, ", \"next\": " + version_json(e.next));
                first = false;
            }
            out += "\n  }\n}\n";
            const auto path = store_dir() / L"index.json", tmp = store_dir() / L"index.json.tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                f << out;
                if (!f) return;
            }
            MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        // ------------------------------------------------------------------ state ------------------------
        std::set<std::string> g_mounted{}; // lower-case ids mounted this session (game thread at startup, then sync)
        std::mutex g_mounted_mu;
        auto mounted(const std::string& key) -> bool
        {
            std::lock_guard lock(g_mounted_mu);
            return g_mounted.contains(key);
        }

        struct Job
        {
            std::string id, pak_url, sig_url;
            Version v;
            enum class State { Queued, Downloading, Ready, Staged, Registered, Failed } state = State::Queued;
            std::string error;
        };
        struct Sync
        {
            std::atomic<bool> running{false}; // a worker is listing or downloading
            std::mutex mu;
            std::vector<Job> jobs; // the last sync's items to fetch (mu)
            bool listing = false;  // the list is being fetched (mu)
            std::string error;     // why the last sync stopped (mu)
        };
        Sync g_sync{};

        struct Claimed
        {
            FName name;
            uint8 type;
        };
        std::vector<Claimed> g_claimed{}; // game thread

        struct Hold
        {
            FWeakObjectPtr title{};
            bool active = false;
            uint64 since = 0, limit_ms = 0;
            bool summoned = false; // the loader opened the popup itself (it opens asynchronously, through the queue)
        };
        Hold g_hold{};

        std::vector<FWeakObjectPtr> g_rebroadcast{}; // player states whose stage kit named an unknown item (game thread)

        auto wait_seconds() -> int
        {
            const StringType v = ini(STR("ServerContentWaitSeconds"), STR("30"));
            try
            {
                return std::clamp(std::stoi(v), 0, 600);
            }
            catch (...)
            {
                return 30;
            }
        }

        // ------------------------------------------------------------------ the worker -------------------
        auto fail(Job& j, std::string why) -> void
        {
            std::lock_guard lock(g_sync.mu);
            j.state = Job::State::Failed;
            j.error = std::move(why);
        }

        // Downloads one item to .part files, checks them, and places them: as the pak (a new item) or as .next (an
        // item mounted this session). Worker thread.
        auto fetch(Job& j) -> void
        {
            {
                std::lock_guard lock(g_sync.mu);
                j.state = Job::State::Downloading;
            }
            const std::string key = lower(j.id);
            const auto pak = pak_path(j.id), sig = sig_path(j.id);
            const auto pak_part = with_suffix(pak, std::format(L".{}.part", GetCurrentProcessId()).c_str());
            const auto sig_part = with_suffix(sig, std::format(L".{}.part", GetCurrentProcessId()).c_str());
            std::error_code ec{};
            for (auto [url, part, sha] : {std::tuple{j.pak_url, pak_part, j.v.sha256}, std::tuple{j.sig_url, sig_part, j.v.sig_sha256}})
            {
                const Http r = http(L"GET", widen(url), {}, {}, &part);
                if (!r.error.empty() || r.status != 200)
                {
                    std::filesystem::remove(part, ec);
                    return fail(j, std::format("download of {} failed: HTTP {} {}", url, r.status, r.error));
                }
                if (r.sha256 != sha)
                {
                    std::filesystem::remove(part, ec);
                    return fail(j, std::format("{} has the wrong sha256 ({}, list {})", url, r.sha256, sha));
                }
            }
            auto cleanup = [&] {
                std::filesystem::remove(pak_part, ec);
                std::filesystem::remove(sig_part, ec);
            };
            std::string note{};
            if (std::string bad = check_sig(pak_part, sig_part, &note); !bad.empty())
            {
                cleanup();
                return fail(j, "its .sig would make FUSER quit at mount: " + bad);
            }
            if (!note.empty()) Output::send<LogLevel::Warning>(STR("[server-content] {}: {}\n"), widen(j.id), widen(note));
            StringType why{};
            const auto items = content::read_items(pak_part, &why);
            const bool one = items.size() == 1 && _wcsicmp(items[0].id.c_str(), widen(j.id).c_str()) == 0;
            if (!one)
            {
                cleanup();
                return fail(j, items.empty() ? "the pak " + narrow(why) : "the pak doesn't hold exactly the item " + j.id);
            }
            const bool update = mounted(key);
            const auto dest_pak = update ? with_suffix(pak, L".next") : pak, dest_sig = update ? with_suffix(sig, L".next") : sig;
            if (!MoveFileExW(pak_part.c_str(), dest_pak.c_str(), MOVEFILE_REPLACE_EXISTING) ||
                !MoveFileExW(sig_part.c_str(), dest_sig.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                const DWORD err = GetLastError();
                cleanup();
                return fail(j, std::format("cannot place {} (error {}): is it in use?", narrow(dest_pak.wstring()), err));
            }
            {
                std::lock_guard lock(g_index_mu);
                IndexEntry& e = g_index[key];
                e.id = j.id;
                if (update)
                    e.has_next = true, e.next = j.v;
                else
                    e.cur = j.v, e.has_next = false;
                save_index();
            }
            std::lock_guard lock(g_sync.mu);
            j.state = update ? Job::State::Staged : Job::State::Ready;
        }

        auto worker(StringType base, StringType token, StringType why) -> void
        {
            const Http list = http(L"GET", base + L"content/list/", token.empty() ? StringType{} : L"Authorization: JWT " + token + L"\r\n", {}, nullptr);
            jsonl::Value root{};
            const bool ok = list.error.empty() && list.status == 200 && jsonl::parse(list.body, &root);
            std::vector<Job> jobs{};
            if (ok)
            {
                const auto custom = content::custom_ids();
                for (const auto& it : root["results"]["items"].a)
                {
                    Job j{it["id"].str(), it["pak_url"].str(), it["sig_url"].str(),
                          Version{it["version"].i64(), it["pak_size"].i64(), it["pak_sha256"].str(), it["sig_sha256"].str()}};
                    const std::string key = lower(j.id);
                    const bool valid_id = !j.id.empty() && j.id.size() <= 64 &&
                                          std::all_of(j.id.begin(), j.id.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
                    if (!valid_id || j.v.sha256.size() != 64 || j.v.sig_sha256.size() != 64 || j.pak_url.empty() || j.sig_url.empty()) continue;
                    if (std::find(custom.begin(), custom.end(), widen(key)) != custom.end()) continue; // custom_content/ wins
                    {
                        std::lock_guard lock(g_index_mu);
                        const auto e = g_index.find(key);
                        if (e != g_index.end() && (e->second.cur.sha256 == j.v.sha256 || (e->second.has_next && e->second.next.sha256 == j.v.sha256))) continue;
                    }
                    jobs.push_back(std::move(j));
                }
            }
            const std::string error = ok ? std::string{}
                                         : (list.status == 404 ? "this fuserd has no server content" : std::format("the list failed: HTTP {} {}", list.status, list.error));
            {
                std::lock_guard lock(g_sync.mu);
                g_sync.listing = false;
                g_sync.jobs = jobs;
                g_sync.error = error;
            }
            if (!ok)
                Output::send<LogLevel::Warning>(STR("[server-content] {}: {}\n"), why, widen(error));
            else
                Output::send<LogLevel::Normal>(STR("[server-content] {}: {} item(s) listed, {} to download\n"), why, root["results"]["items"].size(), jobs.size());
            for (size_t i = 0; i < jobs.size(); ++i)
            {
                Job* j = nullptr;
                {
                    std::lock_guard lock(g_sync.mu);
                    j = &g_sync.jobs[i];
                }
                fetch(*j);
            }
            g_sync.running = false;
        }

        auto start_sync(const CharType* why) -> bool
        {
            if (!supported()) return false;
            const StringType base = api_base();
            if (base.empty() || g_sync.running.exchange(true)) return false;
            {
                std::lock_guard lock(g_sync.mu);
                g_sync.listing = true;
                g_sync.jobs.clear();
                g_sync.error.clear();
            }
            std::thread(worker, base, session_token(), StringType(why)).detach();
            return true;
        }

        // The sync after a login: once per fuserd session token
        auto ensure_login_sync() -> void
        {
            static StringType synced_token{};
            if (const StringType token = session_token(); !token.empty() && token != synced_token && start_sync(STR("login"))) synced_token = token;
        }

        // ------------------------------------------------------------------ game thread ------------------
        auto set_unlocked(const FName& name, uint8 type) -> bool
        {
            UObject* mgr = unlock_mgr();
            auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Hype.UnlockMgr:SetUnlockState"));
            if (!mgr || !fn) return false;
            data_table::Call c(fn);
            *reinterpret_cast<FName*>(c.addr(STR("unlockName"))) = name;
            *c.addr(STR("unlockType")) = type;
            *c.addr(STR("unlockState")) = kUnlockStateUnlocked;
            mgr->ProcessEvent(fn, c.buf.data());
            return true;
        }

        // The claimed assets of a server item that has just been registered
        auto apply_claimed(const StringType& id) -> void
        {
            const FName name(id.c_str());
            for (const auto& c : g_claimed)
                if (c.name == name && set_unlocked(c.name, c.type))
                    Output::send<LogLevel::Normal>(STR("[server-content] {}: owned (claimed assets) - unlocked\n"), id);
        }

        auto rebroadcast() -> void
        {
            auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Hype.DJPlayerState:OnRep_VenueConfig"));
            for (const auto& w : std::exchange(g_rebroadcast, {}))
                if (UObject* ps = w.Get(); ps && fn) ps->ProcessEvent(fn, nullptr);
        }

        // Ready items: mounted and registered now
        auto register_ready() -> void
        {
            std::vector<std::string> ready{};
            {
                std::lock_guard lock(g_sync.mu);
                for (auto& j : g_sync.jobs)
                    if (j.state == Job::State::Ready) ready.push_back(j.id), j.state = Job::State::Registered;
            }
            bool any = false;
            for (const auto& id : ready)
            {
                bool ok_mount = false;
                if (!mount_pak(pak_path(id).c_str(), &ok_mount) || !ok_mount)
                {
                    Output::send<LogLevel::Error>(STR("[server-content] {}: the pak did not mount - it registers at the next launch\n"), widen(id));
                    continue;
                }
                {
                    std::lock_guard lock(g_mounted_mu);
                    g_mounted.insert(lower(id));
                }
                for (const auto& r : content::register_live(pak_path(id)))
                {
                    if (!r.ok)
                    {
                        Output::send<LogLevel::Warning>(STR("[server-content] {}: {}\n"), r.id, r.note);
                        continue;
                    }
                    Output::send<LogLevel::Normal>(STR("[server-content] {}: {}\n"), r.id, r.note);
                    apply_claimed(r.id);
                    any = true;
                }
            }
            if (any) rebroadcast();
        }

        auto sync_busy() -> bool
        {
            if (g_sync.running) return true;
            std::lock_guard lock(g_sync.mu);
            return std::any_of(g_sync.jobs.begin(), g_sync.jobs.end(), [](const Job& j) { return j.state == Job::State::Ready; });
        }

        auto progress(int* done, int* total) -> bool // false while listing
        {
            std::lock_guard lock(g_sync.mu);
            *total = static_cast<int>(g_sync.jobs.size());
            *done = static_cast<int>(std::count_if(g_sync.jobs.begin(), g_sync.jobs.end(), [](const Job& j) {
                return j.state != Job::State::Queued && j.state != Job::State::Downloading;
            }));
            return !g_sync.listing;
        }

        auto status_text() -> StringType
        {
            int done = 0, total = 0;
            if (!progress(&done, &total)) return STR("Checking server content...");
            return std::format(STR("Downloading server content {} / {}"), std::min(done + 1, total), total);
        }

        auto popup_widget() -> UObject*
        {
            std::vector<UObject*> found{};
            UObjectGlobals::FindAllOf(STR("WBP_HypePopup_DlcScanning_C"), found);
            for (UObject* o : found)
                if (!o->GetName().starts_with(STR("Default__"))) return o;
            return nullptr;
        }

        auto call_on(UObject* target, const CharType* fn_name) -> void
        {
            if (!target) return;
            if (UFunction* fn = target->GetFunctionByNameInChain(fn_name))
            {
                data_table::Call c(fn);
                target->ProcessEvent(fn, c.buf.data());
            }
        }

        // The game's DLC scan popup, opened when the title screen didn't open one (BPFL_HypePopup.SummonPopupDlcScanning)
        auto summon_popup(UObject* title) -> void
        {
            UObject* lib = UObjectGlobals::StaticFindObject(nullptr, nullptr, STR("/Game/UI/Shared/Popup/BPFL_HypePopup.Default__BPFL_HypePopup_C"));
            UFunction* fn = lib ? lib->GetFunctionByNameInChain(STR("SummonPopupDlcScanning")) : nullptr;
            if (!fn) return;
            data_table::Call c(fn);
            if (FProperty* ctx = c.prop(STR("__WorldContext"))) *ctx->ContainerPtrToValuePtr<UObject*>(c.buf.data()) = title;
            lib->ProcessEvent(fn, c.buf.data());
        }

        // The popup's text and progress bar, after its own RefreshStatus set the scan's
        auto on_popup_refresh(UObject* popup) -> void
        {
            if (!g_hold.active && !g_sync.running) return;
            if (UFunction* set = popup->GetFunctionByNameInChain(STR("SetStatusText")))
            {
                data_table::Call c(set);
                const FText text(status_text());
                for (FProperty* p : TFieldRange<FProperty>(set, EFieldIterationFlags::IncludeDeprecated))
                {
                    if (!p->HasAnyPropertyFlags(CPF_Parm)) break;
                    p->CopyCompleteValue(p->ContainerPtrToValuePtr<void>(c.buf.data()), &text);
                    break;
                }
                popup->ProcessEvent(set, c.buf.data());
            }
            int done = 0, total = 0;
            const bool listed = progress(&done, &total);
            auto** bar_slot = popup->GetValuePtrByPropertyNameInChain<UObject*>(STR("ProgressBar"));
            UObject* bar = bar_slot ? *bar_slot : nullptr;
            if (!bar) return;
            if (UFunction* vis = bar->GetFunctionByNameInChain(STR("SetVisibility")))
            {
                data_table::Call c(vis);
                *c.addr(STR("InVisibility")) = 4; // SelfHitTestInvisible: shown
                bar->ProcessEvent(vis, c.buf.data());
            }
            if (UFunction* pct = bar->GetFunctionByNameInChain(STR("SetPercent")))
            {
                data_table::Call c(pct);
                *reinterpret_cast<float*>(c.addr(STR("InPercent"))) = listed && total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
                bar->ProcessEvent(pct, c.buf.data());
            }
        }

        auto release_hold(const CharType* why) -> void
        {
            UObject* title = g_hold.title.Get();
            g_hold.active = false;
            if (g_hold.summoned) call_on(popup_widget(), STR("ClosePopup")); // the title screen closes only the one it opened
            g_hold.summoned = false;
            if (!title || *reinterpret_cast<uint8*>(reinterpret_cast<uint8*>(title) + kTitleState) != kWaitingForClaimedAssets)
            {
                Output::send<LogLevel::Normal>(STR("[server-content] hold ended ({}); the title screen has moved on\n"), why);
                return;
            }
            Output::send<LogLevel::Normal>(STR("[server-content] hold released after {} ms ({}) - on to the main menu\n"), GetTickCount64() - g_hold.since, why);
            g_title_set_state_original(title, kWaitingToGoToMainMenu);
        }

        auto drive_hold() -> void
        {
            if (!g_hold.active) return;
            register_ready(); // items that finished register at once, before the main menu
            if (!sync_busy())
                release_hold(STR("the sync is done"));
            else if (GetTickCount64() - g_hold.since >= g_hold.limit_ms)
            {
                Output::send<LogLevel::Warning>(STR("[server-content] the sync is still running after {} s - it goes on in the background\n"),
                                                g_hold.limit_ms / 1000);
                release_hold(STR("time limit"));
            }
            else if (!g_hold.summoned && !popup_widget())
            {
                summon_popup(g_hold.title.Get());
                g_hold.summoned = true;
            }
        }

        // A remote player's stage kit (DJPlayerState._venueConfig) naming an item this machine doesn't have
        auto on_venue_config(UObject* ps) -> void
        {
            if (!ps) return;
            uint8* config = ps->GetValuePtrByPropertyNameInChain<uint8>(STR("_venueConfig"));
            FProperty* prop = ps->GetPropertyByNameInChain(STR("_venueConfig"));
            auto* sp = CastField<FStructProperty>(prop);
            UObject* mgr = unlock_mgr();
            if (!config || !sp || !mgr) return;
            void* names = reinterpret_cast<uint8*>(mgr) + kUnlockNameMap;
            bool unknown = false;
            auto check = [&](const FName& n) {
                if (!n.IsNone() && !data_table::fname_map_find(names, kUnlockNameMapStride, n)) unknown = true;
            };
            // FVenueConfigKeys' venue items (each a DT_UnlocksVenueItems row); its config name and sky package aren't
            static const CharType* const kItemFields[] = {STR("VfxCan"), STR("VfxBackdrop"), STR("VfxAmbient"), STR("ScreenItems"),
                                                          STR("OverrideScreenItem"), STR("CrowdParticles"), STR("LightShow")};
            UStruct* keys = sp->GetStruct().Get();
            for (const CharType* field : kItemFields)
            {
                FProperty* p = keys->FindProperty(FName(field));
                if (!p) continue;
                uint8* v = p->ContainerPtrToValuePtr<uint8>(config);
                if (CastField<FArrayProperty>(p))
                {
                    auto* a = reinterpret_cast<TArray<FName>*>(v);
                    for (int32 i = 0; i < a->Num(); ++i) check((*a)[i]);
                }
                else
                    check(*reinterpret_cast<FName*>(v));
            }
            if (!unknown) return;
            const bool queued = std::any_of(g_rebroadcast.begin(), g_rebroadcast.end(), [&](const FWeakObjectPtr& w) { return w.Get() == ps; });
            if (!queued) g_rebroadcast.push_back(FWeakObjectPtr(ps));
            static uint64 last = 0;
            if (const uint64 now = GetTickCount64(); now - last > 30000 && start_sync(STR("a player's stage kit names an item this machine lacks")))
                last = now;
        }

        auto install_hooks() -> void
        {
            static bool installed = false;
            if (installed) return;
            auto* rep = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Hype.DJPlayerState:OnRep_VenueConfig"));
            if (!rep) return;
            installed = true;
            UObjectGlobals::RegisterHook(rep, {}, [](UnrealScriptFunctionCallableContext& ctx, void*) {
                if (on_game_thread()) on_venue_config(ctx.Context);
            }, nullptr);
            static const FName refresh(STR("RefreshStatus"), FNAME_Add);
            Hook::RegisterProcessEventPostCallback([](Hook::TCallbackIterationData<void>&, UObject* self, UFunction* fn, void*) {
                if (!fn || !self || fn->GetFName() != refresh || !on_game_thread()) return;
                const UClass* cls = self->GetClassPrivate();
                if (cls && cls->GetName() == STR("WBP_HypePopup_DlcScanning_C")) on_popup_refresh(self);
            });
        }
    } // namespace

    // ---------------------------------------------------------------------- the interface -------------------
    auto server_content_start() -> void
    {
        content::set_server_paks([](const std::vector<StringType>& custom) {
            std::vector<std::filesystem::path> out{};
            std::error_code ec{};
            std::filesystem::create_directories(store_dir(), ec);
            for (const auto& entry : std::filesystem::directory_iterator(store_dir(), ec))
            {
                const std::wstring name = entry.path().filename().wstring();
                if (name.ends_with(L".part") || name.ends_with(L".tmp")) std::filesystem::remove(entry.path(), ec);
            }
            load_index();
            if (!supported())
            {
                Output::send<LogLevel::Warning>(STR("[server-content] this exe isn't a verified build - server content is not mounted\n"));
                return out;
            }
            std::lock_guard lock(g_index_mu);
            for (auto& [key, e] : g_index)
            {
                if (e.has_next)
                {
                    const auto next_pak = with_suffix(pak_path(e.id), L".next"), next_sig = with_suffix(sig_path(e.id), L".next");
                    if (std::filesystem::exists(next_pak, ec) && std::filesystem::exists(next_sig, ec) &&
                        MoveFileExW(next_pak.c_str(), pak_path(e.id).c_str(), MOVEFILE_REPLACE_EXISTING) &&
                        MoveFileExW(next_sig.c_str(), sig_path(e.id).c_str(), MOVEFILE_REPLACE_EXISTING))
                    {
                        e.cur = e.next;
                        Output::send<LogLevel::Normal>(STR("[server-content] {}: updated to version {}\n"), widen(e.id), e.cur.version);
                    }
                    e.has_next = false;
                }
                const auto pak = pak_path(e.id);
                if (std::find(custom.begin(), custom.end(), widen(key)) != custom.end())
                {
                    Output::send<LogLevel::Normal>(STR("[server-content] {}: custom_content overrides the server item - not mounted\n"), widen(e.id));
                    continue;
                }
                if (!std::filesystem::exists(sig_path(e.id), ec) || static_cast<int64>(std::filesystem::file_size(pak, ec)) != e.cur.size || ec)
                {
                    Output::send<LogLevel::Warning>(STR("[server-content] {}: its files are missing or changed - it downloads again at login\n"), widen(e.id));
                    e.cur.sha256.clear();
                    continue;
                }
                bool ok = false;
                if (!mount_pak(pak.c_str(), &ok) || !ok)
                {
                    Output::send<LogLevel::Error>(STR("[server-content] {}: the pak did not mount\n"), widen(e.id));
                    continue;
                }
                {
                    std::lock_guard mlock(g_mounted_mu);
                    g_mounted.insert(key);
                }
                out.push_back(pak);
            }
            save_index();
            return out;
        });
    }

    auto drive_server_content() -> void
    {
        install_hooks();
        static uint64 next_check = 0;
        if (const uint64 now = GetTickCount64(); now >= next_check)
        {
            next_check = now + 1000;
            ensure_login_sync();
        }
        if (g_hold.active)
            drive_hold();
        else
            register_ready(); // the background: after the hold, or a re-check
    }

    auto server_content_claimed(const FName& name, uint8 unlock_type) -> void
    {
        if (unlock_type == kUnlockSong) return;
        g_claimed.push_back({name, unlock_type});
        // an item not here yet: the save records it now, and the row reads it when the item registers
        if (!content::registered(name.ToString())) set_unlocked(name, unlock_type);
    }

    auto server_content_recheck(const CharType* why) -> void
    {
        static uint64 last = 0;
        if (const uint64 now = GetTickCount64(); now - last > 60000 && start_sync(why)) last = now;
    }

    auto server_content_offer_ok(const FName& name, uint8 unlock_type) -> bool
    {
        if (unlock_type == kUnlockSong) return true; // songs install when bought (host_hook.cpp's store section)
        UObject* mgr = unlock_mgr();
        if (!mgr) return true;
        return data_table::fname_map_find(reinterpret_cast<uint8*>(mgr) + kUnlockNameMap, kUnlockNameMapStride, name) != nullptr;
    }

    auto title_set_state_detour(void* title, uint8 state) -> void
    {
        if (title && state == kWaitingToGoToMainMenu && on_game_thread()) ensure_login_sync(); // the token may be newer than the tick's last check
        if (title && state == kWaitingToGoToMainMenu && !g_hold.active && on_game_thread() &&
            *reinterpret_cast<uint8*>(static_cast<uint8*>(title) + kTitleState) == kWaitingForClaimedAssets && sync_busy())
        {
            const int wait = wait_seconds();
            if (wait > 0)
            {
                g_hold = Hold{.title = FWeakObjectPtr(static_cast<UObject*>(title)), .active = true, .since = GetTickCount64(),
                              .limit_ms = static_cast<uint64>(wait) * 1000};
                Output::send<LogLevel::Normal>(STR("[server-content] holding the main menu until the sync is done (at most {} s)\n"), wait);
                return;
            }
        }
        g_title_set_state_original(title, state);
    }
} // namespace mp

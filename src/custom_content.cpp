// Custom and server content (custom_content.hpp): the pak reader, the item registry, the startup registration and
// the live one. The content types themselves are in content/.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "custom_content.hpp"
#include "content/content_types.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>

using namespace fue;
using namespace data_table;

namespace content
{
    namespace
    {
        constexpr const char* kContentMount = "../../../Fuser/Content/"; // the only mount point content paks use
        constexpr const char* kItemRoot = "CustomContent/";

        // ------------------------------------------------------------------ pak index ---------------
        // An unencrypted v8 pak's index: mount point and entries (name -> data range). Paks built by the content pak
        // tool and the engine's own unencrypted paks; anything else is refused.
        struct PakEntry
        {
            std::string name;
            uint64 data_offset = 0; // first byte of the file's data
            uint64 size = 0;
            bool plain = false; // uncompressed and unencrypted
        };
        struct PakIndex
        {
            std::string mount;
            std::vector<PakEntry> entries;
        };

        struct ByteReader
        {
            const std::vector<char>& d;
            size_t p = 0;
            bool ok = true;
            auto need(size_t n) -> bool { return ok = ok && p + n <= d.size(); }
            template <typename T>
            auto get() -> T
            {
                T v{};
                if (need(sizeof(T))) std::memcpy(&v, d.data() + p, sizeof(T)), p += sizeof(T);
                return v;
            }
            auto fstring() -> std::string
            {
                const int32 n = get<int32>();
                if (!ok || n == 0) return {};
                if (n > 0)
                {
                    if (!need(static_cast<size_t>(n))) return {};
                    std::string s(d.data() + p, static_cast<size_t>(n - 1));
                    p += static_cast<size_t>(n);
                    return s;
                }
                const size_t chars = static_cast<size_t>(-static_cast<int64>(n));
                if (!need(chars * 2)) return {};
                std::string s{};
                for (size_t i = 0; i + 1 < chars; ++i) s.push_back(static_cast<char>(d[p + i * 2])); // pak names are ASCII
                p += chars * 2;
                return s;
            }
        };

        auto read_pak_index(const std::filesystem::path& path, StringType& why) -> std::optional<PakIndex>
        {
            std::ifstream f(path, std::ios::binary);
            if (!f) return why = L"cannot be opened", std::nullopt;
            f.seekg(0, std::ios::end);
            const uint64 size = static_cast<uint64>(f.tellg());
            // v8 tail: [EncryptionKeyGuid 16][bEncryptedIndex 1][Magic 4][Version 4][IndexOffset 8][IndexSize 8][Hash 20][5 x 32 compression names]
            constexpr uint64 kNames = 5 * 32, kInfo = 44;
            if (size < kNames + kInfo + 17) return why = L"is too small to be a pak", std::nullopt;
            std::vector<char> tail(kInfo + 1);
            f.seekg(static_cast<std::streamoff>(size - kNames - kInfo - 1));
            f.read(tail.data(), static_cast<std::streamsize>(tail.size()));
            ByteReader t{tail};
            const bool encrypted_index = t.get<uint8>() != 0;
            const uint32 magic = t.get<uint32>(), version = t.get<uint32>();
            const uint64 index_offset = t.get<uint64>(), index_size = t.get<uint64>();
            if (!f || magic != 0x5A6F12E1u) return why = L"has no pak footer", std::nullopt;
            if (version != 8) return why = std::format(L"is pak version {}, not 8", version), std::nullopt;
            if (encrypted_index) return why = L"has an encrypted index", std::nullopt;
            if (index_offset + index_size > size || index_size > (64u << 20)) return why = L"has an index outside the file", std::nullopt;

            std::vector<char> idx(static_cast<size_t>(index_size));
            f.seekg(static_cast<std::streamoff>(index_offset));
            f.read(idx.data(), static_cast<std::streamsize>(idx.size()));
            if (!f) return why = L"index cannot be read", std::nullopt;
            ByteReader r{idx};
            PakIndex out{};
            out.mount = r.fstring();
            const int32 count = r.get<int32>();
            for (int32 i = 0; r.ok && i < count; ++i)
            {
                PakEntry e{};
                e.name = r.fstring();
                const uint64 offset = r.get<uint64>();
                e.size = r.get<uint64>();
                r.get<uint64>(); // UncompressedSize
                const uint32 method = r.get<uint32>();
                r.p += 20; // SHA1
                uint64 header = 8 * 3 + 4 + 20 + 1 + 4;
                if (method != 0)
                {
                    const int32 blocks = r.get<int32>();
                    r.p += static_cast<size_t>(std::max(blocks, 0)) * 16;
                    header += 4 + static_cast<uint64>(std::max(blocks, 0)) * 16;
                }
                const uint8 flags = r.get<uint8>();
                r.get<uint32>(); // CompressionBlockSize
                e.data_offset = offset + header;
                e.plain = method == 0 && (flags & 1) == 0;
                out.entries.push_back(std::move(e));
            }
            if (!r.ok) return why = L"has a truncated index", std::nullopt;
            return out;
        }

        auto read_entry(const std::filesystem::path& path, const PakEntry& e) -> std::optional<std::string>
        {
            if (!e.plain || e.size > (1u << 20)) return std::nullopt; // manifests are small text files
            std::ifstream f(path, std::ios::binary);
            std::string s(static_cast<size_t>(e.size), '\0');
            f.seekg(static_cast<std::streamoff>(e.data_offset));
            f.read(s.data(), static_cast<std::streamsize>(s.size()));
            return f ? std::optional<std::string>(std::move(s)) : std::nullopt;
        }

        auto valid_id(const std::string& id) -> bool
        {
            return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
        }

        // A pak's items: each CustomContent/<id>/content.json with its folder's files. `strict` (server paks, which
        // the loader mounts itself) refuses the whole pak for anything outside the item folders or any bad item;
        // otherwise (custom_content/, which the engine mounts anyway) a bad item is skipped and stray files are
        // reported.
        auto parse_pak(const std::filesystem::path& path, Source source, bool strict, StringType* why) -> std::vector<Item>
        {
            const StringType pak = path.filename().wstring();
            auto index = read_pak_index(path, *why);
            if (!index) return {};
            if (lower(index->mount) != lower(kContentMount))
            {
                *why = std::format(L"is mounted at '{}', not '{}'", utf8_to_wide(index->mount), utf8_to_wide(kContentMount));
                return {};
            }
            std::vector<Item> items{};
            std::vector<std::string> stray{};
            for (const auto& e : index->entries)
            {
                const std::string name = e.name;
                if (!lower(name).starts_with(lower(kItemRoot)))
                {
                    stray.push_back(name);
                    continue;
                }
                const std::string rest = name.substr(std::string(kItemRoot).size());
                const size_t slash = rest.find('/');
                if (slash == std::string::npos || slash == 0 || rest.substr(slash + 1).empty())
                {
                    stray.push_back(name);
                    continue;
                }
                if (lower(rest.substr(slash + 1)) != "content.json") continue;
                const std::string folder_id = rest.substr(0, slash);
                Item item{};
                item.source = source;
                item.pak = pak;
                item.folder = std::string(kItemRoot) + folder_id + "/";
                const auto text = read_entry(path, e);
                if (!text || !jsonl::parse(*text, &item.manifest) || item.manifest.t != jsonl::Value::T::Obj)
                {
                    *why = std::format(L"{} is not a JSON object", utf8_to_wide(name));
                    if (strict) return {};
                    Output::send<LogLevel::Error>(STR("[content] {}: {} - skipped\n"), pak, *why);
                    continue;
                }
                const std::string id = item.manifest["id"].str(), type = item.manifest["type"].str();
                const TypeHandler* h = find_handler(type);
                StringType bad{};
                if (!valid_id(id))
                    bad = std::format(L"id '{}' must be 1-64 letters, digits or '_'", utf8_to_wide(id));
                else if (id != folder_id)
                    bad = std::format(L"id '{}' doesn't match its folder {}", utf8_to_wide(id), utf8_to_wide(item.folder));
                else if (!h)
                    bad = std::format(L"'{}' has type '{}', which this loader can't register yet", utf8_to_wide(id), utf8_to_wide(type));
                if (bad.empty())
                {
                    for (const auto& f : index->entries)
                        if (lower(f.name).starts_with(lower(item.folder))) item.files.push_back(f.name.substr(item.folder.size()));
                    item.info = {utf8_to_wide(id), utf8_to_wide(type), unlock_type_name(static_cast<int>(h->unlock_type)),
                                 utf8_to_wide(item.manifest["name"].is_null() ? id : item.manifest["name"].str())};
                    bad = validate_icon_image(item);
                    if (bad.empty()) bad = h->validate(item);
                }
                if (!bad.empty())
                {
                    *why = bad;
                    if (strict) return {};
                    Output::send<LogLevel::Error>(STR("[content] {}: {} - skipped\n"), pak, bad);
                    continue;
                }
                items.push_back(std::move(item));
            }
            // files of a folder without a content.json are outside every item too
            for (const auto& e : index->entries)
            {
                if (!lower(e.name).starts_with(lower(kItemRoot))) continue;
                const bool owned = std::any_of(items.begin(), items.end(), [&](const Item& i) { return lower(e.name).starts_with(lower(i.folder)); });
                if (!owned && std::find(stray.begin(), stray.end(), e.name) == stray.end()) stray.push_back(e.name);
            }
            if (!stray.empty())
            {
                if (strict)
                {
                    *why = std::format(L"holds {} file(s) outside its items' folders (first: {})", stray.size(), utf8_to_wide(stray.front()));
                    return {};
                }
                Output::send<LogLevel::Warning>(STR("[content] {}: {} file(s) outside the item folders (first: {}) - mounted by the engine anyway\n"), pak,
                                                stray.size(), utf8_to_wide(stray.front()));
            }
            if (items.empty() && why->empty()) *why = L"has no CustomContent/<id>/content.json";
            return items;
        }

        // ------------------------------------------------------------------ the registry ------------
        struct Entry
        {
            Item item;
            bool live = false;
        };
        std::map<std::string, Entry> g_items{}; // by lower-case id; game thread
        std::vector<StringType> g_custom_ids{};
        ServerPaks g_server_paks{};
        std::mutex g_mutex; // registered() may be asked from another thread

        auto custom_dir() -> std::filesystem::path
        {
            return std::filesystem::path(module_dir()).parent_path().parent_path() / L"Content" / L"Paks" / L"custom_content";
        }

        // Registers one item: its rows, its unlock row (free for custom content, an EventReward for server content)
        // and, after boot, the live managers. False with the reason logged.
        auto register_item(Item item, bool live) -> bool
        {
            const TypeHandler* h = find_handler(narrow_id(item.info.type));
            const std::string key = lower(narrow_id(item.info.id));
            const CharType* source = item.source == Source::Custom ? STR("custom_content") : STR("server content");
            {
                std::lock_guard lock(g_mutex);
                if (auto it = g_items.find(key); it != g_items.end())
                {
                    Output::send<LogLevel::Warning>(STR("[content] {}: '{}' is already registered (from {}) - skipped\n"), item.pak, item.info.id, it->second.item.pak);
                    return false;
                }
            }
            if (StringType err = h->add_rows(item); !err.empty())
            {
                Output::send<LogLevel::Error>(STR("[content] {}: {} '{}': {} - skipped\n"), item.pak, item.info.type, item.info.id, err);
                return false;
            }
            const FName id(item.info.id.c_str());
            const UnlockCategory category = item.source == Source::Custom ? UnlockCategory::ProgressionReward : UnlockCategory::EventReward;
            StringType why{};
            // without an unlock row UnlockMgr hides the item (LockedHidden) and nothing can unlock it
            uint8* unlock_row = add_unlock_row(h->unlock_type, id, category, &why);
            if (!unlock_row)
            {
                Output::send<LogLevel::Error>(STR("[content] {}: {} '{}': no unlock row ({}) - not usable, skipped\n"), item.pak, item.info.type, item.info.id, why);
                return false;
            }
            bool registered_live = false;
            if (live)
            {
                StringType err = h->register_live ? h->register_live(item) : StringType(STR("its type is registered at startup only"));
                if (err.empty() && !register_unlock_live(h->unlock_type, id, unlock_row, &err)) err = err.empty() ? STR("unlock row not registered") : err;
                if (err.empty())
                    registered_live = true;
                else
                    Output::send<LogLevel::Warning>(STR("[content] {}: '{}' is usable from the next launch: {}\n"), item.pak, item.info.id, err);
            }
            Output::send<LogLevel::Normal>(STR("[content] {} '{}' (\"{}\") from {} ({}{}{})\n"), item.info.type, item.info.id, item.info.name, item.pak, source,
                                           item.source == Source::Server ? STR(", locked until unlocked") : STR(", unlocked"),
                                           live ? (registered_live ? STR(", registered now") : STR(", next launch")) : STR(""));
            std::lock_guard lock(g_mutex);
            if (item.source == Source::Custom) g_custom_ids.push_back(utf8_to_wide(key));
            g_items.emplace(key, Entry{std::move(item), registered_live});
            return true;
        }

        // UnlockMgr has read its tables: its name map holds rows (it is built once, at the game instance's stage 2)
        auto managers_built() -> bool
        {
            UObject* mgr = find_live_object(STR("UnlockMgr"));
            if (!mgr) return false;
            const uint8* map = reinterpret_cast<const uint8*>(mgr) + 0x128;
            return *reinterpret_cast<const int32*>(map + 8) > *reinterpret_cast<const int32*>(map + 0x34);
        }

        auto apply() -> void
        {
            const auto t0 = GetTickCount64();
            // a late start (the managers already read their tables) registers everything live
            const bool late = managers_built();
            if (late) Output::send<LogLevel::Warning>(STR("[content] UnlockMgr has already read its tables at unreal-init - registering content live\n"));
            int custom = 0, server = 0;
            std::error_code ec{};
            for (auto it = std::filesystem::recursive_directory_iterator(custom_dir(), ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            {
                if (!it->is_regular_file(ec) || lower(it->path().extension().string()) != ".pak") continue;
                StringType why{};
                auto items = parse_pak(it->path(), Source::Custom, false, &why);
                if (items.empty()) Output::send<LogLevel::Error>(STR("[content] {} {} - nothing loaded from it\n"), it->path().filename().wstring(), why);
                for (auto& item : items) custom += register_item(std::move(item), late);
            }
            if (g_server_paks)
            {
                for (const auto& pak : g_server_paks(custom_ids()))
                {
                    StringType why{};
                    auto items = parse_pak(pak, Source::Server, true, &why);
                    if (items.empty()) Output::send<LogLevel::Error>(STR("[content] server pak {} {} - not registered\n"), pak.filename().wstring(), why);
                    for (auto& item : items) server += register_item(std::move(item), late);
                }
            }
            if (custom || server)
                Output::send<LogLevel::Normal>(STR("[content] {} custom and {} server item(s) registered ({} ms)\n"), custom, server, GetTickCount64() - t0);
            else
                Output::send<LogLevel::Verbose>(STR("[content] no custom or server content\n"));
        }
    } // namespace

    // ------------------------------------------------------------------ shared helpers --------------------
    auto utf8_to_wide(const std::string& s) -> StringType
    {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        StringType out(static_cast<size_t>(std::max(n, 0)), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
        return out;
    }

    auto narrow_id(const StringType& s) -> std::string
    {
        std::string out{};
        for (wchar_t c : s) out.push_back(c < 0x80 ? static_cast<char>(c) : '?'); // ids and types are ASCII
        return out;
    }

    auto lower(std::string s) -> std::string
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // The first instance that isn't a class default object, an archetype, or a subobject of one (a manager template
    // under a Default__ game instance has the same class as the live manager)
    auto find_live_object(const CharType* class_name) -> UObject*
    {
        constexpr uint32 kDefaultFlags = 0x10 | 0x20; // RF_ClassDefaultObject | RF_ArchetypeObject (EObjectFlags @0x8)
        std::vector<UObject*> found{};
        UObjectGlobals::FindAllOf(class_name, found);
        for (UObject* o : found)
        {
            if (*reinterpret_cast<const uint32*>(reinterpret_cast<const uint8*>(o) + 0x8) & kDefaultFlags) continue;
            bool in_default = false;
            for (const UObject* x = o; x && !in_default; x = x->GetOuterPrivate()) in_default = x->GetName().starts_with(STR("Default__"));
            if (!in_default) return o;
        }
        return nullptr;
    }

    // ------------------------------------------------------------------ the public API ---------------------
    auto read_items(const std::filesystem::path& pak, StringType* why) -> std::vector<ItemInfo>
    {
        std::vector<ItemInfo> out{};
        for (const auto& item : parse_pak(pak, Source::Server, true, why)) out.push_back(item.info);
        return out;
    }

    auto set_server_paks(ServerPaks provider) -> void { g_server_paks = std::move(provider); }

    auto custom_ids() -> std::vector<StringType>
    {
        std::lock_guard lock(g_mutex);
        return g_custom_ids;
    }

    auto registered(const StringType& id) -> std::optional<Registered>
    {
        std::lock_guard lock(g_mutex);
        const auto it = g_items.find(lower(narrow_id(id)));
        if (it == g_items.end()) return std::nullopt;
        return Registered{it->second.item.info, it->second.item.source, it->second.live};
    }

    auto register_live(const std::filesystem::path& pak) -> std::vector<LiveResult>
    {
        std::vector<LiveResult> out{};
        StringType why{};
        auto items = parse_pak(pak, Source::Server, true, &why);
        if (items.empty())
        {
            out.push_back({pak.filename().wstring(), false, why});
            return out;
        }
        for (auto& item : items)
        {
            const StringType id = item.info.id;
            if (const auto r = registered(id))
            {
                out.push_back({id, true, r->source == Source::Custom ? STR("custom_content overrides it") : STR("already registered")});
                continue;
            }
            const bool ok = register_item(std::move(item), true);
            const auto r = registered(id);
            out.push_back({id, ok && r && r->live, ok ? (r && r->live ? STR("registered") : STR("registered for the next launch")) : STR("not registered (see the log)")});
        }
        return out;
    }
} // namespace content

// Called by fnl_start after the unlocks, so the startup registration runs at unreal-init before UnlockMgr and
// UVenueMgr read their tables.
auto custom_content_start() -> void { on_unreal_init(&content::apply); }

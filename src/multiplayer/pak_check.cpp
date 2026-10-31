#include "multiplayer/pak_check.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

// zlib's crc32 (lib/libarchive/zlib.lib, linked for libarchive): the same CRC the engine's chunk hashes use
extern "C" unsigned long crc32(unsigned long crc, const unsigned char* buf, unsigned int len);

// Mirrors fuser_pak_extract.py (parse_footer / parse_index / _parse_entry_fields) for the plaintext indexes of
// community paks. The footer (FPakInfo, v8): ... bEncryptedIndex (1 byte), Magic 0x5A6F12E1, Version, IndexOffset,
// IndexSize, IndexHash[20], then the compression method names. The index: MountPoint (FString), NumEntries (int32),
// then per entry Filename (FString) and FPakEntry: Offset, Size, UncompressedSize (int64), CompressionMethodIndex
// (uint32), Hash[20], [NumBlocks (int32) + 16 bytes each, if compressed], Flags (uint8), CompressionBlockSize (uint32).

namespace p2p
{
    namespace
    {
        constexpr uint32_t kPakMagic = 0x5A6F12E1;
        constexpr uint8_t kEntryEncrypted = 0x01;
        constexpr size_t kFooterWindow = 4096, kMaxIndexBytes = 16u * 1024 * 1024, kFooterFromMagic = 204;
        constexpr int32_t kMaxEntries = 20000, kMaxBlocks = 1 << 20;

        struct Reader
        {
            const std::vector<char>& b;
            size_t at = 0;
            bool ok = true;
            template <typename T> auto get() -> T
            {
                T v{};
                if (!ok || at + sizeof(T) > b.size())
                {
                    ok = false;
                    return v;
                }
                std::memcpy(&v, b.data() + at, sizeof(T));
                at += sizeof(T);
                return v;
            }
            auto skip(size_t n) -> void
            {
                if (!ok || at + n > b.size()) ok = false;
                else at += n;
            }
            // An FString: ANSI only (a negative length means UTF-16, which no community pak uses: refused).
            auto fstring(std::string* out) -> bool
            {
                const int32_t n = get<int32_t>();
                if (!ok || n <= 0 || n > 1024 || at + static_cast<size_t>(n) > b.size() || b[at + static_cast<size_t>(n) - 1] != 0) return ok = false;
                out->assign(b.data() + at, static_cast<size_t>(n) - 1);
                at += static_cast<size_t>(n);
                return true;
            }
        };

        auto starts(const std::string& s, const std::string& p) -> bool { return s.rfind(p, 0) == 0; }

        // A package path relative to /Game/ (no extension) inside the song's own folders: anything under
        // Audio/Songs/<X>/ or DLC/Songs/<X>/, and the album art UI/AlbumArt/T_<X> and T_<X>_small.
        auto own_package(const std::string& rel, const std::string& x) -> bool
        {
            if (rel.empty() || rel.find_first_of("\\:.") != std::string::npos || rel.front() == '/' || rel.back() == '/' || rel.find("//") != std::string::npos)
                return false;
            for (const std::string root : {"Audio/Songs/", "DLC/Songs/"})
                if (starts(rel, root + x + "/")) return true;
            return rel == "UI/AlbumArt/T_" + x || rel == "UI/AlbumArt/T_" + x + "_small";
        }

        // An entry name (relative to the mount point): a package file of one of the song's own packages.
        auto allowed(const std::string& name, const std::string& x) -> bool
        {
            const size_t dot = name.rfind('.');
            if (dot == std::string::npos) return false;
            const std::string ext = name.substr(dot);
            return (ext == ".uasset" || ext == ".uexp" || ext == ".ubulk") && own_package(name.substr(0, dot), x);
        }

        // ---- The package check: what a song's .uasset may declare ----
        // Every community song package is a cooked UE 4.24 package whose exports are top-level objects of seven
        // classes, each with its class default object as template, and whose imports are those classes, their default
        // objects, and objects of those classes in the song's own packages. Nothing else is accepted: a package that
        // defines a class or function (a Blueprint), or imports any other class or package, could put objects into
        // the game whose code is not the game's.
        constexpr uint32_t kPackageTag = 0x9E2A83C1, kFilterEditorOnly = 0x80000000;
        constexpr int32_t kLegacyFileVersion = -7; // UE 4.24
        constexpr size_t kImportBytes = 28, kExportBytes = 104, kMaxUassetBytes = 4u * 1024 * 1024;
        constexpr int32_t kMaxTable = 1 << 16;
        constexpr uint32_t kUncompressedEntryHeader = 8 + 8 + 8 + 4 + 20 + 1 + 4; // the FPakEntry copy before an uncompressed entry's data

        struct SongClass
        {
            const char* package;
            const char* name;
        };
        constexpr SongClass kSongClasses[] = {
            {"/Script/Hype", "CelSongSet"},         {"/Script/Hype", "SongCelData"},          {"/Script/Hype", "SongTransitionCelData"},
            {"/Script/HmxAudio", "HmxMidiSongAsset"}, {"/Script/HmxAudio", "HmxMidiFileAsset"}, {"/Script/HmxAudio", "HmxFusionAsset"},
            {"/Script/Engine", "Texture2D"},
        };
        auto song_class(const std::string& package, const std::string& name) -> bool
        {
            for (const SongClass& c : kSongClasses)
                if (package == c.package && name == c.name) return true;
            return false;
        }
        auto script_package(const std::string& name) -> bool
        {
            for (const SongClass& c : kSongClasses)
                if (name == c.package) return true;
            return false;
        }

        // A name map entry or summary string: ANSI, or UTF-16 (each non-ASCII character becomes '?').
        auto any_fstring(Reader& r, std::string* out) -> bool
        {
            const int32_t n = r.get<int32_t>();
            if (!r.ok || n == 0 || n > 1024 || n < -1024) return r.ok = false;
            out->clear();
            if (n > 0)
            {
                if (r.at + static_cast<size_t>(n) > r.b.size() || r.b[r.at + static_cast<size_t>(n) - 1] != 0) return r.ok = false;
                out->assign(r.b.data() + r.at, static_cast<size_t>(n) - 1);
                r.at += static_cast<size_t>(n);
                return true;
            }
            for (int32_t i = 0; i < -n && r.ok; ++i)
            {
                const uint16_t c = r.get<uint16_t>();
                if (i + 1 < -n) out->push_back(c < 0x80 ? static_cast<char>(c) : '?');
            }
            return r.ok;
        }

        auto check_package(const std::vector<char>& data, const std::string& x, std::string* why) -> bool
        {
            const auto fail = [&](std::string w) {
                *why = std::move(w);
                return false;
            };
            Reader r{data};
            const uint32_t tag = r.get<uint32_t>();
            const int32_t legacy = r.get<int32_t>();
            if (!r.ok || tag != kPackageTag || legacy != kLegacyFileVersion) return fail("not a UE 4.24 package");
            r.skip(4 + 4 + 4); // LegacyUE3Version, FileVersionUE4, FileVersionLicenseeUE4
            const int32_t custom_versions = r.get<int32_t>();
            if (!r.ok || custom_versions < 0 || custom_versions > 64) return fail("bad custom version count");
            r.skip(static_cast<size_t>(custom_versions) * 20);
            r.skip(4); // TotalHeaderSize
            std::string folder;
            if (!any_fstring(r, &folder)) return fail("unreadable summary");
            const uint32_t package_flags = r.get<uint32_t>();
            // Without FilterEditorOnly a LocalizationId follows NameOffset; cooked packages always have it.
            if (!r.ok || !(package_flags & kFilterEditorOnly)) return fail("not a cooked package");
            int32_t v[9]{}; // NameCount, NameOffset, GatherableTextDataCount, ...Offset, ExportCount, ExportOffset, ImportCount, ImportOffset, DependsOffset
            for (int32_t& i : v) i = r.get<int32_t>();
            const auto [name_count, name_offset, text_count, text_offset, export_count, export_offset, import_count, import_offset, depends_offset] = v;
            (void)text_offset;
            if (!r.ok || name_count <= 0 || name_count > kMaxTable || export_count <= 0 || export_count > kMaxTable || import_count < 0 ||
                import_count > kMaxTable || text_count != 0)
                return fail("implausible table counts");
            if (name_offset < static_cast<int32_t>(r.at) || static_cast<size_t>(import_offset) + import_count * kImportBytes != static_cast<size_t>(export_offset) ||
                static_cast<size_t>(export_offset) + export_count * kExportBytes != static_cast<size_t>(depends_offset) ||
                static_cast<size_t>(depends_offset) > data.size())
                return fail("tables out of place");

            // Names (FString + 4 bytes of hashes); they must end where the imports start.
            std::vector<std::string> names(static_cast<size_t>(name_count));
            Reader nr{data, static_cast<size_t>(name_offset)};
            for (std::string& n : names)
            {
                if (!any_fstring(nr, &n)) return fail("unreadable name map");
                nr.skip(4);
            }
            if (!nr.ok || nr.at != static_cast<size_t>(import_offset)) return fail("name map out of place");
            bool bad_name = false;
            const auto name = [&](Reader& t) -> std::string { // an FName: index + number (number N > 0 is the suffix _N-1)
                const int32_t index = t.get<int32_t>(), number = t.get<int32_t>();
                if (!t.ok || index < 0 || index >= name_count || number < 0)
                {
                    bad_name = true;
                    return {};
                }
                return number ? names[static_cast<size_t>(index)] + "_" + std::to_string(number - 1) : names[static_cast<size_t>(index)];
            };

            struct Import
            {
                std::string class_package, class_name, object;
                int32_t outer = 0;
            };
            std::vector<Import> imports(static_cast<size_t>(import_count));
            Reader ir{data, static_cast<size_t>(import_offset)};
            for (Import& i : imports)
            {
                i.class_package = name(ir);
                i.class_name = name(ir);
                i.outer = ir.get<int32_t>();
                i.object = name(ir);
            }
            if (!ir.ok || bad_name) return fail("unreadable import table");
            const auto import_at = [&](int32_t index) -> const Import* { // a package index < 0 is import -index-1
                return index < 0 && -static_cast<int64_t>(index) <= import_count ? &imports[static_cast<size_t>(-(index + 1))] : nullptr;
            };
            const auto package_import = [&](int32_t index) -> const Import* {
                const Import* p = import_at(index);
                return p && p->class_name == "Package" ? p : nullptr;
            };
            for (const Import& i : imports)
            {
                if (i.class_name == "Package")
                {
                    if (i.class_package != "/Script/CoreUObject" || i.outer != 0) return fail("import of package " + i.object + " is malformed");
                    if (!script_package(i.object) && !(starts(i.object, "/Game/") && own_package(i.object.substr(6), x)))
                        return fail("imports package " + i.object + " (outside the song's folders)");
                    continue;
                }
                const Import* outer = package_import(i.outer);
                if (!outer) return fail("import " + i.object + " is not inside an imported package");
                if (i.class_name == "Class")
                {
                    if (i.class_package != "/Script/CoreUObject" || !song_class(outer->object, i.object))
                        return fail("imports class " + outer->object + "." + i.object);
                    continue;
                }
                if (!song_class(i.class_package, i.class_name)) return fail("imports an object of class " + i.class_package + "." + i.class_name);
                if (script_package(outer->object) && (outer->object != i.class_package || i.object != "Default__" + i.class_name))
                    return fail("imports " + outer->object + "." + i.object);
            }

            Reader er{data, static_cast<size_t>(export_offset)};
            for (int32_t e = 0; e < export_count; ++e)
            {
                er.at = static_cast<size_t>(export_offset) + static_cast<size_t>(e) * kExportBytes;
                const int32_t class_index = er.get<int32_t>(), super_index = er.get<int32_t>(), template_index = er.get<int32_t>(),
                              outer_index = er.get<int32_t>();
                const std::string object = name(er);
                if (!er.ok || bad_name) return fail("unreadable export table");
                const Import* cls = import_at(class_index);
                if (!cls || cls->class_name != "Class") return fail("export " + object + " has a class defined in the package");
                if (super_index != 0) return fail("export " + object + " is a struct, class or function");
                if (outer_index != 0) return fail("export " + object + " is a subobject");
                if (template_index != 0)
                {
                    const Import* t = import_at(template_index);
                    if (!t || t->class_name != cls->object || t->object != "Default__" + cls->object) return fail("export " + object + " has a foreign template");
                }
            }
            return true;
        }
    } // namespace

    auto check_song_pak(const std::wstring& path, const std::string& x, std::string* why) -> bool
    {
        const auto fail = [&](std::string w) {
            if (why) *why = std::move(w);
            return false;
        };
        if (x.empty() || x.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
            return fail("bad song name");
        std::error_code ec;
        const uint64_t size = std::filesystem::file_size(path, ec);
        if (ec) return fail("cannot read the file");
        if (size > kMaxSongPakBytes) return fail(std::format("{} bytes is over the 64 MB limit", size));
        if (size < 256) return fail("too small to be a pak");
        std::ifstream in(std::filesystem::path(path), std::ios::binary);
        const size_t window = static_cast<size_t>(std::min<uint64_t>(size, kFooterWindow));
        std::vector<char> tail(window);
        in.seekg(static_cast<std::streamoff>(size - window));
        if (!in.read(tail.data(), static_cast<std::streamsize>(window))) return fail("cannot read the footer");
        // the last magic in the window is the footer's
        size_t magic_at = std::string::npos;
        for (size_t i = window - 4 + 1; i-- > 1;)
        {
            uint32_t m = 0;
            std::memcpy(&m, tail.data() + i, 4);
            if (m == kPakMagic)
            {
                magic_at = i;
                break;
            }
        }
        if (magic_at == std::string::npos || magic_at + 44 > window) return fail("no pak footer");
        // A v8 FPakInfo ends 204 bytes after its magic (magic 4, version 4, offset 8, size 8, hash 20, 5 compression
        // method names of 32): anything else is a cut or padded file.
        if (window - magic_at != kFooterFromMagic) return fail(std::format("footer ends {} bytes after the magic (want {})", window - magic_at, kFooterFromMagic));
        const uint8_t encrypted_index = static_cast<uint8_t>(tail[magic_at - 1]);
        uint32_t version = 0;
        uint64_t index_offset = 0, index_size = 0;
        std::memcpy(&version, tail.data() + magic_at + 4, 4);
        std::memcpy(&index_offset, tail.data() + magic_at + 8, 8);
        std::memcpy(&index_size, tail.data() + magic_at + 16, 8);
        if (version != 8) return fail(std::format("pak version {} (community paks are version 8)", version));
        if (encrypted_index != 0) return fail("encrypted index");
        const uint64_t footer_at = size - window + magic_at;
        if (index_size == 0 || index_size > kMaxIndexBytes || index_offset > footer_at || index_size > footer_at - index_offset)
            return fail("index outside the file");
        std::vector<char> index(static_cast<size_t>(index_size));
        in.seekg(static_cast<std::streamoff>(index_offset));
        if (!in.read(index.data(), static_cast<std::streamsize>(index_size))) return fail("cannot read the index");

        Reader r{index};
        std::string mount;
        if (!r.fstring(&mount)) return fail("unreadable mount point");
        if (mount != "../../../Fuser/Content/") return fail("mount point '" + mount + "' (want ../../../Fuser/Content/)");
        const int32_t entries = r.get<int32_t>();
        if (!r.ok || entries <= 0 || entries > kMaxEntries) return fail("implausible entry count");
        std::vector<std::tuple<std::string, int64_t, int64_t>> packages; // .uasset entries: name, data offset, size
        for (int32_t i = 0; i < entries; ++i)
        {
            std::string name;
            if (!r.fstring(&name)) return fail(std::format("entry {}: unreadable name", i));
            const int64_t offset = r.get<int64_t>(), length = r.get<int64_t>(), uncompressed = r.get<int64_t>();
            const uint32_t method = r.get<uint32_t>();
            r.skip(20);
            if (!r.ok) return fail(std::format("entry {}: index ends early", i));
            // Community song paks are packed uncompressed (their footers name no compression method).
            if (method != 0) return fail(name + ": compressed entry");
            const uint8_t flags = r.get<uint8_t>();
            r.skip(4);
            if (!r.ok) return fail(std::format("entry {}: index ends early", i));
            if (flags & kEntryEncrypted) return fail(name + ": encrypted entry");
            if (offset < 0 || length < 0 || uncompressed != length ||
                static_cast<uint64_t>(offset) + kUncompressedEntryHeader + static_cast<uint64_t>(length) > index_offset)
                return fail(name + ": data outside the file");
            if (!allowed(name, x))
                return fail("'" + name + "' is not a package file in the song's folders (Audio/Songs/" + x + "/, DLC/Songs/" + x + "/, UI/AlbumArt/T_" + x +
                            ", UI/AlbumArt/T_" + x + "_small)");
            if (name.ends_with(".uasset"))
            {
                if (static_cast<uint64_t>(length) > kMaxUassetBytes) return fail(name + ": over 4 MB");
                packages.emplace_back(name, offset + kUncompressedEntryHeader, length);
            }
        }
        // Each .uasset's summary and tables (its exports' data is in the .uexp), read where the engine reads it: after
        // the entry's header copy, for the index entry's size.
        for (const auto& [name, at, length] : packages)
        {
            std::vector<char> data(static_cast<size_t>(length));
            in.seekg(static_cast<std::streamoff>(at));
            if (!in.read(data.data(), static_cast<std::streamsize>(data.size()))) return fail(name + ": cannot read it");
            std::string why_package;
            if (!check_package(data, x, &why_package)) return fail(name + ": " + why_package);
        }
        return true;
    }

    namespace
    {
        constexpr uint32_t kSigMagic = 0x73832DAA, kSigVersion = 1;
        constexpr size_t kSigBlock = 512, kSigHead = 12 + kSigBlock, kSigChunk = 64 * 1024, kMaxSigFileBytes = 1024 * 1024;
        // SHA-256 of the signature block every community .sig carries. The bypass
        // turns this block's decrypted hash into a pass; a .sig with any other block fails the master check, which a
        // new chunk table can't fix.
        constexpr char kSigBlockSha256[] = "06636f00f378a2268c1785048a907da10af3ac06f5989e7b08dee3373b82882f";

        // An incremental BCrypt hash (SHA-1 for pak entries and the index, SHA-256 for the signature block).
        struct Hash
        {
            BCRYPT_ALG_HANDLE alg = nullptr;
            BCRYPT_HASH_HANDLE h = nullptr;
            explicit Hash(LPCWSTR id)
            {
                if (BCryptOpenAlgorithmProvider(&alg, id, nullptr, 0) != 0 || BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) != 0) h = nullptr;
            }
            ~Hash()
            {
                if (h) BCryptDestroyHash(h);
                if (alg) BCryptCloseAlgorithmProvider(alg, 0);
            }
            auto add(const char* p, size_t n) -> bool { return h && BCryptHashData(h, reinterpret_cast<PUCHAR>(const_cast<char*>(p)), static_cast<ULONG>(n), 0) == 0; }
            auto finish(uint8_t* out, size_t n) -> bool { return h && BCryptFinishHash(h, out, static_cast<ULONG>(n), 0) == 0; }
        };

        auto read_all(const std::wstring& path, uint64_t max, std::vector<char>* out) -> bool
        {
            std::error_code ec;
            const uint64_t size = std::filesystem::file_size(path, ec);
            if (ec || size > max) return false;
            out->resize(static_cast<size_t>(size));
            std::ifstream in(std::filesystem::path(path), std::ios::binary);
            return in.read(out->data(), static_cast<std::streamsize>(size)) || size == 0;
        }

        // One FPakEntry (v8), as the index and each entry's header copy store it; at moves past it.
        struct Entry
        {
            int64_t offset = 0, size = 0, uncompressed = 0;
            uint32_t method = 0;
            uint8_t hash[20]{};
            std::vector<std::pair<int64_t, int64_t>> blocks;
        };
        auto read_entry(Reader& r, Entry* e) -> bool
        {
            e->offset = r.get<int64_t>(), e->size = r.get<int64_t>(), e->uncompressed = r.get<int64_t>();
            e->method = r.get<uint32_t>();
            for (uint8_t& b : e->hash) b = r.get<uint8_t>();
            if (e->method != 0)
            {
                const int32_t n = r.get<int32_t>();
                if (!r.ok || n < 0 || n > kMaxBlocks) return false;
                for (int32_t i = 0; i < n && r.ok; ++i)
                {
                    const int64_t start = r.get<int64_t>(), end = r.get<int64_t>();
                    e->blocks.emplace_back(start, end);
                }
            }
            r.get<uint8_t>();
            r.get<uint32_t>();
            return r.ok;
        }

        // True when the pak is exactly what its packer wrote: the footer's SHA-1 matches the index, and every entry's
        // header copy matches its index entry, its data lies before the index, and its data (for a compressed entry,
        // its blocks, relative to the entry) matches the entry's SHA-1.
        auto pak_consistent(const std::vector<char>& d, std::string* why) -> bool
        {
            const auto fail = [&](std::string w) {
                *why = std::move(w);
                return false;
            };
            if (d.size() < kFooterFromMagic + 1) return fail("too small to be a pak");
            const size_t magic_at = d.size() - kFooterFromMagic;
            uint32_t magic = 0, version = 0;
            uint64_t index_offset = 0, index_size = 0;
            std::memcpy(&magic, d.data() + magic_at, 4);
            std::memcpy(&version, d.data() + magic_at + 4, 4);
            std::memcpy(&index_offset, d.data() + magic_at + 8, 8);
            std::memcpy(&index_size, d.data() + magic_at + 16, 8);
            if (magic != kPakMagic || version != 8 || d[magic_at - 1] != 0) return fail("not an unencrypted v8 pak");
            if (index_size == 0 || index_size > kMaxIndexBytes || index_offset > magic_at || index_size > magic_at - index_offset)
                return fail("index outside the file");
            uint8_t got[20]{};
            {
                Hash sha1(BCRYPT_SHA1_ALGORITHM);
                if (!sha1.add(d.data() + index_offset, static_cast<size_t>(index_size)) || !sha1.finish(got, sizeof(got))) return fail("SHA-1 unavailable");
            }
            if (std::memcmp(got, d.data() + magic_at + 24, sizeof(got)) != 0) return fail("the index doesn't match the footer's index hash");

            const std::vector<char> index(d.begin() + static_cast<ptrdiff_t>(index_offset), d.begin() + static_cast<ptrdiff_t>(index_offset + index_size));
            Reader r{index};
            std::string name;
            if (!r.fstring(&name)) return fail("unreadable mount point");
            const int32_t entries = r.get<int32_t>();
            if (!r.ok || entries <= 0 || entries > kMaxEntries) return fail("implausible entry count");
            for (int32_t i = 0; i < entries; ++i)
            {
                Entry e;
                if (!r.fstring(&name) || !read_entry(r, &e)) return fail(std::format("entry {}: unreadable", i));
                if (e.offset < 0 || e.size < 0 || static_cast<uint64_t>(e.offset) >= index_offset) return fail(name + ": outside the file");
                Reader hr{d, static_cast<size_t>(e.offset)};
                Entry copy;
                if (!read_entry(hr, &copy) || copy.size != e.size || copy.uncompressed != e.uncompressed || copy.method != e.method ||
                    std::memcmp(copy.hash, e.hash, sizeof(e.hash)) != 0)
                    return fail(name + ": its header copy doesn't match the index");
                Hash sha1(BCRYPT_SHA1_ALGORITHM);
                if (e.method == 0)
                {
                    if (hr.at + static_cast<uint64_t>(e.size) > index_offset) return fail(name + ": data outside the file");
                    sha1.add(d.data() + hr.at, static_cast<size_t>(e.size));
                }
                else
                    for (const auto& [start, end] : e.blocks)
                    {
                        if (start < 0 || end < start || static_cast<uint64_t>(e.offset + end) > index_offset) return fail(name + ": a block outside the file");
                        sha1.add(d.data() + e.offset + start, static_cast<size_t>(end - start));
                    }
                if (!sha1.finish(got, sizeof(got))) return fail("SHA-1 unavailable");
                if (std::memcmp(got, e.hash, sizeof(got)) != 0) return fail(name + ": data doesn't match its SHA-1");
            }
            return true;
        }
    } // namespace

    auto check_song_sig(const std::wstring& pak_path, const std::wstring& sig_path, std::string* note, unsigned long long max_pak_bytes) -> SigVerdict
    {
        const auto bad = [&](std::string w) {
            *note = std::move(w);
            return SigVerdict::Bad;
        };
        note->clear();
        std::vector<char> sig, pak;
        if (!read_all(sig_path, kMaxSigFileBytes, &sig)) return bad("cannot read the .sig");
        if (sig.size() < kSigHead + 4) return bad(std::format(".sig is {} bytes", sig.size()));
        uint32_t magic = 0, version = 0;
        int32_t block = 0, count = 0;
        std::memcpy(&magic, sig.data(), 4);
        std::memcpy(&version, sig.data() + 4, 4);
        std::memcpy(&block, sig.data() + 8, 4);
        std::memcpy(&count, sig.data() + kSigHead, 4);
        if (magic != kSigMagic || version != kSigVersion || block != static_cast<int32_t>(kSigBlock))
            return bad(std::format(".sig header is magic 0x{:08X}, version {}, block {} bytes", magic, version, block));
        {
            uint8_t got[32]{};
            Hash sha256(BCRYPT_SHA256_ALGORITHM);
            if (!sha256.add(sig.data() + 12, kSigBlock) || !sha256.finish(got, sizeof(got))) return bad("SHA-256 unavailable");
            std::string hex;
            for (uint8_t b : got) hex += std::format("{:02x}", b);
            if (hex != kSigBlockSha256) return bad("the .sig has a non-standard signature block (" + hex.substr(0, 16) + ")");
        }
        if (!read_all(pak_path, max_pak_bytes, &pak) || pak.empty()) return bad("cannot read the pak");

        // The pak's real chunk hashes against the table (a table of the wrong length counts its missing or extra
        // entries as stale).
        const size_t chunks = (pak.size() + kSigChunk - 1) / kSigChunk;
        const bool table_whole = count >= 0 && sig.size() == kSigHead + 4 + 4 * static_cast<size_t>(count);
        const size_t have = table_whole ? static_cast<size_t>(count) : 0;
        std::vector<uint32_t> crcs(chunks);
        size_t stale = have > chunks ? have - chunks : 0;
        for (size_t i = 0; i < chunks; ++i)
        {
            const size_t at = i * kSigChunk, len = (std::min)(kSigChunk, pak.size() - at);
            crcs[i] = static_cast<uint32_t>(crc32(0, reinterpret_cast<const unsigned char*>(pak.data() + at), static_cast<unsigned int>(len)));
            uint32_t want = 0;
            if (i < have) std::memcpy(&want, sig.data() + kSigHead + 4 + 4 * i, 4);
            if (i >= have || want != crcs[i]) ++stale;
        }
        if (stale == 0 && have == chunks) return SigVerdict::Ok;

        std::string why;
        if (!pak_consistent(pak, &why))
            return bad(std::format("{} of {} chunk hashes are stale and the pak is not as packed: {}", stale, chunks, why));
        std::vector<char> fixed(sig.begin(), sig.begin() + kSigHead);
        const int32_t n = static_cast<int32_t>(chunks);
        fixed.insert(fixed.end(), reinterpret_cast<const char*>(&n), reinterpret_cast<const char*>(&n) + 4);
        fixed.insert(fixed.end(), reinterpret_cast<const char*>(crcs.data()), reinterpret_cast<const char*>(crcs.data() + crcs.size()));
        std::wstring tmp = sig_path + L".repair";
        {
            std::ofstream out(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
            out.write(fixed.data(), static_cast<std::streamsize>(fixed.size()));
            if (!out.good()) return bad("cannot write " + std::filesystem::path(tmp).string());
        }
        if (!MoveFileExW(tmp.c_str(), sig_path.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            const DWORD err = GetLastError();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return bad(std::format("cannot replace the .sig (error {})", err));
        }
        *note = table_whole ? std::format("{} of {} chunk hashes were stale; the pak is as packed, so the .sig was rewritten", stale, chunks)
                            : std::format("the chunk table was cut short or padded ({} bytes for {} chunks); the pak is as packed, so the .sig was rewritten",
                                          sig.size(), chunks);
        return SigVerdict::Repaired;
    }
} // namespace p2p

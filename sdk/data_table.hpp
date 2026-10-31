#pragma once
// UDataTable access shared by the runtime table patches (unlocks.cpp, custom_content.cpp): loading a table the way
// the game's managers do, walking its rows and adding rows through the engine's own UDataTable::AddRow.
// Engine layout used besides reflection, shared by every 4.24 build: UDataTable::RowStruct @0x28, RowMap @0x30
// (TMap<FName, uint8*>), AddRow at vtable 0x288.
#include "fue/fue.hpp"

#include <functional>
#include <vector>

namespace data_table
{
    using namespace fue;

    // A ProcessEvent parameter buffer, its parameters constructed and destroyed through their properties
    struct Call
    {
        UFunction* fn;
        std::vector<uint8> buf;
        FNL_API explicit Call(UFunction* f);
        FNL_API ~Call();
        Call(const Call&) = delete;
        Call& operator=(const Call&) = delete;
        FNL_API auto prop(const CharType* n) -> FProperty*;
        FNL_API auto addr(const CharType* n) -> uint8*;

      private:
        template <typename F>
        void each(F&& f)
        {
            for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
            {
                if (!p->HasAnyPropertyFlags(CPF_Parm)) break;
                f(p);
            }
        }
    };

    // path -> FSoftObjectPath -> TSoftObjectPtr -> LoadSynchronous, all through KismetSystemLibrary
    FNL_API auto load_blocking(const CharType* path) -> UObject*;

    // True when p lies in the game exe's image (a vtable slot that still points where 4.24 puts it)
    FNL_API auto in_exe(const void* p) -> bool;

    // A TSet's live elements, `stride` bytes apart: the sparse array's TArray @0 (Num @8), its allocation bits inline
    // @0x10 or at the pointer @0x20. f(uint8* element) is called for each allocated slot.
    template <typename F>
    auto each_set_element(const void* set, size_t stride, F&& f) -> void
    {
        const uint8* s = static_cast<const uint8*>(set);
        uint8* data = *reinterpret_cast<uint8* const*>(s);
        const int32 slots = *reinterpret_cast<const int32*>(s + 8);
        const uint32* bits = *reinterpret_cast<uint32* const*>(s + 0x20);
        if (!bits) bits = reinterpret_cast<const uint32*>(s + 0x10);
        for (int32 i = 0; i < slots; ++i)
            if ((bits[i >> 5] >> (i & 31)) & 1) f(data + static_cast<size_t>(i) * stride);
    }

    // The table's row struct (nullptr when `table` is not a plain UDataTable)
    FNL_API auto row_struct(UObject* table) -> UStruct*;

    // RowMap elements are TPair<FName, uint8*> plus the set's hash links: 0x18 bytes
    template <typename F>
    auto each_row(UObject* table, F&& f) -> void
    {
        each_set_element(reinterpret_cast<const uint8*>(table) + 0x30, 0x18,
                         [&](uint8* e) { f(*reinterpret_cast<FName*>(e), *reinterpret_cast<uint8**>(e + 8)); });
    }

    FNL_API auto find_row(UObject* table, const FName& name) -> uint8*;
    FNL_API auto row_count(UObject* table) -> int32;

    // True when the table's UDataTable::AddRow vtable slot points into the exe, where 4.24 puts it
    FNL_API auto can_add_rows(UObject* table) -> bool;

    // UDataTable::AddRow(name, *tmpl): the engine copies the template row into a new allocation under `name`.
    // Returns the new row, or nullptr (AddRow not where 4.24 puts it, or the row did not appear).
    FNL_API auto add_row(UObject* table, const FName& name, const uint8* tmpl) -> uint8*;

    // ------------------------------------------------------------------ maps keyed by FName ----------------
    // The managers keep what they read from their tables in TMap<FName, V>s, built once. An item added after that
    // is inserted the way TSet::Emplace inserts: an element (a free slot, or a new one at the end), its allocation
    // bit, then its hash link, or a rehash once the element count outgrows the buckets. The set's layout (4.24): the
    // sparse array's TArray @0 (Num @8, Max @0xC), allocation bits inline @0x10 or at @0x20 (NumBits @0x28, MaxBits
    // @0x2C), FirstFreeIndex @0x30, NumFreeIndices @0x34, the hash's bucket inline @0x38 or at @0x40, HashSize
    // @0x48. An element is the pair (the FName key at 0, the value 8-aligned after it), then HashNextId and
    // HashIndex in its last 8 bytes; `stride` is its size.

    // GetTypeHash(FName) as the exe computes it: the number plus a mix of the comparison index.
    FNL_API auto fname_hash(const FName& name) -> uint32;

    // The element holding key, or nullptr.
    FNL_API auto fname_map_find(const void* map, size_t stride, const FName& key) -> uint8*;

    // Adds key to the map and returns the new element's value storage (8 bytes after the element's start), after
    // `init(value)` has constructed the value in it. nullptr (nothing changed) when the key is already there, or
    // when the map's existing hash links don't follow fname_hash (a layout this code doesn't know); why says which.
    FNL_API auto fname_map_add(void* map, size_t stride, const FName& key, const std::function<void(uint8* value)>& init,
                       StringType* why) -> uint8*;

    // A map's element size for values of `value_size` bytes (the pair rounded to 8, plus the two hash links).
    constexpr auto fname_map_stride(size_t value_size) -> size_t { return 8 + ((value_size + 7) & ~size_t{7}) + 8; }

    // A struct value constructed in zeroed `dst` and copied from `src` property by property (the bytes before its
    // first property - FTableRowBase's vtable - copied as they are).
    FNL_API auto copy_struct(UStruct* st, uint8* dst, const uint8* src) -> void;
} // namespace data_table

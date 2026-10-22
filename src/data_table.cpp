#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "data_table.hpp"

namespace data_table
{
    namespace
    {
        constexpr size_t kRowStruct = 0x28, kRowMap = 0x30, kVAddRow = 0x288; // UDataTable (4.24)
    }

    Call::Call(UFunction* f) : fn(f), buf(static_cast<size_t>(f->GetPropertiesSize()) + 16, 0)
    {
        each([&](FProperty* p) { p->InitializeValue_InContainer(buf.data()); });
    }

    Call::~Call()
    {
        each([&](FProperty* p) { p->DestroyValue_InContainer(buf.data()); });
    }

    auto Call::prop(const CharType* n) -> FProperty* { return fn->FindProperty(FName(n)); }
    auto Call::addr(const CharType* n) -> uint8* { return prop(n)->ContainerPtrToValuePtr<uint8>(buf.data()); }

    auto load_blocking(const CharType* path) -> UObject*
    {
        UObject* ksl = UObjectGlobals::StaticFindObject(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        auto* make = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary:MakeSoftObjectPath"));
        auto* conv = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary:Conv_SoftObjPathToSoftObjRef"));
        auto* load = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary:LoadAsset_Blocking"));
        if (!ksl || !make || !conv || !load) return nullptr;
        Call c1(make), c2(conv), c3(load);
        if (!c1.prop(STR("PathString")) || !c2.prop(STR("SoftObjectPath")) || !c3.prop(STR("Asset"))) return nullptr;
        {
            const FString s(path);
            c1.prop(STR("PathString"))->CopyCompleteValue(c1.addr(STR("PathString")), &s);
        }
        ksl->ProcessEvent(make, c1.buf.data());
        c2.prop(STR("SoftObjectPath"))->CopyCompleteValue(c2.addr(STR("SoftObjectPath")), c1.addr(STR("ReturnValue")));
        ksl->ProcessEvent(conv, c2.buf.data());
        c3.prop(STR("Asset"))->CopyCompleteValue(c3.addr(STR("Asset")), c2.addr(STR("ReturnValue")));
        ksl->ProcessEvent(load, c3.buf.data());
        return *reinterpret_cast<UObject**>(c3.addr(STR("ReturnValue")));
    }

    auto in_exe(const void* p) -> bool
    {
        HMODULE mod = nullptr;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(p), &mod) &&
               mod == GetModuleHandleW(nullptr);
    }

    auto row_struct(UObject* table) -> UStruct*
    {
        // a plain UDataTable (UCompositeDataTable's AddRow is a no-op)
        if (!table || table->GetClassPrivate()->GetName() != STR("DataTable")) return nullptr;
        return *reinterpret_cast<UStruct**>(reinterpret_cast<uint8*>(table) + kRowStruct);
    }

    auto find_row(UObject* table, const FName& name) -> uint8*
    {
        uint8* found = nullptr;
        each_row(table, [&](const FName& n, uint8* row) {
            if (n == name) found = row;
        });
        return found;
    }

    auto row_count(UObject* table) -> int32
    {
        const uint8* set = reinterpret_cast<const uint8*>(table) + kRowMap;
        return *reinterpret_cast<const int32*>(set + 8) - *reinterpret_cast<const int32*>(set + 0x34);
    }

    auto can_add_rows(UObject* table) -> bool { return table && in_exe((*reinterpret_cast<void* const* const*>(table))[kVAddRow / sizeof(void*)]); }

    auto add_row(UObject* table, const FName& name, const uint8* tmpl) -> uint8*
    {
        if (!tmpl || !can_add_rows(table)) return nullptr;
        void* fn = (*reinterpret_cast<void* const* const*>(table))[kVAddRow / sizeof(void*)];
        reinterpret_cast<void (*)(UObject*, FName, const void*)>(fn)(table, name, tmpl); // UDataTable::AddRow(FName, const FTableRowBase&)
        return find_row(table, name);
    }

    // ------------------------------------------------------------------ maps keyed by FName ----------------
    namespace
    {
        // TSet's members (4.24)
        constexpr size_t kData = 0, kNum = 8, kMax = 0xC, kBitsInline = 0x10, kBitsSecondary = 0x20, kNumBits = 0x28, kMaxBits = 0x2C,
                         kFirstFree = 0x30, kNumFree = 0x34, kHashInline = 0x38, kHashSecondary = 0x40, kHashSize = 0x48;
        constexpr int32 kInlineBitWords = 4; // TBitArray's TInlineAllocator<4>

        template <typename T>
        auto field(void* set, size_t off) -> T&
        {
            return *reinterpret_cast<T*>(static_cast<uint8*>(set) + off);
        }
        auto bits(void* set) -> uint32*
        {
            uint32* secondary = field<uint32*>(set, kBitsSecondary);
            return secondary ? secondary : &field<uint32>(set, kBitsInline);
        }
        auto buckets(void* set) -> int32*
        {
            int32* secondary = field<int32*>(set, kHashSecondary);
            return secondary ? secondary : &field<int32>(set, kHashInline);
        }
        auto element(void* set, int32 i, size_t stride) -> uint8* { return field<uint8*>(set, kData) + static_cast<size_t>(i) * stride; }
        auto allocated(void* set, int32 i) -> bool { return (bits(set)[i >> 5] >> (i & 31)) & 1; }
        auto hash_next(uint8* e, size_t stride) -> int32& { return *reinterpret_cast<int32*>(e + stride - 8); }
        auto hash_index(uint8* e, size_t stride) -> int32& { return *reinterpret_cast<int32*>(e + stride - 4); }

        // TSetAllocator's bucket count for n elements: 1 below 4, else the power of two at least n / 2 + 8
        auto bucket_count(int32 n) -> int32
        {
            if (n < 4) return 1;
            int32 b = 1;
            while (b < n / 2 + 8) b <<= 1;
            return b;
        }

        // TBitArray::Add(false) storage: room for one more bit (TInlineAllocator<4> words inline, more in a heap block)
        auto grow_bits(void* set) -> void
        {
            const int32 num_bits = field<int32>(set, kNumBits);
            int32& max_bits = field<int32>(set, kMaxBits);
            if (num_bits + 1 <= max_bits) return;
            const int32 words = (num_bits + 1 + 31) / 32;
            if (words <= kInlineBitWords && !field<uint32*>(set, kBitsSecondary))
            {
                max_bits = kInlineBitWords * 32;
                return;
            }
            const int32 new_words = words + 16 + words * 3 / 8;
            auto* fresh = static_cast<uint32*>(FMemory::Malloc(static_cast<size_t>(new_words) * 4));
            std::memset(fresh, 0, static_cast<size_t>(new_words) * 4);
            std::memcpy(fresh, bits(set), static_cast<size_t>((num_bits + 31) / 32) * 4);
            if (uint32* old = field<uint32*>(set, kBitsSecondary)) FMemory::Free(old);
            field<uint32*>(set, kBitsSecondary) = fresh;
            max_bits = new_words * 32;
        }

        // TSparseArray::AddUninitialized: the first free slot, or a new one at the end. The slot's bit is set.
        auto add_slot(void* set, size_t stride) -> int32
        {
            int32 index;
            int32& num_free = field<int32>(set, kNumFree);
            if (num_free > 0)
            {
                index = field<int32>(set, kFirstFree);
                const int32 next = *reinterpret_cast<int32*>(element(set, index, stride) + 4); // FFreeListLink {Prev, Next}
                field<int32>(set, kFirstFree) = next;
                if (--num_free != 0) *reinterpret_cast<int32*>(element(set, next, stride)) = -1;
            }
            else
            {
                int32& num = field<int32>(set, kNum);
                int32& max = field<int32>(set, kMax);
                index = num;
                if (num + 1 > max)
                {
                    max = num + 1 <= 4 && max == 0 ? 4 : num + 16 + num * 3 / 8;
                    field<uint8*>(set, kData) = static_cast<uint8*>(FMemory::Realloc(field<uint8*>(set, kData), static_cast<size_t>(max) * stride));
                }
                ++num;
                grow_bits(set);
                ++field<int32>(set, kNumBits);
            }
            bits(set)[index >> 5] |= 1u << (index & 31);
            return index;
        }

        auto rehash(void* set, size_t stride, int32 size) -> void
        {
            if (int32* old = field<int32*>(set, kHashSecondary)) FMemory::Free(old);
            field<int32*>(set, kHashSecondary) = size > 1 ? static_cast<int32*>(FMemory::Malloc(static_cast<size_t>(size) * 4)) : nullptr;
            field<int32>(set, kHashSize) = size;
            int32* h = buckets(set);
            for (int32 b = 0; b < size; ++b) h[b] = -1;
            const int32 num = field<int32>(set, kNum);
            for (int32 i = 0; i < num; ++i)
            {
                if (!allocated(set, i)) continue;
                uint8* e = element(set, i, stride);
                const int32 b = static_cast<int32>(fname_hash(*reinterpret_cast<FName*>(e)) & static_cast<uint32>(size - 1));
                hash_index(e, stride) = b;
                hash_next(e, stride) = h[b];
                h[b] = i;
            }
        }
    } // namespace

    auto fname_hash(const FName& name) -> uint32
    {
        const uint32 c = name.ComparisonIndex;
        return ((c & 0xffff) >> 4) + (c >> 16) * 0x80001u + (c & 0xffff) + c * 0x10000u + name.Number;
    }

    auto fname_map_find(const void* map, size_t stride, const FName& key) -> uint8*
    {
        void* set = const_cast<void*>(map);
        const int32 num = field<int32>(set, kNum);
        for (int32 i = 0; i < num; ++i)
            if (allocated(set, i) && *reinterpret_cast<FName*>(element(set, i, stride)) == key) return element(set, i, stride);
        return nullptr;
    }

    auto fname_map_add(void* map, size_t stride, const FName& key, const std::function<void(uint8*)>& init, StringType* why) -> uint8*
    {
        if (fname_map_find(map, stride, key))
        {
            *why = STR("the key is already there");
            return nullptr;
        }
        // The existing links must be the ones fname_hash gives: otherwise the layout or the hash isn't 4.24's.
        const int32 size = field<int32>(map, kHashSize), num = field<int32>(map, kNum);
        int32 checked = 0;
        for (int32 i = 0; i < num && checked < 16 && size > 0; ++i)
        {
            if (!allocated(map, i)) continue;
            uint8* e = element(map, i, stride);
            if (hash_index(e, stride) != static_cast<int32>(fname_hash(*reinterpret_cast<FName*>(e)) & static_cast<uint32>(size - 1)))
            {
                *why = std::format(STR("element {}'s hash link doesn't match the expected layout (element size {})"), i, stride);
                return nullptr;
            }
            ++checked;
        }
        const int32 index = add_slot(map, stride);
        uint8* e = element(map, index, stride);
        std::memset(e, 0, stride);
        *reinterpret_cast<FName*>(e) = key;
        init(e + 8);
        const int32 elements = field<int32>(map, kNum) - field<int32>(map, kNumFree);
        const int32 wanted = bucket_count(elements);
        if (size == 0 || wanted > size)
            rehash(map, stride, wanted);
        else
        {
            int32* h = buckets(map);
            const int32 b = static_cast<int32>(fname_hash(key) & static_cast<uint32>(size - 1));
            hash_index(e, stride) = b;
            hash_next(e, stride) = h[b];
            h[b] = index;
        }
        return e + 8;
    }

    auto copy_struct(UStruct* st, uint8* dst, const uint8* src) -> void
    {
        const int32 size = st->GetPropertiesSize();
        std::memset(dst, 0, static_cast<size_t>(size));
        int32 first = size;
        for (FProperty* p : TFieldRange<FProperty>(st, EFieldIterationFlags::IncludeSuper)) first = (std::min)(first, p->GetOffset_Internal());
        std::memcpy(dst, src, static_cast<size_t>(first));
        for (FProperty* p : TFieldRange<FProperty>(st, EFieldIterationFlags::IncludeSuper))
        {
            p->InitializeValue(dst + p->GetOffset_Internal());
            p->CopyCompleteValue(dst + p->GetOffset_Internal(), src + p->GetOffset_Internal());
        }
    }
} // namespace data_table

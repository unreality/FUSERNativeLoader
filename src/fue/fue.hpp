#pragma once
// fue - a minimal UE 4.24 reflection core for FUSER, built into winhttp.dll (FUSERNativeLoader).
//
// It provides the subset of UE4SS's API that FuserNative uses, with the same type and member names (FuserNative
// began as a UE4SS C++ mod). Every engine type here is a *view*: no data
// members, only accessors over engine memory at the offsets UE4SS itself used on FUSER (UEPseudo's stock 4.24 member
// layouts; the vtable slots as UE4SS computed them for FUSER). Addresses are RVAs per
// game build (fue.cpp kBuilds: EGS v1.4.2; others via tools/derive_build.py), selected by the exe's PE identity.
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#define STR(x) L##x

namespace fue
{
    using CharType = wchar_t;
    using StringType = std::wstring;
    using TCHAR = wchar_t;
    using int8 = int8_t;
    using uint8 = uint8_t;
    using int16 = int16_t;
    using uint16 = uint16_t;
    using int32 = int32_t;
    using uint32 = uint32_t;
    using int64 = int64_t;
    using uint64 = uint64_t;

    auto to_wstring(const char* s) -> StringType;
    inline auto to_wstring(const std::string& s) -> StringType { return to_wstring(s.c_str()); }

    // ------------------------------------------------------------------ logging -----------------
    enum class LogLevel { Verbose, Normal, Warning, Error };
    namespace Output
    {
        auto write(LogLevel level, const StringType& text) -> void;
        template <LogLevel L, typename... Args>
        auto send(std::wformat_string<Args...> f, Args&&... args) -> void
        {
            write(L, std::format(f, std::forward<Args>(args)...));
        }
    } // namespace Output
    namespace fmt
    {
        using std::format;
    }

    // ------------------------------------------------------------------ core lifecycle ----------
    // init() (DllMain): verify the exe, resolve addresses, install the ProcessEvent / GameEngine::Tick detours.
    // The first GameEngine::Tick runs the registered "unreal init" callbacks (UE4SS's on_unreal_init).
    auto init(void* self_module) -> bool;
    auto ready() -> bool;
    auto build_name() -> const CharType*; // the matched kBuilds entry, e.g. "EGS v1.4.2"
    auto on_unreal_init(std::function<void()> callback) -> void;
    auto module_dir() -> std::wstring; // the folder holding winhttp.dll
    auto exe_dir() -> std::wstring;

    // ------------------------------------------------------------------ memory (GMalloc) --------
    namespace FMemory
    {
        auto Malloc(size_t count, uint32 alignment = 0) -> void*;
        auto Realloc(void* original, size_t count, uint32 alignment = 0) -> void*;
        auto Free(void* original) -> void;
    } // namespace FMemory

    // ------------------------------------------------------------------ containers --------------
    template <typename T>
    struct TArray
    {
        T* Data = nullptr;
        int32 ArrayNum = 0;
        int32 ArrayMax = 0;

        auto Num() const -> int32 { return ArrayNum; }
        auto GetData() const -> T* { return Data; }
        auto operator[](int32 i) -> T& { return Data[i]; }
        auto operator[](int32 i) const -> const T& { return Data[i]; }
        auto Add(const T& v) -> int32
        {
            if (ArrayNum == ArrayMax)
            {
                ArrayMax = ArrayMax < 4 ? 4 : ArrayMax * 2;
                Data = static_cast<T*>(FMemory::Realloc(Data, sizeof(T) * static_cast<size_t>(ArrayMax)));
            }
            std::memcpy(static_cast<void*>(&Data[ArrayNum]), &v, sizeof(T)); // trivially-copyable element types only
            return ArrayNum++;
        }
        auto RemoveAt(int32 index, int32 count = 1, bool /*allow_shrinking*/ = true) -> void
        {
            if (count <= 0) return;
            std::memmove(static_cast<void*>(&Data[index]), &Data[index + count], sizeof(T) * static_cast<size_t>(ArrayNum - index - count));
            ArrayNum -= count;
        }
    };
    static_assert(sizeof(TArray<void*>) == 0x10);

    // FString = TArray<TCHAR> with a terminating 0; memory from GMalloc (freed by the engine's own destructor when
    // copied into engine storage via a property, or by ours).
    struct FString
    {
        TArray<TCHAR> Chars{};
        FString() = default;
        explicit FString(const TCHAR* s);
        FString(const FString&) = delete;
        FString& operator=(const FString&) = delete;
        ~FString();
        auto operator*() const -> const TCHAR* { return Chars.Data && Chars.ArrayNum ? Chars.Data : L""; }
    };
    static_assert(sizeof(FString) == 0x10);

    // ------------------------------------------------------------------ names -------------------
    enum EFindName : int32 { FNAME_Find = 0, FNAME_Add = 1 };
    struct FName
    {
        uint32 ComparisonIndex = 0;
        uint32 Number = 0;
        FName() = default;
        FName(const CharType* name, EFindName find = FNAME_Add);
        auto ToString() const -> StringType;
        auto operator==(const FName& o) const -> bool { return ComparisonIndex == o.ComparisonIndex && Number == o.Number; }
        auto operator!=(const FName& o) const -> bool { return !(*this == o); }
        auto Equals(const FName& o) const -> bool { return *this == o; }
        auto IsNone() const -> bool { return ComparisonIndex == 0 && Number == 0; }
    };
    static_assert(sizeof(FName) == 8);
    inline const FName NAME_None{};

    // ------------------------------------------------------------------ flags -------------------
    enum EObjectFlags : uint32 { RF_NoFlags = 0 };
    constexpr uint64 CPF_Edit = 0x1, CPF_BlueprintVisible = 0x4, CPF_Parm = 0x80, CPF_OutParm = 0x100, CPF_ZeroConstructor = 0x200, CPF_ReturnParm = 0x400,
                     CPF_Transient = 0x2000, CPF_IsPlainOldData = 0x40000000, CPF_NoDestructor = 0x1000000000ull;
    constexpr uint32 FUNC_Native = 0x400;
    enum class EFieldIterationFlags : uint8 { None = 0, IncludeSuper = 1, IncludeDeprecated = 2, IncludeInterfaces = 4, Default = 3 };
    constexpr auto operator|(EFieldIterationFlags a, EFieldIterationFlags b) -> EFieldIterationFlags
    {
        return static_cast<EFieldIterationFlags>(static_cast<uint8>(a) | static_cast<uint8>(b));
    }
    constexpr auto has(EFieldIterationFlags a, EFieldIterationFlags b) -> bool { return (static_cast<uint8>(a) & static_cast<uint8>(b)) != 0; }

    // ------------------------------------------------------------------ objects (views) ---------
    class UClass;
    class UFunction;
    class UStruct;

    namespace detail
    {
        template <typename T>
        inline auto at(const void* base, size_t offset) -> T&
        {
            return *reinterpret_cast<T*>(const_cast<uint8*>(static_cast<const uint8*>(base)) + offset);
        }
    } // namespace detail

    class UObject
    {
      public:
        UObject() = delete;
        auto GetClassPrivate() const -> UClass* { return detail::at<UClass*>(this, 0x10); }
        auto GetOuterPrivate() const -> UObject* { return detail::at<UObject*>(this, 0x20); }
        auto GetFName() const -> FName { return detail::at<FName>(this, 0x18); }
        auto GetInternalIndex() const -> int32 { return detail::at<int32>(this, 0xC); }
        auto GetName() const -> StringType { return GetFName().ToString(); }
        auto GetPathName() const -> StringType;
        auto GetFullName() const -> StringType;
        auto IsA(const UClass* cls) const -> bool;
        auto IsRootSet() const -> bool;
        auto SetRootSet() -> void;
        auto ProcessEvent(UFunction* fn, void* parms) -> void;
        auto GetPropertyByNameInChain(const CharType* name) const -> class FProperty*;
        auto GetValuePtrByPropertyNameInChain(const CharType* name) -> void*;
        template <typename T>
        auto GetValuePtrByPropertyNameInChain(const CharType* name) -> T*
        {
            return static_cast<T*>(GetValuePtrByPropertyNameInChain(name));
        }
        auto GetFunctionByNameInChain(FName name) const -> UFunction*;
        auto GetFunctionByNameInChain(const CharType* name) const -> UFunction* { return GetFunctionByNameInChain(FName(name, FNAME_Add)); }
    };

    class UField : public UObject
    {
      public:
        auto GetNext() const -> UField* { return detail::at<UField*>(this, 0x28); }
    };

    class UStruct : public UField
    {
      public:
        auto GetSuperStruct() const -> UStruct* { return detail::at<UStruct*>(this, 0x40); }
        auto GetChildren() const -> UField* { return detail::at<UField*>(this, 0x48); }
        auto GetPropertiesSize() const -> int32 { return detail::at<int32>(this, 0x50); }
        auto FindProperty(FName name) const -> class FProperty*; // uncached walk incl. super
    };

    class UScriptStruct : public UStruct
    {
    };

    class UClass : public UStruct
    {
      public:
        auto GetClassDefaultObject() const -> UObject* { return detail::at<UObject*>(this, 0xF8); }
    };

    class FFrame;
    using UnrealScriptFunction = void (*)(UObject* Context, FFrame& TheStack, void* RESULT_DECL);

    class UFunction : public UStruct
    {
      public:
        auto GetFunctionFlags() -> uint32& { return detail::at<uint32>(this, 0x98); }
        auto GetFuncPtr() const -> UnrealScriptFunction { return detail::at<UnrealScriptFunction>(this, 0xC0); }
        auto SetFuncPtr(UnrealScriptFunction f) -> void { detail::at<UnrealScriptFunction>(this, 0xC0) = f; }
    };

    class UEngine : public UObject
    {
    };

    // ------------------------------------------------------------------ properties (4.24 UProperty) --
    struct FFieldClassRef // what UE4SS's FField::GetClass() gives: the property's class, for its name
    {
        UClass* cls = nullptr;
        auto GetFName() const -> FName { return cls ? cls->GetFName() : FName{}; }
    };

    class FProperty : public UField
    {
      public:
        auto GetArrayDim() const -> int32 { return detail::at<int32>(this, 0x30); }
        auto GetElementSize() const -> int32 { return detail::at<int32>(this, 0x34); }
        auto GetPropertyFlags() const -> uint64 { return detail::at<uint64>(this, 0x38); }
        auto GetOffset_Internal() const -> int32 { return detail::at<int32>(this, 0x44); }
        auto GetSize() const -> int32 { return GetElementSize() * GetArrayDim(); }
        auto HasAnyPropertyFlags(uint64 f) const -> bool { return (GetPropertyFlags() & f) != 0; }
        auto GetClass() const -> FFieldClassRef { return {GetClassPrivate()}; }

        template <typename T = void>
        auto ContainerPtrToValuePtr(void* container, int32 index = 0) const -> T*
        {
            return reinterpret_cast<T*>(static_cast<uint8*>(container) + GetOffset_Internal() + index * GetElementSize());
        }
        template <typename T = void>
        auto ContainerPtrToValuePtr(const void* container, int32 index = 0) const -> T*
        {
            return ContainerPtrToValuePtr<T>(const_cast<void*>(container), index);
        }

        // The value ops of 4.24's UProperty (its non-virtual wrappers over the Hype-shifted virtuals)
        auto InitializeValue(void* dest) const -> void;
        auto DestroyValue(void* dest) const -> void;
        auto CopyCompleteValue(void* dest, const void* src) const -> void;
        auto InitializeValue_InContainer(void* container) const -> void { InitializeValue(ContainerPtrToValuePtr(container)); }
        auto DestroyValue_InContainer(void* container) const -> void { DestroyValue(ContainerPtrToValuePtr(container)); }
    };

    class FBoolProperty : public FProperty
    {
      public:
        static constexpr const CharType* StaticClassName = STR("BoolProperty");
        auto GetPropertyValue(const void* a) const -> bool { return (static_cast<const uint8*>(a)[detail::at<uint8>(this, 0x71)] & detail::at<uint8>(this, 0x73)) != 0; }
        auto SetPropertyValue(void* a, bool v) const -> void
        {
            uint8& b = static_cast<uint8*>(a)[detail::at<uint8>(this, 0x71)];
            b = static_cast<uint8>((b & ~detail::at<uint8>(this, 0x73)) | (v ? detail::at<uint8>(this, 0x72) : 0));
        }
    };

    struct FScriptStructRef // UE4SS's FStructProperty::GetStruct() returns a pointer wrapper with Get()
    {
        UScriptStruct* p = nullptr;
        auto Get() const -> UScriptStruct* { return p; }
    };

    class FStructProperty : public FProperty
    {
      public:
        static constexpr const CharType* StaticClassName = STR("StructProperty");
        auto GetStruct() const -> FScriptStructRef { return {detail::at<UScriptStruct*>(this, 0x70)}; }
    };

    class FArrayProperty : public FProperty
    {
      public:
        static constexpr const CharType* StaticClassName = STR("ArrayProperty");
        auto GetInner() const -> FProperty* { return detail::at<FProperty*>(this, 0x70); }
    };

    auto is_property(const UObject* field) -> bool;
    auto class_is(const UObject* obj, const CharType* class_name) -> bool; // exact class name

    template <typename T>
    inline auto CastField(FProperty* p) -> T*
    {
        return p && class_is(p, T::StaticClassName) ? static_cast<T*>(p) : nullptr;
    }

    // TFieldRange<FProperty>(struct, flags): the struct's properties in declaration order, then its supers' (with
    // IncludeSuper). Collected up front: the fields of a live struct don't change while we walk them.
    template <typename T>
    class TFieldRange
    {
      public:
        TFieldRange(const UStruct* st, EFieldIterationFlags flags = EFieldIterationFlags::Default)
        {
            for (const UStruct* s = st; s; s = has(flags, EFieldIterationFlags::IncludeSuper) ? s->GetSuperStruct() : nullptr)
            {
                for (UField* f = s->GetChildren(); f; f = f->GetNext())
                {
                    if (is_property(f)) m_items.push_back(static_cast<T*>(f));
                }
            }
        }
        auto begin() const { return m_items.begin(); }
        auto end() const { return m_items.end(); }

      private:
        std::vector<T*> m_items{};
    };

    // ------------------------------------------------------------------ text ----------------------
    // An engine FText (0x18 bytes: TSharedRef<ITextData> + flags) held by us: created and destroyed through a
    // TextProperty, converted through KismetTextLibrary (as UE4SS does). Game thread only.
    class FText
    {
      public:
        FText();
        explicit FText(const CharType* s);
        explicit FText(const StringType& s) : FText(s.c_str()) {}
        FText(const FText&) = delete;
        FText& operator=(const FText&) = delete;
        ~FText();
        auto ToString() const -> StringType;

      private:
        alignas(8) uint8 m_storage[0x18]{};
    };
    static_assert(sizeof(FText) == 0x18);

    // ------------------------------------------------------------------ weak pointers -------------
    struct FWeakObjectPtr
    {
        int32 ObjectIndex = -1;
        int32 ObjectSerialNumber = 0;
        FWeakObjectPtr() = default;
        explicit FWeakObjectPtr(const UObject* o);
        auto Get() const -> UObject*;
    };

    // ------------------------------------------------------------------ script VM -----------------
    struct FOutParmRec
    {
        FProperty* Property;
        uint8* PropAddr;
        FOutParmRec* NextOutParm;
    };

    // 4.24 FFrame (: FOutputDevice): Node 0x10, Object 0x18, Code 0x20, Locals 0x28, MostRecentProperty 0x30,
    // MostRecentPropertyAddress 0x38, FlowStack 0x40, PreviousFrame 0x70, OutParms 0x78, PropertyChainForCompiledIn
    // 0x80, CurrentNativeFunction 0x88 (Node / Locals / OutParms as FUSER's ProcessInternal reads them).
    class FFrame
    {
      public:
        FFrame() = delete;
        auto Node() const -> UFunction* { return detail::at<UFunction*>(this, 0x10); }
        auto Object() const -> UObject* { return detail::at<UObject*>(this, 0x18); }
        auto Code() -> uint8*& { return detail::at<uint8*>(this, 0x20); }
        auto Locals() const -> uint8* { return detail::at<uint8*>(this, 0x28); }
        auto MostRecentPropertyAddress() -> uint8*& { return detail::at<uint8*>(this, 0x38); }
        auto PreviousFrame() const -> FFrame* { return detail::at<FFrame*>(this, 0x70); }
        auto OutParms() const -> FOutParmRec* { return detail::at<FOutParmRec*>(this, 0x78); }
        auto CurrentNativeFunction() const -> UFunction* { return detail::at<UFunction*>(this, 0x88); }
        auto Step(UObject* context, void* result) -> void; // GNatives[*Code++](context, *this, result)
    };

    // ------------------------------------------------------------------ hooks ---------------------
    // Native-hook callbacks get the frame the native was invoked with. Unlike UE4SS - which, for a call from script,
    // evaluates the parameters into a new frame (Node = the native, PreviousFrame = the caller) - fue passes it
    // through: from script it IS the caller's frame (its Node is the calling function, its Code sits after the call's
    // parameters once the original has run); from ProcessEvent it is the native's own frame. Callbacks that inspect
    // the caller must accept either.
    struct UnrealScriptFunctionCallableContext
    {
        UObject* Context;
        FFrame& TheStack;
        void* RESULT_DECL;
    };
    using UnrealScriptFunctionCallable = std::function<void(UnrealScriptFunctionCallableContext& Context, void* CustomData)>;

    namespace Hook
    {
        template <typename T>
        struct TCallbackIterationData
        {
        };
        struct FCallbackOptions
        {
            bool bOnce = false;
            bool bReadonly = false;
            StringType OwnerModName{};
            StringType HookName{};
        };
        using EngineTickCallback = std::function<void(TCallbackIterationData<void>&, UEngine*, float, bool)>;
        using ProcessEventCallback = std::function<void(TCallbackIterationData<void>&, UObject*, UFunction*, void*)>;
        auto RegisterEngineTickPreCallback(EngineTickCallback cb, FCallbackOptions opts = {}) -> void;
        auto RegisterProcessEventPreCallback(ProcessEventCallback cb, FCallbackOptions opts = {}) -> void;
        auto RegisterProcessEventPostCallback(ProcessEventCallback cb, FCallbackOptions opts = {}) -> void;
    } // namespace Hook

    // ------------------------------------------------------------------ object globals ------------
    namespace UObjectGlobals
    {
        auto StaticFindObject(UClass* cls, UObject* outer, const CharType* path) -> UObject*;
        template <typename T>
        auto StaticFindObject(UClass* cls, UObject* outer, const CharType* path) -> T
        {
            return static_cast<T>(StaticFindObject(cls, outer, path));
        }
        auto FindAllOf(const CharType* class_name, std::vector<UObject*>& out) -> void;
        auto StaticConstructObject(UClass* cls, UObject* outer, FName name, EObjectFlags flags, UObject* tmpl) -> UObject*;
        template <typename T>
        auto NewObject(UObject* outer, UClass* cls, FName name = NAME_None, EObjectFlags flags = RF_NoFlags, UObject* tmpl = nullptr) -> T*
        {
            return static_cast<T*>(StaticConstructObject(cls, outer, name, flags, tmpl));
        }
        // A native UFunction hook: Func swapped to a generic thunk that runs pre, the original, then post.
        auto RegisterHook(UFunction* fn, UnrealScriptFunctionCallable pre, UnrealScriptFunctionCallable post, void* data) -> std::pair<int, int>;
        auto object_count() -> int32; // live GUObjectArray elements (diagnostics)
    } // namespace UObjectGlobals
} // namespace fue

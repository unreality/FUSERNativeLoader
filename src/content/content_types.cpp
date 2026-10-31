// The content types: the loader's own and the ones mods register. Every unlockable type can have one: its handler
// adds the item's game rows (the unlock row is common to all, content_unlocks.cpp) and, when it can, inserts the item
// into the managers after boot. A type with a content.json but no handler is refused with its name in the log.
#include "content/content_types.hpp"

#include <algorithm>

using namespace fue;
using namespace data_table;

namespace content
{
    auto projector_handler() -> TypeHandler;   // content_projector.cpp
    auto outfit_part_handler() -> TypeHandler; // content_outfit.cpp

    namespace
    {
        auto handlers() -> std::vector<TypeHandler>&
        {
            static std::vector<TypeHandler> list = {
                projector_handler(),
                outfit_part_handler(),
            };
            return list;
        }
    } // namespace

    auto type_handlers() -> const std::vector<TypeHandler>& { return handlers(); }

    auto register_type(const TypeHandler& handler) -> bool
    {
        const StringType type = to_wstring(handler.type ? handler.type : "");
        if (type.empty() || !handler.validate || !handler.add_rows || handler.unlock_type >= UnlockType::Count)
        {
            Output::send<LogLevel::Error>(STR("[content] content type '{}': a name, validate, add_rows and an unlock type are needed - not registered\n"), type);
            return false;
        }
        if (find_handler(handler.type))
        {
            Output::send<LogLevel::Error>(STR("[content] content type '{}' exists already - not registered\n"), type);
            return false;
        }
        handlers().push_back(handler);
        Output::send<LogLevel::Normal>(STR("[content] content type '{}' registered ({} unlocks{})\n"), type, unlock_type_name(static_cast<int>(handler.unlock_type)),
                                       handler.register_live ? STR(", live") : STR(", from the next launch when added later"));
        return true;
    }

    auto find_handler(const std::string& type) -> const TypeHandler*
    {
        for (const auto& h : type_handlers())
            if (type == h.type) return &h;
        return nullptr;
    }

    namespace
    {
        // A static UFunction called on its class default object
        auto call_static(const CharType* cdo_path, Call& c) -> bool
        {
            UObject* cdo = UObjectGlobals::StaticFindObject(nullptr, nullptr, cdo_path);
            if (!cdo) return false;
            cdo->ProcessEvent(c.fn, c.buf.data());
            return true;
        }
        auto function(const CharType* path) -> UFunction* { return UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path); }
    } // namespace

    auto validate_icon_image(const Item& item) -> StringType
    {
        const std::string file = item.manifest["icon_image"].str();
        if (file.empty()) return {};
        if (file.find_first_of("/\\") != std::string::npos || !lower(file).ends_with(".png"))
            return STR("its icon_image must name a .png file in the item's folder");
        const bool present = std::any_of(item.files.begin(), item.files.end(), [&](const std::string& f) { return lower(f) == lower(file); });
        if (!present) return std::format(STR("its icon_image {} is not in the pak"), utf8_to_wide(file));
        return {};
    }

    auto set_icon_images(const Item& item, UStruct* rs, uint8* row) -> StringType
    {
        const std::string file = item.manifest["icon_image"].str();
        if (file.empty()) return {};
        UFunction* import_fn = function(STR("/Script/Engine.KismetRenderingLibrary:ImportFileAsTexture2D"));
        UFunction* soft_fn = function(STR("/Script/Engine.KismetSystemLibrary:Conv_ObjectToSoftObjectReference"));
        if (!import_fn || !soft_fn) return STR("the engine's texture import functions are missing");
        Call import(import_fn), soft(soft_fn);
        {
            // the engine's file system finds pak files by the path relative to the binaries
            const FString path(utf8_to_wide("../../../Fuser/Content/" + item.folder + file).c_str());
            import.prop(STR("Filename"))->CopyCompleteValue(import.addr(STR("Filename")), &path);
        }
        if (!call_static(STR("/Script/Engine.Default__KismetRenderingLibrary"), import)) return STR("KismetRenderingLibrary is missing");
        UObject* texture = *reinterpret_cast<UObject**>(import.addr(STR("ReturnValue")));
        if (!texture) return std::format(STR("the engine could not read {} as an 8-bit PNG"), utf8_to_wide(file));
        texture->SetRootSet(); // the rows point at it through soft pointers, which don't keep it alive
        *reinterpret_cast<UObject**>(soft.addr(STR("Object"))) = texture;
        if (!call_static(STR("/Script/Engine.Default__KismetSystemLibrary"), soft)) return STR("KismetSystemLibrary is missing");
        int set = 0;
        for (const CharType* name : {STR("PreviewImage"), STR("SmallRewardImage"), STR("LargePreviewImage")})
        {
            if (FProperty* p = rs->FindProperty(FName(name)))
            {
                p->CopyCompleteValue(p->ContainerPtrToValuePtr<void>(row), soft.addr(STR("ReturnValue")));
                ++set;
            }
        }
        return set ? StringType{} : STR("the row has no preview image fields");
    }
} // namespace content

// The projector content type: a looping video for the Projector A/B/C stage screens. content.json:
//   {"type": "projector", "id": "MyScreen", "name": "My Screen", "video": "MyScreen.mp4",
//    "use_video_alpha": false, "use_original_video": false, "tightness": 0.0, "icon": "ColorGlow1",
//    "icon_image": "icon.png"}
// The video gets a UFileMediaSource ("./CustomContent/<id>/<video>", PrecacheFile so it is read through the pak file
// system) in a UMediaPlaylist, and DT_VenueCustom_ScreenPlaylist a row <id>: a copy of the `icon` row (its preview
// images; default ColorGlow1) with Playlist, UseVideoAlpha, UseOriginalVideo, Tightness and DisplayName set, and the
// preview images replaced by icon_image when there is one (set_icon_images).
// BP_Screen_Manager opens the playlist on the stage MediaPlayer like any stock screen; M_VenueScreens applies the
// same beat/VU modulation. UseOriginalVideo keeps the video's colours; without it the material recolours the video.
// UseVideoAlpha keys out the stock Corgi / BeachBalls background colour.
//
// After boot, UVenueMgr (FUN_1407ab360, at the game instance's stage 3) has copied the table into
// ScreenPlaylistCustomizationItems (TMap<FName, FVenueCustomItemScreenPackage>) and made one
// UVenueCustomizationItemWrapper per row (FUN_140799460: NewObject(VenueMgr) named VCI_<slot>_<key>, ItemSlot
// ScreenItem0, itemKey) in ScreenPackageCustomizationItemWrappers and _nameToVenueCustomizationItemWrapper. A live
// item is added to those three the same way.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "content/content_types.hpp"

#include <algorithm>
#include <optional>

using namespace fue;
using namespace data_table;

namespace content
{
    namespace
    {
        constexpr const CharType* kScreenTablePath = STR("/Game/Maps/Maps_Shared/Customization/DT_VenueCustom_ScreenPlaylist.DT_VenueCustom_ScreenPlaylist");
        constexpr const CharType* kDefaultIcon = STR("ColorGlow1"); // a stock video screen whose preview images an item borrows
        constexpr uint8 kScreenSlot = 3;                              // EVenueCustomizationSlot::ScreenItem0, as UVenueMgr files every screen

        // A static UFunction called on its class default object
        auto call_static(const CharType* cdo_path, Call& c) -> bool
        {
            UObject* cdo = UObjectGlobals::StaticFindObject(nullptr, nullptr, cdo_path);
            if (!cdo) return false;
            cdo->ProcessEvent(c.fn, c.buf.data());
            return true;
        }

        // A Content-relative path as the engine's file system sees it (../../../Fuser/Content/<path>, the form the pak
        // platform file matches against a pak's mount point). False when the pak holding it is not mounted.
        auto engine_file_exists(const StringType& content_relative) -> std::optional<bool>
        {
            auto* exists_fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.BlueprintPathsLibrary:FileExists"));
            if (!exists_fn) return std::nullopt;
            Call exists(exists_fn);
            {
                const FString path((STR("../../../Fuser/Content/") + content_relative).c_str());
                exists.prop(STR("InPath"))->CopyCompleteValue(exists.addr(STR("InPath")), &path);
            }
            if (!call_static(STR("/Script/Engine.Default__BlueprintPathsLibrary"), exists)) return std::nullopt;
            return *reinterpret_cast<bool*>(exists.addr(STR("ReturnValue")));
        }

        // A rooted UFileMediaSource for ./<video> (PrecacheFile: the whole file is read through the engine's file
        // system, which serves it from the mounted pak) in a rooted one-item UMediaPlaylist
        auto make_playlist(const StringType& id, const StringType& video, UObject* outer) -> UObject*
        {
            auto* source_cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/MediaAssets.FileMediaSource"));
            auto* playlist_cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/MediaAssets.MediaPlaylist"));
            auto* set_path = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/MediaAssets.FileMediaSource:SetFilePath"));
            auto* add = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/MediaAssets.MediaPlaylist:Add"));
            if (!source_cls || !playlist_cls || !set_path || !add) return nullptr;

            UObject* source = UObjectGlobals::NewObject<UObject>(outer, source_cls, FName((STR("FNL_ProjectorVideo_") + id).c_str()));
            UObject* playlist = UObjectGlobals::NewObject<UObject>(outer, playlist_cls, FName((STR("FNL_ProjectorPlaylist_") + id).c_str()));
            if (!source || !playlist) return nullptr;
            source->SetRootSet();
            playlist->SetRootSet();
            {
                Call c(set_path);
                const FString path((STR("./") + video).c_str());
                c.prop(STR("Path"))->CopyCompleteValue(c.addr(STR("Path")), &path);
                source->ProcessEvent(set_path, c.buf.data());
            }
            if (auto* precache = CastField<FBoolProperty>(source->GetPropertyByNameInChain(STR("PrecacheFile"))))
                precache->SetPropertyValue(precache->ContainerPtrToValuePtr<void>(source), true);
            {
                Call c(add);
                *reinterpret_cast<UObject**>(c.addr(STR("MediaSource"))) = source;
                playlist->ProcessEvent(add, c.buf.data());
                if (!*reinterpret_cast<bool*>(c.addr(STR("ReturnValue")))) return nullptr;
            }
            return playlist;
        }

        auto set_bool(UStruct* rs, uint8* row, const CharType* name, bool v) -> void
        {
            if (auto* p = CastField<FBoolProperty>(rs->FindProperty(FName(name)))) p->SetPropertyValue(p->ContainerPtrToValuePtr<void>(row), v);
        }

        auto screen_table(UStruct** rs) -> UObject*
        {
            UObject* table = UObjectGlobals::StaticFindObject(nullptr, nullptr, kScreenTablePath);
            if (!table) table = load_blocking(kScreenTablePath);
            *rs = row_struct(table);
            if (!*rs || (*rs)->GetName() != STR("VenueCustomItemScreenPackage") || !can_add_rows(table)) return nullptr;
            table->SetRootSet(); // UVenueMgr loads it through the same soft path and gets this object
            return table;
        }

        auto video_path(const Item& item) -> std::string { return item.folder + item.manifest["video"].str(); }

        auto validate(const Item& item) -> StringType
        {
            const std::string video = item.manifest["video"].str();
            if (video.empty() || video.find_first_of("/\\") != std::string::npos) return STR("its content.json names no video file in the item's folder");
            const bool present = std::any_of(item.files.begin(), item.files.end(), [&](const std::string& f) { return lower(f) == lower(video); });
            if (!present) return std::format(STR("its video {} is not in the pak"), utf8_to_wide(video));
            return {};
        }

        auto add_rows(const Item& item) -> StringType
        {
            UStruct* rs = nullptr;
            UObject* screens = screen_table(&rs);
            if (!screens) return STR("DT_VenueCustom_ScreenPlaylist is not a table of FVenueCustomItemScreenPackage");
            FProperty* p_playlist = rs->FindProperty(FName(STR("Playlist")));
            FProperty* p_tightness = rs->FindProperty(FName(STR("Tightness")));
            FProperty* p_display = rs->FindProperty(FName(STR("DisplayName")));
            if (!p_playlist || !p_tightness || !p_display) return STR("a screen row field is missing");
            const FName id(item.info.id.c_str());
            if (find_row(screens, id)) return STR("a screen row of that name already exists (stock or another pak)");

            const StringType icon = utf8_to_wide(item.manifest["icon"].str());
            const uint8* tmpl = icon.empty() ? nullptr : find_row(screens, FName(icon.c_str()));
            if (!icon.empty() && !tmpl)
                Output::send<LogLevel::Warning>(STR("[content] {}: icon row '{}' does not exist - using {}\n"), item.pak, icon, kDefaultIcon);
            if (!tmpl) tmpl = find_row(screens, FName(kDefaultIcon));
            if (!tmpl) return STR("the default icon row is missing");

            UObject* outer = UObjectGlobals::StaticFindObject(nullptr, nullptr, STR("/Engine/Transient"));
            const StringType video = utf8_to_wide(video_path(item));
            UObject* playlist = make_playlist(item.info.id, video, outer ? outer : screens);
            if (!playlist) return STR("the media objects could not be created");
            uint8* row = add_row(screens, id, tmpl);
            if (!row) return STR("the screen row could not be added");
            *p_playlist->ContainerPtrToValuePtr<UObject*>(row) = playlist;
            set_bool(rs, row, STR("UseVideoAlpha"), item.manifest["use_video_alpha"].boolean());
            set_bool(rs, row, STR("UseOriginalVideo"), item.manifest["use_original_video"].boolean());
            set_bool(rs, row, STR("IsNoneItem"), false);
            *p_tightness->ContainerPtrToValuePtr<float>(row) = static_cast<float>(item.manifest["tightness"].num());
            {
                const FText name(item.info.name);
                p_display->CopyCompleteValue(p_display->ContainerPtrToValuePtr<void>(row), &name);
            }
            if (StringType err = set_icon_images(item, rs, row); !err.empty())
                Output::send<LogLevel::Warning>(STR("[content] {}: '{}' icon_image not used ({}) - showing {}'s images\n"), item.pak, item.info.id, err,
                                                icon.empty() ? StringType(kDefaultIcon) : icon);
            if (const auto seen = engine_file_exists(video); seen && !*seen)
                Output::send<LogLevel::Error>(STR("[content] {}: the engine cannot see {} - is the pak mounted (its .sig present)?\n"), item.pak, video);
            return {};
        }

        auto register_live(const Item& item) -> StringType
        {
            UObject* mgr = find_live_object(STR("VenueMgr"));
            if (!mgr) return STR("no UVenueMgr");
            UStruct* rs = nullptr;
            UObject* screens = screen_table(&rs);
            const FName id(item.info.id.c_str());
            uint8* row = screens ? find_row(screens, id) : nullptr;
            if (!row) return STR("its screen row is missing");
            void* items = mgr->GetValuePtrByPropertyNameInChain(STR("ScreenPlaylistCustomizationItems"));
            auto* wrappers = mgr->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("ScreenPackageCustomizationItemWrappers"));
            void* by_name = mgr->GetValuePtrByPropertyNameInChain(STR("_nameToVenueCustomizationItemWrapper"));
            auto* wrapper_cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Hype.VenueCustomizationItemWrapper"));
            if (!items || !wrappers || !by_name || !wrapper_cls) return STR("UVenueMgr's screen lists were not found");

            const size_t item_stride = fname_map_stride(static_cast<size_t>(rs->GetPropertiesSize()));
            constexpr size_t kWrapperStride = fname_map_stride(sizeof(void*));
            if (fname_map_find(items, item_stride, id)) return {}; // already there
            StringType err{};
            if (!fname_map_add(items, item_stride, id, [&](uint8* v) { copy_struct(rs, v, row); }, &err)) return STR("UVenueMgr's screen map: ") + err;

            UObject* wrapper = UObjectGlobals::NewObject<UObject>(mgr, wrapper_cls, FName(std::format(STR("VCI_{}_{}"), kScreenSlot, item.info.id).c_str()));
            if (!wrapper) return STR("the item wrapper could not be created");
            if (auto* slot = wrapper->GetValuePtrByPropertyNameInChain<uint8>(STR("ItemSlot"))) *slot = kScreenSlot;
            if (auto* key = wrapper->GetValuePtrByPropertyNameInChain<FName>(STR("itemKey"))) *key = id;
            wrappers->Add(wrapper);
            if (!fname_map_add(by_name, kWrapperStride, id, [&](uint8* v) { *reinterpret_cast<UObject**>(v) = wrapper; }, &err))
                return STR("UVenueMgr's wrapper map: ") + err;
            return {};
        }
    } // namespace

    auto projector_handler() -> TypeHandler { return {"projector", UnlockType::VenueItem, &validate, &add_rows, &register_live}; }
} // namespace content

// The outfit part content type: a new character part (a hat, glasses, ...) whose mesh is a cooked static mesh in the
// pak. content.json:
//   {"type": "outfit_part", "id": "PumpkinLantern", "name": "Pumpkin Lantern", "mesh": "SM_Hat_PumpkinLantern",
//    "part_template": "/Game/Character/Outfit/Hat/Baseball/OP_Hat_Baseball",
//    "variant_template": "/Game/Character/Outfit/Hat/Baseball/MV_Hat_Baseball_Palettet", "icon_image": "icon.png"}
// The mesh is /Game/CustomContent/<id>/<mesh> (<mesh>.uasset + .uexp in the item's folder). Its material slots must
// be named as the template's meshes name theirs: the template's OutfitMaterialSlot (e.g. MI_Hat_Baseball) and, when
// the template sets a HairMaterialSlot, that slot too - the character's hair material is set on HairMaterialSlot,
// and a one-slot mesh without it gets the hair material on its only slot.
//
// The item is a UOutfitPart OP_<id> made from part_template and one UOutfitMaterialVariant <id> made from
// variant_template (NewObject with the stock asset as its template, so every field starts as the stock one's). The
// part gets the name, the icon_image (PreviewImage / SmallRewardImage / LargePreviewImage), the mesh in DefaultMesh
// and every HairMeshes / BodyMeshes / HeadMeshes entry, and the variant as its only OutfitMaterialVariants entry. Each
// object lives in a package of its own, /Game/CustomContent/<id>/OP_<id> and /Game/CustomContent/<id>/<id>: avatar
// configs (FAvatarConfig, saved and replicated) hold parts and variants as soft object paths, which resolve to these
// objects on every client that has the item. The unlock row is the variant's (DT_UnlocksOutfitPartVariants, keyed
// by variant name), so it is named <id>.
//
// UOutfitPartMgr (EGS v1.4.2 FUN_1405d82d0, the game instance's stage 1, before UnlockMgr) asks the asset registry for
// every UOutfitPart, which in a cooked game lists only the game's own. For each part it fills _OutfitPartsBySlot,
// _OutfitPartsByName and, per variant, _OutfitPartVariantsByName, one UOutfitVariantColorPalettePair per palette
// (FUN_1405ccdd0: NewObject(mgr) "<variant>_palette<i>", MaterialVariant, HasPalette and PaletteRow when the variant
// has a ColorPaletteSampler, DisplayName = ColorPaletteNames[i], or the variant's when that is empty) in
// _VariantPalettePairsByVariantName and, all of a part's, in _VariantPalettePairsByOutfitPartName, then links each
// variant to its part (FUN_1405e1aa0: OutfitPart, and shortName = the variant name after its last '_'). Each slot's
// list is sorted by SortPriority (highest first), then DisplayName. The loader does the same for its parts once
// that init has run: on the first engine tick after it, or at once for an item registered while the game runs.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "content/content_types.hpp"

#include <algorithm>
#include <map>
#include <mutex>

using namespace fue;
using namespace data_table;

namespace content
{
    namespace
    {
        constexpr uint32 kPublicStandalone = 0x1 | 0x2; // RF_Public | RF_Standalone
        constexpr size_t kSoftPtrSize = 0x28;          // TSoftObjectPtr: FWeakObjectPtr, tag, FSoftObjectPath
        constexpr size_t kSoftPathName = 0x10;         // FSoftObjectPath::AssetPathName in a TSoftObjectPtr
        // TMap<K, TSoftObjectPtr> with a key of at most 8 bytes (EHairLengthCategory, ECharacterBodyType, UOutfitPart*)
        constexpr size_t kSoftMapStride = 8 + kSoftPtrSize + 8;

        struct Built
        {
            Item item;
            UObject* part = nullptr;
            UObject* variant = nullptr;
            bool in_mgr = false;
        };
        std::map<std::string, Built> g_built{}; // by lower-case id; game thread
        bool g_tick_registered = false;

        auto add_pending_to_manager() -> void;

        auto cls(const CharType* path) -> UClass* { return UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, path); }
        auto function(const CharType* path) -> UFunction* { return UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path); }

        // "/Game/A/B" -> "/Game/A/B.B"
        auto object_path(const std::string& package) -> StringType
        {
            const std::string leaf = package.substr(package.find_last_of('/') + 1);
            return utf8_to_wide(package + "." + leaf);
        }

        template <typename T>
        auto field(UObject* o, const CharType* name) -> T* { return o->GetValuePtrByPropertyNameInChain<T>(name); }

        auto set_bool(UObject* o, const CharType* name, bool v) -> void
        {
            if (auto* p = CastField<FBoolProperty>(o->GetPropertyByNameInChain(name))) p->SetPropertyValue(p->ContainerPtrToValuePtr<void>(o), v);
        }

        // The text of an FText held by the engine (read only)
        auto text_of(const void* engine_text) -> StringType { return reinterpret_cast<const FText*>(engine_text)->ToString(); }

        // A soft object reference to `path` (not loaded), copied into `dst` through `soft_prop` (a TSoftObjectPtr property)
        auto set_soft(FProperty* soft_prop, void* dst, const StringType& path) -> bool
        {
            UObject* ksl = UObjectGlobals::StaticFindObject(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
            UFunction* make = function(STR("/Script/Engine.KismetSystemLibrary:MakeSoftObjectPath"));
            UFunction* conv = function(STR("/Script/Engine.KismetSystemLibrary:Conv_SoftObjPathToSoftObjRef"));
            if (!ksl || !make || !conv || soft_prop->GetSize() != static_cast<int32>(kSoftPtrSize)) return false;
            Call c1(make), c2(conv);
            {
                const FString s(path.c_str());
                c1.prop(STR("PathString"))->CopyCompleteValue(c1.addr(STR("PathString")), &s);
            }
            ksl->ProcessEvent(make, c1.buf.data());
            c2.prop(STR("SoftObjectPath"))->CopyCompleteValue(c2.addr(STR("SoftObjectPath")), c1.addr(STR("ReturnValue")));
            ksl->ProcessEvent(conv, c2.buf.data());
            soft_prop->CopyCompleteValue(dst, c2.addr(STR("ReturnValue")));
            return true;
        }

        // Every value of a TMap<K, TSoftObjectPtr> set to `path`. The template's values are checked first: each must
        // hold a /Game/ path where the layout puts it, or nothing is changed.
        auto set_soft_map_values(UObject* o, const CharType* map_name, FProperty* soft_prop, const StringType& path) -> StringType
        {
            void* map = o->GetValuePtrByPropertyNameInChain(map_name);
            if (!map) return std::format(STR("the part has no {}"), map_name);
            bool layout_ok = true;
            each_set_element(map, kSoftMapStride, [&](uint8* e) {
                const StringType held = reinterpret_cast<const FName*>(e + 8 + kSoftPathName)->ToString();
                layout_ok = layout_ok && held.starts_with(STR("/Game/"));
            });
            if (!layout_ok) return std::format(STR("{} doesn't hold soft paths where this loader expects them"), map_name);
            bool ok = true;
            each_set_element(map, kSoftMapStride, [&](uint8* e) { ok = set_soft(soft_prop, e + 8, path) && ok; });
            return ok ? StringType{} : std::format(STR("{} could not be set"), map_name);
        }

        // A rooted object `name` made from `tmpl` in a new package `package`
        auto make_object(const StringType& package, UClass* c, const StringType& name, UObject* tmpl) -> UObject*
        {
            UClass* package_cls = cls(STR("/Script/CoreUObject.Package"));
            if (!package_cls) return nullptr;
            UObject* pkg = UObjectGlobals::StaticConstructObject(package_cls, nullptr, FName(package.c_str()), static_cast<EObjectFlags>(0x1), nullptr);
            if (!pkg) return nullptr;
            pkg->SetRootSet();
            UObject* o = UObjectGlobals::StaticConstructObject(c, pkg, FName(name.c_str()), static_cast<EObjectFlags>(kPublicStandalone), tmpl);
            if (o) o->SetRootSet();
            return o;
        }

        auto validate(const Item& item) -> StringType
        {
            const std::string mesh = item.manifest["mesh"].str();
            if (mesh.empty() || mesh.find_first_of("/\\.") != std::string::npos)
                return STR("its content.json names no mesh (the cooked asset's name, without an extension) in the item's folder");
            for (const char* ext : {".uasset", ".uexp"})
            {
                const std::string file = lower(mesh + ext);
                if (std::none_of(item.files.begin(), item.files.end(), [&](const std::string& f) { return lower(f) == file; }))
                    return std::format(STR("its mesh {}{} is not in the pak"), utf8_to_wide(mesh), utf8_to_wide(ext));
            }
            for (const char* key : {"part_template", "variant_template"})
            {
                const std::string path = item.manifest[key].str();
                if (!path.starts_with("/Game/") || path.find_first_of("\\. ") != std::string::npos)
                    return std::format(STR("its {} must be a stock /Game/ asset path"), utf8_to_wide(key));
            }
            const StringType id = item.info.id;
            if (id.size() > 61) return STR("an outfit part's id is at most 61 characters (its part is OP_<id>)");
            return {};
        }

        auto add_rows(const Item& item) -> StringType
        {
            UClass* part_cls = cls(STR("/Script/Hype.OutfitPart"));
            UClass* variant_cls = cls(STR("/Script/Hype.OutfitMaterialVariant"));
            if (!part_cls || !variant_cls) return STR("the game's outfit classes were not found");
            const StringType id = item.info.id;
            const StringType root = utf8_to_wide("/Game/" + item.folder.substr(0, item.folder.size() - 1)); // /Game/CustomContent/<id>
            const StringType part_name = STR("OP_") + id;
            if (UObjectGlobals::StaticFindObject(nullptr, nullptr, (root + STR("/") + part_name).c_str()) ||
                UObjectGlobals::StaticFindObject(nullptr, nullptr, (root + STR("/") + id).c_str()))
                return STR("its objects already exist");

            const StringType part_tmpl_path = object_path(item.manifest["part_template"].str());
            const StringType variant_tmpl_path = object_path(item.manifest["variant_template"].str());
            UObject* part_tmpl = load_blocking(part_tmpl_path.c_str());
            UObject* variant_tmpl = load_blocking(variant_tmpl_path.c_str());
            if (!part_tmpl || !part_tmpl->IsA(part_cls)) return std::format(STR("its part_template {} is not a stock outfit part"), part_tmpl_path);
            if (!variant_tmpl || !variant_tmpl->IsA(variant_cls))
                return std::format(STR("its variant_template {} is not a stock outfit material variant"), variant_tmpl_path);
            part_tmpl->SetRootSet(); // the templates stay loaded: palette pairs and menus read through the copies' references
            variant_tmpl->SetRootSet();

            // the template's own class: NewObject copies a template of the same class
            UObject* variant = make_object(root + STR("/") + id, variant_tmpl->GetClassPrivate(), id, variant_tmpl);
            UObject* part = make_object(root + STR("/") + part_name, part_tmpl->GetClassPrivate(), part_name, part_tmpl);
            if (!variant || !part) return STR("the part and variant objects could not be created");

            // ---- the part
            if (FProperty* p = part->GetPropertyByNameInChain(STR("DisplayName")))
            {
                const FText name(item.info.name);
                p->CopyCompleteValue(p->ContainerPtrToValuePtr<void>(part), &name);
            }
            set_bool(part, STR("IsNoneItem"), false);
            set_bool(part, STR("IsHiddenFromUI"), false);
            set_bool(part, STR("IsDefaultHairMeshForLengthCategory"), false);
            FProperty* default_mesh = part->GetPropertyByNameInChain(STR("DefaultMesh"));
            if (!default_mesh) return STR("the part has no DefaultMesh");
            const std::string mesh = item.manifest["mesh"].str();
            const StringType mesh_path = utf8_to_wide("/Game/" + item.folder + mesh + "." + mesh);
            if (!set_soft(default_mesh, default_mesh->ContainerPtrToValuePtr<void>(part), mesh_path)) return STR("DefaultMesh could not be set");
            for (const CharType* map : {STR("HairMeshes"), STR("BodyMeshes"), STR("HeadMeshes")})
                if (StringType err = set_soft_map_values(part, map, default_mesh, mesh_path); !err.empty()) return err;
            auto* variants = field<TArray<UObject*>>(part, STR("OutfitMaterialVariants"));
            if (!variants) return STR("the part has no OutfitMaterialVariants");
            if (variants->Num() > 0)
            {
                (*variants)[0] = variant;
                variants->ArrayNum = 1;
            }
            else
                variants->Add(variant);
            if (StringType err = set_icon_images(item, part->GetClassPrivate(), reinterpret_cast<uint8*>(part)); !err.empty())
                Output::send<LogLevel::Warning>(STR("[content] {}: '{}' icon_image not used ({}) - showing {}'s images\n"), item.pak, id, err, part_tmpl->GetName());

            // ---- the variant: linked to its part, as the manager links stock ones
            if (auto* owner = field<FWeakObjectPtr>(variant, STR("OutfitPart"))) *owner = FWeakObjectPtr(part);
            if (auto* short_name = field<FName>(variant, STR("shortName")))
            {
                const size_t us = id.find_last_of(L'_');
                *short_name = us == StringType::npos ? FName() : FName(id.substr(us + 1).c_str());
            }

            g_built[lower(narrow_id(id))] = Built{item, part, variant, false};
            if (!g_tick_registered)
            {
                g_tick_registered = true;
                Hook::RegisterEngineTickPreCallback([](Hook::TCallbackIterationData<void>&, UEngine*, float, bool) { add_pending_to_manager(); });
            }
            return {};
        }

        // ---------------------------------------------------------------- UOutfitPartMgr ---------------------
        constexpr size_t kPartMapStride = fname_map_stride(sizeof(void*));
        constexpr size_t kPairsMapStride = fname_map_stride(sizeof(TArray<UObject*>)); // FOutfitVariantColorPalettePairsList

        // The manager, once its init has filled its part map
        auto outfit_mgr() -> UObject*
        {
            UObject* mgr = find_live_object(STR("OutfitPartMgr"));
            if (!mgr) return nullptr;
            const auto* by_name = static_cast<const uint8*>(mgr->GetValuePtrByPropertyNameInChain(STR("_OutfitPartsByName")));
            if (!by_name || *reinterpret_cast<const int32*>(by_name + 8) <= *reinterpret_cast<const int32*>(by_name + 0x34)) return nullptr;
            return mgr;
        }

        // True when a goes before b in a slot list: higher SortPriority first, then DisplayName
        auto sorts_before(UObject* a, UObject* b) -> bool
        {
            const int32* pa = field<int32>(a, STR("SortPriority"));
            const int32* pb = field<int32>(b, STR("SortPriority"));
            if (pa && pb && *pa != *pb) return *pa > *pb;
            const uint8* ta = field<uint8>(a, STR("DisplayName"));
            const uint8* tb = field<uint8>(b, STR("DisplayName"));
            return ta && tb && _wcsicmp(text_of(ta).c_str(), text_of(tb).c_str()) < 0;
        }

        // The variant's palette pairs, as FUN_1405ccdd0 makes them
        auto make_pairs(UObject* mgr, UObject* variant, TArray<UObject*>& out) -> StringType
        {
            UClass* pair_cls = cls(STR("/Script/Hype.OutfitVariantColorPalettePair"));
            UFunction* count_fn = function(STR("/Script/Hype.OutfitMaterialVariant:GetNumColorPaletteOptions"));
            if (!pair_cls || !count_fn) return STR("the palette pair class was not found");
            int32 count = 1;
            {
                Call c(count_fn);
                variant->ProcessEvent(count_fn, c.buf.data());
                count = std::max(1, *reinterpret_cast<int32*>(c.addr(STR("ReturnValue"))));
            }
            auto* sampler = field<UObject*>(variant, STR("ColorPaletteSampler"));
            const bool has_sampler = sampler && *sampler;
            auto* names = field<TArray<uint8>>(variant, STR("ColorPaletteNames"));
            FProperty* pair_text = pair_cls->FindProperty(FName(STR("DisplayName")));
            FProperty* pair_variant = pair_cls->FindProperty(FName(STR("MaterialVariant")));
            FProperty* pair_row = pair_cls->FindProperty(FName(STR("PaletteRow")));
            FProperty* variant_text = variant->GetPropertyByNameInChain(STR("DisplayName"));
            if (!pair_text || !pair_variant || !pair_row || !variant_text) return STR("a palette pair field is missing");
            for (int32 i = 0; i < count; ++i)
            {
                UObject* pair = UObjectGlobals::NewObject<UObject>(mgr, pair_cls, FName(std::format(STR("{}_palette{}"), variant->GetName(), i).c_str()));
                if (!pair) return STR("a palette pair could not be created");
                *pair_variant->ContainerPtrToValuePtr<UObject*>(pair) = variant;
                void* text = pair_text->ContainerPtrToValuePtr<void>(pair);
                if (has_sampler)
                {
                    set_bool(pair, STR("HasPalette"), true);
                    *pair_row->ContainerPtrToValuePtr<int32>(pair) = i;
                    if (names && i < names->Num()) pair_text->CopyCompleteValue(text, names->GetData() + static_cast<size_t>(i) * 0x18);
                }
                if (text_of(text).empty()) pair_text->CopyCompleteValue(text, variant_text->ContainerPtrToValuePtr<void>(variant));
                out.Add(pair);
            }
            return {};
        }

        auto add_to_manager(UObject* mgr, Built& b) -> StringType
        {
            UObject* part = b.part;
            UObject* variant = b.variant;
            auto* by_slot = field<TArray<TArray<UObject*>>>(mgr, STR("_OutfitPartsBySlot"));
            void* by_name = mgr->GetValuePtrByPropertyNameInChain(STR("_OutfitPartsByName"));
            void* variants_by_name = mgr->GetValuePtrByPropertyNameInChain(STR("_OutfitPartVariantsByName"));
            void* pairs_by_part = mgr->GetValuePtrByPropertyNameInChain(STR("_VariantPalettePairsByOutfitPartName"));
            void* pairs_by_variant = mgr->GetValuePtrByPropertyNameInChain(STR("_VariantPalettePairsByVariantName"));
            const uint8* slot = field<uint8>(part, STR("slot"));
            if (!by_slot || !by_name || !variants_by_name || !pairs_by_part || !pairs_by_variant || !slot) return STR("UOutfitPartMgr's lists were not found");
            if (*slot >= by_slot->Num()) return std::format(STR("the part's slot {} has no list"), *slot);
            const FName part_name = part->GetFName(), variant_name = variant->GetFName();
            if (fname_map_find(by_name, kPartMapStride, part_name)) return {}; // already there

            TArray<UObject*> pairs{};
            if (StringType err = make_pairs(mgr, variant, pairs); !err.empty()) return err;
            StringType err{};
            if (!fname_map_add(by_name, kPartMapStride, part_name, [&](uint8* v) { *reinterpret_cast<UObject**>(v) = part; }, &err))
                return STR("the part map: ") + err;
            if (!fname_map_add(variants_by_name, kPartMapStride, variant_name, [&](uint8* v) { *reinterpret_cast<UObject**>(v) = variant; }, &err))
                return STR("the variant map: ") + err;
            for (void* map : {pairs_by_variant, pairs_by_part})
            {
                const FName key = map == pairs_by_part ? part_name : variant_name;
                if (!fname_map_add(map, kPairsMapStride, key, [&](uint8* v) {
                        auto* list = reinterpret_cast<TArray<UObject*>*>(v);
                        for (int32 i = 0; i < pairs.Num(); ++i) list->Add(pairs[i]);
                    }, &err))
                    return STR("a palette pair map: ") + err;
            }
            FMemory::Free(pairs.Data);

            TArray<UObject*>& list = (*by_slot)[*slot];
            int32 at = 0;
            while (at < list.Num() && !sorts_before(part, list[at])) ++at;
            list.Add(part); // grows the list; then the part moves to its place
            std::memmove(&list[at + 1], &list[at], sizeof(UObject*) * static_cast<size_t>(list.Num() - 1 - at));
            list[at] = part;
            return {};
        }

        auto add_pending_to_manager() -> void
        {
            bool pending = false;
            for (auto& [key, b] : g_built) pending = pending || !b.in_mgr;
            if (!pending) return;
            static ULONGLONG next_check = 0; // finding the manager walks every object: a few times a second is enough
            const ULONGLONG now = GetTickCount64();
            if (now < next_check) return;
            next_check = now + 250;
            UObject* mgr = outfit_mgr();
            if (!mgr) return;
            for (auto& [key, b] : g_built)
            {
                if (b.in_mgr) continue;
                b.in_mgr = true; // tried once: a failure is logged, not retried every tick
                if (StringType err = add_to_manager(mgr, b); !err.empty())
                    Output::send<LogLevel::Error>(STR("[content] {}: outfit part '{}' not added to UOutfitPartMgr: {}\n"), b.item.pak, b.item.info.id, err);
                else
                    Output::send<LogLevel::Normal>(STR("[content] outfit part '{}' ({}) added to UOutfitPartMgr\n"), b.item.info.id, b.part->GetPathName());
            }
        }

        auto register_live(const Item& item) -> StringType
        {
            auto it = g_built.find(lower(narrow_id(item.info.id)));
            if (it == g_built.end()) return STR("its part was not built");
            UObject* mgr = outfit_mgr();
            if (!mgr) return {}; // the manager is not built yet: the tick adds it once it is
            it->second.in_mgr = true;
            StringType err = add_to_manager(mgr, it->second);
            if (err.empty())
                Output::send<LogLevel::Normal>(STR("[content] outfit part '{}' ({}) added to UOutfitPartMgr\n"), item.info.id, it->second.part->GetPathName());
            return err;
        }
    } // namespace

    auto outfit_part_handler() -> TypeHandler { return {"outfit_part", UnlockType::OutfitPartVariant, &validate, &add_rows, &register_live}; }
} // namespace content

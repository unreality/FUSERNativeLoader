// The settings registry and the Settings screen's Mods tab (settings.hpp).
//
// Registry: each row's key, kind and value, read from FuserNative.ini when it is registered. A change (a Mods tab row
// or settings_set) rewrites the lines of the registered keys that are set, in place; every other line - other
// components' keys, comments, blank lines and line endings - is kept as it was, the file read as bytes so non-ASCII
// text survives, and replaced through a temporary file. A key that was never set is not written.
//
// The Mods tab. WBP_Screen_Settings is a WBP_Component_TabList over WidgetSwitcher_SettingsMenus, whose pages
// (General, Display, Audio, Accessibility, Account) are WBP_HypeList_Helpers each holding a ScrollBox with a VerticalBox
// of WBP_Setting_* rows. At the screen's OnInitialized (before its Slate exists) a page built like Accessibility's goes
// into the switcher before Account, and a TabArray entry before Account's: the TabList builds its buttons from
// TabArray at its PreConstruct (button i has MyIndex i), the screen's PreConstruct maps switcher page i to MyIndex i
// (QuickInitialize), and its tab-changed event switches to page ActiveIndex. Account stays last because outside the
// shell the screen's Update Accounts Tab removes it from TabArray / TabWidgetArray and later appends it again, and
// TrySetTabIndex refuses an index past TabArray's end.
// The page holds a heading per group with rows - the loader's own groups, then each loaded mod in load order - and its
// rows. A heading is a WBP_Setting_Labeled_Button (the screen's AboutButton class) with its button collapsed; rows are
// built like the screen's Language (dropdown), SubtitlesSetting (toggle) and ScrollSpeedSlider (slider), with our
// page's helper as their HypeListParent (a dropdown asks it where it sits in the ScrollBox). Values and enabled states
// are put on the rows at every OnScreenActivated: one Settings screen serves every opening, the pause menu's included.
//
// About FUSER: the screen's AboutButton shows UHypeGameInstance::GetBuildVersionPopupString in a text popup. A post-hook
// on that native appends the loader's version and every installed mod, with why any of them isn't running.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "loader/settings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fue/fue.hpp"
#include "loader/mod_manager.hpp"

#ifndef FNL_VERSION
#define FNL_VERSION "dev"
#endif

using namespace fue;

namespace
{
    constexpr uint8_t kCollapsed = 1; // ESlateVisibility
    constexpr const CharType* kTabLabel = STR("Mods");

    struct Entry
    {
        FnlMod* mod = nullptr;
        StringType group;           // the loader's own rows: their heading
        std::string key;
        StringType label;
        FnlSettingKind kind = FNL_SETTING_TOGGLE;
        int32 def = 0;
        std::vector<StringType> choices{};
        std::vector<int32> values{}; // empty: the entry's index is stored
        int32 min = 0, max = 0, step = 1;
        void (*on_change)(void*, int32_t, int) = nullptr;
        void* ctx = nullptr;
        int32 value = 0;
        bool saved = false;          // in FuserNative.ini
        FWeakObjectPtr row{};
    };

    std::vector<std::unique_ptr<Entry>> g_entries{};
    std::map<std::string, bool> g_enabled{};
    // The Mods tab: its screen, page helper and VerticalBox, the row templates, and the groups with a heading on it
    FWeakObjectPtr g_screen{}, g_helper{}, g_box{}, g_tpl_dropdown{}, g_tpl_toggle{}, g_tpl_slider{}, g_tpl_heading{};
    std::vector<StringType> g_headings{};

    // A heading's underline: an Image in the heading row's LayoutFootprint canvas, under its label text. UMG's TextBlock
    // has no underline in this engine, and the text's width is only known once it is laid out, so the line is placed
    // from the drawn geometry, every few ticks after each OnScreenActivated until the Mods page has been drawn
    // (place_underlines).
    // The label's place in its row doesn't change, so a placed underline is measured again only when the label's size
    // does (another language): a later measurement can be wrong, the label's own geometry being cached from an earlier
    // frame (the page scrolled since) while the row's is current.
    struct Underline
    {
        FWeakObjectPtr text{}, canvas{}, image{};
        bool placed = false;                         // checked for this opening of the screen
        float measured_w = -1.0f, measured_h = -1.0f; // the label's size when it was last placed (-1: never)
    };
    std::vector<Underline> g_underlines{};
    bool g_underlines_pending = false;
    constexpr int kUnderlineEveryTicks = 10;
    constexpr float kUnderlineGap = 1.0f, kUnderlineThickness = 2.0f, kUnderlineOverhang = 8.0f;
    constexpr uint8_t kHitTestInvisible = 3; // ESlateVisibility

    // ---------------------------------------------------------------- reflection and UMG -----------------------
    auto find_field(UStruct* st, const CharType* name) -> FProperty*
    {
        if (!st) return nullptr;
        const FName n(name, FNAME_Add);
        for (FProperty* p : TFieldRange<FProperty>(st, EFieldIterationFlags::IncludeSuper | EFieldIterationFlags::IncludeDeprecated))
            if (p->GetFName() == n) return p;
        return nullptr;
    }

    auto find_fn(const CharType* path) -> UFunction* { return UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path); }

    auto own_fn(UObject* obj, const CharType* name) -> UFunction* { return obj ? obj->GetFunctionByNameInChain(FName(name, FNAME_Add)) : nullptr; }

    auto obj_prop(UObject* obj, const CharType* name) -> UObject*
    {
        auto* p = obj ? obj->GetValuePtrByPropertyNameInChain<UObject*>(name) : nullptr;
        return p ? *p : nullptr;
    }

    // A ProcessEvent call with its parameters constructed and destroyed through the engine's property ops.
    class Call
    {
      public:
        explicit Call(UFunction* fn) : m_fn(fn)
        {
            if (!m_fn) return;
            int32 end = 0;
            for (FProperty* p : TFieldRange<FProperty>(m_fn, EFieldIterationFlags::IncludeDeprecated)) end = std::max(end, p->GetOffset_Internal() + p->GetSize());
            m_buf.assign(static_cast<size_t>(end) + 16, 0);
            each([&](FProperty* p) { p->InitializeValue_InContainer(m_buf.data()); });
        }
        ~Call()
        {
            if (m_fn) each([&](FProperty* p) { p->DestroyValue_InContainer(m_buf.data()); });
        }
        Call(const Call&) = delete;
        Call& operator=(const Call&) = delete;
        template <typename T>
        auto set(const CharType* name, T value) -> Call&
        {
            if (auto* a = addr(name)) *reinterpret_cast<T*>(a) = value;
            return *this;
        }
        template <typename T>
        auto get(const CharType* name) -> T
        {
            auto* a = addr(name);
            return a ? *reinterpret_cast<T*>(a) : T{};
        }
        auto copy_in(const CharType* name, const void* src) -> Call&
        {
            FProperty* p = find_field(m_fn, name);
            if (p && src) p->CopyCompleteValue(p->ContainerPtrToValuePtr<void>(m_buf.data()), src);
            return *this;
        }
        auto call(UObject* target) -> bool
        {
            if (!m_fn || !target) return false;
            target->ProcessEvent(m_fn, m_buf.data());
            return true;
        }
        auto addr(const CharType* name) -> uint8*
        {
            FProperty* p = find_field(m_fn, name);
            return p ? p->ContainerPtrToValuePtr<uint8>(m_buf.data()) : nullptr;
        }

      private:
        template <typename F>
        auto each(F&& f) -> void
        {
            for (FProperty* p : TFieldRange<FProperty>(m_fn, EFieldIterationFlags::IncludeDeprecated))
            {
                if (!p->HasAnyPropertyFlags(CPF_Parm)) break;
                f(p);
            }
        }
        UFunction* m_fn;
        std::vector<uint8> m_buf{};
    };

    auto create_widget(UObject* world_context, UClass* cls, UObject* player) -> UObject*
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.WidgetBlueprintLibrary:Create"));
        static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
        Call c(fn);
        c.set<UObject*>(STR("WorldContextObject"), world_context).set<UObject*>(STR("WidgetType"), cls).set<UObject*>(STR("OwningPlayer"), player);
        return c.call(cdo) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    auto owning_player(UObject* widget) -> UObject*
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.Widget:GetOwningPlayer"));
        Call c(fn);
        return c.call(widget) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    auto widget_parent(UObject* widget) -> UObject*
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.Widget:GetParent"));
        Call c(fn);
        return c.call(widget) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    auto is_panel(UObject* widget) -> bool;

    auto add_child(UObject* panel, UObject* child) -> UObject*
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.PanelWidget:AddChild"));
        if (!is_panel(panel)) return nullptr;
        Call c(fn);
        c.set<UObject*>(STR("Content"), child);
        return c.call(panel) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    auto remove_child(UObject* panel, UObject* child) -> bool
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.PanelWidget:RemoveChild"));
        if (!is_panel(panel)) return false;
        Call c(fn);
        c.set<UObject*>(STR("Content"), child);
        return c.call(panel) && c.get<bool>(STR("ReturnValue"));
    }

    // Only on a UPanelWidget: its native functions on any other widget read that widget's memory as a panel's
    auto is_panel(UObject* widget) -> bool
    {
        static UClass* panel = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.PanelWidget"));
        return widget && panel && widget->IsA(panel);
    }

    auto child_count(UObject* panel) -> int32
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.PanelWidget:GetChildrenCount"));
        if (!is_panel(panel)) return 0;
        Call c(fn);
        return c.call(panel) ? c.get<int32>(STR("ReturnValue")) : 0;
    }

    auto child_at(UObject* panel, int32 index) -> UObject*
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.PanelWidget:GetChildAt"));
        if (!is_panel(panel)) return nullptr;
        Call c(fn);
        c.set<int32>(STR("Index"), index);
        return c.call(panel) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    auto child_index(UObject* panel, UObject* child) -> int32
    {
        for (int32 i = 0, n = child_count(panel); i < n; ++i)
            if (child_at(panel, i) == child) return i;
        return -1;
    }

    // A widget named `name` in a user widget's own tree (a designer widget that isn't a variable has no property)
    auto find_named(UObject* widget, const FName& name, int depth = 0) -> UObject*
    {
        if (!widget || depth > 12) return nullptr;
        if (widget->GetFName() == name) return widget;
        for (int32 i = 0, n = child_count(widget); i < n; ++i)
            if (UObject* found = find_named(child_at(widget, i), name, depth + 1)) return found;
        return nullptr;
    }

    auto tree_widget(UObject* user_widget, const CharType* name) -> UObject*
    {
        if (UObject* w = obj_prop(user_widget, name)) return w;
        return find_named(obj_prop(obj_prop(user_widget, STR("WidgetTree")), STR("RootWidget")), FName(name, FNAME_Add));
    }

    // The same, also inside the user widgets nested in it (their own trees)
    auto find_nested(UObject* widget, const FName& name, int depth = 0) -> UObject*
    {
        if (!widget || depth > 16) return nullptr;
        if (widget->GetFName() == name) return widget;
        for (int32 i = 0, n = child_count(widget); i < n; ++i)
            if (UObject* found = find_nested(child_at(widget, i), name, depth + 1)) return found;
        return find_nested(obj_prop(obj_prop(widget, STR("WidgetTree")), STR("RootWidget")), name, depth + 1);
    }

    struct Vec2
    {
        float x = 0, y = 0;
    };

    auto desired_size(UObject* widget) -> Vec2
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.Widget:GetDesiredSize"));
        Call c(fn);
        return c.call(widget) ? c.get<Vec2>(STR("ReturnValue")) : Vec2{};
    }

    // A point in `from`'s local space, in `to`'s; false while either has no geometry yet (not drawn)
    auto map_point(UObject* from, Vec2 point, UObject* to, Vec2& out, Vec2* to_size = nullptr) -> bool
    {
        static UFunction* geometry = [] {
            UFunction* f = find_fn(STR("/Script/UMG.Widget:GetCachedGeometry"));
            return f ? f : find_fn(STR("/Script/UMG.Widget:GetTickSpaceGeometry"));
        }();
        static UFunction* size = find_fn(STR("/Script/UMG.SlateBlueprintLibrary:GetLocalSize"));
        static UFunction* to_abs = find_fn(STR("/Script/UMG.SlateBlueprintLibrary:LocalToAbsolute"));
        static UFunction* to_local = find_fn(STR("/Script/UMG.SlateBlueprintLibrary:AbsoluteToLocal"));
        static UObject* lib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__SlateBlueprintLibrary"));
        Call g_from(geometry), g_to(geometry);
        if (!g_from.call(from) || !g_to.call(to)) return false;
        for (Call* g : {&g_from, &g_to})
        {
            Call s(size);
            s.copy_in(STR("Geometry"), g->addr(STR("ReturnValue"))).call(lib);
            const Vec2 v = s.get<Vec2>(STR("ReturnValue"));
            if (v.x <= 0 || v.y <= 0) return false;
            if (g == &g_to && to_size) *to_size = v;
        }
        Call a(to_abs);
        a.copy_in(STR("Geometry"), g_from.addr(STR("ReturnValue"))).set<Vec2>(STR("LocalCoordinate"), point).call(lib);
        Call l(to_local);
        l.copy_in(STR("Geometry"), g_to.addr(STR("ReturnValue"))).set<Vec2>(STR("AbsoluteCoordinate"), a.get<Vec2>(STR("ReturnValue"))).call(lib);
        out = l.get<Vec2>(STR("ReturnValue"));
        return true;
    }

    auto set_visibility(UObject* widget, uint8_t visibility) -> void
    {
        static UFunction* fn = find_fn(STR("/Script/UMG.Widget:SetVisibility"));
        Call c(fn);
        c.set<uint8_t>(STR("InVisibility"), visibility).call(widget);
    }

    auto set_text_param(UObject* obj, const CharType* fn_name, const CharType* param, const CharType* text) -> bool
    {
        Call c(own_fn(obj, fn_name));
        const FText value(text);
        return c.copy_in(param, &value).call(obj);
    }

    // A slot's layout (the named fields, through the slot's own setters, so a live Slate slot updates too)
    struct SlotField
    {
        const CharType* prop;
        const CharType* setter;
        const CharType* param;
    };

    auto copy_slot(UObject* from, UObject* to, const SlotField* fields, size_t n) -> void
    {
        if (!from || !to) return;
        for (size_t i = 0; i < n; ++i)
        {
            FProperty* sp = find_field(from->GetClassPrivate(), fields[i].prop);
            if (!sp) continue;
            Call c(find_fn(fields[i].setter));
            c.copy_in(fields[i].param, sp->ContainerPtrToValuePtr<void>(from)).call(to);
        }
    }

    auto copy_vbox_slot(UObject* from, UObject* to) -> void
    {
        static const SlotField fields[] = {
                {STR("Size"), STR("/Script/UMG.VerticalBoxSlot:SetSize"), STR("InSize")},
                {STR("Padding"), STR("/Script/UMG.VerticalBoxSlot:SetPadding"), STR("InPadding")},
                {STR("HorizontalAlignment"), STR("/Script/UMG.VerticalBoxSlot:SetHorizontalAlignment"), STR("InHorizontalAlignment")},
                {STR("VerticalAlignment"), STR("/Script/UMG.VerticalBoxSlot:SetVerticalAlignment"), STR("InVerticalAlignment")},
        };
        copy_slot(from, to, fields, std::size(fields));
    }

    auto copy_switcher_slot(UObject* from, UObject* to) -> void
    {
        static const SlotField fields[] = {
                {STR("Padding"), STR("/Script/UMG.WidgetSwitcherSlot:SetPadding"), STR("InPadding")},
                {STR("HorizontalAlignment"), STR("/Script/UMG.WidgetSwitcherSlot:SetHorizontalAlignment"), STR("InHorizontalAlignment")},
                {STR("VerticalAlignment"), STR("/Script/UMG.WidgetSwitcherSlot:SetVerticalAlignment"), STR("InVerticalAlignment")},
        };
        copy_slot(from, to, fields, std::size(fields));
    }

    auto copy_scroll_slot(UObject* from, UObject* to) -> void
    {
        static const SlotField fields[] = {
                {STR("Padding"), STR("/Script/UMG.ScrollBoxSlot:SetPadding"), STR("InPadding")},
                {STR("HorizontalAlignment"), STR("/Script/UMG.ScrollBoxSlot:SetHorizontalAlignment"), STR("InHorizontalAlignment")},
        };
        copy_slot(from, to, fields, std::size(fields));
    }

    auto copy_canvas_slot(UObject* from, UObject* to) -> void
    {
        static const SlotField fields[] = {
                {STR("LayoutData"), STR("/Script/UMG.CanvasPanelSlot:SetLayout"), STR("InLayoutData")},
                {STR("bAutoSize"), STR("/Script/UMG.CanvasPanelSlot:SetAutoSize"), STR("InbAutoSize")},
                {STR("ZOrder"), STR("/Script/UMG.CanvasPanelSlot:SetZOrder"), STR("InZOrder")},
        };
        copy_slot(from, to, fields, std::size(fields));
    }

    // The plain configuration values (bool/byte/int/float/enum/name/class, and value structs such as styles when asked)
    // a class declares, from one object to another; object references, containers, delegates and transient state stay.
    // Never structs on a Blueprint class: its UberGraphFrame is one.
    auto copy_plain_properties(UStruct* owner, UObject* from, UObject* to, bool with_structs, const std::vector<StringType>& skip = {}) -> void
    {
        static const FName struct_type(STR("StructProperty"), FNAME_Add);
        static const FName plain[] = {FName(STR("BoolProperty"), FNAME_Add), FName(STR("ByteProperty"), FNAME_Add), FName(STR("IntProperty"), FNAME_Add),
                                      FName(STR("FloatProperty"), FNAME_Add), FName(STR("EnumProperty"), FNAME_Add), FName(STR("NameProperty"), FNAME_Add),
                                      FName(STR("ClassProperty"), FNAME_Add)};
        if (!owner || !from || !to) return;
        for (FProperty* p : TFieldRange<FProperty>(owner, EFieldIterationFlags::None))
        {
            if (p->HasAnyPropertyFlags(CPF_Transient | CPF_Parm)) continue;
            const FName type = p->GetClass().GetFName();
            if (std::find(std::begin(plain), std::end(plain), type) == std::end(plain) && !(with_structs && type == struct_type)) continue;
            if (std::find(skip.begin(), skip.end(), p->GetName()) != skip.end()) continue;
            p->CopyCompleteValue(p->ContainerPtrToValuePtr<void>(to), p->ContainerPtrToValuePtr<void>(from));
        }
    }

    auto widget_class() -> UClass*
    {
        static UClass* c = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Widget"));
        return c;
    }

    // ---------------------------------------------------------------- registry ---------------------------------
    auto find(const char* key) -> Entry*
    {
        if (!key) return nullptr;
        for (auto& e : g_entries)
            if (e->key == key) return e.get();
        return nullptr;
    }

    auto ini_path() -> std::filesystem::path { return std::filesystem::path(module_dir()) / L"FuserNative.ini"; }

    // The file's key=value lines (the first of a repeated key)
    auto read_ini() -> std::map<std::string, std::string>
    {
        std::map<std::string, std::string> out{};
        std::ifstream in(ini_path(), std::ios::binary);
        std::string line{};
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t eq = line.find('=');
            if (eq != std::string::npos && eq > 0) out.emplace(line.substr(0, eq), line.substr(eq + 1));
        }
        return out;
    }

    auto save() -> void
    {
        const auto path = ini_path();
        std::vector<std::pair<std::string, std::string>> ours{};
        for (auto& e : g_entries)
            if (e->saved) ours.emplace_back(e->key + "=", std::to_string(e->value));
        std::vector<std::string> lines{};
        bool crlf = false;
        {
            std::ifstream in(path, std::ios::binary);
            std::string line{};
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                    crlf = true;
                }
                lines.push_back(line);
            }
        }
        if (lines.empty()) lines.emplace_back("; FuserNative settings (the game's Settings screen keeps its own lines up to date)");
        std::vector<bool> written(ours.size(), false);
        std::vector<std::string> out{};
        for (const std::string& line : lines)
        {
            bool handled = false;
            for (size_t i = 0; i < ours.size() && !handled; ++i)
            {
                if (line.rfind(ours[i].first, 0) != 0) continue;
                handled = true;
                if (!written[i]) out.push_back(ours[i].first + ours[i].second); // a repeat of the key is dropped
                written[i] = true;
            }
            if (!handled) out.push_back(line);
        }
        for (size_t i = 0; i < ours.size(); ++i)
            if (!written[i]) out.push_back(ours[i].first + ours[i].second);
        auto tmp = path;
        tmp += L".tmp";
        bool ok = false;
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            for (const std::string& l : out) f << l << (crlf ? "\r\n" : "\n");
            ok = f.good();
        }
        ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!ok) Output::send<LogLevel::Error>(STR("[settings] saving {} failed\n"), path.wstring());
    }

    // The dropdown entry showing a value: its index, or the entry whose stored value is nearest
    auto choice_index(const Entry& e) -> int32
    {
        if (e.values.empty()) return std::clamp(e.value, 0, static_cast<int32>(e.choices.size()) - 1);
        int32 best = 0;
        for (int32 i = 1; i < static_cast<int32>(e.values.size()); ++i)
            if (std::abs(static_cast<int64>(e.values[i]) - e.value) < std::abs(static_cast<int64>(e.values[best]) - e.value)) best = i;
        return best;
    }

    auto describe(const Entry& e) -> StringType
    {
        switch (e.kind)
        {
        case FNL_SETTING_TOGGLE: return e.value ? STR("On") : STR("Off");
        case FNL_SETTING_CHOICE: return e.choices[choice_index(e)];
        default: return std::format(STR("{}"), e.value);
        }
    }

    auto snap(const Entry& e, float raw) -> int32
    {
        const int32 v = e.min + static_cast<int32>(std::lround((raw - static_cast<float>(e.min)) / static_cast<float>(e.step))) * e.step;
        return std::clamp(v, e.min, e.max);
    }

    auto enabled(const Entry& e) -> bool
    {
        const auto it = g_enabled.find(e.key);
        return it == g_enabled.end() || it->second;
    }

    // ---------------------------------------------------------------- rows -------------------------------------
    // The entry's value and enabled state onto its row (without broadcasting a change)
    auto show(Entry& e) -> void
    {
        UObject* row = e.row.Get();
        if (!row) return;
        switch (e.kind)
        {
        case FNL_SETTING_TOGGLE: {
            // as the stock screen does for its toggles: InitializeToggleState also snaps the switch's graphic
            Call c(own_fn(row, STR("InitializeToggleState")));
            c.set<bool>(STR("inToggled"), e.value != 0).call(row);
            break;
        }
        case FNL_SETTING_CHOICE: {
            Call c(own_fn(row, STR("SetCurrentValue")));
            c.set<float>(STR("in_NewValue"), static_cast<float>(choice_index(e))).set<bool>(STR("IsInitializing"), true).call(row);
            break;
        }
        case FNL_SETTING_SLIDER: {
            Call c(own_fn(row, STR("SetCurrentValueNonAlpha")));
            c.set<float>(STR("in_NewValue"), static_cast<float>(std::clamp(e.value, e.min, e.max))).set<bool>(STR("IsInitializing"), true).call(row);
            break;
        }
        }
        Call c(own_fn(row, STR("SetIsEnabled")));
        c.set<bool>(STR("bInIsEnabled"), enabled(e)).call(row);
    }

    auto set_list_parent(UObject* row) -> void
    {
        if (auto* dst = row->GetValuePtrByPropertyNameInChain<UObject*>(STR("HypeListParent"))) *dst = g_helper.Get();
    }

    // A widget like `tpl` appended to the page, laid out like it
    auto append_like(UObject* tpl) -> UObject*
    {
        UObject* screen = g_screen.Get();
        UObject* box = g_box.Get();
        UObject* row = screen && box && tpl ? create_widget(screen, tpl->GetClassPrivate(), owning_player(screen)) : nullptr;
        if (!row) return nullptr;
        set_list_parent(row);
        return row;
    }

    auto place(UObject* row, UObject* tpl) -> bool
    {
        UObject* slot = add_child(g_box.Get(), row);
        copy_vbox_slot(obj_prop(tpl, STR("Slot")), slot);
        return slot != nullptr;
    }

    auto add_heading(const StringType& text) -> void
    {
        UObject* tpl = g_tpl_heading.Get();
        UObject* row = append_like(tpl);
        if (!row) return;
        UObject* button = tree_widget(row, STR("Button"));
        if (!set_text_param(row, STR("SetLabelContent"), STR("in_LabelContent"), text.c_str()) || !button)
            Output::send<LogLevel::Warning>(STR("[settings] heading '{}': SetLabelContent {}, Button {}\n"), text, own_fn(row, STR("SetLabelContent")) != nullptr,
                                            button != nullptr);
        set_visibility(button, kCollapsed);
        place(row, tpl);

        // the underline, hidden until it is placed; in the text's colour
        static UClass* image_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.Image"));
        static UClass* canvas_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.CanvasPanel"));
        static UClass* text_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.TextBlock"));
        UObject* label = find_nested(tree_widget(row, STR("Label")), FName(STR("Default_TextSlot"), FNAME_Add));
        if (label && !(text_class && label->IsA(text_class))) label = nullptr;
        UObject* canvas = tree_widget(row, STR("LayoutFootprint"));
        if (canvas && !(canvas_class && canvas->IsA(canvas_class))) canvas = nullptr;
        UObject* tree = obj_prop(row, STR("WidgetTree"));
        UObject* image = label && canvas && tree && image_class ? UObjectGlobals::NewObject<UObject>(tree, image_class) : nullptr;
        if (!image || !add_child(canvas, image))
        {
            Output::send<LogLevel::Warning>(STR("[settings] heading '{}': no underline (text {}, canvas {}, image {})\n"), text, label != nullptr, canvas != nullptr,
                                            image != nullptr);
            return;
        }
        set_visibility(image, kCollapsed);
        if (FProperty* color = find_field(label->GetClassPrivate(), STR("ColorAndOpacity"))) // FSlateColor: SpecifiedColor first
        {
            Call c(find_fn(STR("/Script/UMG.Image:SetColorAndOpacity")));
            c.copy_in(STR("InColorAndOpacity"), color->ContainerPtrToValuePtr<void>(label)).call(image);
        }
        g_underlines.push_back({FWeakObjectPtr(label), FWeakObjectPtr(canvas), FWeakObjectPtr(image)});
    }

    // Each underline under its text, once the text is laid out: from the text's bottom-left to its bottom-right
    auto place_underlines() -> void
    {
        static UFunction* set_position = find_fn(STR("/Script/UMG.CanvasPanelSlot:SetPosition"));
        static UFunction* set_size = find_fn(STR("/Script/UMG.CanvasPanelSlot:SetSize"));
        bool pending = false;
        for (Underline& u : g_underlines)
        {
            UObject* text = u.text.Get();
            UObject* canvas = u.canvas.Get();
            UObject* image = u.image.Get();
            if (u.placed || !text || !canvas || !image) continue;
            const Vec2 size = desired_size(text);
            if (size.x > 0 && std::abs(size.x - u.measured_w) < 0.5f && std::abs(size.y - u.measured_h) < 0.5f)
            {
                u.placed = true; // the same label: where it was
                continue;
            }
            // a placement under the label and in its row (a few units' overhang allowed), or none yet: tried again at
            // the next round
            Vec2 left{}, right{}, row{};
            if (size.x <= 0 || !map_point(text, {0, size.y}, canvas, left, &row) || !map_point(text, size, canvas, right) || left.x < -1.0f ||
                left.y < 0.0f || left.y > row.y + kUnderlineOverhang || right.x - left.x < 1.0f)
            {
                pending = true;
                continue;
            }
            u.measured_w = size.x;
            u.measured_h = size.y;
            UObject* slot = obj_prop(image, STR("Slot"));
            Call pos(set_position);
            pos.set<Vec2>(STR("InPosition"), {left.x, left.y + kUnderlineGap}).call(slot);
            Call sz(set_size);
            sz.set<Vec2>(STR("InSize"), {right.x - left.x, kUnderlineThickness}).call(slot);
            set_visibility(image, kHitTestInvisible);
            u.placed = true;
            static int logged = 0;
            if (logged++ < 8)
                Output::send<LogLevel::Verbose>(STR("[settings] Mods tab: underline at ({:.0f}, {:.0f}) width {:.0f} (text {:.0f} x {:.0f})\n"), left.x, left.y + kUnderlineGap,
                                                right.x - left.x, size.x, size.y);
        }
        g_underlines_pending = pending;
    }

    auto build_row(Entry& e) -> void
    {
        UObject* tpl = e.kind == FNL_SETTING_TOGGLE ? g_tpl_toggle.Get() : e.kind == FNL_SETTING_CHOICE ? g_tpl_dropdown.Get() : g_tpl_slider.Get();
        UObject* row = append_like(tpl);
        if (!row)
        {
            Output::send<LogLevel::Warning>(STR("[settings] {}: no row (template {})\n"), to_wstring(e.key), tpl != nullptr);
            return;
        }
        if (e.kind == FNL_SETTING_TOGGLE)
        {
            for (const CharType* name : {STR("_OnLabel"), STR("_OffLabel")})
                if (FProperty* f = find_field(tpl->GetClassPrivate(), name)) f->CopyCompleteValue(f->ContainerPtrToValuePtr<void>(row), f->ContainerPtrToValuePtr<void>(tpl));
        }
        if (e.kind == FNL_SETTING_CHOICE)
        {
            // the class defaults carry the graphics-quality entries: emptied first
            if (auto* arr = CastField<FArrayProperty>(find_field(row->GetClassPrivate(), STR("_DropdownEntries"))))
            {
                auto* raw = arr->ContainerPtrToValuePtr<TArray<uint8>>(row);
                const int32 es = arr->GetInner()->GetElementSize();
                for (int32 i = 0; i < raw->Num(); ++i) arr->GetInner()->DestroyValue(raw->GetData() + static_cast<size_t>(i) * es);
                raw->ArrayNum = 0;
            }
        }
        if (e.kind == FNL_SETTING_SLIDER)
        {
            if (auto* grid = row->GetValuePtrByPropertyNameInChain<float>(STR("_LabelGridSize"))) *grid = static_cast<float>(e.step);
            if (auto* bias = row->GetValuePtrByPropertyNameInChain<float>(STR("LabelValueRoundingBias"))) *bias = 0.0f;
            if (FProperty* units = find_field(row->GetClassPrivate(), STR("_UnitsText")))
            {
                const FText plain(STR("{Quantity}")); // the value text: just the number
                units->CopyCompleteValue(units->ContainerPtrToValuePtr<void>(row), &plain);
            }
            Call extents(own_fn(row, STR("SetLabelExtents")));
            extents.set<float>(STR("in_MinLabelValue"), static_cast<float>(e.min)).set<float>(STR("in_MaxLabelValue"), static_cast<float>(e.max)).call(row);
            Call reset(own_fn(row, STR("OverrideDoubleClickToResetSlider")));
            reset.set<bool>(STR("DoubleClickToReset"), false).call(row);
        }
        set_text_param(row, STR("SetLabelContent"), STR("in_LabelContent"), e.label.c_str());
        if (e.kind == FNL_SETTING_CHOICE)
            for (const StringType& c : e.choices) set_text_param(row, STR("AddUniqueDropdownEntry"), STR("NewDropdownOption"), c.c_str());
        if (!place(row, tpl)) return;
        if (e.kind == FNL_SETTING_SLIDER)
        {
            // after AddChild: its Construct sets up the notches from the grid; again here in case that came first
            const float v = static_cast<float>(std::clamp(e.value, e.min, e.max));
            Call values(own_fn(row, STR("SetSliderValuesNonAlpha")));
            values.set<float>(STR("MinNonAlpha"), static_cast<float>(e.min))
                    .set<float>(STR("MaxNonAlpha"), static_cast<float>(e.max))
                    .set<float>(STR("DefaultValueNonAlpha"), v)
                    .set<float>(STR("CurrentValueNonAlpha"), v)
                    .set<bool>(STR("IsInitializing"), true)
                    .call(row);
            Call notches(own_fn(row, STR("SetupNotchedBehavior")));
            notches.call(row);
        }
        e.row = FWeakObjectPtr(row); // its value goes on at OnScreenActivated (refresh), as the stock rows' do
    }

    // A group's heading key and text: a mod's, or one of the loader's own
    auto group_key(const Entry& e) -> StringType { return e.mod ? fnl_mod_heading(e.mod) : e.group + STR(" ") + to_wstring(FNL_VERSION); }

    auto ensure_heading(const StringType& text) -> void
    {
        if (std::find(g_headings.begin(), g_headings.end(), text) != g_headings.end()) return;
        g_headings.push_back(text);
        add_heading(text);
    }

    // A row registered after the tab was built: appended, under its heading
    auto append_late(Entry& e) -> void
    {
        if (!g_box.Get()) return;
        ensure_heading(group_key(e));
        build_row(e);
        show(e);
    }

    auto fill_page() -> void
    {
        // the loader's own groups, in registration order, each heading followed by its rows
        std::vector<StringType> groups{};
        for (auto& e : g_entries)
            if (!e->mod && std::find(groups.begin(), groups.end(), e->group) == groups.end()) groups.push_back(e->group);
        for (const StringType& group : groups)
        {
            for (auto& e : g_entries)
            {
                if (e->mod || e->group != group) continue;
                ensure_heading(group_key(*e));
                build_row(*e);
            }
        }
        // then every loaded mod with rows, in load order (the About FUSER popup lists them all)
        for (FnlMod* m : fnl_mods_loaded())
        {
            for (auto& e : g_entries)
            {
                if (e->mod != m) continue;
                ensure_heading(fnl_mod_heading(m));
                build_row(*e);
            }
        }
    }

    // ---------------------------------------------------------------- the tab ----------------------------------
    // A copy of TabArray's element `from` inserted at `at`, labelled `label`
    auto insert_tab(UObject* tabs, int32 from, int32 at, const CharType* label) -> bool
    {
        auto* arr = CastField<FArrayProperty>(find_field(tabs ? tabs->GetClassPrivate() : nullptr, STR("TabArray")));
        auto* st = arr ? CastField<FStructProperty>(arr->GetInner()) : nullptr;
        if (!st) return false;
        auto* raw = arr->ContainerPtrToValuePtr<TArray<uint8>>(tabs);
        const int32 n = raw->Num();
        if (from < 0 || from >= n || at < 0 || at > n) return false;
        FProperty* inner = arr->GetInner();
        const size_t es = static_cast<size_t>(inner->GetElementSize());
        uint8* data = static_cast<uint8*>(FMemory::Realloc(raw->Data, (static_cast<size_t>(n) + 1) * es));
        raw->Data = data;
        raw->ArrayMax = n + 1;
        std::memmove(data + (at + 1) * es, data + at * es, (n - at) * es);
        raw->ArrayNum = n + 1;
        inner->InitializeValue(data + at * es);
        inner->CopyCompleteValue(data + at * es, data + static_cast<size_t>(from < at ? from : from + 1) * es);
        for (FProperty* f : TFieldRange<FProperty>(st->GetStruct().Get(), EFieldIterationFlags::None))
        {
            if (f->GetName().rfind(STR("TextLabel"), 0) != 0) continue;
            const FText text(label);
            f->CopyCompleteValue(f->ContainerPtrToValuePtr<void>(data + at * es), &text);
        }
        return true;
    }

    auto build_tab(UObject* screen) -> void
    {
        UObject* switcher = obj_prop(screen, STR("WidgetSwitcher_SettingsMenus"));
        UObject* tabs = obj_prop(screen, STR("WBP_Component_TabList"));
        UObject* account = obj_prop(screen, STR("Account_HypeListHelper"));
        UObject* tpl_helper = obj_prop(screen, STR("Accessibility_HypeListHelper"));
        UObject* tpl_scroll = obj_prop(screen, STR("Accessibility_ScrollBox"));
        UObject* tpl_box = child_at(tpl_scroll, 0);
        UObject* heading = obj_prop(screen, STR("AboutButton"));
        g_tpl_heading = FWeakObjectPtr(heading ? heading : obj_prop(screen, STR("SwitchOnly_GoOnlineButton")));
        g_tpl_dropdown = FWeakObjectPtr(obj_prop(screen, STR("Language")));
        g_tpl_toggle = FWeakObjectPtr(obj_prop(screen, STR("SubtitlesSetting")));
        g_tpl_slider = FWeakObjectPtr(obj_prop(screen, STR("ScrollSpeedSlider")));
        if (!switcher || !tabs || !tpl_helper || !tpl_scroll || !tpl_box)
        {
            Output::send<LogLevel::Warning>(STR("[settings] Mods tab: the screen's switcher {} / tab list {} / Accessibility page {} {} {} missing - no Mods tab\n"),
                                            switcher != nullptr, tabs != nullptr, tpl_helper != nullptr, tpl_scroll != nullptr, tpl_box != nullptr);
            return;
        }

        // 1. the page: a helper like Accessibility's, holding a ScrollBox holding a VerticalBox
        UObject* tree = tpl_scroll->GetOuterPrivate(); // the screen's WidgetTree
        UObject* helper = create_widget(screen, tpl_helper->GetClassPrivate(), owning_player(screen));
        UObject* scroll = UObjectGlobals::NewObject<UObject>(tree, tpl_scroll->GetClassPrivate());
        UObject* box = UObjectGlobals::NewObject<UObject>(tree, tpl_box->GetClassPrivate());
        if (!helper || !scroll || !box)
        {
            Output::send<LogLevel::Warning>(STR("[settings] Mods tab: creating the page failed (helper {}, scroll {}, box {})\n"), helper != nullptr, scroll != nullptr,
                                            box != nullptr);
            return;
        }
        static UClass* list_widget = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Hype.HypeListWidget"));
        // as crate edit's extra pages: the helper's own settings without structs, and of its UWidget ones Visibility; the
        // native ScrollBox and VerticalBox with their style structs and UWidget settings
        copy_plain_properties(list_widget, tpl_helper, helper, false);
        copy_plain_properties(tpl_helper->GetClassPrivate(), tpl_helper, helper, false);
        if (FProperty* vis = find_field(widget_class(), STR("Visibility"))) vis->CopyCompleteValue(vis->ContainerPtrToValuePtr<void>(helper), vis->ContainerPtrToValuePtr<void>(tpl_helper));
        for (auto [from, to] : {std::pair{tpl_scroll, scroll}, std::pair{tpl_box, box}})
        {
            copy_plain_properties(from->GetClassPrivate(), from, to, true);
            copy_plain_properties(widget_class(), from, to, true, {STR("bIsVariable"), STR("bCreatedByConstructionScript")});
        }
        copy_scroll_slot(obj_prop(tpl_box, STR("Slot")), add_child(scroll, box));
        {
            // UUserWidget::SetContentForSlot isn't a UFUNCTION in this build: the NamedSlot's content is set directly
            static UFunction* fn = find_fn(STR("/Script/UMG.ContentWidget:SetContent"));
            UObject* named = obj_prop(helper, STR("ListContentSlot"));
            Call c(fn);
            c.set<UObject*>(STR("Content"), scroll).call(named);
            if (widget_parent(scroll) != named)
            {
                Output::send<LogLevel::Warning>(STR("[settings] Mods tab: placing the ScrollBox in the page failed\n"));
                return;
            }
        }
        {
            static UFunction* fn = find_fn(STR("/Script/Hype.HypeListWidget:SetScrollBox"));
            Call c(fn);
            c.set<UObject*>(STR("inScrollbox"), scroll).set<bool>(STR("setAsItemParent"), true).call(helper);
        }

        // 2. into the switcher before Account (the pages after it are taken out and put back behind ours)
        const int32 n_pages = child_count(switcher);
        int32 at = account ? child_index(switcher, account) : -1;
        if (at < 0) at = n_pages;
        std::vector<std::pair<UObject*, UObject*>> tail{}; // page, its old slot
        while (child_count(switcher) > at)
        {
            UObject* c = child_at(switcher, at);
            tail.emplace_back(c, obj_prop(c, STR("Slot")));
            if (!remove_child(switcher, c)) break;
        }
        copy_switcher_slot(obj_prop(tpl_helper, STR("Slot")), add_child(switcher, helper));
        for (auto& [c, old] : tail) copy_switcher_slot(old, add_child(switcher, c));

        // 3. the tab, at the same index (Account's entry may already be gone, so no further than the end): a copy of
        // General's entry (its cog icon and background), labelled Mods
        auto* tab_array = CastField<FArrayProperty>(find_field(tabs->GetClassPrivate(), STR("TabArray")));
        const int32 n_tabs = tab_array ? tab_array->ContainerPtrToValuePtr<TArray<uint8>>(tabs)->Num() : 0;
        const int32 tab_at = std::min(at, n_tabs);
        const bool tab_ok = insert_tab(tabs, 0, tab_at, kTabLabel);
        auto* buttons = tabs->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("TabWidgetArray"));
        if (tab_ok && buttons && buttons->Num() > 0)
        {
            Call c(own_fn(tabs, STR("RebuildTabsWithNewStrings")));
            c.call(tabs);
        }

        g_screen = FWeakObjectPtr(screen);
        g_helper = FWeakObjectPtr(helper);
        g_box = FWeakObjectPtr(box);
        g_headings.clear();
        g_underlines.clear();
        fill_page();
        Output::send<LogLevel::Normal>(STR("[settings] Mods tab: page {} of {}, tab {} of {} ({}), {} heading(s), {} row(s)\n"), at, child_count(switcher), tab_at,
                                       n_tabs + (tab_ok ? 1 : 0), tab_ok ? STR("ok") : STR("NOT ADDED"), g_headings.size(), child_count(box) - static_cast<int32>(g_headings.size()));
    }

    auto refresh(UObject* screen) -> void
    {
        if (screen != g_screen.Get()) return;
        // the screen's PreConstruct moves each stock page's scroll prompt; ours follows Accessibility's
        UObject* tpl_prompt = obj_prop(obj_prop(screen, STR("Accessibility_HypeListHelper")), STR("ListScrollPrompt"));
        UObject* prompt = obj_prop(g_helper.Get(), STR("ListScrollPrompt"));
        copy_canvas_slot(obj_prop(tpl_prompt, STR("Slot")), obj_prop(prompt, STR("Slot")));
        for (auto& e : g_entries) show(*e);
        for (Underline& u : g_underlines) u.placed = false; // the text's layout may differ (language, resolution)
        g_underlines_pending = !g_underlines.empty();
    }

    // ---------------------------------------------------------------- changes ----------------------------------
    auto change(Entry& e, int32 value) -> void
    {
        if (value == e.value && e.saved) return;
        e.value = value;
        e.saved = true;
        save();
        Output::send<LogLevel::Normal>(STR("[settings] {} -> {}\n"), e.label, describe(e));
        if (e.on_change) e.on_change(e.ctx, value, 0);
    }

    auto row_entry(UObject* row, FnlSettingKind kind) -> Entry*
    {
        for (auto& e : g_entries)
            if (e->kind == kind && e->row.Get() == row) return e.get();
        return nullptr;
    }

    auto param(UFunction* fn, void* parms, const CharType* name) -> void*
    {
        FProperty* p = find_field(fn, name);
        return p ? p->ContainerPtrToValuePtr<void>(parms) : nullptr;
    }

    auto on_process_event_post(UObject* context, UFunction* fn, void* parms) -> void
    {
        if (!context || !fn) return;
        static const FName settings_screen(STR("WBP_Screen_Settings_C"), FNAME_Add), initialized(STR("OnInitialized"), FNAME_Add),
                activated(STR("OnScreenActivated"), FNAME_Add), selection(STR("OnDropdownSelectionChanged_Event_0"), FNAME_Add),
                toggled(STR("BndEvt__WBP_Component_ToggleSwitch_Generic_K2Node_ComponentBoundEvent_1_OnToggleStateChanged__DelegateSignature"), FNAME_Add),
                slid(STR("BndEvt__Slider_Contrast_K2Node_ComponentBoundEvent_1_HypeSliderValueChanged__DelegateSignature"), FNAME_Add);
        const FName name = fn->GetFName();
        if (name == selection)
        {
            Entry* e = row_entry(context, FNL_SETTING_CHOICE);
            auto* index = static_cast<int32*>(param(fn, parms, STR("SelectedIndex")));
            if (!e || !index || *index < 0 || *index >= static_cast<int32>(e->choices.size())) return;
            change(*e, e->values.empty() ? *index : e->values[*index]);
            return;
        }
        if (name == toggled)
        {
            Entry* e = row_entry(context, FNL_SETTING_TOGGLE);
            FProperty* p = find_field(fn, STR("isToggled"));
            if (!e || !p) return;
            change(*e, static_cast<FBoolProperty*>(p)->GetPropertyValue(p->ContainerPtrToValuePtr<void>(parms)) ? 1 : 0);
            return;
        }
        if (name == slid)
        {
            Entry* e = row_entry(context, FNL_SETTING_SLIDER);
            auto* alpha = static_cast<float*>(param(fn, parms, STR("newValue")));
            if (!e || !alpha) return;
            change(*e, snap(*e, static_cast<float>(e->min) + *alpha * static_cast<float>(e->max - e->min)));
            return;
        }
        if ((name == initialized || name == activated) && context->GetClassPrivate()->GetFName() == settings_screen)
        {
            try
            {
                if (name == initialized) build_tab(context);
                else refresh(context);
            }
            catch (const std::exception& ex)
            {
                Output::send<LogLevel::Error>(STR("[settings] Mods tab: exception {}\n"), to_wstring(ex.what()));
            }
        }
    }
} // namespace

auto settings_add(FnlMod* mod, const wchar_t* group, const FnlSetting& row) -> bool
{
    const bool kind_ok = row.kind == FNL_SETTING_TOGGLE || (row.kind == FNL_SETTING_CHOICE && row.choices && row.choice_count > 0) ||
                         (row.kind == FNL_SETTING_SLIDER && row.max > row.min && row.step > 0);
    const std::string key = row.key ? row.key : "";
    const bool key_ok = !key.empty() && key.find_first_of("= \t;#\r\n") == std::string::npos;
    if (row.size < sizeof(FnlSetting) || !key_ok || !row.label || !kind_ok || (!mod && !group))
    {
        Output::send<LogLevel::Error>(STR("[settings] {}: malformed row - not added\n"), to_wstring(key));
        return false;
    }
    if (find(key.c_str()))
    {
        Output::send<LogLevel::Error>(STR("[settings] {}: key already registered - not added\n"), to_wstring(key));
        return false;
    }
    auto e = std::make_unique<Entry>();
    e->mod = mod;
    e->group = group ? group : STR("");
    e->key = key;
    e->label = row.label;
    e->kind = row.kind;
    e->def = row.default_value;
    if (row.kind == FNL_SETTING_CHOICE)
    {
        for (uint32_t i = 0; i < row.choice_count; ++i) e->choices.emplace_back(row.choices[i] ? row.choices[i] : STR(""));
        if (row.choice_values) e->values.assign(row.choice_values, row.choice_values + row.choice_count);
    }
    e->min = row.min, e->max = row.max, e->step = row.step;
    e->on_change = row.on_change;
    e->ctx = row.ctx;

    e->value = e->def;
    const auto ini = read_ini();
    if (auto it = ini.find(key); it != ini.end())
    {
        char* end = nullptr;
        const long v = std::strtol(it->second.c_str(), &end, 10);
        const bool number = end && end != it->second.c_str() && *end == '\0';
        const bool valid = number && (e->kind != FNL_SETTING_CHOICE || !e->values.empty() || (v >= 0 && v < static_cast<long>(e->choices.size())));
        if (valid)
        {
            e->value = e->kind == FNL_SETTING_TOGGLE ? (v != 0) : static_cast<int32>(v);
            e->saved = true;
        }
        else
            Output::send<LogLevel::Warning>(STR("[settings] {}={} isn't a value it takes - the default stands\n"), to_wstring(key), to_wstring(it->second));
    }
    else if (e->kind == FNL_SETTING_TOGGLE)
        e->value = e->def != 0;
    Entry& added = *e;
    g_entries.push_back(std::move(e));
    Output::send<LogLevel::Verbose>(STR("[settings] {}: {} = {} ({})\n"), group_key(added), to_wstring(key), describe(added), added.saved ? STR("saved") : STR("default"));
    if (added.on_change) added.on_change(added.ctx, added.value, FNL_SETTING_INITIAL | (added.saved ? FNL_SETTING_SAVED : 0));
    append_late(added);
    return true;
}

auto settings_value(const char* key) -> int32_t
{
    const Entry* e = find(key);
    return e ? e->value : 0;
}

auto settings_set(const char* key, int32_t value) -> void
{
    Entry* e = find(key);
    if (!e) return;
    const bool changed = value != e->value || !e->saved;
    e->value = value;
    e->saved = true;
    if (changed) save();
    show(*e);
}

auto settings_enable(const char* key, bool on) -> void
{
    if (!key) return;
    g_enabled[key] = on;
    if (Entry* e = find(key)) show(*e);
}

namespace
{
    // ---------------------------------------------------------------- About FUSER ------------------------------
    FProperty* g_about_return = nullptr; // GetBuildVersionPopupString's ReturnValue (an FText)

    auto on_about_text(UnrealScriptFunctionCallableContext& ctx, void*) -> void
    {
        if (!ctx.RESULT_DECL || !g_about_return) return;
        try
        {
            FText read{};
            g_about_return->CopyCompleteValue(&read, ctx.RESULT_DECL);
            StringType text = read.ToString() + STR("\n\nFUSERNativeLoader ") + to_wstring(FNL_VERSION);
            const std::vector<StringType> mods = fnl_mods_about();
            text += mods.empty() ? STR("\nNo mods installed") : STR("\nMods:");
            for (const StringType& line : mods) text += STR("\n    ") + line;
            const FText value(text);
            g_about_return->CopyCompleteValue(ctx.RESULT_DECL, &value);
        }
        catch (const std::exception& e)
        {
            Output::send<LogLevel::Error>(STR("[settings] About FUSER: exception {}\n"), to_wstring(e.what()));
        }
    }

    auto hook_about() -> void
    {
        UFunction* fn = find_fn(STR("/Script/Hype.HypeGameInstance:GetBuildVersionPopupString"));
        g_about_return = find_field(fn, STR("ReturnValue"));
        if (!fn || !g_about_return)
        {
            Output::send<LogLevel::Warning>(STR("[settings] About FUSER: GetBuildVersionPopupString {} - the popup won't list the mods\n"),
                                            fn ? STR("has no ReturnValue") : STR("not found"));
            return;
        }
        UObjectGlobals::RegisterHook(fn, [](UnrealScriptFunctionCallableContext&, void*) {}, &on_about_text, nullptr);
    }
} // namespace

auto settings_start() -> void
{
    on_unreal_init(&hook_about);
    Hook::RegisterProcessEventPostCallback([](Hook::TCallbackIterationData<void>&, UObject* self, UFunction* fn, void* parms) { on_process_event_post(self, fn, parms); });
    Hook::RegisterEngineTickPreCallback([](Hook::TCallbackIterationData<void>&, UEngine*, float, bool) {
        static uint32 n = 0;
        if (g_underlines_pending && ++n % kUnderlineEveryTicks == 0) place_underlines();
    });
}

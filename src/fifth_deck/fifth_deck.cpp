// The fifth deck: a deck bound to ETrackType::Deck0A (17), spawned at run time the way the Battle desk adds its extra
// decks (FUN_140610730: spawn ADesk.DeckClass, attach to the desk, append to ADesk.Decks). Outside Battle nothing uses
// track 17: Battle relabels its four decks Deck0A-Deck3B, and UAudioTrackMgr holds a silent UDummyAudioTrack for each
// of those labels. While the deck is up a real UAudioTrack takes track 17's place, playing a deck's stem event (see
// "the deck's audio track" below), so a disc dropped on the deck plays: ADeck's play (FUN_1405fee70) passes the deck's
// own track to the controller (FUN_140602940), which calls UAudioTrackMgr::SetTrackCel with it unchanged. Its sound is
// moved from that deck's mixer strip onto one of its own under bus:/Music/MusicSub, where it gets the master effects
// but no other deck's (see "the deck's own strip"). A smart riser plays the fifth deck's stem's riser too, on a second
// real track in track 18's place (see "the deck's riser"). UDeckState's per-track arrays (HypeGameState.DeckState: solo/mute
// at +0xb0, gain at +0xc0) are indexed by ETrackType and hold every track. Hot Clips keep their own track (Audition0).
// The desk is widened to take it: its mesh swapped for the five-deck desk (/Game/Mods/FuserNative/SM_WorkspaceDesk_5Deck,
// the stock desk with its arc lengthened) and the five decks (the fifth on the right) placed at the stock decks' spacing
// along the deck circle (see "widened desk" below). The deck's controls (BP_DeckUi) are a
// separate actor, spawned from Decks[3]'s and drawn above the desk's round buttons (BP_Desk_3dUI). The deck and its
// controls are attached to the desk, which the level script moves in and out. While the deck is up, UDeckState's
// solo and mute (which cover Deck0-3 only) are hooked to include track 17 (see "solo / mute" below), and the
// multiplayer stages' fades move its gain with the others (see "stage gains"). Apart from the deck, the Hot Clip
// Volume Normalisation toggle (HotClipVolume= in FuserNative.ini, off by default) stops a song audition leaving Hot
// Clips on the quieter preview event (see "Hot Clip track volume").
// The Fifth Deck toggle (FifthDeck= in FuserNative.ini) turns it on and off, on stage too. Both toggles are rows on
// the Settings screen's Mods tab, under this mod's heading (register_settings):
//   - on: the deck is shown in a freestyle set and in multiplayer sets, once the desk view is ready (see "putting the
//     deck up" below);
//   - off: no deck in a freestyle set (one already up is cleared and removed, the desk restored). In a multiplayer set
//     the deck is still put up, hidden: no controls, the desk left as it is and its discs hidden, so another player's
//     fifth deck plays, queues, swaps and lands on the beat here too (see "multiplayer" below).
// Not in Battle, the campaign's shows, recording or playback. Recordings, saved moments and the hottest mix keep to
// Decks[0-3] and leave the fifth deck out; the hottest mix's playback ejects it with the others. The game functions it uses are known for EGS v1.4.2, EGS CL1317808 and Steam
// CL1317755 (see "game functions, by build" below); on any other build the deck never goes up.
// Debug builds add Ctrl+Shift+5 (game window focused, deck shown): lay the desk out again and log the state; for 120 s
// after the deck goes up, track 17's audio state is logged on every change and the deck's events as they fire.
#include "fifth_deck.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "fue/fue.hpp"
#include "hook_range.hpp"
#include <funchook.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <vector>

using namespace fue;

namespace
{
    constexpr uint8 kTrack = 17;      // ETrackType::Deck0A
    constexpr uint8 kStemTrack = 3;   // ETrackType::Deck3: the deck whose stem event the fifth deck plays
    constexpr uint8 kRiserTrack = 18; // ETrackType::Deck1A: the fifth deck's riser
    constexpr uint8 kRiserFrom = 16;  // ETrackType::TransitionSong3: Deck3's riser track, whose event the fifth deck's riser plays
    constexpr int32 kTrackCount = 25; // ETrackType::Num
#if FNL_DEBUG
    constexpr uint64 kWatchMs = 120000;
#endif

    // ---------------------------------------------------------------- reflection helpers ---------
    auto find_field(UStruct* st, const CharType* name) -> FProperty*
    {
        if (!st) return nullptr;
        const FName n(name, FNAME_Add);
        for (FProperty* p : TFieldRange<FProperty>(st, EFieldIterationFlags::IncludeSuper | EFieldIterationFlags::IncludeDeprecated))
        {
            if (p->GetFName() == n) return p;
        }
        return nullptr;
    }

    auto find_fn(const CharType* path) -> UFunction* { return UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path); }

    auto parms_size(UFunction* fn) -> size_t
    {
        int32 end = 0;
        for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
        {
            end = std::max(end, p->GetOffset_Internal() + p->GetSize());
        }
        return static_cast<size_t>(end);
    }

    // A ProcessEvent call with its parameters constructed and destroyed through the engine's property ops.
    class Call
    {
      public:
        explicit Call(UFunction* fn) : m_fn(fn), m_buf(fn ? parms_size(fn) + 16 : 0, 0)
        {
            if (m_fn) each([&](FProperty* p) { p->InitializeValue_InContainer(m_buf.data()); });
        }
        explicit Call(const CharType* path) : Call(find_fn(path)) {}
        ~Call()
        {
            if (m_fn) each([&](FProperty* p) { p->DestroyValue_InContainer(m_buf.data()); });
        }
        Call(const Call&) = delete;
        Call& operator=(const Call&) = delete;

        auto addr(const CharType* name) -> uint8*
        {
            FProperty* p = m_fn ? find_field(m_fn, name) : nullptr;
            return p ? p->ContainerPtrToValuePtr<uint8>(m_buf.data()) : nullptr;
        }
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
            FProperty* p = m_fn ? find_field(m_fn, name) : nullptr;
            if (p && src) p->CopyCompleteValue(p->ContainerPtrToValuePtr<void>(m_buf.data()), src);
            return *this;
        }
        auto call(UObject* target) -> bool
        {
            if (!m_fn || !target) return false;
            target->ProcessEvent(m_fn, m_buf.data());
            return true;
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
        std::vector<uint8_t> m_buf;
    };

    // A function on the object's own class chain (Blueprint functions included).
    auto own_call_fn(UObject* o, const CharType* name) -> UFunction* { return o ? o->GetFunctionByNameInChain(name) : nullptr; }

    auto obj_prop(UObject* obj, const CharType* name) -> UObject*
    {
        auto* p = obj ? obj->GetValuePtrByPropertyNameInChain<UObject*>(name) : nullptr;
        return p ? *p : nullptr;
    }

    // Not a class default object or archetype (EObjectFlags @0x8: RF_ClassDefaultObject 0x10, RF_ArchetypeObject
    // 0x20), nor inside one: a widget Blueprint's WidgetArchetype holds a template of every widget in its tree, which
    // the screen's next instance is copied from
    auto is_live_object(UObject* o) -> bool
    {
        for (UObject* x = o; x; x = x->GetOuterPrivate())
            if ((*reinterpret_cast<const uint32*>(reinterpret_cast<const uint8*>(x) + 0x8) & 0x30) || x->GetName().rfind(STR("Default__"), 0) == 0) return false;
        return true;
    }

    auto live_of(const CharType* class_name) -> std::vector<UObject*>
    {
        std::vector<UObject*> all{}, live{};
        UObjectGlobals::FindAllOf(class_name, all);
        for (UObject* o : all)
            if (is_live_object(o)) live.push_back(o);
        return live;
    }

    auto name_of(UObject* o) -> StringType { return o ? o->GetName() : StringType{STR("-")}; }

    struct Vec
    {
        float x, y, z;
    };

    auto operator+(Vec a, Vec b) -> Vec { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    auto operator-(Vec a, Vec b) -> Vec { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    auto travel(Vec d) -> float { return std::abs(d.x) + std::abs(d.y) + std::abs(d.z); }

    // ---------------------------------------------------------------- raw reads ---------------------
    // A raw TArray { Data, Num, Max } (UDeckState's arrays are not reflected). Copied out under SEH, so a wrong offset
    // logs nonsense instead of faulting. (No C++ objects in here: __try can't unwind them.)
    struct RawArray
    {
        const uint8* data;
        int32 num;
        int32 max;
    };

    auto read_raw_array(const void* base, size_t off, RawArray* out) -> bool
    {
        __try
        {
            std::memcpy(out, static_cast<const uint8*>(base) + off, sizeof(RawArray));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto read_bytes(const uint8* src, uint8* dst, size_t n) -> bool
    {
        __try
        {
            std::memcpy(dst, src, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // ---------------------------------------------------------------- state -------------------------
    FWeakObjectPtr g_deck{};
    FWeakObjectPtr g_desk{};
    bool g_hidden = false;  // the deck is the hidden one (multiplayer, Fifth Deck off)
    bool g_enabled = false; // the Settings screen's Fifth Deck toggle
    constexpr uint64 kScanMs = 1000; // the least time between two scans of every object for an actor not found yet
#if FNL_DEBUG
    uint64 g_watch_until = 0;
    int g_event_lines = 0;

    auto game_has_focus() -> bool
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    auto pressed() -> bool
    {
        static bool was = false;
        const bool down = game_has_focus() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('5') & 0x8000);
        const bool edge = down && !was;
        was = down;
        return edge;
    }

    auto deck_track(UObject* deck) -> int
    {
        auto* t = deck ? deck->GetValuePtrByPropertyNameInChain<uint8>(STR("track")) : nullptr;
        return t ? static_cast<int>(*t) : -1;
    }
#endif

    // A getter returning an FVector or FRotator (Pitch, Yaw, Roll), on an actor or component
    auto vec_of(UObject* o, const CharType* fn) -> Vec
    {
        Call c(own_call_fn(o, fn));
        return c.call(o) ? c.get<Vec>(STR("ReturnValue")) : Vec{};
    }

    auto actor_location(UObject* actor) -> Vec { return vec_of(actor, STR("K2_GetActorLocation")); }
    auto actor_rotation(UObject* actor) -> Vec { return vec_of(actor, STR("K2_GetActorRotation")); }

    auto deck_ui(UObject* deck) -> UObject* { return obj_prop(deck, STR("DeckUi")); }

    auto attach_parent(UObject* actor) -> UObject*
    {
        Call c(own_call_fn(actor, STR("GetAttachParentActor")));
        return c.call(actor) ? c.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    auto desk_decks(UObject* desk) -> TArray<UObject*>* { return desk ? desk->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("Decks")) : nullptr; }

    // The first live actor of a class, remembered in `seen` and looked for again (a scan of every object) at most once
    // every kScanMs while there is none
    auto cached_live(const CharType* class_name, FWeakObjectPtr& seen, uint64& next_scan) -> UObject*
    {
        if (UObject* o = seen.Get()) return o;
        const uint64 now = GetTickCount64();
        if (now < next_scan) return nullptr;
        next_scan = now + kScanMs;
        const auto live = live_of(class_name);
        if (live.empty()) return nullptr;
        seen = FWeakObjectPtr(live.front());
        return live.front();
    }

    // The stage's desk: a live BP_Desk holding decks (not the Battle desk, a subclass)
    auto find_desk() -> UObject*
    {
        static FWeakObjectPtr seen{};
        static uint64 next_scan = 0;
        UObject* desk = cached_live(STR("BP_Desk_C"), seen, next_scan);
        auto* decks = desk_decks(desk);
        return decks && decks->Num() > 0 ? desk : nullptr;
    }

    // The desk's 3D UI (BP_Desk_3dUI: the round buttons and the per-deck FX indicators)
    auto find_desk_ui() -> UObject*
    {
        static FWeakObjectPtr seen{};
        static uint64 next_scan = 0;
        return cached_live(STR("BP_Desk_3dUI_C"), seen, next_scan);
    }

    auto game_state_of(UObject* context) -> UObject*
    {
        static UObject* statics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
        Call gs(STR("/Script/Engine.GameplayStatics:GetGameState"));
        gs.set<UObject*>(STR("WorldContextObject"), context);
        return gs.call(statics) ? gs.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    // HypeGameState.DeckState, through the desk's world
    auto deck_state(UObject* desk) -> UObject* { return obj_prop(game_state_of(desk), STR("DeckState")); }

    // The game instance's UAudioTrackMgr: one audio track per ETrackType (Tracks)
    auto audio_track_mgr(UObject* context) -> UObject*
    {
        static UObject* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Hype.Default__HypeGameInstance"));
        Call gi(STR("/Script/Hype.HypeGameInstance:GetHypeGameInstance"));
        gi.set<UObject*>(STR("contextObj"), context);
        UObject* audio = gi.call(cdo) ? obj_prop(gi.get<UObject*>(STR("ReturnValue")), STR("theHypeAudio")) : nullptr;
        Call mgr(STR("/Script/Hype.HypeAudio:GetAudioTrackMgr"));
        return mgr.call(audio) ? mgr.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

#if FNL_DEBUG
    // One of UDeckState's per-track arrays, as "[i]..." for each entry (stride bytes, formatted by `fmt`)
    template <typename F>
    auto deck_state_array(UObject* ds, size_t off, size_t stride, F&& fmt, RawArray* out) -> StringType
    {
        *out = RawArray{};
        if (!read_raw_array(ds, off, out)) return STR("unreadable");
        StringType entries{};
        if (out->data && out->num > 0 && out->num <= 64)
        {
            uint8 raw[64 * 4]{};
            if (read_bytes(out->data, raw, static_cast<size_t>(out->num) * stride))
                for (int32 i = 0; i < out->num; ++i) entries += std::format(STR("{}[{}]{}"), i ? STR(" ") : STR(""), i, fmt(raw + static_cast<size_t>(i) * stride));
        }
        return entries;
    }

    auto log_decks(UObject* desk) -> void
    {
        auto* decks = desk_decks(desk);
        if (!decks) return;
        for (int32 i = 0; i < decks->Num(); ++i)
        {
            UObject* d = (*decks)[i];
            const Vec l = d ? actor_location(d) : Vec{};
            const Vec r = d ? actor_rotation(d) : Vec{};
            UObject* ui = deck_ui(d);
            const Vec lu = ui ? actor_location(ui) : Vec{};
            auto* ui_track = ui ? ui->GetValuePtrByPropertyNameInChain<uint8>(STR("TrackType")) : nullptr;
            Output::send<LogLevel::Normal>(STR("[5deck]   Decks[{}] {} track {} at ({:.1f}, {:.1f}, {:.1f}) yaw {:.1f}; DeckUi {} (track {}) at ({:.1f}, {:.1f}, {:.1f}) on {}\n"), i,
                                           name_of(d), deck_track(d), l.x, l.y, l.z, r.y, name_of(ui), ui_track ? static_cast<int>(*ui_track) : -1, lu.x, lu.y, lu.z,
                                           name_of(ui ? attach_parent(ui) : nullptr));
        }
    }

    // UDeckState's per-track arrays (EGS v1.4.2 layout), both indexed by ETrackType with no bounds check:
    // FDeckMuteSoloState { isMutePressed, isSoloPressed } at +0xb0 (FUN_1405f9f40 / FUN_1405f9f50) and the deck gain
    // floats at +0xc0 (GetDeckGain, FUN_1405f9a70).
    auto log_deck_state(UObject* desk) -> void
    {
        UObject* ds = deck_state(desk);
        if (!ds)
        {
            Output::send<LogLevel::Warning>(STR("[5deck] no HypeGameState.DeckState\n"));
            return;
        }
        RawArray a{};
        const StringType solo = deck_state_array(ds, 0xb0, 2, [](const uint8* e) { return std::format(STR("m{}s{}"), e[0], e[1]); }, &a);
        Output::send<LogLevel::Normal>(STR("[5deck] {} solo/mute (+0xb0): Num {} Max {} - {} {}\n"), ds->GetName(), a.num, a.max,
                                       a.num > kTrack ? STR("holds track 17") : STR("TOO SHORT for track 17"), solo);
        const StringType gain = deck_state_array(ds, 0xc0, 4, [](const uint8* e) { return std::format(STR("{:.2f}"), *reinterpret_cast<const float*>(e)); }, &a);
        Output::send<LogLevel::Normal>(STR("[5deck] {} deck gain (+0xc0): Num {} Max {} - {} {}\n"), ds->GetName(), a.num, a.max,
                                       a.num > kTrack ? STR("holds track 17") : STR("TOO SHORT for track 17"), gain);
    }

    // Track 17's entries in UDeckState's arrays, when they exist
    auto deck_state_line(UObject* desk) -> StringType
    {
        UObject* ds = deck_state(desk);
        RawArray solo{}, gain{};
        if (!ds || !read_raw_array(ds, 0xb0, &solo) || !read_raw_array(ds, 0xc0, &gain)) return STR("deck state -");
        uint8 sm[2]{};
        float g = -1.0f;
        const bool have_sm = solo.data && solo.num > kTrack && read_bytes(solo.data + kTrack * 2, sm, 2);
        const bool have_g = gain.data && gain.num > kTrack && read_bytes(gain.data + kTrack * 4, reinterpret_cast<uint8*>(&g), 4);
        return std::format(STR("deck state mute {} solo {} gain {}"), have_sm ? std::format(STR("{}"), sm[0]) : STR("-"), have_sm ? std::format(STR("{}"), sm[1]) : STR("-"),
                           have_g ? std::format(STR("{:.2f}"), g) : STR("-"));
    }

    auto track_line(UObject* mgr) -> StringType
    {
        Call cel(STR("/Script/Hype.AudioTrackMgr:GetTrackCelShortname"));
        Call playing(STR("/Script/Hype.AudioTrackMgr:IsAnythingPlayingOnTrack"));
        Call muted(STR("/Script/Hype.AudioTrackMgr:GetTrackIsMuted"));
        Call gain(STR("/Script/Hype.AudioTrackMgr:GetGainForTrack"));
        for (Call* c : {&cel, &playing, &muted, &gain}) c->set<uint8>(STR("trackType"), kTrack).call(mgr);
        return std::format(STR("cel {} playing {} muted {} gain {:.2f}"), cel.get<FName>(STR("ReturnValue")).ToString(), static_cast<int>(playing.get<bool>(STR("ReturnValue"))),
                           static_cast<int>(muted.get<bool>(STR("ReturnValue"))), gain.get<float>(STR("ReturnValue")));
    }

    auto log_state(UObject* desk, UObject* deck) -> void
    {
        Output::send<LogLevel::Normal>(STR("[5deck] state ({}): desk {}, fifth deck {} (track {}), playing {}, queued {}\n"), build_name(), name_of(desk), name_of(deck),
                                       deck_track(deck), name_of(obj_prop(deck, STR("CurPlayingDisc"))), name_of(obj_prop(deck, STR("CurQueuedDisc"))));
        log_decks(desk);
        log_deck_state(desk);
        if (UObject* mgr = audio_track_mgr(desk)) Output::send<LogLevel::Normal>(STR("[5deck] audio track 17: {}\n"), track_line(mgr));
    }
#endif

    // (x, y) turned by `deg` about Z
    auto yaw_turn(Vec v, float deg) -> Vec
    {
        const float r = deg * 3.14159265f / 180.0f, c = std::cos(r), s = std::sin(r);
        return {v.x * c - v.y * s, v.x * s + v.y * c, v.z};
    }

    // ---------------------------------------------------------------- widened desk ------------------
    // The desk's four decks stand on a circle about a point on its centre line (Decks[2] and [3] fix it), each turned
    // and tilted by its angle on it, on the desk's sloping top. The desk's mesh is swapped for the five-deck one
    // (SM_WorkspaceDesk_5Deck, built by tools/desk_5deck_fbx.py with its deck places kMeshStep apart). The stock desk
    // is a surface of revolution about a vertical axis (see mesh_axis; about 11 units nearer the player than the deck
    // circle's centre); the five-deck one is the stock one with everything beyond the stock outer decks (its raised
    // corners and ends) turned outwards about that axis, and the middle lengthened to meet them.
    // Five decks stand on the deck circle, kMeshStep degrees apart: Decks[0-3] in the first four places, the fifth deck
    // in the last (the right end). Each deck sits on the desk's slope, faces out from the circle, and takes the turn and
    // tilt the original decks have at that facing; its controls take the offset, turn and tilt the original decks'
    // controls have there. Both come from Decks[2] and [3] (mirrored on the left): odd values (turn, sideways offset
    // and tilt) pass through zero at the centre, even ones (forward tilt, offset) hold Decks[2]'s inside it. The
    // per-deck FX indicators move with their decks. The round buttons (BP_Desk_3dUI) turn with the desk's ends about
    // the mesh's axis, place and facing, so each keeps its stock pose on the raised corner beneath it. The beat
    // timeline across the desk's back is stretched to the five-deck desk's width.
    constexpr const CharType* kFiveDeckMesh = STR("/Game/Mods/FuserNative/SM_WorkspaceDesk_5Deck.SM_WorkspaceDesk_5Deck");
    constexpr float kMeshStep = 19.8f;      // the five-deck mesh's deck spacing (desk_5deck_fbx.py --step)
    constexpr float kStockDeskEnd = 45.24f; // the work surface's ends, degrees from the centre line: stock
    constexpr float kMeshDeskEnd = 55.14f;  // and five-deck (turned out by 2 * kMeshStep less the stock outer decks' 29.7)
    constexpr float kRad = 3.14159265f / 180.0f;

    struct Pose
    {
        Vec loc, rot;
    };

    // A part of the desk's 3D UI: a round button (deck -1) or a deck's FX indicator, moved with that deck
    struct DeskPart
    {
        FWeakObjectPtr com;
        Vec loc;
        Vec rot;
        int deck;
    };

    struct Vec2
    {
        float x, y;
    };

    // A HUD panel laid out (WBP_Hud) beside one of the desk's round buttons: its tooltip, or the canvas panel holding
    // its FX tooltip and FX options. It is moved on
    // screen as far as its button appears to move, through its canvas slot's position when it has one, else its
    // render translation, but kept on screen: a tooltip's visible box reaches from left to right (HUD units) about
    // its slot's centre.
    struct HudPanel
    {
        FWeakObjectPtr widget;
        size_t part; // index in Original::parts: its button
        bool by_slot;
        Vec2 at;     // the slot's position or the render translation, as laid out
        float left, right;
        float anchor_x;  // the slot's anchor, across the HUD
        float centre_x;  // the slot's centre from its position: (0.5 - alignment) * size
    };

    // The desk as the level placed it, taken before anything moves, so every layout is computed from it
    struct Original
    {
        Pose decks[4];
        Pose uis[4];
        Vec desk_at; // where the desk stood: everything here is restored shifted by how far it has moved since
        float desk_y;
        FWeakObjectPtr mesh;
        FWeakObjectPtr stock_mesh; // the UStaticMesh the level gave it
        FWeakObjectPtr timeline;
        Vec timeline_scale;
        std::vector<DeskPart> parts;
        std::vector<HudPanel> panels;
    };
    Original g_original{};

    struct Layout
    {
        Pose decks[5];
        Pose uis[5];
        float centre_x, radius; // the deck circle
        float turn;             // how far the desk's ends, and so the round buttons, turn outwards about the mesh's axis
    };

    auto beat_timeline() -> UObject*
    {
        for (UObject* w : live_of(STR("BP_3DHypeWidget_C")))
            if (w->GetName().find(STR("BeatTimeline")) != StringType::npos) return w;
        return nullptr;
    }

    // The five-deck mesh from FUSERNativeMods_P.pak, loaded (through AssetRegistryHelpers::GetAsset, which loads by
    // object path) the first time it is wanted and rooted, as only the desk's component holds it while it is up
    auto five_deck_mesh() -> UObject*
    {
        static FWeakObjectPtr mesh{};
        static bool tried = false;
        if (UObject* m = mesh.Get()) return m;
        if (tried) return nullptr;
        tried = true;
        UObject* m = UObjectGlobals::StaticFindObject(nullptr, nullptr, kFiveDeckMesh);
        if (!m)
        {
            Call get(STR("/Script/AssetRegistry.AssetRegistryHelpers:GetAsset"));
            auto* sp = CastField<FStructProperty>(find_field(find_fn(STR("/Script/AssetRegistry.AssetRegistryHelpers:GetAsset")), STR("InAssetData")));
            FProperty* object_path = sp && sp->GetStruct().Get() ? find_field(sp->GetStruct().Get(), STR("ObjectPath")) : nullptr;
            if (uint8* data = get.addr(STR("InAssetData")); data && object_path)
            {
                *object_path->ContainerPtrToValuePtr<FName>(data) = FName(kFiveDeckMesh, FNAME_Add);
                get.call(UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/AssetRegistry.Default__AssetRegistryHelpers")));
            }
            m = UObjectGlobals::StaticFindObject(nullptr, nullptr, kFiveDeckMesh);
        }
        if (!m)
        {
            Output::send<LogLevel::Warning>(STR("[5deck] {} not found (FUSERNativeMods_P.pak): no fifth deck\n"), kFiveDeckMesh);
            return nullptr;
        }
        if (!m->IsRootSet()) m->SetRootSet();
        mesh = FWeakObjectPtr(m);
        return m;
    }

    // The HUD's panels for the round buttons, as laid out: the Global (key / tempo) and Transition tooltips in WBP_Hud,
    // and the FX panels in its WBP_Hud_Fx (the master one beside the FX button, one beside each deck's FX indicator).
    // Each FX tooltip (shown while its deck is hovered) shares a canvas panel with that deck's FX options
    // (WBP_SingleDeckFX_Options, opened from it), so that canvas panel is what moves, taking both with it. Found as
    // live widgets by class and name: WBP_Hud's own references to them can be null. The visible boxes are the
    // tooltips' Instruments panels: the Global one's 264 wide, its right edge 70 left of the tooltip's centre; the
    // Transition one's content from 83 to about 264 right of it.
    auto take_hud_panels(UObject* desk_ui, Original* o) -> void
    {
        const struct
        {
            const CharType* button;
            const CharType* cls;
            const CharType* panel;
            float left, right;
            bool by_parent; // its parent canvas panel moves
        } pairs[] = {{STR("3d_Global_HypeWidget"), STR("WBP_GlobalControlsTrigger_Tooltip_C"), STR("WBP_GlobalControlsTrigger_Tooltip"), -334.0f, -70.0f, false},
                     {STR("3d_Transition_HypeWidget"), STR("WBP_TransitionTrigger_Tooltip_C"), STR("WBP_TransitionTrigger_Tooltip"), 83.0f, 264.0f, false},
                     {STR("3d_FX_HypeWidget"), STR("WBP_FxPanel_Tooltip_C"), STR("WBP_FxPanel_Tooltip_Master"), 0.0f, 0.0f, true},
                     {STR("3d_FX_Indicator_0"), STR("WBP_FxPanel_Tooltip_C"), STR("WBP_FxPanel_Tooltip"), 0.0f, 0.0f, true},
                     {STR("3d_FX_Indicator_1"), STR("WBP_FxPanel_Tooltip_C"), STR("WBP_FxPanel_Tooltip_1"), 0.0f, 0.0f, true},
                     {STR("3d_FX_Indicator_2"), STR("WBP_FxPanel_Tooltip_C"), STR("WBP_FxPanel_Tooltip_2"), 0.0f, 0.0f, true},
                     {STR("3d_FX_Indicator_3"), STR("WBP_FxPanel_Tooltip_C"), STR("WBP_FxPanel_Tooltip_3"), 0.0f, 0.0f, true}};
        for (const auto& p : pairs)
        {
            UObject* button = obj_prop(desk_ui, p.button);
            size_t part = o->parts.size();
            for (size_t i = 0; i < o->parts.size(); ++i)
                if (button && o->parts[i].com.Get() == button) part = i;
            int found = 0;
            for (UObject* tooltip : live_of(p.cls))
            {
                if (part == o->parts.size() || tooltip->GetName() != p.panel) continue;
                UObject* panel = tooltip;
                if (p.by_parent)
                {
                    Call parent(own_call_fn(tooltip, STR("GetParent")));
                    panel = parent.call(tooltip) ? parent.get<UObject*>(STR("ReturnValue")) : nullptr;
                    if (!panel) continue; // in no panel
                }
                ++found;
                UObject* slot = obj_prop(panel, STR("Slot"));
                Call get(own_call_fn(slot, STR("GetPosition")));
                if (own_call_fn(slot, STR("SetPosition")) && get.call(slot))
                {
                    Call anchors(own_call_fn(slot, STR("GetAnchors")));   // FAnchors { Minimum, Maximum }
                    Call alignment(own_call_fn(slot, STR("GetAlignment")));
                    Call size(own_call_fn(slot, STR("GetSize")));
                    const float anchor_x = anchors.call(slot) ? anchors.get<Vec2>(STR("ReturnValue")).x : 0.0f;
                    const float centre_x = alignment.call(slot) && size.call(slot)
                                               ? (0.5f - alignment.get<Vec2>(STR("ReturnValue")).x) * size.get<Vec2>(STR("ReturnValue")).x
                                               : 0.0f;
                    o->panels.push_back({FWeakObjectPtr(panel), part, true, get.get<Vec2>(STR("ReturnValue")), p.left, p.right, anchor_x, centre_x});
                    continue;
                }
                auto* transform = panel->GetValuePtrByPropertyNameInChain<Vec2>(STR("RenderTransform")); // FWidgetTransform::Translation first
                o->panels.push_back({FWeakObjectPtr(panel), part, false, transform ? *transform : Vec2{}, 0.0f, 0.0f, 0.0f, 0.0f});
            }
#if FNL_DEBUG
            Output::send<LogLevel::Normal>(STR("[5deck] HUD panel {}{} for {}: {} live{}\n"), p.panel, p.by_parent ? STR("'s canvas panel") : STR(""), p.button, found,
                                           part == o->parts.size() ? STR(" (button not on the desk UI)") : STR(""));
#else
            (void)found;
#endif
        }
    }

    auto take_original(UObject* desk) -> bool
    {
        auto* decks = desk_decks(desk);
        UObject* mesh = obj_prop(desk, STR("SM_Desk_v4"));
        if (!decks || decks->Num() < 4 || !mesh) return false;
        Original o{};
        for (int i = 0; i < 4; ++i)
        {
            UObject* d = (*decks)[i];
            UObject* ui = deck_ui(d);
            o.decks[i] = {actor_location(d), actor_rotation(d)};
            o.uis[i] = ui ? Pose{actor_location(ui), actor_rotation(ui)} : o.decks[i];
        }
        o.desk_at = actor_location(desk);
        o.desk_y = o.desk_at.y;
        o.mesh = FWeakObjectPtr(mesh);
        // a desk still wearing the five-deck mesh keeps the stock mesh already taken
        UObject* worn = obj_prop(mesh, STR("StaticMesh"));
        o.stock_mesh = worn && worn == five_deck_mesh() ? g_original.stock_mesh : FWeakObjectPtr(worn);
        UObject* timeline = beat_timeline();
        o.timeline = FWeakObjectPtr(timeline);
        o.timeline_scale = vec_of(timeline, STR("GetActorScale3D"));
        if (UObject* desk_ui = find_desk_ui())
        {
            const struct
            {
                const CharType* name;
                int deck;
            } parts[] = {{STR("3d_FX_HypeWidget"), -1},   {STR("3d_Global_HypeWidget"), -1}, {STR("3d_Transition_HypeWidget"), -1},
                         {STR("3d_FX_Indicator_0"), 0},  {STR("3d_FX_Indicator_1"), 1},    {STR("3d_FX_Indicator_2"), 2},
                         {STR("3d_FX_Indicator_3"), 3}};
            for (const auto& p : parts)
            {
                if (UObject* com = obj_prop(desk_ui, p.name))
                    o.parts.push_back({FWeakObjectPtr(com), vec_of(com, STR("K2_GetComponentLocation")), vec_of(com, STR("K2_GetComponentRotation")), p.deck});
            }
            take_hud_panels(desk_ui, &o);
        }
        g_original = std::move(o);
        return true;
    }

    // A value even in the angle (tilt forwards, offset towards the player): Decks[2]'s inside its angle, then linear
    // through Decks[3]'s
    auto even_at(float a, float a2, float v2, float a3, float v3) -> float
    {
        const float m = std::abs(a);
        return m <= a2 ? v2 : v2 + (v3 - v2) * (m - a2) / (a3 - a2);
    }

    // A value odd in the angle (turn, sideways offset and tilt): zero at the centre, through Decks[2] and [3]'s
    auto odd_at(float a, float a2, float v2, float a3, float v3) -> float
    {
        const float m = std::abs(a);
        const float v = m <= a2 ? v2 * m / a2 : v2 + (v3 - v2) * (m - a2) / (a3 - a2);
        return a < 0 ? -v : v;
    }

    auto wrap_degrees(float d) -> float
    {
        while (d >= 180.0f) d -= 360.0f;
        while (d < -180.0f) d += 360.0f;
        return d;
    }

    // A deck's controls in its own frame: offset (turned back by the deck's yaw), turn away from facing the deck
    // head-on (yaw - deck yaw - 180), forward and sideways tilt
    struct UiFrame
    {
        Vec offset;
        float turn, pitch, roll;
    };

    auto ui_frame(const Pose& deck, const Pose& ui) -> UiFrame
    {
        return {yaw_turn(ui.loc - deck.loc, -deck.rot.y), wrap_degrees(ui.rot.y - deck.rot.y - 180.0f), ui.rot.x, ui.rot.z};
    }

    // The five decks' places; none without the five-deck mesh
    auto compute_layout(Layout* out) -> bool
    {
        const Original& o = g_original;
        if (!five_deck_mesh()) return false;

        // the circle through Decks[2] and [3], centred on the desk's centre line
        const Vec p2 = o.decks[2].loc, p3 = o.decks[3].loc;
        const float y2 = p2.y - o.desk_y, y3 = p3.y - o.desk_y;
        if (std::abs(p3.x - p2.x) < 0.01f) return false;
        const float cx = (p3.x * p3.x + y3 * y3 - p2.x * p2.x - y2 * y2) / (2.0f * (p3.x - p2.x));
        const float radius = std::hypot(p2.x - cx, y2);
        const float a2 = std::atan2(y2, p2.x - cx) / kRad, a3 = std::atan2(y3, p3.x - cx) / kRad;
        const Pose& d2 = o.decks[2];
        const Pose& d3 = o.decks[3];
        const UiFrame u2 = ui_frame(d2, o.uis[2]), u3 = ui_frame(d3, o.uis[3]);
        const float slope = (d3.loc.z - d2.loc.z) / (d3.loc.x - d2.loc.x); // the desk's top falls away from the player

        for (int i = 0; i < 5; ++i)
        {
            const float a = static_cast<float>(i - 2) * kMeshStep; // the place's angle on the circle, which it faces
            const float x = cx + radius * std::cos(a * kRad);
            const Pose deck{{x, o.desk_y + radius * std::sin(a * kRad), d2.loc.z + (x - d2.loc.x) * slope},
                            {even_at(a, a2, d2.rot.x, a3, d3.rot.x), odd_at(a, a2, d2.rot.y, a3, d3.rot.y), odd_at(a, a2, d2.rot.z, a3, d3.rot.z)}};
            const Vec offset{even_at(a, a2, u2.offset.x, a3, u3.offset.x), odd_at(a, a2, u2.offset.y, a3, u3.offset.y), even_at(a, a2, u2.offset.z, a3, u3.offset.z)};
            out->decks[i] = deck;
            out->uis[i] = {deck.loc + yaw_turn(offset, deck.rot.y),
                           {even_at(a, a2, u2.pitch, a3, u3.pitch), deck.rot.y + 180.0f + odd_at(a, a2, u2.turn, a3, u3.turn), odd_at(a, a2, u2.roll, a3, u3.roll)}};
        }
        out->centre_x = cx;
        out->radius = radius;
        out->turn = 2.0f * kMeshStep - a3;
        return true;
    }

    auto set_pose(UObject* actor, const Pose& p) -> void
    {
        Call move(own_call_fn(actor, STR("K2_SetActorLocation")));
        move.set<Vec>(STR("NewLocation"), p.loc).set<bool>(STR("bSweep"), false).set<bool>(STR("bTeleport"), true).call(actor);
        Call turn(own_call_fn(actor, STR("K2_SetActorRotation")));
        turn.set<Vec>(STR("NewRotation"), p.rot).set<bool>(STR("bTeleportPhysics"), true).call(actor);
    }

    auto set_world_location(UObject* com, Vec loc) -> void
    {
        Call c(own_call_fn(com, STR("K2_SetWorldLocation")));
        c.set<Vec>(STR("NewLocation"), loc).set<bool>(STR("bSweep"), false).set<bool>(STR("bTeleport"), true).call(com);
    }

    auto set_world_rotation(UObject* com, Vec rot) -> void
    {
        Call c(own_call_fn(com, STR("K2_SetWorldRotation")));
        c.set<Vec>(STR("NewRotation"), rot).set<bool>(STR("bSweep"), false).set<bool>(STR("bTeleport"), true).call(com);
    }

    auto math_lib() -> UObject*
    {
        static UObject* math = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetMathLibrary"));
        return math;
    }

    // The desk mesh's arc axis in the world: the stock and five-deck work surfaces are surfaces of revolution about the
    // local line x = kMeshAxisX, y = 0 (parallel to local Z), and the five-deck desk's ends are turned about it. Not
    // the deck circle's centre: the level's decks stand on a circle about a point about 11 units further out.
    constexpr float kMeshAxisX = -110.86f;

    struct Axis
    {
        Vec at, up;
    };

    auto mesh_axis(UObject* mesh, Axis* out) -> bool
    {
        Call to_world(own_call_fn(mesh, STR("K2_GetComponentToWorld")));
        Call where(STR("/Script/Engine.KismetMathLibrary:TransformLocation"));
        Call up(own_call_fn(mesh, STR("GetUpVector")));
        if (!mesh || !to_world.call(mesh) || !up.call(mesh)) return false;
        if (!where.copy_in(STR("T"), to_world.addr(STR("ReturnValue"))).set<Vec>(STR("Location"), Vec{kMeshAxisX, 0.0f, 0.0f}).call(math_lib())) return false;
        *out = {where.get<Vec>(STR("ReturnValue")), up.get<Vec>(STR("ReturnValue"))};
        return true;
    }

    // A point and a rotation turned `deg` about an axis
    auto turn_about(const Axis& axis, float deg, Vec loc, Vec rot, Vec* loc_out, Vec* rot_out) -> bool
    {
        Call v(STR("/Script/Engine.KismetMathLibrary:RotateAngleAxis"));
        Call r(STR("/Script/Engine.KismetMathLibrary:RotatorFromAxisAndAngle"));
        Call compose(STR("/Script/Engine.KismetMathLibrary:ComposeRotators"));
        if (!v.set<Vec>(STR("InVect"), loc - axis.at).set<float>(STR("AngleDeg"), deg).set<Vec>(STR("Axis"), axis.up).call(math_lib())) return false;
        if (!r.set<Vec>(STR("Axis"), axis.up).set<float>(STR("Angle"), deg).call(math_lib())) return false;
        if (!compose.set<Vec>(STR("A"), rot).set<Vec>(STR("B"), r.get<Vec>(STR("ReturnValue"))).call(math_lib())) return false; // A, then B
        *loc_out = axis.at + v.get<Vec>(STR("ReturnValue"));
        *rot_out = compose.get<Vec>(STR("ReturnValue"));
        return true;
    }

    // KeepWorld, as BP_Desk attaches its decks
    auto attach_to(UObject* actor, UObject* parent) -> void
    {
        Call attach(own_call_fn(actor, STR("K2_AttachToActor")));
        attach.set<UObject*>(STR("ParentActor"), parent)
            .set<uint8>(STR("LocationRule"), 1)
            .set<uint8>(STR("RotationRule"), 1)
            .set<uint8>(STR("ScaleRule"), 1)
            .set<bool>(STR("bWeldSimulatedBodies"), true)
            .call(actor);
    }

    // Spawn deferred, so `before` runs on the actor ahead of its construction script and BeginPlay
    template <typename F>
    auto spawn_deferred(UObject* context, UObject* cls, const uint8* transform, UObject* owner, F&& before) -> UObject*
    {
        static UObject* statics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
        Call begin(STR("/Script/Engine.GameplayStatics:BeginDeferredActorSpawnFromClass"));
        begin.set<UObject*>(STR("WorldContextObject"), context)
            .set<UObject*>(STR("ActorClass"), cls)
            .copy_in(STR("SpawnTransform"), transform)
            .set<uint8>(STR("CollisionHandlingOverride"), 1) // AlwaysSpawn
            .set<UObject*>(STR("Owner"), owner);
        UObject* actor = begin.call(statics) ? begin.get<UObject*>(STR("ReturnValue")) : nullptr;
        if (!actor) return nullptr;
        before(actor);
        Call finish(STR("/Script/Engine.GameplayStatics:FinishSpawningActor"));
        finish.set<UObject*>(STR("Actor"), actor).copy_in(STR("SpawnTransform"), transform).call(statics);
        return actor;
    }

#if FNL_DEBUG
    auto log_layout(const Layout& lay) -> void
    {
        Output::send<LogLevel::Normal>(STR("[5deck] five-deck desk: decks {:.1f} deg apart, round buttons turned {:.1f} deg, deck circle centre x {:.1f} radius {:.1f}\n"), kMeshStep,
                                       lay.turn, lay.centre_x, lay.radius);
        for (int i = 0; i < 5; ++i)
            Output::send<LogLevel::Normal>(STR("[5deck]   place {}: deck ({:.1f}, {:.1f}, {:.1f}) pitch {:.1f} yaw {:.1f} roll {:.1f}; controls ({:.1f}, {:.1f}, {:.1f}) yaw {:.1f}\n"), i,
                                           lay.decks[i].loc.x, lay.decks[i].loc.y, lay.decks[i].loc.z, lay.decks[i].rot.x, lay.decks[i].rot.y, lay.decks[i].rot.z, lay.uis[i].loc.x,
                                           lay.uis[i].loc.y, lay.uis[i].loc.z, lay.uis[i].rot.y);
    }
#endif

    auto first_player(UObject* world_context) -> UObject*
    {
        static UObject* statics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
        Call pc(STR("/Script/Engine.GameplayStatics:GetPlayerController"));
        pc.set<UObject*>(STR("WorldContextObject"), world_context).set<int32>(STR("PlayerIndex"), 0);
        return pc.call(statics) ? pc.get<UObject*>(STR("ReturnValue")) : nullptr;
    }

    auto camera_location(UObject* player, Vec* out) -> bool
    {
        UObject* camera = obj_prop(player, STR("PlayerCameraManager"));
        Call c(own_call_fn(camera, STR("GetCameraLocation")));
        if (!camera || !c.call(camera)) return false;
        *out = c.get<Vec>(STR("ReturnValue"));
        return true;
    }

    // Where the camera is and which way it looks (Pitch, Yaw, Roll)
    struct CameraPose
    {
        Vec at, turn;
    };

    auto camera_pose(UObject* player, CameraPose* out) -> bool
    {
        UObject* camera = obj_prop(player, STR("PlayerCameraManager"));
        Call turn(own_call_fn(camera, STR("GetCameraRotation")));
        if (!camera_location(player, &out->at) || !turn.call(camera)) return false;
        out->turn = turn.get<Vec>(STR("ReturnValue"));
        return true;
    }

    // How far apart two camera poses are: the larger of the distance moved and the degrees turned
    auto pose_change(const CameraPose& a, const CameraPose& b) -> float
    {
        const Vec t = a.turn - b.turn;
        return std::max({travel(a.at - b.at), std::abs(wrap_degrees(t.x)), std::abs(wrap_degrees(t.y)), std::abs(wrap_degrees(t.z))});
    }

    // Where a world point appears on screen, in viewport pixels
    auto project(UObject* player, Vec world, Vec2* out) -> bool
    {
        static UObject* statics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
        Call c(STR("/Script/Engine.GameplayStatics:ProjectWorldToScreen"));
        c.set<UObject*>(STR("Player"), player).set<Vec>(STR("WorldPosition"), world).set<bool>(STR("bPlayerViewportRelative"), false);
        if (!c.call(statics) || !c.get<bool>(STR("ReturnValue"))) return false;
        *out = c.get<Vec2>(STR("ScreenPosition"));
        return true;
    }

    // HUD units per viewport pixel: the inverse of the viewport's DPI scale
    auto hud_units_per_pixel(UObject* desk) -> float
    {
        static UObject* layout = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetLayoutLibrary"));
        Call c(STR("/Script/UMG.WidgetLayoutLibrary:GetViewportScale"));
        c.set<UObject*>(STR("WorldContextObject"), desk);
        const float dpi = c.call(layout) ? c.get<float>(STR("ReturnValue")) : 0.0f;
        return dpi > 0.0f ? 1.0f / dpi : 1.0f;
    }

    // The desk UI parts' places from the last layout, kept so the HUD panels can follow the desk when it moves as a
    // whole (rising into place at a set's start), shifted by how far it moved, and be worked out again when the camera
    // settles somewhere else (it is still closing in on the desk when the layout goes up)
    struct Placed
    {
        std::vector<Vec> moved_to;
        Vec desk_at, applied_at;
        CameraPose camera; // when the panels were last moved
        bool valid;
    };
    Placed g_placed{};

    // Each round button's HUD panels, moved on screen as far as the button appears to move
    auto move_hud_panels(UObject* desk, const std::vector<Vec>& moved_to, Vec shift = {}) -> void
    {
        const Original& o = g_original;
        UObject* player = first_player(desk);
        if (!player)
        {
            Output::send<LogLevel::Warning>(STR("[5deck] HUD panels not moved: no player controller\n"));
            return;
        }
        constexpr float kMargin = 12.0f; // HUD units kept clear at the screen's edges
        const float scale = hud_units_per_pixel(desk);
        Call viewport(own_call_fn(player, STR("GetViewportSize")));
        const float width = viewport.call(player) ? static_cast<float>(viewport.get<int32>(STR("SizeX"))) * scale : 0.0f; // the HUD's width
        for (const HudPanel& p : o.panels)
        {
            UObject* panel = p.widget.Get();
            Vec2 from{}, to{};
            if (!panel || !project(player, o.parts[p.part].loc + shift, &from) || !project(player, moved_to[p.part] + shift, &to))
            {
                Output::send<LogLevel::Warning>(STR("[5deck] HUD panel {} not moved: {}\n"), name_of(panel), panel ? STR("its button is off screen") : STR("gone"));
                continue;
            }
            Vec2 at{p.at.x + (to.x - from.x) * scale, p.at.y + (to.y - from.y) * scale};
            if (p.by_slot && p.left < p.right && width > 0.0f)
            {
                const float centre = p.anchor_x * width + at.x + p.centre_x;
                if (centre + p.left < kMargin) at.x += kMargin - (centre + p.left);
                else if (centre + p.right > width - kMargin) at.x -= centre + p.right - (width - kMargin);
            }
            if (p.by_slot)
            {
                UObject* slot = obj_prop(panel, STR("Slot"));
                Call set(own_call_fn(slot, STR("SetPosition")));
                set.set<Vec2>(STR("InPosition"), at).call(slot);
            }
            else
            {
                Call set(own_call_fn(panel, STR("SetRenderTranslation")));
                set.set<Vec2>(STR("Translation"), at).call(panel);
            }
#if FNL_DEBUG
            Output::send<LogLevel::Normal>(STR("[5deck] HUD panel {} moved by ({:.0f}, {:.0f}) to ({:.0f}, {:.0f}) by {} ({:.3f} HUD units per pixel)\n"), panel->GetName(),
                                           at.x - p.at.x, at.y - p.at.y, at.x, at.y, p.by_slot ? STR("slot position") : STR("render translation"), scale);
#endif
        }
    }

    // The desk's mesh component wearing a static mesh; false when the component refuses it (a static-mobility component
    // in a running world). SetStaticMesh itself returns false for the mesh already worn.
    auto set_static_mesh(UObject* com, UObject* static_mesh) -> bool
    {
        if (!static_mesh) return false;
        if (obj_prop(com, STR("StaticMesh")) == static_mesh) return true;
        Call c(own_call_fn(com, STR("SetStaticMesh")));
        return c.set<UObject*>(STR("NewMesh"), static_mesh).call(com) && c.get<bool>(STR("ReturnValue"));
    }

    // The five-deck mesh on the desk taken by take_original, before the deck goes up on it
    auto wear_five_deck_mesh() -> bool
    {
        if (set_static_mesh(g_original.mesh.Get(), five_deck_mesh())) return true;
        Output::send<LogLevel::Warning>(STR("[5deck] the desk's mesh component refused the five-deck mesh: no fifth deck\n"));
        return false;
    }

    // Everything to the layout on the five-deck desk: the timeline stretched, the five decks and their controls placed,
    // the round buttons turned with the desk's ends and the FX indicators moved with their decks, the HUD panels
    // following them, then BP_Desk's own deck refresh
    auto apply_layout(UObject* desk, const Layout& lay) -> void
    {
        const Original& o = g_original;
        auto* decks = desk_decks(desk);
        if (!decks) return;
        for (int32 i = 0; i < decks->Num() && i < 5; ++i)
        {
            UObject* d = (*decks)[i];
            set_pose(d, lay.decks[i]);
            if (UObject* ui = deck_ui(d)) set_pose(ui, lay.uis[i]);
        }
        const float timeline_stretch = std::sin(kMeshDeskEnd * kRad) / std::sin(kStockDeskEnd * kRad); // the desks' backs: the sines of their ends' angles
        if (UObject* timeline = o.timeline.Get())
        {
            Call c(own_call_fn(timeline, STR("SetActorScale3D")));
            c.set<Vec>(STR("NewScale3D"), {o.timeline_scale.x, o.timeline_scale.y * timeline_stretch, o.timeline_scale.z}).call(timeline);
        }
        UObject* mesh = o.mesh.Get();
        Axis axis{};
        const bool have_axis = mesh_axis(mesh, &axis);
        if (!have_axis) Output::send<LogLevel::Warning>(STR("[5deck] no desk mesh transform: the round buttons stay in their stock places\n"));
        int moved = 0;
        std::vector<Vec> moved_to(o.parts.size());
        for (size_t i = 0; i < o.parts.size(); ++i)
        {
            const DeskPart& p = o.parts[i];
            Vec rot = p.rot;
            if (p.deck >= 0)
                moved_to[i] = p.loc + (lay.decks[p.deck].loc - o.decks[p.deck].loc);
            else
            {
                // A round button turns with the desk's ends about the mesh's arc axis, outwards (away from the centre
                // line), so it keeps its stock place and facing on the raised corner beneath it
                moved_to[i] = p.loc;
                if (have_axis && turn_about(axis, lay.turn, p.loc, p.rot, &moved_to[i], &rot) && std::abs(moved_to[i].y - o.desk_y) < std::abs(p.loc.y - o.desk_y))
                    turn_about(axis, -lay.turn, p.loc, p.rot, &moved_to[i], &rot);
            }
            UObject* com = p.com.Get();
            if (!com) continue;
            ++moved;
            set_world_location(com, moved_to[i]);
            if (p.deck < 0)
            {
                set_world_rotation(com, rot);
#if FNL_DEBUG
                Output::send<LogLevel::Normal>(STR("[5deck] round button {} moved ({:.1f}, {:.1f}, {:.1f}), yaw {:.1f} -> {:.1f}\n"), name_of(com), moved_to[i].x - p.loc.x,
                                               moved_to[i].y - p.loc.y, moved_to[i].z - p.loc.z, p.rot.y, rot.y);
#endif
            }
        }
        const Vec desk_at = actor_location(desk);
        CameraPose camera{};
        camera_pose(first_player(desk), &camera);
        g_placed = {moved_to, desk_at, desk_at, camera, true};
        move_hud_panels(desk, moved_to);
        Call refresh(own_call_fn(desk, STR("RefreshDecks")));
        const bool refreshed = refresh.call(desk);
#if FNL_DEBUG
        Output::send<LogLevel::Normal>(STR("[5deck] applied: {} decks placed, desk mesh {}, timeline {}, {} desk UI parts moved, RefreshDecks {}\n"), std::min(decks->Num(), 5),
                                       name_of(obj_prop(mesh, STR("StaticMesh"))),
                                       o.timeline.Get() ? std::format(STR("stretched x{:.2f}"), timeline_stretch) : StringType{STR("MISSING")}, moved,
                                       refreshed ? STR("called") : STR("missing"));
        if (have_axis)
            Output::send<LogLevel::Normal>(STR("[5deck] desk mesh arc axis at ({:.1f}, {:.1f}) in the world; deck circle centre ({:.1f}, {:.1f}), turn {:.1f} deg\n"), axis.at.x,
                                           axis.at.y, lay.centre_x, o.desk_y, lay.turn);
#else
        (void)moved, (void)refreshed;
#endif
    }

    // The HUD panels back where they were laid out, those still alive
    auto restore_hud_panels() -> void
    {
        for (const HudPanel& p : g_original.panels)
        {
            UObject* panel = p.widget.Get();
            if (!panel) continue;
            if (p.by_slot)
            {
                UObject* slot = obj_prop(panel, STR("Slot"));
                Call set(own_call_fn(slot, STR("SetPosition")));
                set.set<Vec2>(STR("InPosition"), p.at).call(slot);
            }
            else
            {
                Call set(own_call_fn(panel, STR("SetRenderTranslation")));
                set.set<Vec2>(STR("Translation"), p.at).call(panel);
            }
        }
        g_placed.valid = false;
    }

    // The desk as it was before the layout, wherever it has moved to since: Decks[0-3] and their controls, the stock
    // mesh, the timeline's scale, the desk's 3D UI parts and their HUD panels
    auto restore_desk(UObject* desk) -> void
    {
        const Original& o = g_original;
        auto* decks = desk_decks(desk);
        if (!decks || decks->Num() < 4) return;
        const Vec shift = actor_location(desk) - o.desk_at;
        for (int32 i = 0; i < 4; ++i)
        {
            UObject* d = (*decks)[i];
            set_pose(d, {o.decks[i].loc + shift, o.decks[i].rot});
            if (UObject* ui = deck_ui(d)) set_pose(ui, {o.uis[i].loc + shift, o.uis[i].rot});
        }
        set_static_mesh(o.mesh.Get(), o.stock_mesh.Get());
        if (UObject* timeline = o.timeline.Get())
        {
            Call c(own_call_fn(timeline, STR("SetActorScale3D")));
            c.set<Vec>(STR("NewScale3D"), o.timeline_scale).call(timeline);
        }
        for (const DeskPart& p : o.parts)
        {
            UObject* com = p.com.Get();
            if (!com) continue;
            set_world_location(com, p.loc + shift);
            set_world_rotation(com, p.rot);
        }
        restore_hud_panels();
        Call refresh(own_call_fn(desk, STR("RefreshDecks")));
        refresh.call(desk);
#if FNL_DEBUG
        Output::send<LogLevel::Normal>(STR("[5deck] desk restored (moved ({:.1f}, {:.1f}, {:.1f}) since the layout)\n"), shift.x, shift.y, shift.z);
#endif
    }

    // The desk's round buttons (BP_Desk_3dUI: FX, Global, Transition) are translucent 3D widgets near the outer
    // decks' controls; draw the new controls above the Transition button's.
    auto raise_ui(UObject* ui) -> int32
    {
        int32 priority = 1;
        if (UObject* desk_ui = find_desk_ui())
            if (UObject* w = obj_prop(desk_ui, STR("3d_Transition_HypeWidget")))
                if (auto* p = w->GetValuePtrByPropertyNameInChain<int32>(STR("TranslucencySortPriority"))) priority = std::max(priority, *p + 1);
        UObject* widget = obj_prop(ui, STR("HypeWidget"));
        Call c(own_call_fn(widget, STR("SetTranslucentSortPriority")));
        return c.set<int32>(STR("NewTranslucentSortPriority"), priority).call(widget) ? priority : -1;
    }

    // A placed deck's controls are a separate BP_DeckUi actor (the 3D WBP_SingleDeckControls widget: name strip,
    // hover collider, wheel volume, Solo / Mute / Eject / Swap), linked through the deck's DeckUi variable on the map
    // instance. Spawn one for the new deck from Decks[3]'s, with the same attachment, TrackType set before its BeginPlay
    // registers the collider with HypeWidgetCollisionMgr.
    auto spawn_deck_ui(UObject* ref_deck, UObject* deck, const Pose& pose) -> UObject*
    {
        UObject* ref_ui = deck_ui(ref_deck);
        Call xf(own_call_fn(ref_ui, STR("GetTransform")));
        if (!ref_ui || !xf.call(ref_ui))
        {
            Output::send<LogLevel::Warning>(STR("[5deck] {} has no DeckUi: no controls for the new deck\n"), name_of(ref_deck));
            return nullptr;
        }
        UObject* ui = spawn_deferred(deck, ref_ui->GetClassPrivate(), xf.addr(STR("ReturnValue")), deck, [](UObject* a) {
            if (auto* t = a->GetValuePtrByPropertyNameInChain<uint8>(STR("TrackType"))) *t = kTrack;
        });
        if (!ui)
        {
            Output::send<LogLevel::Error>(STR("[5deck] deck UI spawn returned nothing\n"));
            return nullptr;
        }
        set_pose(ui, pose);
        UObject* ref_parent = attach_parent(ref_ui);
        UObject* parent = ref_parent == ref_deck ? deck : ref_parent;
        if (parent) attach_to(ui, parent);
        Call link(own_call_fn(deck, STR("SetDeckUi")));
        link.set<UObject*>(STR("DeckUi"), ui).call(deck);
        const int32 priority = raise_ui(ui);
        if (deck_ui(deck) != ui) Output::send<LogLevel::Warning>(STR("[5deck] deck UI {} not linked to {}\n"), ui->GetName(), deck->GetName());
#if FNL_DEBUG
        Output::send<LogLevel::Normal>(STR("[5deck] spawned deck UI {} ({}), attached to {} ({} is on {}); DeckUi {}; sort priority {}\n"), ui->GetName(),
                                       name_of(ref_ui->GetClassPrivate()), name_of(parent), name_of(ref_ui), name_of(ref_parent),
                                       deck_ui(deck) == ui ? STR("linked") : STR("NOT linked"), priority);
#else
        (void)priority;
#endif
        return ui;
    }

    // BP_Deck's Battle power display (3d_BattlePower_HypeWidget, a WBP_Battle_PowerDisplay) is built on the component's
    // first tick, and its Construct reads the game mode through the world's game instance with no null check
    // (GetHypeGameMode, FUN_1406c2700). A deck added at run time has no use for it outside Battle, so it goes before
    // that tick.
    auto drop_battle_widget(UObject* deck) -> void
    {
        UObject* widget = obj_prop(deck, STR("3d_BattlePower_HypeWidget"));
        Call destroy(own_call_fn(widget, STR("K2_DestroyComponent")));
        if (!widget || !destroy.set<UObject*>(STR("Object"), widget).call(widget))
            Output::send<LogLevel::Warning>(STR("[5deck] {}: Battle power widget {} not removed\n"), name_of(deck), name_of(widget));
    }

    // DeckClass deferred at Decks[3]'s transform (so `track` is set before its BeginPlay binds to the desk state),
    // attached and appended as the Battle desk does
    auto spawn_deck_actor(UObject* desk) -> UObject*
    {
        auto* decks = desk_decks(desk);
        auto* deck_class = desk->GetValuePtrByPropertyNameInChain<UObject*>(STR("DeckClass"));
        UObject* d3 = decks && decks->Num() == 4 ? (*decks)[3] : nullptr;
        Call xf(own_call_fn(d3, STR("GetTransform")));
        if (!d3 || !deck_class || !*deck_class || !xf.call(d3))
        {
            Output::send<LogLevel::Warning>(STR("[5deck] not spawning: {} decks (need exactly 4), DeckClass {}\n"), decks ? decks->Num() : -1, name_of(deck_class ? *deck_class : nullptr));
            return nullptr;
        }
        UObject* deck = spawn_deferred(desk, *deck_class, xf.addr(STR("ReturnValue")), desk, [](UObject* a) {
            if (auto* t = a->GetValuePtrByPropertyNameInChain<uint8>(STR("track"))) *t = kTrack;
        });
        if (!deck)
        {
            Output::send<LogLevel::Error>(STR("[5deck] BeginDeferredActorSpawnFromClass returned nothing\n"));
            return nullptr;
        }
        drop_battle_widget(deck);
        attach_to(deck, desk);
        decks->Add(deck);
        return deck;
    }

    // What the Battle desk fires after retyping a deck (FUN_1407fe890); BP_Deck passes it on to its DeckUi
    auto announce_deck(UObject* desk, UObject* deck) -> void
    {
        Call notify(own_call_fn(deck, STR("OnNewTrackType")));
        notify.set<uint8>(STR("newTrackType"), kTrack).call(deck);
#if FNL_DEBUG
        Output::send<LogLevel::Normal>(STR("[5deck] spawned {} ({}) track {}; Decks now {}\n"), deck->GetName(), name_of(deck->GetClassPrivate()), deck_track(deck),
                                       desk_decks(desk)->Num());
#else
        (void)desk;
#endif
    }

    // The deck with its controls, on the five-deck desk
    auto spawn_fifth_deck(UObject* desk) -> UObject*
    {
        Layout lay{};
        if (!take_original(desk) || !compute_layout(&lay))
        {
            Output::send<LogLevel::Warning>(STR("[5deck] not spawning: no desk layout (desk mesh {})\n"), name_of(obj_prop(desk, STR("SM_Desk_v4"))));
            return nullptr;
        }
        if (!wear_five_deck_mesh()) return nullptr;
#if FNL_DEBUG
        log_layout(lay);
#endif
        UObject* d3 = (*desk_decks(desk))[3];
        UObject* deck = spawn_deck_actor(desk);
        if (!deck) return nullptr;
        set_pose(deck, lay.decks[4]);
        spawn_deck_ui(d3, deck, lay.uis[4]);
        announce_deck(desk, deck);
        apply_layout(desk, lay);
        return deck;
    }

    auto set_hidden(UObject* actor, bool hidden = true) -> void
    {
        Call c(own_call_fn(actor, STR("SetActorHiddenInGame")));
        c.set<bool>(STR("bNewHidden"), hidden).call(actor);
    }

    auto set_collision(UObject* actor, bool enabled) -> void
    {
        Call c(own_call_fn(actor, STR("SetActorEnableCollision")));
        c.set<bool>(STR("bNewActorEnableCollision"), enabled).call(actor);
    }

    // A hidden deck stands below Decks[3], without collision, so it never takes a drop meant for it
    auto put_below(UObject* deck, UObject* d3) -> void
    {
        constexpr float kBelow = 200.0f;
        set_hidden(deck);
        set_collision(deck, false);
        set_pose(deck, {actor_location(d3) - Vec{0.0f, 0.0f, kBelow}, actor_rotation(d3)});
    }

    // The deck alone, hidden (multiplayer with Fifth Deck off, or until the desk view is ready): no controls, the desk
    // left as it is
    auto spawn_hidden_deck(UObject* desk) -> UObject*
    {
        UObject* d3 = desk_decks(desk) && desk_decks(desk)->Num() == 4 ? (*desk_decks(desk))[3] : nullptr;
        UObject* deck = spawn_deck_actor(desk);
        if (!deck) return nullptr;
        put_below(deck, d3);
        announce_deck(desk, deck);
        Call refresh(own_call_fn(desk, STR("RefreshDecks")));
        refresh.call(desk);
        return deck;
    }

    auto destroy_actor(UObject* actor) -> bool
    {
        Call c(own_call_fn(actor, STR("K2_DestroyActor")));
        return c.call(actor);
    }

    // The shown deck's controls, unlinked and destroyed
    auto destroy_deck_ui(UObject* deck) -> void
    {
        UObject* ui = deck_ui(deck);
        if (!ui) return;
        Call unlink(own_call_fn(deck, STR("SetDeckUi")));
        unlink.set<UObject*>(STR("DeckUi"), nullptr).call(deck);
        destroy_actor(ui);
    }

    // A shown deck turned into the hidden one: the desk restored, its controls gone, the deck hidden below Decks[3]
    auto hide_shown_deck(UObject* desk, UObject* deck) -> void
    {
        restore_desk(desk);
        destroy_deck_ui(deck);
        put_below(deck, (*desk_decks(desk))[3]);
        g_hidden = true;
        Call refresh(own_call_fn(desk, STR("RefreshDecks")));
        refresh.call(desk);
    }

    // The deck taken off the desk and destroyed, with its controls
    auto remove_deck(UObject* desk, UObject* deck) -> void
    {
        if (auto* decks = desk_decks(desk))
            for (int32 i = decks->Num() - 1; i >= 0; --i)
                if ((*decks)[i] == deck) decks->RemoveAt(i);
        destroy_deck_ui(deck);
        destroy_actor(deck);
        Call refresh(own_call_fn(desk, STR("RefreshDecks")));
        refresh.call(desk);
        Output::send<LogLevel::Normal>(STR("[5deck] fifth deck removed; Decks now {}\n"), desk_decks(desk) ? desk_decks(desk)->Num() : -1);
    }

#if FNL_DEBUG
    // Ctrl+Shift+5: the layout applied again to what is already up
    auto relayout(UObject* desk) -> void
    {
        Layout lay{};
        if (!compute_layout(&lay)) return;
        log_layout(lay);
        apply_layout(desk, lay);
    }

    // Track 17's audio state, logged when it changes, every 250 ms in the watch window
    auto watch_audio() -> void
    {
        static uint64 last = 0;
        static StringType seen{};
        const uint64 now = GetTickCount64();
        if (now >= g_watch_until || now - last < 250) return;
        last = now;
        UObject* deck = g_deck.Get();
        UObject* mgr = deck ? audio_track_mgr(deck) : nullptr;
        if (!mgr) return;
        const StringType line = track_line(mgr) + std::format(STR("; deck playing {} queued {}; {}"), name_of(obj_prop(deck, STR("CurPlayingDisc"))),
                                                              name_of(obj_prop(deck, STR("CurQueuedDisc"))), deck_state_line(g_desk.Get()));
        if (line == seen) return;
        seen = line;
        Output::send<LogLevel::Normal>(STR("[5deck] audio track 17: {}\n"), line);
    }
#endif

    // ---------------------------------------------------------------- game functions, by build -------
    // The game functions the fifth deck calls and hooks, per executable (fue's build names; the FUN_ names in the
    // comments here are EGS v1.4.2's). On the other builds each is the same function, instruction-identical to EGS
    // v1.4.2's apart from relative addresses, so the field offsets used here hold there too. A build without a row
    // gets no fifth deck.
    struct BuildAddrs
    {
        const CharType* name;
        uintptr_t solo;                 // FUN_140602f70: UDeckState solo
        uintptr_t mute;                 // FUN_1405fd6f0: UDeckState mute
        uintptr_t any_solo;             // FUN_1405f9f10: any deck soloed
        uintptr_t apply;                // FUN_14059c350: solo / mute applied to the audio tracks
        uintptr_t copy_solo_mute;       // FUN_140602ab0: solo / mute entries copied in
        uintptr_t track_playing;        // FUN_140597cb0: anything playing on a track
        uintptr_t audio_controller;     // FUN_140591640: an object's audio controller
        uintptr_t track_mgr;            // FUN_140591800: the controller's audio track manager
        uintptr_t drop_queue;           // FUN_1405f3da0: every deck's queued stem dropped
        uintptr_t queued_cels;          // FUN_1405d9e60: Decks[0-3]'s queued cels
        uintptr_t transition_tick;      // FUN_140673230: the transition manager's tick
        uintptr_t play_transition;      // FUN_14066ea80: PlayTransition
        uintptr_t spawn_local;          // FUN_1405f4c00: a disc spawned on a deck, on this client only
        uintptr_t eject_for_transition; // FUN_1405f4520: a deck's playing stem ejected, on this client only
        uintptr_t eject_queue_local;    // FUN_1405f4460: a deck's queued stem ejected, on this client only
        uintptr_t has_queued;           // FUN_1405f9d00: a deck with a live queued disc
        uintptr_t all_null;             // FUN_1405d6ee0: a cel list with nothing in it
        uintptr_t four_decks_empty;     // FUN_1405f10b0: ADesk::AreFourDecksEmpty
        uintptr_t set_gain;             // FUN_140602b40: UDeckState gain for a track
        uintptr_t stage_fade;           // FUN_1406303e0: the multiplayer lobby's fade-out
        uintptr_t hottest_fade;         // FUN_140638680: the hottest mix's playback fade and restore
        uintptr_t stage_reset;          // FUN_140633440: a round's start: gains back to 1.0 and the decks ejected
        uintptr_t lobby_reset;          // FUN_140633f50: _OnDJQueueUpdated, the same reset in the lobby
        uintptr_t event_path;           // FUN_1406c22d0: a track's FMOD event path
        uintptr_t voice_start;          // FUN_1422f79f0: a Harmonix voice started
        uintptr_t voice_callback;       // FUN_1422f7e20: a voice's FMOD event instance callback
        uintptr_t track_volume;         // FUN_1405abcc0: an audio track's volume curve
        uintptr_t play_params;          // FUN_1405af4b0: an audio track's voice parameters for a cel
        uintptr_t audition_set;         // FUN_1405a1b90: UAudioTrackMgr::SetAuditionTracks
        uintptr_t set_audition_mode;    // FUN_1405a1b80: the audition event mode's setter
    };
    constexpr BuildAddrs kBuildAddrs[] = {
        {.name = STR("EGS v1.4.2"),
         .solo = 0x602f70, .mute = 0x5fd6f0, .any_solo = 0x5f9f10, .apply = 0x59c350, .copy_solo_mute = 0x602ab0,
         .track_playing = 0x597cb0, .audio_controller = 0x591640, .track_mgr = 0x591800, .drop_queue = 0x5f3da0,
         .queued_cels = 0x5d9e60, .transition_tick = 0x673230, .play_transition = 0x66ea80, .spawn_local = 0x5f4c00,
         .eject_for_transition = 0x5f4520, .eject_queue_local = 0x5f4460, .has_queued = 0x5f9d00, .all_null = 0x5d6ee0, .four_decks_empty = 0x5f10b0, .set_gain = 0x602b40, .stage_fade = 0x6303e0,
         .hottest_fade = 0x638680, .stage_reset = 0x633440, .lobby_reset = 0x633f50, .event_path = 0x6c22d0,
         .voice_start = 0x22f79f0, .voice_callback = 0x22f7e20, .track_volume = 0x5abcc0, .play_params = 0x5af4b0, .audition_set = 0x5a1b90, .set_audition_mode = 0x5a1b80},
        {.name = STR("EGS CL1317808"),
         .solo = 0x602f70, .mute = 0x5fd6f0, .any_solo = 0x5f9f10, .apply = 0x59c350, .copy_solo_mute = 0x602ab0,
         .track_playing = 0x597cb0, .audio_controller = 0x591640, .track_mgr = 0x591800, .drop_queue = 0x5f3da0,
         .queued_cels = 0x5d9e60, .transition_tick = 0x673230, .play_transition = 0x66ea80, .spawn_local = 0x5f4c00,
         .eject_for_transition = 0x5f4520, .eject_queue_local = 0x5f4460, .has_queued = 0x5f9d00, .all_null = 0x5d6ee0, .four_decks_empty = 0x5f10b0, .set_gain = 0x602b40, .stage_fade = 0x6303e0,
         .hottest_fade = 0x638680, .stage_reset = 0x633440, .lobby_reset = 0x633f50, .event_path = 0x6c22d0,
         .voice_start = 0x22f7a20, .voice_callback = 0x22f7e50, .track_volume = 0x5abcc0, .play_params = 0x5af4b0, .audition_set = 0x5a1b90, .set_audition_mode = 0x5a1b80},
        {.name = STR("Steam CL1317755"),
         .solo = 0x639b70, .mute = 0x6342f0, .any_solo = 0x630af0, .apply = 0x5d3980, .copy_solo_mute = 0x6396b0,
         .track_playing = 0x5cf2e0, .audio_controller = 0x5c8c70, .track_mgr = 0x5c8e30, .drop_queue = 0x62a970,
         .queued_cels = 0x611230, .transition_tick = 0x6a8f00, .play_transition = 0x6a4750, .spawn_local = 0x62b7d0,
         .eject_for_transition = 0x62b0f0, .eject_queue_local = 0x62b030, .has_queued = 0x6308e0, .all_null = 0x60e2b0, .four_decks_empty = 0x627c80, .set_gain = 0x639740, .stage_fade = 0x666440,
         .hottest_fade = 0x66e6e0, .stage_reset = 0x6694a0, .lobby_reset = 0x669fb0, .event_path = 0x6f7f20,
         .voice_start = 0x232c3d0, .voice_callback = 0x232c800, .track_volume = 0x5e3260, .play_params = 0x5e6a50, .audition_set = 0x5d9130, .set_audition_mode = 0x5d9120},
    };
    const BuildAddrs* g_addrs = nullptr; // this executable's row, once the hooks are prepared

    auto exe_at(uintptr_t rva) -> uintptr_t { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + rva; }

    auto fifth_deck_up() -> bool { return g_deck.Get() != nullptr; }

    // ---------------------------------------------------------------- solo / mute -------------------
    // UDeckState's solo and mute only cover Deck0-3:
    //   - solo FUN_140602f70(state, track, on) records the solo only for track < 4, then sets {mute 1, solo 0} on the
    //     other decks 0-3 (on) or clears their mute (off). Track 17's solo is never stored, so its button always sends
    //     "on" again: the other decks are muted and never unmuted;
    //   - any-solo FUN_1405f9f10 reads tracks 0-3;
    //   - mute FUN_1405fd6f0(state, track, on, ?) stores track 17's mute, but unmuting while a deck is soloed (which
    //     cancels that solo and unmutes the empty decks) walks tracks 0-3 only.
    // While the fifth deck is up (shown or hidden), these three are hooked to treat track 17 as a deck; without it they
    // pass straight through. The apply loop run after them (FUN_14059c350(track mgr): each track but the audition and
    // transition ones muted when its mute is pressed or another track is soloed, and HypeEvents.OnSoloMuteChanged, which
    // BP_Deck and the deck's buttons follow, broadcast for each change) already covers track 17, and asks the hooked
    // any-solo; it is run again after the hooks' own changes. It also mutes each of Deck0-3's riser tracks with its deck
    // (FUN_1406cb840); after it, the fifth deck's riser track is given track 17's mute the same way (see "the deck's
    // riser").
    // The solo / mute entries come from other players too: a SoloMuteStates action or the join snapshot is copied in
    // whole (FUN_140602ab0), at the sender's length. A join snapshot seeding Deck0-3 only carries four, which would
    // leave track 17 out (and mute writes [track] unchecked), so a shorter copy is padded to every track, neither muted
    // nor soloed. That hook is always on once installed (a stock client holds all 25 anyway).
    using SoloFn = void (*)(void* state, uint8 track, bool on);
    using MuteFn = void (*)(void* state, uint8 track, bool on, uint8 arg);
    using AnySoloFn = bool (*)(void* state);
    using ApplyFn = void (*)(void* mgr);
    using CopySoloMuteFn = void (*)(void* state, const void* entries); // entries: a TArray<FDeckMuteSoloState>
    using TrackPlayingFn = bool (*)(void* mgr, uint8 track);
    using PtrFn = void* (*)(void*);

    constexpr uint8 kSoloMuteDecks[] = {0, 1, 2, 3, kTrack};

    SoloFn g_solo = nullptr;
    MuteFn g_mute = nullptr;
    AnySoloFn g_any_solo = nullptr;
    ApplyFn g_apply = nullptr;
    CopySoloMuteFn g_copy_solo_mute = nullptr;
    funchook_t* g_hooks = nullptr;
    bool g_hooks_installed = false;

    // UDeckState's FDeckMuteSoloState entries { isMutePressed, isSoloPressed }, if they reach track 17
    auto solo_mute_entries(void* state) -> uint8*
    {
        RawArray a{};
        if (!state || !read_raw_array(state, 0xb0, &a) || !a.data || a.num <= kTrack) return nullptr;
        return const_cast<uint8*>(a.data);
    }

    auto any_solo(const uint8* s) -> bool
    {
        return std::any_of(std::begin(kSoloMuteDecks), std::end(kSoloMuteDecks), [&](uint8 t) { return s[t * 2 + 1] != 0; });
    }

    // The audio track manager, as the state's own functions find it (FUN_140591800(FUN_140591640(state)))
    auto track_mgr_of(void* state) -> void*
    {
        void* controller = reinterpret_cast<PtrFn>(exe_at(g_addrs->audio_controller))(state);
        return controller ? reinterpret_cast<PtrFn>(exe_at(g_addrs->track_mgr))(controller) : nullptr;
    }

    auto any_solo_detour(void* state) -> bool
    {
        if (g_any_solo(state)) return true;
        const uint8* s = fifth_deck_up() ? solo_mute_entries(state) : nullptr;
        return s && s[kTrack * 2 + 1];
    }

    auto sync_riser_mute() -> void;

    auto apply_detour(void* mgr) -> void
    {
        g_apply(mgr);
        sync_riser_mute();
    }

    auto reapply(void* state) -> void
    {
        if (void* mgr = track_mgr_of(state)) apply_detour(mgr);
    }

    // Solo on track 17 is stored like a deck's; any deck's solo mutes track 17 (on) or unmutes it (off), as the
    // original does for decks 0-3.
    auto solo_detour(void* state, uint8 track, bool on) -> void
    {
        g_solo(state, track, on);
        uint8* s = fifth_deck_up() ? solo_mute_entries(state) : nullptr;
        if (!s || (track != kTrack && track >= 4)) return;
        if (track == kTrack)
        {
            s[kTrack * 2] = 0;
            s[kTrack * 2 + 1] = on;
        }
        else if (on)
        {
            s[kTrack * 2] = 1;
            s[kTrack * 2 + 1] = 0;
        }
        else
        {
            s[kTrack * 2] = 0;
        }
#if FNL_DEBUG
        Output::send<LogLevel::Normal>(STR("[5deck] solo track {} {}: track {} now mute {} solo {}\n"), track, on ? STR("on") : STR("off"), kTrack, s[kTrack * 2],
                                       s[kTrack * 2 + 1]);
#endif
        reapply(state);
    }

    // Unmuting a deck while one is soloed cancels the solo and unmutes the decks with nothing playing; the original
    // does that for decks 0-3 (and only checks their solos), this repeats it with track 17 included.
    auto mute_detour(void* state, uint8 track, bool on, uint8 arg) -> void
    {
        if (track == kTrack && !solo_mute_entries(state))
        {
            Output::send<LogLevel::Warning>(STR("[5deck] mute track {} {} skipped: the solo/mute entries stop short of it\n"), kTrack, on ? STR("on") : STR("off"));
            return;
        }
        g_mute(state, track, on, arg);
        uint8* s = fifth_deck_up() ? solo_mute_entries(state) : nullptr;
        if (!s) return;
        if (!on && any_solo(s))
        {
            void* mgr = track_mgr_of(state);
            const auto playing = reinterpret_cast<TrackPlayingFn>(exe_at(g_addrs->track_playing));
            for (uint8 t : kSoloMuteDecks)
            {
                if (t == track) continue;
                if (s[t * 2 + 1])
                    s[t * 2 + 1] = 0;
                else if (!mgr || !playing(mgr, t))
                    s[t * 2] = 0;
            }
        }
#if FNL_DEBUG
        Output::send<LogLevel::Normal>(STR("[5deck] mute track {} {}: track {} now mute {} solo {}\n"), track, on ? STR("on") : STR("off"), kTrack, s[kTrack * 2],
                                       s[kTrack * 2 + 1]);
#endif
        reapply(state);
    }

    auto copy_solo_mute_detour(void* state, const void* entries) -> void
    {
        RawArray a{};
        if (!read_raw_array(entries, 0, &a) || a.num < 0 || a.num >= kTrackCount)
        {
            g_copy_solo_mute(state, entries);
            return;
        }
        uint8 padded[kTrackCount * 2]{};
        if (a.data && a.num > 0 && !read_bytes(a.data, padded, static_cast<size_t>(a.num) * 2))
        {
            g_copy_solo_mute(state, entries);
            return;
        }
        const RawArray grown{padded, kTrackCount, kTrackCount};
        g_copy_solo_mute(state, &grown);
#if FNL_DEBUG
        static int32 logged = -1;
        if (logged == a.num) return;
        logged = a.num;
        Output::send<LogLevel::Normal>(STR("[5deck] solo/mute entries copied in with {} of {} tracks: padded to all\n"), a.num, kTrackCount);
#endif
    }

    // The local entries grown to every track, if they are shorter (copied in before the hook was installed)
    auto ensure_solo_mute_size(UObject* state) -> void
    {
        RawArray a{};
        if (!g_hooks_installed || !state || !read_raw_array(state, 0xb0, &a) || a.num >= kTrackCount) return;
        copy_solo_mute_detour(state, reinterpret_cast<const uint8*>(state) + 0xb0);
    }

    // ---------------------------------------------------------------- queue drop and risers --------
    // Dropping the queue (the queue switch, Input.Workspace.DropCue: UDeskState::DoDropQueue FUN_1405f6550, which then
    // sends ServerDoDropQueue; the same FUN_1405f3da0 applies another player's DoDropQueue action, and a transition's
    // own drop) walks every deck on the desk, the fifth too, but first lists Decks[0-3]'s queued cels
    // (FUN_1405d9e60, one slot per deck) and does nothing when they are all empty. With only the fifth deck queued, its
    // cel is offered in the list's last empty slot for that one call, so the drop goes ahead; the slot is only read as
    // "a cel was queued" (FUN_1405cede0, the drop's tick) and kept with the drop for its trick.
    // A transition (a smart riser) ejects the stem playing on each of Decks[0-3] just before its drop
    // (the transition manager's tick FUN_140673230: FUN_1405f4520(desk state, track, no stem queued, false) for each,
    // where it sets its +0xdd flag), so a deck without a queued stem falls silent at the drop while a queued one
    // takes its place. The fifth deck is ejected the same way, in the tick that sets the flag.
    // Before that, the transition queues stems for the drop itself (PlayTransition FUN_14066ea80, through the local
    // spawn FUN_1405f4c00(desk state, cel, Queue, track, transform, false) for each of Decks[0-3]): a smart riser with
    // nothing queued queues each playing deck's own stem again, so the decks come back in at the drop (so does one in
    // its clear-the-queue mode, +0xf9), and a multiplayer handover (+0x118) queues the next DJ's stage kit. Whenever
    // it queues a stem on any of Decks[0-3], the fifth deck, playing with nothing queued, gets its own playing stem
    // queued the same way, so it comes back in at the drop with the others. It runs on every client, as the stock
    // queueing does.
    // "Nothing queued" is PlayTransition's own test of Decks[0-3]'s queued cels (FUN_1405d6ee0, every entry null, at
    // two calls: the riser's choice of cels and its re-queue), so a stem queued on the fifth deck alone would still
    // have the riser queue every playing deck's own stem again. While PlayTransition runs with only the fifth deck
    // queued, those two calls are answered "something is queued", as for one queued deck of four: nothing is queued
    // again, the decks with nothing queued fall silent at the drop and the fifth deck's queued stem plays. The list
    // itself is left as it is, since the riser plays each entry's riser sound on its deck's own track; the fifth deck's
    // riser is played after it, on a track of its own (see "the deck's riser"). In the clear-the-queue mode the fifth
    // deck's queued stem is ejected, as Decks[0-3]'s are, and its playing stem queued.
    // A smart riser can only be started while some deck plays: in a freestyle set the riser button's state
    // (GameFlowMgr::GetGreatestTransitionWorkspaceState, FUN_1405f96d0) is "disabled" while ADesk::AreFourDecksEmpty
    // (FUN_1405f10b0: no live playing disc, +0x2a0, on the decks of tracks 0-3) holds, and the transition manager checks
    // the same. A playing stem on the fifth deck makes the desk not empty, as one on any of Decks[0-3] does.
    using DropQueueFn = void (*)(void* desk_state);
    using QueuedCelsFn = TArray<UObject*>* (*)(TArray<UObject*>* out, void* desk_state);
    using TransitionTickFn = void (*)(void* transitions);
    using EjectForTransitionFn = void (*)(void* desk_state, uint8 track, bool none_queued, bool arg);
    using HasQueuedFn = bool (*)(void* deck); // FUN_1405f9d00: a live queued disc
    // a5: a pointer, a6: a float and a7: a flag, all on the stack, passed through as they are
    using PlayTransitionFn = uint64 (*)(void* transitions, void* a2, uint32 a3, uint32 a4, void* a5, uint64 a6, uint64 a7);
    using SpawnLocalFn = void (*)(void* desk_state, UObject* cel, uint8 action, uint8 track, const void* transform, bool arg);
    using AllNullFn = bool (*)(const TArray<UObject*>* list);
    using EjectQueueLocalFn = void (*)(void* desk_state, uint8 track);
    using DecksEmptyFn = bool (*)(void* desk);

    constexpr size_t kTransitionDeskStateOffset = 0x148, kTransitionEjectedOffset = 0xdd;
    constexpr size_t kTransitionKitOffset = 0x118, kTransitionModeOffset = 0xf9;
    constexpr size_t kTransitionCurrentOffset = 0xd0; // the transition playing, null between transitions
    constexpr size_t kTransitionSyncOffset = 0xe8;    // zero: the risers' sync type is 1 (read before PlayTransition sets it)
    constexpr size_t kTransitionRisersOffset = 0x103; // the decks' risers are played
    constexpr uint8 kClearQueueMode = 1;
    constexpr uintptr_t kPlayTransitionSize = 0x1000; // its body, both all-null calls in it (+0x648, +0xb04)
    constexpr uint8 kQueueAction = 2; // EActionAfterDiscSpawn::Queue

    DropQueueFn g_drop_queue = nullptr;
    QueuedCelsFn g_queued_cels = nullptr;
    TransitionTickFn g_transition_tick = nullptr;
    PlayTransitionFn g_play_transition = nullptr;
    AllNullFn g_all_null = nullptr;
    DecksEmptyFn g_four_decks_empty = nullptr;
    UObject* g_offer_queued = nullptr; // the fifth deck's queued cel, offered to the next queued-cels list
    bool g_only_fifth_queued = false;  // PlayTransition runs with a stem queued on the fifth deck and none on Decks[0-3]
    bool g_riser_live = false;         // the fifth deck's riser started, in a transition still playing

    auto play_fifth_riser(UObject* cel, uint32 unmute_tick, uint32 sync_tick, bool sync_type) -> void;
    auto stop_fifth_riser() -> void;

    auto queued_cel(UObject* deck) -> UObject* { return obj_prop(obj_prop(deck, STR("CurQueuedDisc")), STR("CelData")); }
    auto playing_cel(UObject* deck) -> UObject* { return obj_prop(obj_prop(deck, STR("CurPlayingDisc")), STR("CelData")); }
    auto current_transition(const void* transitions) -> void* { return *reinterpret_cast<void* const*>(static_cast<const uint8*>(transitions) + kTransitionCurrentOffset); }

    auto queued_cels_detour(TArray<UObject*>* out, void* desk_state) -> TArray<UObject*>*
    {
        TArray<UObject*>* list = g_queued_cels(out, desk_state);
        UObject* offer = g_offer_queued;
        g_offer_queued = nullptr;
        if (!offer || !list || list->Num() < 1) return list;
        for (int32 i = 0; i < list->Num(); ++i)
            if ((*list)[i]) return list;
        (*list)[list->Num() - 1] = offer;
        return list;
    }

    auto drop_queue_detour(void* desk_state) -> void
    {
        UObject* deck = g_deck.Get();
        g_offer_queued = deck ? queued_cel(deck) : nullptr;
        g_drop_queue(desk_state);
        g_offer_queued = nullptr;
    }

    auto transition_tick_detour(void* transitions) -> void
    {
        // a transition cut short since the last tick (FUN_140669980) stops the risers; one ending in the tick lets them play out
        if (g_riser_live && !current_transition(transitions)) stop_fifth_riser();
        auto* ejected = static_cast<uint8*>(transitions) + kTransitionEjectedOffset;
        const bool before = *ejected != 0;
        g_transition_tick(transitions);
        if (g_riser_live && !current_transition(transitions)) g_riser_live = false;
        UObject* deck = g_deck.Get();
        void* desk_state = *reinterpret_cast<void**>(static_cast<uint8*>(transitions) + kTransitionDeskStateOffset);
        if (before || !*ejected || !deck || !desk_state || !obj_prop(deck, STR("CurPlayingDisc"))) return;
        const bool none_queued = !reinterpret_cast<HasQueuedFn>(exe_at(g_addrs->has_queued))(deck);
        reinterpret_cast<EjectForTransitionFn>(exe_at(g_addrs->eject_for_transition))(desk_state, kTrack, none_queued, false);
#if FNL_DEBUG
        Output::send<LogLevel::Normal>(STR("[5deck] transition: fifth deck's stem ejected for the drop ({})\n"),
                                       none_queued ? STR("nothing queued") : STR("its queued stem follows"));
#endif
    }

    auto four_decks_empty_detour(void* desk) -> bool
    {
        if (!g_four_decks_empty(desk)) return false;
        UObject* deck = desk && desk == g_desk.Get() ? g_deck.Get() : nullptr;
        UObject* disc = obj_prop(deck, STR("CurPlayingDisc"));
        return !(disc && FWeakObjectPtr(disc).Get());
    }

    auto all_null_detour(const TArray<UObject*>* list) -> bool
    {
        const bool empty = g_all_null(list);
        if (!empty || !g_only_fifth_queued) return empty;
        const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress()), start = exe_at(g_addrs->play_transition);
        return from <= start || from >= start + kPlayTransitionSize;
    }

    auto play_transition_detour(void* transitions, void* a2, uint32 a3, uint32 a4, void* a5, uint64 a6, uint64 a7) -> uint64
    {
        UObject* desk = g_desk.Get();
        auto* decks = desk_decks(desk);
        UObject* deck = g_deck.Get();
        UObject* before[4]{};
        bool any_before = false;
        for (int32 i = 0; decks && i < 4 && i < decks->Num(); ++i)
        {
            before[i] = queued_cel((*decks)[i]);
            any_before = any_before || before[i];
        }
        UObject* fifth_queued = queued_cel(deck);
        UObject* fifth_playing = playing_cel(deck);
        const auto* t = static_cast<const uint8*>(transitions);
        const bool sync_type = t[kTransitionSyncOffset] == 0;
        g_only_fifth_queued = decks && decks->Num() >= 4 && !any_before && fifth_queued;
#if FNL_DEBUG
        const bool only_fifth = g_only_fifth_queued;
#endif
        const uint64 result = g_play_transition(transitions, a2, a3, a4, a5, a6, a7);
        g_only_fifth_queued = false;
        void* desk_state = *reinterpret_cast<void**>(static_cast<uint8*>(transitions) + kTransitionDeskStateOffset);
#if FNL_DEBUG
        if (only_fifth)
            Output::send<LogLevel::Normal>(STR("[5deck] transition: only the fifth deck has a stem queued: no deck's own stem queued again\n"));
#endif
        if (a5)
            stop_fifth_riser(); // a handover stops the risers
        else if (result && t[kTransitionRisersOffset])
        {
            // the cel PlayTransition would have listed for it: the queued one, unless no deck has one queued (or in the
            // clear-the-queue mode), then the playing one
            const bool queued_list = t[kTransitionModeOffset] == 0 && (any_before || fifth_queued);
            if (UObject* cel = queued_list ? fifth_queued : fifth_playing) play_fifth_riser(cel, a3, a4, sync_type);
        }
        if (desk_state && deck && !t[kTransitionKitOffset] && t[kTransitionModeOffset] == kClearQueueMode && obj_prop(deck, STR("CurQueuedDisc")))
        {
            reinterpret_cast<EjectQueueLocalFn>(exe_at(g_addrs->eject_queue_local))(desk_state, kTrack);
#if FNL_DEBUG
            Output::send<LogLevel::Normal>(STR("[5deck] transition: clear-the-queue mode: fifth deck's queued stem ejected\n"));
#endif
        }
        UObject* playing = playing_cel(deck);
        if (!decks || !playing || !desk_state || obj_prop(deck, STR("CurQueuedDisc"))) return result;
        bool queued = false;
        for (int32 i = 0; i < 4 && i < decks->Num(); ++i)
        {
            UObject* now = queued_cel((*decks)[i]);
            queued = queued || (now && now != before[i]);
        }
#if FNL_DEBUG
        StringType lists{};
        for (int32 i = 0; i < 4 && i < decks->Num(); ++i)
            lists += std::format(STR(" [{}] {} -> {}"), i, name_of(before[i]), name_of(queued_cel((*decks)[i])));
        Output::send<LogLevel::Normal>(STR("[5deck] PlayTransition: kit {} mode {}; Decks[0-3] queued:{}\n"), t[kTransitionKitOffset],
                                       t[kTransitionModeOffset], lists);
#endif
        if (!queued) return result;
        alignas(16) static constexpr float kIdentity[12] = {0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0}; // FTransform: rotation, translation, scale
        reinterpret_cast<SpawnLocalFn>(exe_at(g_addrs->spawn_local))(desk_state, playing, kQueueAction, kTrack, kIdentity, false);
        if (queued_cel(deck) != playing)
            Output::send<LogLevel::Warning>(STR("[5deck] transition: fifth deck's playing stem not queued again for the drop\n"));
#if FNL_DEBUG
        else
            Output::send<LogLevel::Normal>(STR("[5deck] transition: fifth deck's playing stem queued again for the drop\n"));
#endif
        return result;
    }

    // ---------------------------------------------------------------- stage gains -------------------
    // A multiplayer set moves the decks' gains itself, through UDeckState's gain (FUN_140602b40, which the decks' volume
    // wheels reach through NetSetGainForTrack too), for Deck0-3 only:
    //   - the lobby's fade-out (FUN_1406303e0, while the stage is EMultiplayerGameplayStage::Lobby);
    //   - the hottest mix's playback, faded in and out and restored (FUN_140638680);
    //   - the reset as a round starts (FUN_140633440, stage RoundStart_SetupGameplay) and its inlined copy in the lobby
    //     when the DJ queue empties (FUN_140633f50, _OnDJQueueUpdated): once the join snapshot is in, gains back to 1.0
    //     and each deck's stems ejected on this client.
    // While one of them runs, each gain it sets for Deck0 is set for track 17 too, and after a reset that ran the fifth
    // deck's stems are ejected the same way. The hottest mix's playback ejects Deck0-3's playing and queued stems on this
    // client (FUN_1405f4520 / FUN_1405f4460) once its fade-out reaches silence and again just before it spawns the
    // hottest mix's stems; while it runs, each eject it does for Deck0 is done for track 17 too, so the fifth deck falls
    // silent with the others and stays out of the replay. A round with no hottest mix has no playback stage (FDS drops
    // it, as FUSER does), so nothing is ejected and the decks, the fifth among them, play on.
    using GainFn = void (*)(void* state, uint8 track, float gain);
    using StageFn = void (*)(void* game_state, void* a2, void* a3, void* a4); // a2-a4: passed through, unread here

    GainFn g_set_gain = nullptr;
    StageFn g_stage_fade = nullptr;
    StageFn g_hottest_fade = nullptr;
    StageFn g_stage_reset = nullptr;
    StageFn g_lobby_reset = nullptr;
    int g_in_stage = 0;          // one of the stage functions is running
    bool g_stage_set_gain = false; // it set Deck0's gain

    // UDeckState's gain floats, if they reach track 17 (the gain setter writes [track] unchecked)
    auto gains_reach_track(void* state) -> bool
    {
        RawArray a{};
        return state && read_raw_array(state, 0xc0, &a) && a.data && a.num > kTrack;
    }

    auto set_gain_detour(void* state, uint8 track, float gain) -> void
    {
        g_set_gain(state, track, gain);
        if (!g_in_stage || track != 0 || !fifth_deck_up() || !gains_reach_track(state)) return;
        g_set_gain(state, kTrack, gain);
        g_stage_set_gain = true;
    }

    auto run_stage(StageFn fn, void* game_state, void* a2, void* a3, void* a4) -> bool
    {
        ++g_in_stage;
        g_stage_set_gain = false;
        fn(game_state, a2, a3, a4);
        --g_in_stage;
        return g_stage_set_gain;
    }

    auto stage_fade_detour(void* game_state, void* a2, void* a3, void* a4) -> void { run_stage(g_stage_fade, game_state, a2, a3, a4); }

    EjectForTransitionFn g_eject_playing = nullptr;
    EjectQueueLocalFn g_eject_queued = nullptr;
    bool g_in_hottest = false; // the hottest mix's playback is running

    auto eject_playing_detour(void* desk_state, uint8 track, bool none_queued, bool arg) -> void
    {
        g_eject_playing(desk_state, track, none_queued, arg);
        if (!g_in_hottest || track != 0 || !fifth_deck_up()) return;
#if FNL_DEBUG
        if (UObject* disc = obj_prop(g_deck.Get(), STR("CurPlayingDisc")))
            Output::send<LogLevel::Normal>(STR("[5deck] hottest mix: fifth deck's playing stem {} ejected with Decks[0-3]'s\n"), name_of(disc));
#endif
        g_eject_playing(desk_state, kTrack, none_queued, arg);
    }

    auto eject_queued_detour(void* desk_state, uint8 track) -> void
    {
        g_eject_queued(desk_state, track);
        if (!g_in_hottest || track != 0 || !fifth_deck_up()) return;
#if FNL_DEBUG
        if (UObject* disc = obj_prop(g_deck.Get(), STR("CurQueuedDisc")))
            Output::send<LogLevel::Normal>(STR("[5deck] hottest mix: fifth deck's queued stem {} ejected with Decks[0-3]'s\n"), name_of(disc));
#endif
        g_eject_queued(desk_state, kTrack);
    }

    auto hottest_fade_detour(void* game_state, void* a2, void* a3, void* a4) -> void
    {
        g_in_hottest = true;
        run_stage(g_hottest_fade, game_state, a2, a3, a4);
        g_in_hottest = false;
    }

    // After a reset that ran: the fifth deck's stems ejected on this client, as the reset does for Deck0-3
    auto reset_deck5(const CharType* when) -> void
    {
        UObject* deck = g_deck.Get();
        UObject* desk_state = obj_prop(game_state_of(deck), STR("DeskState"));
        if (!desk_state) return;
        reinterpret_cast<EjectForTransitionFn>(exe_at(g_addrs->eject_for_transition))(desk_state, kTrack, true, false);
        reinterpret_cast<EjectQueueLocalFn>(exe_at(g_addrs->eject_queue_local))(desk_state, kTrack);
#if FNL_DEBUG
        Output::send<LogLevel::Normal>(STR("[5deck] {} reset: fifth deck's gain back to 1.0, its stems ejected\n"), when);
#else
        (void)when;
#endif
    }

    auto stage_reset_detour(void* game_state, void* a2, void* a3, void* a4) -> void
    {
        if (run_stage(g_stage_reset, game_state, a2, a3, a4)) reset_deck5(STR("round start"));
    }

    auto lobby_reset_detour(void* game_state, void* queue, void* a3, void* a4) -> void
    {
        if (run_stage(g_lobby_reset, game_state, queue, a3, a4)) reset_deck5(STR("lobby"));
    }

    // ---------------------------------------------------------------- the deck's audio track ---------
    // UAudioTrackMgr.Tracks holds one audio track per ETrackType, made at start-up (FUN_1405a4a70: NewObject of the
    // track's class with the manager as outer, then mHypeAudio, mMgr and the track's index at +0x5c): UAudioTrack for
    // the decks, the transitions and the instruments, UAuditionAudioTrack for Audition0-3, and UDummyAudioTrack, which
    // never makes a sound, for Battle's Deck0A-Deck3B. While the deck is up, a UAudioTrack made the same way stands in
    // track 17's place; when the deck goes, a new UDummyAudioTrack goes back, so Battle finds its silent labels.
    // A UAudioTrack reads its FMOD event's path from a per-track table (FUN_1406c22d0, at each cel start), empty for
    // track 17. While the real track is in, track 17 plays Deck3's event, event:/song/stems/lead: a deck's event, with
    // the stems' compressor, analysis and stutter, at a deck's level (a UAudioTrack's volume curve, FUN_1405abcc0, is the
    // decks'). It is given as the event's ID string ("{...}", which FMOD's getEvent takes as well as a path) once that
    // is known, so the deck's voices can be told from Deck3's (see "the deck's own strip"); until then, as Deck3's path.
    // The fifth deck's riser track (18) plays Deck3's riser event (see "the deck's riser").
    using EventPathFn = void* (*)(void* out, uint8 track); // out: an FString

    constexpr size_t kTrackIndexOffset = 0x5c;
    constexpr size_t kStemIdChars = 38; // "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"

    EventPathFn g_event_path = nullptr;
    std::atomic<bool> g_real_track{false}; // track 17's slot holds our UAudioTrack
    std::atomic<bool> g_riser_real{false}; // track 18's slot holds our riser UAudioTrack
    FWeakObjectPtr g_track_mgr{};
    FWeakObjectPtr g_real_track_obj{};
    FWeakObjectPtr g_riser_track_obj{};
    char g_stem_id[kStemIdChars + 1]{};   // the stem event's ID string, once g_stem_id_ready
    std::atomic<bool> g_stem_id_ready{false};

    struct RawString // FString
    {
        CharType* data;
        int32 num;
        int32 max;
    };

    auto event_path_detour(void* out, uint8 track) -> void*
    {
        if (track == kRiserTrack && g_riser_real) return g_event_path(out, kRiserFrom);
        if (track != kTrack || !g_real_track) return g_event_path(out, track);
        if (!g_stem_id_ready) return g_event_path(out, kStemTrack);
        // in GMalloc memory, as the caller frees it
        auto* text = static_cast<CharType*>(FMemory::Malloc((kStemIdChars + 1) * sizeof(CharType)));
        for (size_t i = 0; i <= kStemIdChars; ++i) text[i] = static_cast<CharType>(g_stem_id[i]);
        *static_cast<RawString*>(out) = {text, static_cast<int32>(kStemIdChars + 1), static_cast<int32>(kStemIdChars + 1)};
        return out;
    }

    auto mgr_tracks(UObject* mgr) -> TArray<UObject*>* { return mgr ? mgr->GetValuePtrByPropertyNameInChain<TArray<UObject*>>(STR("Tracks")) : nullptr; }

    // An audio track of the class for a slot, set up as the manager sets up its own
    auto make_track(UObject* mgr, const CharType* class_path, uint8 slot) -> UObject*
    {
        auto* cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, class_path);
        UObject* track = cls ? UObjectGlobals::NewObject<UObject>(mgr, cls) : nullptr;
        if (!track) return nullptr;
        if (auto* p = track->GetValuePtrByPropertyNameInChain<UObject*>(STR("mHypeAudio"))) *p = obj_prop(mgr, STR("HypeAudio"));
        if (auto* p = track->GetValuePtrByPropertyNameInChain<UObject*>(STR("mMgr"))) *p = mgr;
        *reinterpret_cast<int32*>(reinterpret_cast<uint8*>(track) + kTrackIndexOffset) = slot;
        return track;
    }

    // Track 17's gain as the deck state holds it (1.0 if it can't be read)
    auto deck_state_gain(UObject* state) -> float
    {
        RawArray a{};
        float g = 1.0f;
        if (state && read_raw_array(state, 0xc0, &a) && a.data && a.num > kTrack) read_bytes(a.data + kTrack * 4, reinterpret_cast<uint8*>(&g), 4);
        return g;
    }

    // ---------------------------------------------------------------- the deck's riser ---------------
    // A smart riser (PlayTransition FUN_14066ea80) plays the transition's own sound on Transition and, with the decks'
    // risers on (+0x103), each listed deck's riser on its own riser track (TransitionSong0-3, tracks 13-16):
    // UAudioTrackMgr::SetTrackCel(that track, the deck's cel's SongTransitionCelData's shortName, the transition's unmute
    // and sync ticks, sync type 1 if +0xe8 was clear). The list is Decks[0-3]'s queued cels, or their playing ones when
    // none is queued or in the clear-the-queue mode (+0xf9); a multiplayer handover lists the next DJ's stage kit and
    // first stops all four tracks. A riser plays out by itself; a transition cut short (FUN_140669980: its current
    // transition, +0xd0, cleared) stops them. The fifth deck has no riser track, so while it is up another real
    // UAudioTrack stands in track 18's place (Deck1A, another of Battle's silent labels, put back with track 17's) for its
    // riser, played after PlayTransition the same way when the fifth deck's cel would have been listed: its queued cel when
    // the list is the queued one, else its playing one. Like Deck3's riser it plays TransitionSong3's event
    // (event:/song/transition_lead, which is not under MusicSub: no deck's effects reach a riser). A track decides by its
    // index whether it is one of the transition tracks (FUN_1406cd920: Transition or TransitionSong0-3), which plays
    // its cel once (the voice parameters' play mode +0x88 gets 0x100, FUN_1405af4b0) where any other track loops it; and
    // the volume curve (FUN_1405abcc0: the gain times a steady 0.9 for the transition tracks, times 0.8 and then 0.6
    // after 3 s for the others) asks the same. Track 18 is given both as TransitionSong3. Each deck's riser track is
    // muted with the deck (the solo / mute apply loop, FUN_1406cb840): track 18 is given track 17's mute after each
    // apply. It starts at TransitionSong3's gain. A transition cut short between two of the transition manager's ticks
    // stops it (see the tick).
    using VolumeFn = float (*)(void* track, float delta);
    using PlayParamsFn = void (*)(void* track, void* params);

    constexpr size_t kVolumeBytes = 0x60; // what the curve reads: gain +0x4c, time played +0x54, track index +0x5c
    constexpr size_t kPlayModeOffset = 0x88;
    constexpr uint32 kPlayOnce = 0x100;

    VolumeFn g_track_volume = nullptr;
    PlayParamsFn g_play_params = nullptr;
    std::atomic<void*> g_riser_track_raw{nullptr}; // the riser track, for the volume curve and its voices

    auto track_volume_detour(void* track, float delta) -> float
    {
        if (!track || track != g_riser_track_raw.load()) return g_track_volume(track, delta);
        alignas(16) uint8 as_riser[kVolumeBytes];
        std::memcpy(as_riser, track, sizeof as_riser);
        *reinterpret_cast<int32*>(as_riser + kTrackIndexOffset) = kRiserFrom;
        return g_track_volume(as_riser, delta);
    }

    auto play_params_detour(void* track, void* params) -> void
    {
        g_play_params(track, params);
        if (track && params && track == g_riser_track_raw.load()) *reinterpret_cast<uint32*>(static_cast<uint8*>(params) + kPlayModeOffset) |= kPlayOnce;
    }

    auto sync_riser_mute() -> void
    {
        UObject* mgr = g_riser_real ? g_track_mgr.Get() : nullptr;
        if (!mgr) return;
        Call deck(own_call_fn(mgr, STR("GetTrackIsMuted"))), riser(own_call_fn(mgr, STR("GetTrackIsMuted")));
        if (!deck.set<uint8>(STR("trackType"), kTrack).call(mgr) || !riser.set<uint8>(STR("trackType"), kRiserTrack).call(mgr)) return;
        const bool muted = deck.get<bool>(STR("ReturnValue"));
        if (riser.get<bool>(STR("ReturnValue")) == muted) return;
        Call set(own_call_fn(mgr, STR("SetTrackMute")));
        set.set<uint8>(STR("trackType"), kRiserTrack).set<bool>(STR("mute"), muted).call(mgr);
    }

    // The riser track put in track 18's place (the manager holding our track 17), at TransitionSong3's gain
    auto put_in_riser_track(UObject* mgr, TArray<UObject*>* tracks) -> void
    {
        UObject* old = (*tracks)[kRiserTrack];
        if (old && old == g_riser_track_obj.Get())
        {
            g_riser_real = true;
            return;
        }
        UObject* riser = make_track(mgr, STR("/Script/Hype.AudioTrack"), kRiserTrack);
        if (!riser)
        {
            Output::send<LogLevel::Error>(STR("[5deck] no AudioTrack made for track {}: the fifth deck has no riser\n"), kRiserTrack);
            return;
        }
        (*tracks)[kRiserTrack] = riser;
        g_riser_track_obj = FWeakObjectPtr(riser);
        g_riser_track_raw = riser;
        g_riser_real = true;
        Call get(own_call_fn(mgr, STR("GetGainForTrack")));
        get.set<uint8>(STR("trackType"), kRiserFrom).call(mgr);
        const float gain = get.get<float>(STR("ReturnValue"));
        Call set(own_call_fn(mgr, STR("SetGainForTrack")));
        set.set<uint8>(STR("trackType"), kRiserTrack).set<float>(STR("gain"), gain).call(mgr);
        sync_riser_mute();
#if FNL_DEBUG
        Output::send<LogLevel::Normal>(STR("[5deck] track {}: {} ({}) replaced by {} for the fifth deck's riser, gain {:.2f}\n"), kRiserTrack, name_of(old),
                                       name_of(old ? old->GetClassPrivate() : nullptr), riser->GetName(), gain);
#endif
    }

    auto play_fifth_riser(UObject* cel, uint32 unmute_tick, uint32 sync_tick, bool sync_type) -> void
    {
        UObject* mgr = g_riser_real ? g_track_mgr.Get() : nullptr;
        UObject* riser = obj_prop(cel, STR("SongTransitionCelData"));
        auto* name = riser ? riser->GetValuePtrByPropertyNameInChain<FName>(STR("shortName")) : nullptr;
        if (!mgr || !name)
        {
#if FNL_DEBUG
            Output::send<LogLevel::Normal>(STR("[5deck] transition: no riser for the fifth deck's stem {} ({})\n"), name_of(cel),
                                           mgr ? STR("the stem has none") : STR("no riser track"));
#endif
            return;
        }
        sync_riser_mute();
        Call set(own_call_fn(mgr, STR("SetTrackCel")));
        set.set<uint8>(STR("trackType"), kRiserTrack)
            .set<FName>(STR("shortName"), *name)
            .set<int32>(STR("unmuteTick"), static_cast<int32>(unmute_tick))
            .set<int32>(STR("timelineSyncTick"), static_cast<int32>(sync_tick))
            .set<uint8>(STR("syncType"), sync_type ? 1 : 0);
        if (!set.call(mgr)) return;
        g_riser_live = true;
#if FNL_DEBUG
        Output::send<LogLevel::Normal>(STR("[5deck] transition: fifth deck's riser {} (stem {}) on track {}, unmute tick {}, sync tick {}, sync type {}\n"), name->ToString(),
                                       name_of(cel), kRiserTrack, static_cast<int32>(unmute_tick), static_cast<int32>(sync_tick), sync_type ? 1 : 0);
#endif
    }

    auto stop_fifth_riser() -> void
    {
        const bool live = g_riser_live;
        g_riser_live = false;
        UObject* mgr = g_riser_real ? g_track_mgr.Get() : nullptr;
        if (!mgr) return;
        Call remove(own_call_fn(mgr, STR("RemoveTrackCel")));
        remove.set<uint8>(STR("trackType"), kRiserTrack).set<int32>(STR("scheduleTick"), -1).set<int32>(STR("timelineSyncTick"), -1).call(mgr);
#if FNL_DEBUG
        if (live) Output::send<LogLevel::Normal>(STR("[5deck] transition: cut short or handed over: the fifth deck's riser stopped\n"));
#else
        (void)live;
#endif
    }

    auto load_fmod() -> void;

    // The real track put in track 17's place, given the deck state's gain for it and its solo / mute; the riser track with it
    auto put_in_real_track(UObject* desk) -> void
    {
        UObject* mgr = audio_track_mgr(desk);
        auto* tracks = mgr_tracks(mgr);
        if (!tracks || tracks->Num() != kTrackCount)
        {
            Output::send<LogLevel::Error>(STR("[5deck] no audio track manager with {} tracks ({}, {} tracks): the fifth deck stays silent\n"), kTrackCount, name_of(mgr),
                                          tracks ? tracks->Num() : -1);
            return;
        }
        UObject* old = (*tracks)[kTrack];
        if (old && old == g_real_track_obj.Get())
        {
            g_real_track = true;
            put_in_riser_track(mgr, tracks);
            return;
        }
        UObject* track = make_track(mgr, STR("/Script/Hype.AudioTrack"), kTrack);
        if (!track)
        {
            Output::send<LogLevel::Error>(STR("[5deck] no AudioTrack made: the fifth deck stays silent\n"));
            return;
        }
        (*tracks)[kTrack] = track;
        g_track_mgr = FWeakObjectPtr(mgr);
        g_real_track_obj = FWeakObjectPtr(track);
        g_real_track = true;
        load_fmod();
        UObject* state = deck_state(desk);
        const float gain = deck_state_gain(state);
        Call set(own_call_fn(mgr, STR("SetGainForTrack")));
        set.set<uint8>(STR("trackType"), kTrack).set<float>(STR("gain"), gain).call(mgr);
        if (state) reapply(state);
#if FNL_DEBUG
        Output::send<LogLevel::Normal>(STR("[5deck] track {}: {} ({}) replaced by {}, gain {:.2f}\n"), kTrack, name_of(old), name_of(old ? old->GetClassPrivate() : nullptr),
                                       track->GetName(), gain);
#endif
        put_in_riser_track(mgr, tracks);
    }

    // A silent track back in a slot of ours, after anything still playing on our real one there is stopped
    auto put_back_dummy(UObject* mgr, TArray<UObject*>* tracks, uint8 slot, UObject* ours) -> void
    {
        if (!tracks || tracks->Num() != kTrackCount || !ours || (*tracks)[slot] != ours)
        {
            Output::send<LogLevel::Warning>(STR("[5deck] track {} not restored: its slot no longer holds the fifth deck's track\n"), slot);
            return;
        }
        Call playing(own_call_fn(mgr, STR("IsAnythingPlayingOnTrack")));
        if (playing.set<uint8>(STR("trackType"), slot).call(mgr) && playing.get<bool>(STR("ReturnValue")))
        {
            Call eject(own_call_fn(mgr, STR("EjectTrackCel")));
            eject.set<uint8>(STR("trackType"), slot).set<float>(STR("rampOutSecs"), 0.0f).call(mgr);
        }
        UObject* dummy = make_track(mgr, STR("/Script/Hype.DummyAudioTrack"), slot);
        if (!dummy)
        {
            Output::send<LogLevel::Error>(STR("[5deck] no DummyAudioTrack made: track {} keeps the fifth deck's track\n"), slot);
            return;
        }
        (*tracks)[slot] = dummy;
#if FNL_DEBUG
        Output::send<LogLevel::Normal>(STR("[5deck] track {}: silent again ({})\n"), slot, dummy->GetName());
#endif
    }

    // The deck gone: silent tracks back in track 17's and the riser's places
    auto restore_dummy_track() -> void
    {
        if (!g_real_track) return;
        g_real_track = false;
        g_riser_real = false;
        g_riser_live = false;
        g_riser_track_raw = nullptr;
        UObject* mgr = g_track_mgr.Get();
        UObject* ours = g_real_track_obj.Get();
        UObject* riser = g_riser_track_obj.Get();
        g_track_mgr = FWeakObjectPtr{};
        g_real_track_obj = FWeakObjectPtr{};
        g_riser_track_obj = FWeakObjectPtr{};
        auto* tracks = mgr_tracks(mgr);
        put_back_dummy(mgr, tracks, kTrack, ours);
        if (riser) put_back_dummy(mgr, tracks, kRiserTrack, riser);
    }

    // ---------------------------------------------------------------- the deck's own strip -----------
    // Each deck plays into its own strip of the FMOD mix: a bus under bus:/Music/MusicSub (Beat, Bass, Loop, Lead) with
    // the deck's tape stop and buffer effects and a send to its effects bus (FX_<deck>), which a deck's own effects act
    // on. MusicSub's own chain (tape stop, buffer, stutter, the sends to FX_MusicSub and the music reverb) acts on
    // everything under it: the master effects. The fifth deck gets a strip of its own, a channel group ("FNL Fifth
    // Deck") under MusicSub's, so Deck3's effects stay off it while the master effects, the Music volume and the pause
    // bus still reach it. No effect slot addresses it (EAudioEffectSlot stops at Deck3Slot), so it has no effects of its
    // own.
    // A stem plays through a Harmonix voice. Started (FUN_1422f79f0(voice, ?, params, source), which hands all four on
    // to FUN_1422a4dd0, where the source becomes the voice's render source at +0x1b0: the detour must pass every one
    // through unchanged), a voice for an event (params
    // +0x30 == 1) looks its event up by the path at params +0x38 (getEvent on the Studio system its FMOD system object
    // holds: voice +0x48, of type +0x10, 1: Studio at +0x1c0, 2: at +0x438), makes the instance with itself as the
    // instance's user data, and finishes it in the instance's callback (FUN_1422f7e20): once the instance's channel
    // group exists (0x80, CREATE_PROGRAMMER_SOUND; nothing is done once the voice's done flag +0x1e0 is set, or in its
    // state 6 at +0x1c), it adds its stem DSP there. The first voice started gives the stem event's ID string (getEvent
    // of event:/song/stems/lead, then its ID); from then on track 17 plays the event by that ID (see "the deck's audio
    // track"), so a voice started with it is the fifth deck's, and it is noted before it starts. Its instance's channel
    // group is then moved under the strip in the callback, after the voice's own work (ChannelGroup::addGroup takes a
    // group off its bus, as the voice itself does when its owner hands it a group). The strip is made the first time,
    // under the first group on the way up from the instance holding both HMX.TapeStartStop and HMX.Stutter (MusicSub: a
    // deck bus has no stutter, an event no tape stop), and moved under it again whenever that group changes. The
    // callback runs on FMOD's thread, the voice start maybe too: they only use FMOD's own calls and the noted voices,
    // and leave their log lines for the game thread. Without FMOD's functions the fifth deck plays on Deck3's strip.
    using VoiceStartFn = void (*)(void* voice, void* arg, void* params, void* source);
    using VoiceCallbackFn = int32 (*)(uint32 type, void* instance, void* parameters);

    constexpr size_t kVoiceSystemOffset = 0x48, kVoiceStateOffset = 0x1c, kVoiceDoneOffset = 0x1e0;
    constexpr size_t kSystemTypeOffset = 0x10, kSystemStudioOffset1 = 0x1c0, kSystemStudioOffset2 = 0x438;
    constexpr size_t kParamsKindOffset = 0x30, kParamsPathOffset = 0x38;
    constexpr int32 kParamsEvent = 1;
    constexpr uint32 kInstanceReady = 0x80; // FMOD_STUDIO_EVENT_CALLBACK_CREATE_PROGRAMMER_SOUND
    constexpr int32 kVoiceStopped = 6;
    constexpr int kRecentVoices = 8;

    VoiceStartFn g_voice_start = nullptr;
    VoiceCallbackFn g_voice_callback = nullptr;
    std::atomic<void*> g_deck_voices[kRecentVoices]{}; // fifth-deck voices started, waiting for their instance
    std::atomic<uint32> g_next_voice{0};

    // FMOD's C API from the game's own fmod.dll / fmodstudio.dll; its handles are the C++ objects' pointers
    struct FmodGuid
    {
        uint32 data1;
        uint16 data2, data3;
        uint8 data4[8];
    };
    struct Fmod
    {
        int32 (*get_event)(void* studio, const char* path, void** description);
        int32 (*event_id)(void* description, FmodGuid* id);
        int32 (*instance_user_data)(void* instance, void** data);
        int32 (*instance_group)(void* instance, void** group);
        int32 (*parent_group)(void* group, void** parent);
        int32 (*num_dsps)(void* group, int32* n);
        int32 (*dsp_at)(void* group, int32 index, void** dsp);
        int32 (*dsp_info)(void* dsp, char* name, uint32* version, int32* channels, int32* width, int32* height);
        int32 (*system_of)(void* group, void** system);
        int32 (*create_group)(void* system, const char* name, void** group);
        int32 (*add_group)(void* group, void* child, int32 propagate_clock, void** connection);
    };
    Fmod g_fmod{};
    std::atomic<bool> g_fmod_ready{false};

    struct Strip
    {
        void* system;
        void* parent; // MusicSub's channel group
        void* group;
    };
    std::mutex g_strip_lock; // over g_strip and g_strip_lines
    Strip g_strip{};
    std::vector<StringType> g_strip_lines{};

    auto load_fmod() -> void
    {
        if (g_fmod_ready) return;
        HMODULE core = GetModuleHandleW(L"fmod.dll"), studio = GetModuleHandleW(L"fmodstudio.dll");
        const auto get = [](HMODULE m, const char* name, auto& fn) {
            fn = m ? reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name)) : nullptr;
            return fn != nullptr;
        };
        Fmod f{};
        const bool ok = get(studio, "FMOD_Studio_System_GetEvent", f.get_event) && get(studio, "FMOD_Studio_EventDescription_GetID", f.event_id) &&
                        get(studio, "FMOD_Studio_EventInstance_GetUserData", f.instance_user_data) &&
                        get(studio, "FMOD_Studio_EventInstance_GetChannelGroup", f.instance_group) && get(core, "FMOD_ChannelGroup_GetParentGroup", f.parent_group) &&
                        get(core, "FMOD_ChannelGroup_GetNumDSPs", f.num_dsps) && get(core, "FMOD_ChannelGroup_GetDSP", f.dsp_at) &&
                        get(core, "FMOD_DSP_GetInfo", f.dsp_info) && get(core, "FMOD_ChannelGroup_GetSystemObject", f.system_of) &&
                        get(core, "FMOD_System_CreateChannelGroup", f.create_group) && get(core, "FMOD_ChannelGroup_AddGroup", f.add_group);
        if (!ok)
        {
            Output::send<LogLevel::Warning>(STR("[5deck] FMOD's functions not found (fmod.dll {}, fmodstudio.dll {}): the fifth deck plays on Deck3's strip\n"),
                                            core ? STR("loaded") : STR("not loaded"), studio ? STR("loaded") : STR("not loaded"));
            return;
        }
        g_fmod = f;
        g_fmod_ready = true;
    }

    auto strip_line(StringType line) -> void { g_strip_lines.push_back(std::move(line)); } // under g_strip_lock

    auto has_dsp(void* group, const char* name) -> bool
    {
        int32 n = 0;
        if (g_fmod.num_dsps(group, &n) != 0) return false;
        for (int32 i = 0; i < n; ++i)
        {
            void* dsp = nullptr;
            char info[33]{};
            if (g_fmod.dsp_at(group, i, &dsp) == 0 && dsp && g_fmod.dsp_info(dsp, info, nullptr, nullptr, nullptr, nullptr) == 0 && std::strcmp(info, name) == 0) return true;
        }
        return false;
    }

    // MusicSub's channel group, on the way up from an instance's
    auto find_music_sub(void* group) -> void*
    {
        void* g = group;
        for (int depth = 0; depth < 8; ++depth)
        {
            void* parent = nullptr;
            if (g_fmod.parent_group(g, &parent) != 0 || !parent) return nullptr;
            g = parent;
            if (has_dsp(g, "HMX.TapeStartStop") && has_dsp(g, "HMX.Stutter")) return g;
        }
        return nullptr;
    }

    // An instance's channel group moved onto the strip, made (or moved under MusicSub again) first if it has to be
    auto move_to_strip(void* instance) -> void
    {
        void* group = nullptr;
        void* system = nullptr;
        if (g_fmod.instance_group(instance, &group) != 0 || !group || g_fmod.system_of(group, &system) != 0) return;
        void* music_sub = find_music_sub(group);
        std::lock_guard lock(g_strip_lock);
        if (!music_sub)
        {
            strip_line(STR("fifth deck stem left on Deck3's strip: no MusicSub group above its instance"));
            return;
        }
        if (g_strip.system != system || !g_strip.group)
        {
            void* strip = nullptr;
            const int32 made = g_fmod.create_group(system, "FNL Fifth Deck", &strip);
            if (made != 0 || !strip)
            {
                strip_line(std::format(STR("fifth deck stem left on Deck3's strip: createChannelGroup {}"), made));
                return;
            }
            g_strip = {system, nullptr, strip};
        }
        if (g_strip.parent != music_sub)
        {
            const int32 under = g_fmod.add_group(music_sub, g_strip.group, 1, nullptr);
            if (under != 0)
            {
                strip_line(std::format(STR("fifth deck stem left on Deck3's strip: its strip not put under MusicSub {} (addGroup {})"), music_sub, under));
                return;
            }
#if FNL_DEBUG
            strip_line(std::format(STR("fifth deck strip {} under MusicSub {}"), g_strip.group, music_sub));
#endif
            g_strip.parent = music_sub;
        }
        const int32 moved = g_fmod.add_group(g_strip.group, group, 1, nullptr);
        void* parent = nullptr;
        g_fmod.parent_group(group, &parent);
        if (moved != 0 || parent != g_strip.group)
            strip_line(std::format(STR("fifth deck stem {} not moved onto its strip: addGroup {}, now under {}"), group, moved, parent));
#if FNL_DEBUG
        else
            strip_line(std::format(STR("fifth deck stem {} moved onto its strip"), group));
#endif
    }

    // A noted fifth-deck voice, taken off the list
    auto take_deck_voice(void* voice) -> bool
    {
        for (auto& v : g_deck_voices)
        {
            void* expected = voice;
            if (voice && v.compare_exchange_strong(expected, nullptr)) return true;
        }
        return false;
    }

    // The stem event's ID string, from the Studio system the voice uses
    auto learn_stem_id(const uint8* voice) -> void
    {
        static std::atomic<bool> warned{false};
        const auto* system = *reinterpret_cast<const uint8* const*>(voice + kVoiceSystemOffset);
        const int32 type = system ? *reinterpret_cast<const int32*>(system + kSystemTypeOffset) : 0;
        void* studio = type == 1   ? *reinterpret_cast<void* const*>(system + kSystemStudioOffset1)
                       : type == 2 ? *reinterpret_cast<void* const*>(system + kSystemStudioOffset2)
                                   : nullptr;
        void* description = nullptr;
        FmodGuid id{};
        const int32 found = studio ? g_fmod.get_event(studio, "event:/song/stems/lead", &description) : -1;
        const int32 got = found == 0 && description ? g_fmod.event_id(description, &id) : -1;
        if (got != 0)
        {
            if (warned.exchange(true)) return;
            std::lock_guard lock(g_strip_lock);
            strip_line(std::format(STR("stem event's ID not found (system type {}, Studio {}, getEvent {}, getID {}): the fifth deck plays on Deck3's strip"), type,
                                   studio, found, got));
            return;
        }
        std::snprintf(g_stem_id, sizeof g_stem_id, "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", id.data1, id.data2, id.data3, id.data4[0], id.data4[1],
                      id.data4[2], id.data4[3], id.data4[4], id.data4[5], id.data4[6], id.data4[7]);
        g_stem_id_ready = true;
#if FNL_DEBUG
        std::lock_guard lock(g_strip_lock);
        strip_line(std::format(STR("track 17 plays event:/song/stems/lead by its ID {}"), to_wstring(g_stem_id)));
#endif
    }

    auto voice_start_detour(void* voice, void* arg, void* params, void* source) -> void
    {
        if (g_real_track && g_fmod_ready && voice && params && *reinterpret_cast<const int32*>(static_cast<uint8*>(params) + kParamsKindOffset) == kParamsEvent)
        {
            if (!g_stem_id_ready) learn_stem_id(static_cast<const uint8*>(voice));
            const char* path = *reinterpret_cast<const char* const*>(static_cast<uint8*>(params) + kParamsPathOffset);
            if (g_stem_id_ready && path && std::strcmp(path, g_stem_id) == 0) g_deck_voices[g_next_voice++ % kRecentVoices] = voice;
        }
        g_voice_start(voice, arg, params, source);
    }

    auto voice_callback_detour(uint32 type, void* instance, void* parameters) -> int32
    {
        bool ours = false;
        void* voice = nullptr;
        if (type == kInstanceReady && g_real_track && g_fmod_ready && g_fmod.instance_user_data(instance, &voice) == 0 && voice)
        {
            const auto* v = static_cast<const uint8*>(voice);
            ours = !v[kVoiceDoneOffset] && *reinterpret_cast<const int32*>(v + kVoiceStateOffset) != kVoiceStopped && take_deck_voice(voice);
        }
        const int32 result = g_voice_callback(type, instance, parameters);
        if (ours) move_to_strip(instance);
        return result;
    }

    // The FMOD thread's lines, logged here
    auto log_strip_lines() -> void
    {
        std::vector<StringType> lines{};
        {
            std::lock_guard lock(g_strip_lock);
            lines.swap(g_strip_lines);
        }
        for (const StringType& line : lines) Output::send<LogLevel::Normal>(STR("[5deck] {}\n"), line);
    }

    // ---------------------------------------------------------------- Hot Clip track volume --------
    // Hot Clips (track 5, a UAuditionAudioTrack) pick their FMOD event each time one starts (FUN_1405a5de0), from a
    // global mode (FUN_1405af090): 1 event:/song/quickclip, into bus:/Music with the stems' compressor; 0
    // event:/song/preview, into bus:/Music/Audition, 8 dB down. The gameplay pip dispatcher sets 1 on BeginPlay
    // (FUN_140628d90), but SetAuditionTracks (FUN_1405a1b90, a song audition on tracks 5-8) always leaves it at 0, so
    // every Hot Clip drops to the quiet event until the next gameplay start. While the Hot Clip Volume Normalisation
    // toggle is on, the mode is put back as it was after each call. Its hook is apart from the deck's: prepared with
    // them, installed at the first tick with the toggle on, whether or not a fifth deck ever goes up.
    using AuditionSetFn = void (*)(void* mgr, uint64 cel0, uint64 cel1, uint64 cel2, uint64 cel3, int32 arg, uint8 a, uint8 b, uint8 cutscene);

    AuditionSetFn g_audition_set = nullptr;
    int32* g_audition_mode = nullptr;
    funchook_t* g_clip_hooks = nullptr;
    bool g_clip_volume = false; // the Settings screen's Hot Clip Volume Normalisation toggle

    // The mode's address, from its setter FUN_1405a1b80: mov [rip+rel32], ecx; ret
    auto find_audition_mode() -> int32*
    {
        const auto setter = exe_at(g_addrs->set_audition_mode);
        uint8 code[7]{};
        if (!read_bytes(reinterpret_cast<const uint8*>(setter), code, sizeof code) || code[0] != 0x89 || code[1] != 0x0d || code[6] != 0xc3)
            return nullptr;
        int32 rel = 0;
        std::memcpy(&rel, code + 2, sizeof rel);
        return reinterpret_cast<int32*>(setter + 6 + rel);
    }

    auto audition_set_detour(void* mgr, uint64 cel0, uint64 cel1, uint64 cel2, uint64 cel3, int32 arg, uint8 a, uint8 b, uint8 cutscene) -> void
    {
        const int32 mode = g_audition_mode ? *g_audition_mode : 0;
        g_audition_set(mgr, cel0, cel1, cel2, cel3, arg, a, b, cutscene);
        if (g_audition_mode && g_clip_volume) *g_audition_mode = mode;
    }

    auto prepare_clip_hook() -> void
    {
        g_audition_set = reinterpret_cast<AuditionSetFn>(exe_at(g_addrs->audition_set));
        void* function = reinterpret_cast<void*>(g_audition_set);
        g_clip_hooks = funchook_create();
        const int prep = g_clip_hooks ? funchook_prepare(g_clip_hooks, reinterpret_cast<void**>(&g_audition_set), reinterpret_cast<void*>(&audition_set_detour)) : -1;
        if (prep == 0 && hook_in_range(function, reinterpret_cast<void*>(g_audition_set))) return;
        Output::send<LogLevel::Error>(STR("[5deck] Hot Clip volume hook: prepare {} at {} failed or out of range - no Hot Clip Volume Normalisation\n"), prep, function);
        if (g_clip_hooks) funchook_destroy(g_clip_hooks);
        g_clip_hooks = nullptr;
    }

    auto install_clip_hook() -> void
    {
        static bool tried = false;
        if (tried || !g_clip_volume || !g_clip_hooks) return;
        tried = true;
        g_audition_mode = find_audition_mode();
        if (!g_audition_mode)
        {
            Output::send<LogLevel::Warning>(STR("[5deck] audition event mode not found: a song audition still leaves Hot Clips on the preview event\n"));
            return;
        }
        Output::send<LogLevel::Normal>(STR("[5deck] Hot Clip volume hook: install {}\n"), funchook_install(g_clip_hooks, 0));
    }

    // ---------------------------------------------------------------- hooks -------------------------
    // Prepared at the mod's start, the first engine callback (funchook reserves its trampoline page near the exe while
    // that memory is still free, see hook_range.hpp), installed with the first fifth deck.
    auto prepare_hooks() -> void
    {
        const CharType* build = build_name();
        for (const BuildAddrs& row : kBuildAddrs)
            if (build && StringType(build) == row.name) g_addrs = &row;
        if (!g_addrs)
        {
            Output::send<LogLevel::Warning>(STR("[5deck] no fifth deck on {}: its game functions are not known for it\n"), build ? build : STR("this build"));
            return;
        }
        prepare_clip_hook();
        g_solo = reinterpret_cast<SoloFn>(exe_at(g_addrs->solo));
        g_mute = reinterpret_cast<MuteFn>(exe_at(g_addrs->mute));
        g_any_solo = reinterpret_cast<AnySoloFn>(exe_at(g_addrs->any_solo));
        g_apply = reinterpret_cast<ApplyFn>(exe_at(g_addrs->apply));
        g_copy_solo_mute = reinterpret_cast<CopySoloMuteFn>(exe_at(g_addrs->copy_solo_mute));
        g_drop_queue = reinterpret_cast<DropQueueFn>(exe_at(g_addrs->drop_queue));
        g_queued_cels = reinterpret_cast<QueuedCelsFn>(exe_at(g_addrs->queued_cels));
        g_transition_tick = reinterpret_cast<TransitionTickFn>(exe_at(g_addrs->transition_tick));
        g_play_transition = reinterpret_cast<PlayTransitionFn>(exe_at(g_addrs->play_transition));
        g_all_null = reinterpret_cast<AllNullFn>(exe_at(g_addrs->all_null));
        g_four_decks_empty = reinterpret_cast<DecksEmptyFn>(exe_at(g_addrs->four_decks_empty));
        g_set_gain = reinterpret_cast<GainFn>(exe_at(g_addrs->set_gain));
        g_stage_fade = reinterpret_cast<StageFn>(exe_at(g_addrs->stage_fade));
        g_hottest_fade = reinterpret_cast<StageFn>(exe_at(g_addrs->hottest_fade));
        g_eject_playing = reinterpret_cast<EjectForTransitionFn>(exe_at(g_addrs->eject_for_transition));
        g_eject_queued = reinterpret_cast<EjectQueueLocalFn>(exe_at(g_addrs->eject_queue_local));
        g_stage_reset = reinterpret_cast<StageFn>(exe_at(g_addrs->stage_reset));
        g_lobby_reset = reinterpret_cast<StageFn>(exe_at(g_addrs->lobby_reset));
        g_event_path = reinterpret_cast<EventPathFn>(exe_at(g_addrs->event_path));
        g_voice_start = reinterpret_cast<VoiceStartFn>(exe_at(g_addrs->voice_start));
        g_voice_callback = reinterpret_cast<VoiceCallbackFn>(exe_at(g_addrs->voice_callback));
        g_track_volume = reinterpret_cast<VolumeFn>(exe_at(g_addrs->track_volume));
        g_play_params = reinterpret_cast<PlayParamsFn>(exe_at(g_addrs->play_params));
        g_hooks = funchook_create();
        const struct
        {
            void** target;
            void* detour;
        } hooks[] = {{reinterpret_cast<void**>(&g_solo), reinterpret_cast<void*>(&solo_detour)},
                     {reinterpret_cast<void**>(&g_mute), reinterpret_cast<void*>(&mute_detour)},
                     {reinterpret_cast<void**>(&g_any_solo), reinterpret_cast<void*>(&any_solo_detour)},
                     {reinterpret_cast<void**>(&g_apply), reinterpret_cast<void*>(&apply_detour)},
                     {reinterpret_cast<void**>(&g_copy_solo_mute), reinterpret_cast<void*>(&copy_solo_mute_detour)},
                     {reinterpret_cast<void**>(&g_drop_queue), reinterpret_cast<void*>(&drop_queue_detour)},
                     {reinterpret_cast<void**>(&g_queued_cels), reinterpret_cast<void*>(&queued_cels_detour)},
                     {reinterpret_cast<void**>(&g_transition_tick), reinterpret_cast<void*>(&transition_tick_detour)},
                     {reinterpret_cast<void**>(&g_play_transition), reinterpret_cast<void*>(&play_transition_detour)},
                     {reinterpret_cast<void**>(&g_all_null), reinterpret_cast<void*>(&all_null_detour)},
                     {reinterpret_cast<void**>(&g_four_decks_empty), reinterpret_cast<void*>(&four_decks_empty_detour)},
                     {reinterpret_cast<void**>(&g_set_gain), reinterpret_cast<void*>(&set_gain_detour)},
                     {reinterpret_cast<void**>(&g_stage_fade), reinterpret_cast<void*>(&stage_fade_detour)},
                     {reinterpret_cast<void**>(&g_hottest_fade), reinterpret_cast<void*>(&hottest_fade_detour)},
                     {reinterpret_cast<void**>(&g_eject_playing), reinterpret_cast<void*>(&eject_playing_detour)},
                     {reinterpret_cast<void**>(&g_eject_queued), reinterpret_cast<void*>(&eject_queued_detour)},
                     {reinterpret_cast<void**>(&g_stage_reset), reinterpret_cast<void*>(&stage_reset_detour)},
                     {reinterpret_cast<void**>(&g_lobby_reset), reinterpret_cast<void*>(&lobby_reset_detour)},
                     {reinterpret_cast<void**>(&g_event_path), reinterpret_cast<void*>(&event_path_detour)},
                     {reinterpret_cast<void**>(&g_voice_start), reinterpret_cast<void*>(&voice_start_detour)},
                     {reinterpret_cast<void**>(&g_voice_callback), reinterpret_cast<void*>(&voice_callback_detour)},
                     {reinterpret_cast<void**>(&g_track_volume), reinterpret_cast<void*>(&track_volume_detour)},
                     {reinterpret_cast<void**>(&g_play_params), reinterpret_cast<void*>(&play_params_detour)}};
        for (const auto& h : hooks)
        {
            void* function = *h.target;
            const int prep = g_hooks ? funchook_prepare(g_hooks, h.target, h.detour) : -1;
            if (prep != 0 || !hook_in_range(function, *h.target))
            {
                Output::send<LogLevel::Error>(STR("[5deck] hooks: prepare {} at {} failed or out of range - no fifth deck\n"), prep, function);
                if (g_hooks) funchook_destroy(g_hooks);
                g_hooks = nullptr;
                return;
            }
        }
    }

    auto install_hooks() -> void
    {
        static bool tried = false;
        if (tried || !g_hooks) return;
        tried = true;
        const int inst = funchook_install(g_hooks, 0);
        g_hooks_installed = inst == 0;
        Output::send<LogLevel::Normal>(STR("[5deck] hooks (solo / mute, queue drop, transitions, stage gains, track 17's audio and strip, track 18's riser): install {}\n"), inst);
    }

    // ---------------------------------------------------------------- putting the deck up -----------
    // The deck shown or hidden, with the hooks and its audio track; the local solo / mute entries grown to every track
    auto put_up_deck(UObject* desk, bool visible) -> bool
    {
        UObject* deck = visible ? spawn_fifth_deck(desk) : spawn_hidden_deck(desk);
        if (!deck) return false;
        g_desk = FWeakObjectPtr(desk);
        g_deck = FWeakObjectPtr(deck);
        g_hidden = !visible;
#if FNL_DEBUG
        g_watch_until = GetTickCount64() + kWatchMs;
        g_event_lines = 0;
#endif
        install_hooks();
        ensure_solo_mute_size(deck_state(desk));
        put_in_real_track(desk);
        return true;
    }

#if FNL_DEBUG
    // A world point in an actor's own frame
    auto in_frame(UObject* actor, Vec world) -> Vec
    {
        Call t(own_call_fn(actor, STR("GetTransform")));
        Call c(STR("/Script/Engine.KismetMathLibrary:InverseTransformLocation"));
        if (!t.call(actor) || !c.copy_in(STR("T"), t.addr(STR("ReturnValue"))).set<Vec>(STR("Location"), world).call(math_lib())) return Vec{};
        return c.get<Vec>(STR("ReturnValue"));
    }

    // Each deck's disc VFX (BP_Deck's DiscVFX_* actors, BP_VFX_Disc) and their glow (DiscVizMesh, the stem's level
    // around the disc) in the deck's own frame, with the glow material's VU (the track whose level it shows)
    auto log_disc_vfx(UObject* desk) -> void
    {
        auto* decks = desk_decks(desk);
        if (!decks) return;
        for (int32 i = 0; i < decks->Num(); ++i)
        {
            UObject* d = (*decks)[i];
            if (!d) continue;
            const Vec turn = actor_rotation(d);
            for (const CharType* name : {STR("DiscVFX_CurrentlyPlaying"), STR("DiscVFX_Transition"), STR("DiscVFX_Placement")})
            {
                UObject* vfx = obj_prop(d, name);
                if (!vfx) continue;
                UObject* glow = obj_prop(vfx, STR("DiscVizMesh"));
                const Vec v = in_frame(d, actor_location(vfx)), vt = actor_rotation(vfx) - turn;
                const Vec g = glow ? in_frame(d, vec_of(glow, STR("K2_GetComponentLocation"))) : Vec{};
                const Vec gt = glow ? vec_of(glow, STR("K2_GetComponentRotation")) - turn : Vec{};
                const Vec gs = glow ? vec_of(glow, STR("K2_GetComponentScale")) : Vec{};
                Call shown(own_call_fn(glow, STR("IsVisible")));
                UObject* material = obj_prop(vfx, STR("DiscVizMaterial"));
                Call vu(own_call_fn(material, STR("K2_GetScalarParameterValue")));
                vu.set<FName>(STR("ParameterName"), FName(STR("VU"), FNAME_Add));
                Output::send<LogLevel::Normal>(STR("[5deck]   Decks[{}] {} {}: at ({:.2f}, {:.2f}, {:.2f}) turned ({:.1f}, {:.1f}, {:.1f}) on {}; glow at ({:.2f}, {:.2f}, {:.2f}) ")
                                                   STR("turned ({:.1f}, {:.1f}, {:.1f}) scale {:.3f} {}, VU {}\n"),
                                               i, name, name_of(vfx), v.x, v.y, v.z, wrap_degrees(vt.x), wrap_degrees(vt.y), wrap_degrees(vt.z), name_of(attach_parent(vfx)), g.x,
                                               g.y, g.z, wrap_degrees(gt.x), wrap_degrees(gt.y), wrap_degrees(gt.z), gs.x,
                                               glow && shown.call(glow) && shown.get<bool>(STR("ReturnValue")) ? STR("shown") : STR("hidden"),
                                               vu.call(material) ? std::format(STR("{:.2f}"), vu.get<float>(STR("ReturnValue"))) : StringType{STR("-")});
            }
        }
        CameraPose camera{};
        if (camera_pose(first_player(desk), &camera))
            Output::send<LogLevel::Normal>(STR("[5deck]   camera at ({:.1f}, {:.1f}, {:.1f}) pitch {:.1f} yaw {:.1f} roll {:.1f}; desk at {:.1f}, {:.1f}, {:.1f}\n"), camera.at.x,
                                           camera.at.y, camera.at.z, camera.turn.x, camera.turn.y, camera.turn.z, actor_location(desk).x, actor_location(desk).y,
                                           actor_location(desk).z);
    }

    auto on_key() -> void
    {
        UObject* desk = g_desk.Get();
        UObject* deck = g_deck.Get();
        if (!desk || !deck)
        {
            Output::send<LogLevel::Warning>(STR("[5deck] no fifth deck up: turn Fifth Deck on in Settings and go on stage (freestyle)\n"));
            if (UObject* stock = find_desk())
            {
                Output::send<LogLevel::Normal>(STR("[5deck] the desk's disc VFX as they stand:\n"));
                log_disc_vfx(stock);
            }
            return;
        }
        if (!g_hidden) relayout(desk);
        log_state(desk, deck);
        log_disc_vfx(desk);
    }
#endif

    // ---------------------------------------------------------------- where it goes up --------------
    // In a freestyle set (EHypeGameMode Freestyle) the deck goes up only with Fifth Deck on, shown straight away once
    // the desk view is ready (see desk_view below; in the menus the desk's 3D UI is never attached to the desk, so it
    // never goes up there).
    // In a multiplayer set (MultiplayerFreestyle, MultiplayerMainStage) it goes up whatever the toggle says. Other
    // players' actions reach this client's desk through its UDeskState (the action consumer's applier,
    // FUN_1406265e0) the way they reached theirs, so a SpawnDisc on track 17 lands on the fifth deck here as it did
    // there. With no deck on track 17, the desk's play, eject, queue and swap (FUN_1406109f0 and the others) do nothing
    // for it, and with no real audio track there it would make no sound anyway. So every client running this puts the
    // deck up by itself, hidden, once the desk is there and before the join snapshot (whose playing and queued stems
    // include track 17's) is applied, where it can be; with Fifth Deck on it is then shown once the desk view is ready.
    // A client without this hears nothing from the fifth deck.
    //   - A performer switch leaves the fifth deck as it is, like Deck0-3: a DJ with Fifth Deck on carries on with it.
    //     While the local player controls the decks (HypePlayerState._controllingDecksState is ControllingDecks) with
    //     Fifth Deck off, so with no controls for it, the deck is kept clear in the background: any stem playing or
    //     queued on it ejected, and its solo and mute released, sent like any other DJ action (see "keeping it clear").
    //     That covers taking over from a DJ who left it playing, a stem of theirs landing after the switch, and turning
    //     Fifth Deck off during one's own turn.
    //   - A hidden deck's discs (other players' stems arriving on it) are hidden as they reach it.
    // LevelLoadConfig's game mode already reads multiplayer while the player is still matchmaking in the world they are
    // leaving, so the deck waits for the set's own game state (a multiplayer set's is replicated from the server) and
    // is forgotten with it when the world changes.
    // The toggle is followed on stage: turned off, a shown deck is hidden (multiplayer) or cleared and removed
    // (freestyle: its stems ejected first, so track 17 falls silent, and the deck taken off the restored desk once they
    // have left it); turned on, a hidden deck is shown and a freestyle set gets one. Whenever the deck is forgotten,
    // track 17 gets its silent track back.
    constexpr uint8 kFreestyle = 0, kMultiplayerFreestyle = 4, kMultiplayerMainStage = 6; // EHypeGameMode
    constexpr uint8 kControllingDecks = 2;                                                // EControllingDecksState
    constexpr uint64 kPollMs = 250, kTurnPollMs = 100;
    constexpr float kDeskViewRange = 400.0f; // camera to desk in the desk view (about 155 in a freestyle set)
    constexpr uint64 kDeskStillMs = 1000;     // the desk in one place this long has finished moving into the view
    constexpr float kDeskStillTolerance = 0.1f;
    constexpr uint64 kRemoveWaitMs = 3000; // a removed deck's ejected stems are given this long to leave it
    constexpr size_t kSyncedTickOffset = 0x1040; // HypePlayerState: the join snapshot's tick once applied (FUN_140699ac0), else -1

    FWeakObjectPtr g_level_config{};
    FWeakObjectPtr g_session_state{}; // the game state of the set the deck went up in
    bool g_session_multiplayer = false;
    FWeakObjectPtr g_hidden_discs[2]{};
    uint64 g_remove_after = 0; // being removed: its stems ejected, taken off the desk once they have left it or then

    // ULevelLoadConfig._gameMode (EHypeGameMode), or -1
    auto game_mode() -> int
    {
        static uint64 next_scan = 0;
        UObject* config = cached_live(STR("LevelLoadConfig"), g_level_config, next_scan);
        auto* mode = config ? config->GetValuePtrByPropertyNameInChain<uint8>(STR("_gameMode")) : nullptr;
        return mode ? static_cast<int>(*mode) : -1;
    }

    // The context's game state when it is a set's of the kind wanted: a multiplayer set's
    // (AHypeGameStateMultiplayerFreestyle / AHypeGameStateMainStage), or another stage's (AHypeGameStateVenue)
    auto session_game_state(UObject* context, bool multiplayer) -> UObject*
    {
        static UClass* classes[] = {
            UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Hype.HypeGameStateMultiplayerFreestyle")),
            UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Hype.HypeGameStateMainStage")),
        };
        static UClass* venue = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Hype.HypeGameStateVenue"));
        UObject* game_state = game_state_of(context);
        if (!game_state) return nullptr;
        bool is_multiplayer = false;
        for (UClass* cls : classes) is_multiplayer = is_multiplayer || (cls && game_state->IsA(cls));
        if (multiplayer) return is_multiplayer ? game_state : nullptr;
        return !is_multiplayer && venue && game_state->IsA(venue) ? game_state : nullptr;
    }

    auto local_player_state(UObject* context) -> UObject* { return obj_prop(first_player(context), STR("PlayerState")); }

    auto synced_tick(UObject* player_state) -> int32
    {
        int32 tick = -1;
        if (player_state) read_bytes(reinterpret_cast<const uint8*>(player_state) + kSyncedTickOffset, reinterpret_cast<uint8*>(&tick), sizeof(tick));
        return tick;
    }

    // Everything about the deck dropped: removed, or its set gone (the world changed), at once, without waiting for the
    // old world's actors to be collected. HUD panels that outlive the desk go back to their stock places.
    auto forget_deck() -> void
    {
        restore_dummy_track();
        restore_hud_panels();
        g_original.panels.clear();
        g_deck = FWeakObjectPtr{};
        g_desk = FWeakObjectPtr{};
        g_session_state = FWeakObjectPtr{};
        g_session_multiplayer = false;
        for (FWeakObjectPtr& disc : g_hidden_discs) disc = FWeakObjectPtr{};
        g_hidden = false;
        g_placed.valid = false;
        g_remove_after = 0;
    }

    // The desk view: the desk's 3D UI attached to the desk (BP_Desk_3dUI attaches itself only when the game state and
    // the gameplay screen are up), the desk at rest (it rises into place at the start of a set, BP_Desk's PopUp
    // timeline: the round buttons' HUD panels are only right from where it stops) and the camera at
    // the desk
    struct DeskView
    {
        bool ready;
        float distance;
        Vec at;
        StringType why;
    };

    auto desk_view(UObject* desk) -> DeskView
    {
        static Vec still_at{};
        static uint64 still_since = 0;
        const uint64 now = GetTickCount64();
        DeskView v{false, -1.0f, actor_location(desk), {}};
        if (travel(v.at - still_at) > kDeskStillTolerance)
        {
            still_at = v.at;
            still_since = now;
        }
        UObject* desk_ui = find_desk_ui();
        Vec camera{};
        if (!desk_ui || attach_parent(desk_ui) != desk)
        {
            v.why = STR("the desk's 3D UI is not attached to the desk yet");
            return v;
        }
        if (!camera_location(first_player(desk), &camera))
        {
            v.why = STR("no camera");
            return v;
        }
        const Vec d = camera - v.at;
        v.distance = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (now - still_since < kDeskStillMs)
            v.why = std::format(STR("the desk is moving (at {:.1f}, {:.1f}, {:.1f})"), v.at.x, v.at.y, v.at.z);
        else if (v.distance > kDeskViewRange)
            v.why = std::format(STR("camera {:.1f} from the desk (needs {:.0f} or less)"), v.distance, kDeskViewRange);
        else
            v.ready = true;
        return v;
    }

    auto log_wait(const DeskView& v) -> void
    {
#if FNL_DEBUG
        static uint64 next_line = 0;
        const uint64 now = GetTickCount64();
        if (now < next_line) return;
        next_line = now + 5000;
        Output::send<LogLevel::Normal>(STR("[5deck] fifth deck waits for the desk view: {}\n"), v.why);
#else
        (void)v;
#endif
    }

    // The hidden deck shown with its controls and the widened desk
    auto show_hidden_deck(UObject* desk, UObject* deck) -> bool
    {
        const DeskView view = desk_view(desk);
        if (!view.ready)
        {
            log_wait(view);
            return false;
        }
        Layout lay{};
        auto* decks = desk_decks(desk);
        if (!decks || decks->Num() != 5 || !take_original(desk) || !compute_layout(&lay))
        {
            Output::send<LogLevel::Warning>(STR("[5deck] fifth deck left hidden: no desk layout ({} decks, desk mesh {})\n"), decks ? decks->Num() : -1,
                                            name_of(obj_prop(desk, STR("SM_Desk_v4"))));
            return false;
        }
        if (!wear_five_deck_mesh()) return false;
        g_hidden = false;
        set_hidden(deck, false);
        set_collision(deck, true);
        for (FWeakObjectPtr& slot : g_hidden_discs)
        {
            if (UObject* disc = slot.Get()) set_hidden(disc, false);
            slot = FWeakObjectPtr{};
        }
#if FNL_DEBUG
        log_layout(lay);
#endif
        set_pose(deck, lay.decks[4]);
        spawn_deck_ui((*decks)[3], deck, lay.uis[4]);
        apply_layout(desk, lay);
        Output::send<LogLevel::Normal>(STR("[5deck] desk view ready (camera {:.1f} from the desk at ({:.1f}, {:.1f}, {:.1f})): fifth deck shown\n"), view.distance,
                                       view.at.x, view.at.y, view.at.z);
        return true;
    }

    auto eject_queue_deck5(UObject* desk_state) -> bool
    {
        Call c(own_call_fn(desk_state, STR("EjectQueue")));
        c.set<uint8>(STR("trackType"), kTrack);
        return c.call(desk_state);
    }

    auto eject_deck5(UObject* desk_state) -> bool
    {
        Call c(own_call_fn(desk_state, STR("EjectTrack")));
        c.set<uint8>(STR("trackType"), kTrack).set<bool>(STR("isAutoEject"), false);
        return c.call(desk_state);
    }

    // The deck's queued and playing stems ejected through UDeskState, as its own controls do: what was done, or empty
    auto eject_stems(UObject* deck) -> StringType
    {
        UObject* desk_state = obj_prop(game_state_of(deck), STR("DeskState"));
        StringType done{};
        if (obj_prop(deck, STR("CurQueuedDisc"))) done += eject_queue_deck5(desk_state) ? STR(" queue ejected") : STR(" queue eject MISSING");
        if (obj_prop(deck, STR("CurPlayingDisc"))) done += eject_deck5(desk_state) ? STR(" stem ejected") : STR(" eject MISSING");
        return done;
    }

    // Fifth Deck turned off in a freestyle set: the stems ejected now, the deck removed when they have left it
    auto start_removal(UObject* deck) -> void
    {
        const StringType done = eject_stems(deck);
        g_remove_after = GetTickCount64() + (done.empty() ? 0 : kRemoveWaitMs);
        Output::send<LogLevel::Normal>(STR("[5deck] Fifth Deck off: removing the fifth deck:{}\n"), done.empty() ? StringType{STR(" nothing on it")} : done);
    }

    auto finish_removal(UObject* desk, UObject* deck) -> void
    {
        const bool empty = !obj_prop(deck, STR("CurQueuedDisc")) && !obj_prop(deck, STR("CurPlayingDisc"));
        if (!empty && GetTickCount64() < g_remove_after) return;
        if (!g_hidden) restore_desk(desk);
        remove_deck(desk, deck);
        forget_deck();
    }

    // The deck put up, shown, hidden or removed as the set and the Fifth Deck toggle want it
    auto manage_deck() -> void
    {
        static uint64 next = 0;
        const uint64 now = GetTickCount64();
        if (now < next || !g_hooks) return;
        next = now + kPollMs;
        if ((g_deck.ObjectIndex != -1 && !g_deck.Get()) || (g_session_state.ObjectIndex != -1 && !g_session_state.Get()))
        {
            Output::send<LogLevel::Normal>(STR("[5deck] set left: fifth deck forgotten\n"));
            forget_deck();
        }
        UObject* desk = g_desk.Get();
        UObject* deck = g_deck.Get();
        if (desk && deck)
        {
            if (g_remove_after)
                finish_removal(desk, deck);
            else if (!g_session_multiplayer && !g_enabled)
                start_removal(deck);
            else if (g_session_multiplayer && !g_enabled && !g_hidden)
            {
                hide_shown_deck(desk, deck);
                Output::send<LogLevel::Normal>(STR("[5deck] Fifth Deck off: the fifth deck is hidden for the rest of the set\n"));
            }
            else if (g_session_multiplayer && g_enabled && g_hidden)
                show_hidden_deck(desk, deck);
            return;
        }
        const int mode = game_mode();
        const bool multiplayer = mode == kMultiplayerFreestyle || mode == kMultiplayerMainStage;
        if (!multiplayer && !(mode == kFreestyle && g_enabled)) return;
        if (multiplayer) install_hooks(); // they pass through without the deck; the solo / mute padding is wanted before the join snapshot
        desk = find_desk();
        auto* decks = desk ? desk_decks(desk) : nullptr;
        UObject* session = desk ? session_game_state(desk, multiplayer) : nullptr;
        if (!decks || decks->Num() != 4 || !session) return;
        if (multiplayer)
        {
            const int32 synced = synced_tick(local_player_state(desk));
            if (!put_up_deck(desk, false)) return;
            Output::send<LogLevel::Normal>(STR("[5deck] multiplayer set (game mode {}): fifth deck up, {}; join snapshot {}\n"), mode,
                                           g_enabled ? STR("hidden until the desk view is ready") : STR("hidden (Fifth Deck off)"),
                                           synced == -1 ? StringType{STR("not applied yet")} : std::format(STR("ALREADY applied (tick {})"), synced));
        }
        else
        {
            const DeskView view = desk_view(desk);
            if (!view.ready)
            {
                log_wait(view);
                return;
            }
            if (!put_up_deck(desk, true)) return;
            Output::send<LogLevel::Normal>(STR("[5deck] freestyle set: fifth deck shown (camera {:.1f} from the desk)\n"), view.distance);
        }
        g_session_state = FWeakObjectPtr(session);
        g_session_multiplayer = multiplayer;
    }

    // The desk moved as a whole (risen into place at a set's start, or moved for recording) or the camera moved (still
    // closing in on the desk when the layout went up), and both came to rest: the round buttons' HUD panels worked out
    // again from there. A camera shake passes: the panels are moved only once the camera is still somewhere new.
    auto follow_desk() -> void
    {
        static uint64 next = 0, still_since = 0;
        static Vec still_at{};
        static CameraPose still_camera{};
        const uint64 now = GetTickCount64();
        if (!g_placed.valid || g_hidden || now < next) return;
        next = now + kPollMs;
        UObject* desk = g_desk.Get();
        if (!desk) return;
        const Vec at = actor_location(desk);
        CameraPose camera{};
        if (!camera_pose(first_player(desk), &camera)) return;
        if (travel(at - still_at) > kDeskStillTolerance || pose_change(camera, still_camera) > kDeskStillTolerance)
        {
            still_at = at;
            still_camera = camera;
            still_since = now;
            return;
        }
        if (now - still_since < kDeskStillMs) return;
        if (travel(at - g_placed.applied_at) <= kDeskStillTolerance && pose_change(camera, g_placed.camera) <= kDeskStillTolerance) return;
        g_placed.applied_at = at;
        g_placed.camera = camera;
        const Vec shift = at - g_placed.desk_at;
        move_hud_panels(desk, g_placed.moved_to, shift);
#if FNL_DEBUG
        Output::send<LogLevel::Normal>(STR("[5deck] desk and camera at rest (desk at ({:.1f}, {:.1f}, {:.1f}), moved ({:.1f}, {:.1f}, {:.1f}) since the layout; camera ")
                                           STR("at ({:.1f}, {:.1f}, {:.1f}) pitch {:.1f} yaw {:.1f}): HUD panels moved with them\n"),
                                       at.x, at.y, at.z, shift.x, shift.y, shift.z, camera.at.x, camera.at.y, camera.at.z, camera.turn.x, camera.turn.y);
#endif
    }

    auto net_solo_mute(UObject* audio, const CharType* fn, const CharType* flag) -> bool
    {
        Call c(own_call_fn(audio, fn));
        c.set<uint8>(STR("trackType"), kTrack).set<bool>(flag, false);
        return c.call(audio);
    }

    // ---------------------------------------------------------------- keeping it clear --------------
    // While the local player controls the decks in a multiplayer set with Fifth Deck off, every kTurnPollMs: a stem
    // playing or queued on the fifth deck is ejected and its solo released, through UDeskState and the player's
    // AudioController as its own controls do; so is its mute, unless that only follows another deck's solo (releasing
    // it would cancel that solo). While one is on its way, it is sent again only after kResendMs.
    constexpr uint64 kResendMs = 1000;

    auto keep_clear() -> void
    {
        static uint64 next = 0;
        static uint64 sent[4]{}; // eject, eject queue, solo release, mute release
        const uint64 now = GetTickCount64();
        if (now < next) return;
        next = now + kTurnPollMs;
        UObject* deck = g_deck.Get();
        if (!deck || !g_session_multiplayer || g_enabled) return;
        UObject* player_state = local_player_state(deck);
        auto* state = player_state ? player_state->GetValuePtrByPropertyNameInChain<uint8>(STR("_controllingDecksState")) : nullptr;
        if (!state || *state != kControllingDecks) return;
        UObject* game_state = game_state_of(deck);
        UObject* desk_state = obj_prop(game_state, STR("DeskState"));
        UObject* audio = obj_prop(first_player(deck), STR("AudioControllerComponent"));
        const auto due = [&](int i) {
            if (now - sent[i] < kResendMs) return false;
            sent[i] = now;
            return true;
        };
        StringType done{};
        if (obj_prop(deck, STR("CurPlayingDisc")) && due(0)) done += eject_deck5(desk_state) ? STR(" stem ejected") : STR(" eject MISSING");
        if (obj_prop(deck, STR("CurQueuedDisc")) && due(1)) done += eject_queue_deck5(desk_state) ? STR(" queue ejected") : STR(" queue eject MISSING");
        if (const uint8* s = solo_mute_entries(obj_prop(game_state, STR("DeckState"))))
        {
            bool solo_elsewhere = false;
            for (uint8 t = 0; t < 4; ++t) solo_elsewhere = solo_elsewhere || s[t * 2 + 1];
            if (s[kTrack * 2 + 1] && due(2))
                done += net_solo_mute(audio, STR("NetSoloTrack"), STR("shouldSolo")) ? STR(" solo released") : STR(" solo release MISSING");
            else if (s[kTrack * 2] && !s[kTrack * 2 + 1] && !solo_elsewhere && due(3))
                done += net_solo_mute(audio, STR("NetMuteTrack"), STR("shouldMute")) ? STR(" mute released") : STR(" mute release MISSING");
        }
        if (!done.empty()) Output::send<LogLevel::Normal>(STR("[5deck] controlling the decks with Fifth Deck off: fifth deck cleared:{}\n"), done);
    }

    auto hide_hidden_deck_discs() -> void
    {
        UObject* deck = g_hidden ? g_deck.Get() : nullptr;
        if (!deck) return;
        const CharType* slots[] = {STR("CurPlayingDisc"), STR("CurQueuedDisc")};
        for (int i = 0; i < 2; ++i)
        {
            UObject* disc = obj_prop(deck, slots[i]);
            if (!disc || g_hidden_discs[i].Get() == disc) continue;
            set_hidden(disc);
            g_hidden_discs[i] = FWeakObjectPtr(disc);
        }
    }
    // ---------------------------------------------------------------- settings ------------------------
    // The Fifth Deck toggle: at startup and on every change; acted on at the next tick, on stage too
    auto fifth_deck_set_enabled(bool on) -> void
    {
        if (on != g_enabled) Output::send<LogLevel::Normal>(STR("[5deck] Fifth Deck {}\n"), on ? STR("on") : STR("off"));
        g_enabled = on;
    }

    // The Hot Clip Volume Normalisation toggle: its hook goes in at the next tick with it on
    auto hot_clip_volume_set_enabled(bool on) -> void
    {
        if (on != g_clip_volume) Output::send<LogLevel::Normal>(STR("[5deck] Hot Clip Volume Normalisation {}\n"), on ? STR("on") : STR("off"));
        g_clip_volume = on;
    }

    // The two toggles on the Settings screen's Mods tab, under this mod's heading, each on a build its hooks are
    // prepared for
    const FnlApi* g_api = nullptr;
    FnlMod* g_self = nullptr;

    auto register_settings() -> void
    {
        auto toggle = [](const char* key, const CharType* label, void (*on_change)(void*, int32_t, int)) {
            FnlSetting s{};
            s.size = sizeof(s);
            s.key = key;
            s.label = label;
            s.kind = FNL_SETTING_TOGGLE;
            s.on_change = on_change;
            return s;
        };
        if (g_hooks)
        {
            const FnlSetting deck = toggle("FifthDeck", STR("Fifth Deck"), [](void*, int32_t v, int) { fifth_deck_set_enabled(v != 0); });
            g_api->add_setting(g_self, &deck);
        }
        if (g_clip_hooks)
        {
            const FnlSetting clips = toggle("HotClipVolume", STR("Hot Clip Volume Normalisation"), [](void*, int32_t v, int) { hot_clip_volume_set_enabled(v != 0); });
            g_api->add_setting(g_self, &clips);
        }
    }
} // namespace

auto fifth_deck_start(const FnlApi* api, FnlMod* self) -> void
{
    g_api = api;
    g_self = self;
    prepare_hooks();
    Hook::RegisterEngineTickPreCallback([](Hook::TCallbackIterationData<void>&, UEngine*, float, bool) {
#if FNL_DEBUG
        if (pressed()) on_key();
        watch_audio();
#endif
        install_clip_hook();
        manage_deck();
        follow_desk();
        keep_clear();
        hide_hidden_deck_discs();
        log_strip_lines();
    });
#if FNL_DEBUG
    // The fifth deck's own events (its BlueprintImplementableEvents and the delegates it binds: On* / _On*), as they
    // fire; not our own calls on it, nor per-frame Blueprint updates
    Hook::RegisterProcessEventPostCallback([](Hook::TCallbackIterationData<void>&, UObject* self, UFunction* fn, void*) {
        if (!self || !fn || GetTickCount64() >= g_watch_until || g_event_lines >= 200) return;
        const StringType n = fn->GetName();
        if (self != g_deck.Get()) return;
        if (n.rfind(STR("On"), 0) != 0 && n.rfind(STR("_On"), 0) != 0) return;
        ++g_event_lines;
        Output::send<LogLevel::Normal>(STR("[5deck] deck event {}\n"), n);
    });
#endif
    register_settings();
    Output::send<LogLevel::Verbose>(STR("[5deck] ready{}\n"), g_hooks ? STR("") : STR(" (not on this build: the deck never goes up)"));
}

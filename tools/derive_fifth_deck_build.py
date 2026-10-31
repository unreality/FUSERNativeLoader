"""Derive the fifth deck's per-build exe addresses (src/fifth_deck/fifth_deck.cpp, kBuildAddrs) for another FUSER executable.

The fifth deck calls and hooks about twenty-five game functions by address, known for EGS v1.4.2. For another build of the
same engine this finds each one with derive_mp_build.py's methods (prologue, call sites, vtable neighbours, native
pair tables, whole body) and compares every function's whole body with the reference, wildcarding relative fields:

  CONFIRMED   two methods agree and none disagree
  body same   the function's instructions are identical apart from relative addresses, so the field offsets it uses
              (and that fifth_deck.cpp mirrors) are unchanged

"check" symbols are not in the row: game functions using the unreflected offsets fifth_deck.cpp hard-codes but no
listed function touches; their bodies being the same shows those offsets hold.

It prints a table and the C++ kBuildAddrs entry for the target. Every address must be CONFIRMED with its body the
same before the entry is added: fifth_deck.cpp puts the deck up only on a build with a row.

Usage:
  python derive_fifth_deck_build.py --target <exe> --name "Steam CL1317755" [--ref <EGS v1.4.2 exe>] [--json out.json]
  python derive_fifth_deck_build.py --self-test     (reference against itself)
Needs pefile, capstone, numpy (as derive_build.py).
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import derive_build as db  # noqa: E402
import derive_mp_build as mp  # noqa: E402

# (C++ field, kind, EGS v1.4.2 RVA): fifth_deck.cpp's BuildAddrs members, in their order
SYMBOLS = [
    # solo / mute
    ("solo", "fn", 0x602F70),                 # UDeckState solo (hooked)
    ("mute", "fn", 0x5FD6F0),                 # UDeckState mute (hooked)
    ("any_solo", "fn", 0x5F9F10),             # any deck soloed (hooked)
    ("apply", "fn", 0x59C350),                # solo / mute applied to the audio tracks
    ("copy_solo_mute", "fn", 0x602AB0),       # solo / mute entries copied in (hooked)
    ("track_playing", "fn", 0x597CB0),
    ("audio_controller", "fn", 0x591640),
    ("track_mgr", "fn", 0x591800),
    # queue drop and transitions
    ("drop_queue", "fn", 0x5F3DA0),           # hooked
    ("queued_cels", "fn", 0x5D9E60),          # hooked
    ("transition_tick", "fn", 0x673230),      # hooked (desk state +0x148, ejected flag +0xdd)
    ("play_transition", "fn", 0x66EA80),      # PlayTransition (hooked; desk state +0x148, kit flag +0x118, mode +0xf9)
    ("spawn_local", "fn", 0x5F4C00),          # a disc spawned on a deck on this client only
    ("eject_for_transition", "fn", 0x5F4520),
    ("eject_queue_local", "fn", 0x5F4460),
    ("has_queued", "fn", 0x5F9D00),
    ("all_null", "fn", 0x5D6EE0),             # a cel list with nothing in it (hooked; PlayTransition's two calls)
    ("four_decks_empty", "fn", 0x5F10B0),     # ADesk::AreFourDecksEmpty (hooked; deck +0x2a0 playing disc)
    # gain over the multiplayer stages
    ("set_gain", "fn", 0x602B40),             # UDeckState gain for a track (hooked)
    ("stage_fade", "fn", 0x6303E0),           # the lobby's fade-out (hooked)
    ("hottest_fade", "fn", 0x638680),         # hottest-mix playback fade and restore (hooked)
    ("stage_reset", "fn", 0x633440),          # round start: gain back to 1.0, decks ejected (hooked)
    ("lobby_reset", "fn", 0x633F50),          # _OnDJQueueUpdated: the same reset in the lobby (hooked)
    # the deck's audio track and its sound
    ("event_path", "fn", 0x6C22D0),           # a track's FMOD event path (hooked; track list DAT_143b3e790)
    ("voice_start", "fn", 0x22F79F0),         # a voice started (hooked; event path +0x38, system +0x48 -> +0x10/+0x1c0/+0x438)
    ("voice_callback", "fn", 0x22F7E20),      # a voice's event instance callback (hooked; voice +0x1c, +0x1e0)
    ("track_volume", "fn", 0x5ABCC0),         # an audio track's volume curve (hooked; track index +0x5c, gain +0x4c)
    ("play_params", "fn", 0x5AF4B0),          # an audio track's voice parameters (hooked; play mode +0x88, once 0x100)
    # Hot Clip track volume
    ("audition_set", "fn", 0x5A1B90),         # SetAuditionTracks (hooked; leaves the audition event mode at 0)
    ("set_audition_mode", "fn", 0x5A1B80),    # the audition event mode's setter (read for the mode's address)
    # checks
    ("chk_solo_mute_entry", "check", 0x5F9F40),   # UDeckState solo / mute entries +0xb0
    ("chk_deck_gain", "check", 0x5F9A70),         # UDeckState gain floats +0xc0
    ("chk_late_join_wait", "check", 0x699AC0),    # HypePlayerState: the join snapshot's tick +0x1040
    ("chk_track_init", "check", 0x5A4A70),        # UAudioTrackMgr: each track's index +0x5c, mHypeAudio, mMgr
]


def run(ref_path, tgt_path, name, json_out):
    ref, tgt = db.Image(ref_path), db.Image(tgt_path)
    if (ref.stamp, ref.size) != (db.REF_BUILD["stamp"], db.REF_BUILD["size"]):
        sys.exit(f"reference is not {db.REF_BUILD['name']} (stamp {ref.stamp:#x}, size {ref.size:#x})")
    print(f"reference {db.REF_BUILD['name']}: {ref_path}\ntarget    {name}: {tgt_path}")
    print(f"engine: {db.version_strings(tgt)}\n")
    result, all_ok, bodies_differ = {}, True, []
    for field, kind, rva in SYMBOLS:
        votes, notes = mp.resolve_fn(ref, tgt, rva)
        addr, status = db.verdict(votes)
        entry = {"ref": rva, "rva": addr, "status": status, "notes": notes}
        same = None
        if addr is not None:
            same, n = mp.body_same(ref, tgt, rva, addr)
            entry["body_same"], entry["body_bytes"] = same, n
            if not same: bodies_differ.append(field)
        all_ok &= status == "CONFIRMED" and same is True
        result[field] = entry
        body = "" if same is None else ("body same" if same else "BODY DIFFERS")
        print(f"{field:24} {rva:#9x} -> {('%#x' % addr) if addr is not None else '-':>10}  {status:22} {body}")
        if status != "CONFIRMED":
            for n_ in notes: print(f"{'':26}{n_}")
    print(f"\n{sum(e['status'] == 'CONFIRMED' for e in result.values())}/{len(SYMBOLS)} CONFIRMED; "
          f"bodies differing: {bodies_differ or 'none'}")
    print("\nfifth_deck.cpp kBuildAddrs entry:")
    print(cpp_entry(name, result))
    print(f"\nOVERALL: {'all CONFIRMED, bodies same' if all_ok else 'NOT all confirmed with bodies the same - resolve before adding'}")
    if json_out:
        with open(json_out, "w") as f:
            json.dump({"name": name, "symbols": result, "ok": all_ok, "bodies_differ": bodies_differ}, f, indent=2)
    return all_ok


def cpp_entry(name, result):
    """A designated-initializer BuildAddrs row (check symbols left out)."""
    parts = [f".{field} = {result[field]['rva']:#x}" if result[field]["rva"] is not None else f".{field} = 0 /* NOT FOUND */"
             for field, kind, _ in SYMBOLS if kind != "check"]
    lines, cur = [], "         "
    for p in parts:
        if len(cur) + len(p) + 2 > 118:
            lines.append(cur.rstrip()); cur = "         "
        cur += p + ", "
    lines.append(cur.rstrip().rstrip(",") + "},")
    return f"        {{.name = STR(\"{name}\"),\n" + "\n".join(lines)


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ref", default=db.DEFAULT_REF, help="reference exe (EGS v1.4.2)")
    ap.add_argument("--target", help="exe to derive addresses for (a Steamless-unpacked copy for Steam)")
    ap.add_argument("--name", default="Steam CL1317755", help="build name: fue's kBuilds name for that exe")
    ap.add_argument("--json", help="write the result as JSON")
    ap.add_argument("--self-test", action="store_true", help="derive the reference from itself")
    a = ap.parse_args()
    if a.self_test:
        sys.exit(0 if run(a.ref, a.ref, db.REF_BUILD["name"], a.json) else 1)
    if not a.target: ap.error("--target or --self-test")
    sys.exit(0 if run(a.ref, a.target, a.name, a.json) else 1)


if __name__ == "__main__":
    main()

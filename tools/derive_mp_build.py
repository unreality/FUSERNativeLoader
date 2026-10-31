"""Derive FUSERMultiplayer.dll's per-build exe addresses (host_hook.cpp, kBuildAddrs) for another FUSER executable.

The multiplayer DLL calls and hooks about thirty game functions by address (the symbols in RETIRED are derived as well,
for the removed listen-host code, but are not in the DLL's row). They are known for EGS v1.4.2. For another
build of the same engine (the Steam exe, the later EGS exe) this finds each one with derive_build.py's methods
(prologue, call sites, vtable neighbours for functions; RIP-relative references for data) and also compares every
function's whole body with the reference, wildcarding rel32 / RIP-relative fields:

  CONFIRMED   two methods agree and none disagree
  body same   the function's instructions are identical apart from relative addresses, so the field offsets and
              vtable slots it uses (and that host_hook.cpp mirrors) are unchanged

It prints a table and the C++ kBuildAddrs entry for the target. Every address must be CONFIRMED before the entry is
added: the DLL treats a build as supported only when its row is complete.

Usage:
  python derive_mp_build.py --target <exe> --name "Steam CL1317755" [--ref <EGS v1.4.2 exe>] [--json out.json]
  python derive_mp_build.py --self-test     (reference against itself: everything must CONFIRM with bodies same)
Needs pefile, capstone, numpy (as derive_build.py).
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import derive_build as db  # noqa: E402

# (C++ field, kind, EGS v1.4.2 RVA, data extent). Field names are host_hook.cpp's BuildAddrs members; arrays use [i].
SYMBOLS = [
    # the Development server and the set driver (formerly kVerified)
    ("dev_vtable", "vtable", 0x2A8C270, "init_slot_fn"),  # extent: the anchor function stored in it
    ("init_slot_fn", "fn", 0x718A30, 0),
    ("build_stages[0]", "fn", 0x62F670, 0),
    ("build_stages[1]", "fn", 0x6080A0, 0),
    ("set_stage[0]", "fn", 0x636BF0, 0),
    ("set_stage[1]", "fn", 0x60F140, 0),
    ("set_stage[2]", "fn", 0x636BA0, 0),
    ("set_session_leader", "fn", 0x6965A0, 0),
    ("set_hype_user_id", "fn", 0x696260, 0),
    ("set_team", "fn", 0x6963E0, 0),
    ("assign_dj_indices", "fn", 0x636630, 0),
    ("rebuild_dj_queue", "fn", 0x633500, 0),
    ("set_dj_index", "fn", 0x6961B0, 0),
    ("broadcast", "fn", 0x35AB80, 0),
    ("local_player_mode", "fn", 0x6302F0, 0),
    # drop hooks (g_drop_hooks)
    ("drop_song_item_play", "fn", 0x7B34D0, 0),
    ("drop_desk_spawn_disc", "fn", 0x5FDBD0, 0),
    ("drop_local_disc_spawn", "fn", 0x6109F0, 0),
    ("drop_send_spawn_disc", "fn", 0x68AF70, 0),
    # gameplay helpers
    ("deck_disc_of", "fn", 0x62E320, 0),
    ("deck_desk_of", "fn", 0x5F8B50, 0),
    ("praise_mps_of", "fn", 0x6232B0, 0),
    ("stage_at_measure", "fn", 0x632440, 0),
    ("praise_count", "fn", 0x65D0B0, 0),
    ("venue_travel", "fn", 0x7B2DE0, 0),
    ("venue_end_play", "fn", 0x68BCA0, 0),
    ("stock_kick", "fn", 0x695990, 0),
    ("ps_set_game_session_id", "fn", 0x696220, 0),
    ("mm_join", "fn", 0x706890, 0),
    ("mm_set_state", "fn", 0x71BDD0, 0),
    ("mm_ticket_response", "fn", 0x71D110, 0),
    ("mm_ticket_status", "fn", 0x708610, 0),
    ("cel_missing_queue", "fn", 0x62A920, 0),
    ("cel_missing_snapshot", "fn", 0x621700, 0),
    ("travel_complete", "fn", 0x7B66D0, 0),
    ("actions_mark_dirty", "fn", 0x62AAE0, 0),
    ("action_items_grow", "fn", 0x62B300, 0),
    ("action_item_ctor", "fn", 0x619440, 0),
    ("action_append", "fn", 0x69F5E0, 0),
    ("action_counter", "data", 0x3B3D79C, 4),
    ("action_apply", "fn", 0x6265E0, 0),
    ("audio_of", "fn", 0x591640, 0),
    ("hud_presentation", "fn", 0x865BB0, 0),
    ("mps_client_tick_a", "fn", 0x630E20, 0),
    ("mps_client_tick_b", "fn", 0x6303E0, 0),
    ("mps_client_tick_c", "fn", 0x6306E0, 0),
    ("timeline_of", "fn", 0x591800, 0),
    ("clock_tick", "fn", 0x5928E0, 0),
    ("timeline_set_anchor", "fn", 0x5A1F20, 0),
    ("clock_measure", "fn", 0x592780, 0),
    ("requests_onrep", "fn", 0x65F980, 0),
    ("ps_set_ready_a", "fn", 0x69BC90, 0),
    ("ps_set_ready_b", "fn", 0x69B8D0, 0),
    ("mps_rebuild_set", "fn", 0x635810, 0),
    ("deck_set_volume", "fn", 0x602B40, 0),
    # song install (Route B, song transfers)
    ("platform_file_manager", "fn", 0x912720, 0),
    ("find_platform_file", "fn", 0x9103C0, 0),
    ("pak_mount", "fn", 0x17C5330, 0),
    ("cel_mgr_of", "fn", 0x5915C0, 0),
    ("cel_mgr_register", "fn", 0x59C740, 0),
    ("unlock_map_find", "fn", 0x4640E0, 0),
    ("unlock_map_alloc", "fn", 0x341BF0, 0),
    ("fname_hash", "fn", 0x9EAB10, 0),
    ("unlock_map_link", "fn", 0x358BC0, 0),
    # event highway and the Diamond Store
    ("new_highway_event", "fn", 0x771540, 0),
    ("ec_list_done", "fn", 0x790530, 0),
    ("ec_purchase_done", "fn", 0x791730, 0),
    ("claimed_done", "fn", 0x7901C0, 0),
    ("ec_purchase_request", "fn", 0x789280, 0),
    # shared mixes
    ("playback_load", "fn", 0x658A70, 0),
    # server content: the title screen's state setter (the login sync's hold before the main menu)
    ("title_set_state", "fn", 0x78D190, 0),
    # check only (not in the C++ row): game functions using the unreflected field offsets host_hook.cpp hard-codes
    # but none of the functions above touch. Their bodies being the same shows those offsets hold.
    ("chk_backend_token", "check", 0x6E30D0, 0),       # kBackendToken: BackendServerMgr +0x168
    ("chk_shell_screen_push", "check", 0x78EC20, 0),   # kScreenTable / kScreenMapKind: shell flow +0x1c0, entry +0x40
    ("chk_lobby_complete", "check", 0x62BAC0, 0),      # kEveryoneReady: MPS +0x2a0
    ("chk_mark_lobby_complete", "check", 0x695C40, 0),
    ("chk_request_mgr", "check", 0x659E10, 0),         # kLiveRequestSize: FMultiplayerLiveRequest 0x48
    ("chk_action_unique_id", "check", 0x62C780, 0),    # kActions / action_counter use
    ("chk_travel_driver", "check", 0x7B8560, 0),       # kTravelState: VenueTravelMgr +0x48
    ("chk_unlock_table_load", "check", 0x7427E0, 0),   # kUnlockNameMap / kUnlockSongRows: UnlockMgr +0x128 / +0x98
    ("chk_unlock_rows_of", "check", 0x7335B0, 0),      # the per-type row arrays (content.cpp): UnlockMgr +0x98..+0x118
    ("chk_hottest_mix_score", "check", 0x651A10, 0),   # kHistory: MPS +0x390
    ("chk_late_join_wait", "check", 0x699AC0, 0),      # kSotw: state of the world
]

# Symbols of the removed listen-host / sidecar code (the Development server, the set driver and the re-created server
# RPCs, removed from host_hook.cpp 2026-10-07; FDS replaced them). Still derived and body-checked, so a new build's
# table keeps the record, but left out of the C++ row: VerifiedBuild no longer has these members. Several "check"
# entries above (kScreenTable, kEveryoneReady, kLiveRequestSize, kActions, kTravelState, kHistory, kSotw) cover
# offsets only that code used.
RETIRED = {
    "dev_vtable", "init_slot_fn", "build_stages[0]", "build_stages[1]", "set_stage[0]", "set_stage[1]", "set_stage[2]",
    "set_session_leader", "set_hype_user_id", "set_team", "assign_dj_indices", "set_dj_index", "broadcast",
    "local_player_mode", "praise_mps_of", "stage_at_measure", "praise_count", "ps_set_game_session_id",
    "mm_ticket_response", "mm_ticket_status", "travel_complete", "actions_mark_dirty", "action_items_grow",
    "action_item_ctor", "action_append", "action_counter", "action_apply", "hud_presentation", "mps_client_tick_a",
    "mps_client_tick_b", "mps_client_tick_c", "timeline_of", "timeline_set_anchor", "clock_measure", "requests_onrep",
    "ps_set_ready_a", "ps_set_ready_b", "mps_rebuild_set",
    # the client workarounds (FDS_WORKAROUND: client_follow's queue rebuild, the lobby volume restore), removed
    # 2026-10-08 once FDS was confirmed to do it right
    "rebuild_dj_queue", "deck_set_volume",
}
assert RETIRED <= {s[0] for s in SYMBOLS}, RETIRED - {s[0] for s in SYMBOLS}


def body_insns(img, rva):
    """The function's code: from rva to the next .pdata start or the first jump table it indexes, whichever is first,
    without trailing int3 padding. Jump tables (`mov reg, [base + idx*4 + table_rva]`) sit after the code, and their
    contents and the padding before them move with the function."""
    i = int(db.np.searchsorted(img.fn_starts, rva, side="right"))
    end = min(int(img.fn_starts[i]) if i < len(img.fn_starts) else rva + 0x400, rva + 0x4000)
    ins = []
    for x in img.insns(rva, end - rva):
        if x.address >= end: break
        if x.disp_size == 4 and "rip" not in x.op_str:
            d = int.from_bytes(bytes(x.bytes[x.disp_offset:x.disp_offset + 4]), "little")
            if rva < d < end: end = d
        ins.append(x)
    ins = [x for x in ins if x.address < end]
    while ins and ins[-1].mnemonic == "int3": ins.pop()
    return ins


def norm(img, x):
    """db.wildcard plus the fields that move with the code: rel8 branch targets and image-relative (jump table)
    displacements."""
    b = db.wildcard(x)
    if x.imm_size == 1 and (x.mnemonic.startswith("j") or x.mnemonic.startswith("loop")):
        for k in range(x.imm_offset, x.imm_offset + 1): b[k] = None
    if x.disp_size == 4 and "rip" not in x.op_str:
        d = int.from_bytes(bytes(x.bytes[x.disp_offset:x.disp_offset + 4]), "little")
        if img.is_code(d):
            for k in range(x.disp_offset, x.disp_offset + 4): b[k] = None
    return b


def body_sig(img, rva):
    """The function's whole body, normalised."""
    return [b for x in body_insns(img, rva) for b in norm(img, x)]


def body_match(ref, tgt, rva, limit=2):
    """Target functions whose whole body equals the reference function's (wildcarded): (hits, unique in ref)."""
    sig = body_sig(ref, rva)
    if len(sig) < 8:
        return [], False
    in_ref = ref.find_all(sig, ref.exec, limit=2)
    return tgt.find_all(sig, tgt.exec, limit=limit), in_ref == [rva]


def ptr_slots(img, va_rva, limit=8):
    """8-aligned read-only data positions holding the VA of va_rva."""
    va = (img.base + va_rva).to_bytes(8, "little")
    out = []
    for a, b in img.rdata:
        p = img.mem.find(va, a, b)
        while p >= 0 and len(out) <= limit:
            if p % 8 == 0: out.append(p)
            p = img.mem.find(va, p + 1, b)
    return out


def read_ptr(img, pos):
    return int.from_bytes(img.mem[pos:pos + 8], "little") - img.base


VT_WINDOW = 32  # vtable slots compared on each side


def slot_fp(img, pos):
    p = read_ptr(img, pos)
    return db.fingerprint(img, p) if img.is_code(p) else ("data", img.mem[pos:pos + 8])


def vtable_window_votes(ref, tgt, rva):
    """Virtual overrides with no unique prologue / body and no callers (sibling classes often share an identical
    body): for each reference vtable slot holding the function, compare the window of +-VT_WINDOW slots, by function
    fingerprint, with the slot of every target function whose body equals it. The best target slot must match at
    least 90% and beat every other candidate; inherited neighbours alone cannot, as sibling vtables differ somewhere.
    Returns (votes, notes); a candidate whose body is also identical gets a second vote."""
    votes, notes = db.Counter(), []
    cands, _ = body_match(ref, tgt, rva, limit=64)
    span = range(-VT_WINDOW, VT_WINDOW + 1)
    for s in ptr_slots(ref, rva)[:8]:
        window = [slot_fp(ref, s + 8 * i) for i in span]
        scored = []
        for c in cands:
            for t in ptr_slots(tgt, c, limit=16):
                scored.append((sum(slot_fp(tgt, t + 8 * i) == w for i, w in zip(span, window)), c, t))
        scored.sort(reverse=True)
        if not scored: continue
        best, c, t = scored[0]
        runner = next((x[0] for x in scored[1:] if x[1] != c), 0)
        notes.append(f"vtable window at {s:#x}: best {c:#x} ({best}/{len(window)}), next other {runner}")
        if best >= 0.9 * len(window) and best > runner:
            votes[c] += 1
    for c in list(votes):
        votes[c] = min(votes[c], 1) + 1  # one for the window match, one for the identical body (cands are body matches)
    return votes, notes


def native_pair_votes(ref, tgt, rva):
    """UFunction thunks: UHT's FNameNativePtrPair tables hold {const char* name, FNativeFuncPtr}. Find the reference
    pair holding this function, then the target pair with the same name whose neighbouring pairs' names match too."""
    def name_at(img, pos):
        """The C identifier a pointer at pos points to, or None."""
        p = read_ptr(img, pos)
        if not (0 <= p < len(img.mem)): return None
        end = img.mem.find(b"\0", p, p + 256)
        s = img.mem[p:end] if end > p else b""
        return s if s and all(c == 0x5F or 0x30 <= c <= 0x39 or 0x41 <= c <= 0x5A or 0x61 <= c <= 0x7A for c in s) else None

    votes, notes = db.Counter(), []
    va = (ref.base + rva).to_bytes(8, "little")
    p = ref.mem.find(va)
    while p >= 0:
        n = name_at(ref, p - 8) if p % 8 == 0 else None
        if n:
            # the pair is {name at p-8, function at p}; the class's other pairs are 16 bytes apart. Different classes
            # can share a function name and its neighbours, so +-8 pairs' names must match.
            ctx = [name_at(ref, p - 8 + 16 * j) for j in range(-8, 9)]
            notes.append(f"native pair {n.decode(errors='replace')}")
            found = []
            s = tgt.mem.find(b"\0" + n + b"\0")
            while s >= 0:
                sva = (tgt.base + s + 1).to_bytes(8, "little")
                t = tgt.mem.find(sva)
                while t >= 0:
                    if t % 8 == 0 and [name_at(tgt, t + 16 * j) for j in range(-8, 9)] == ctx:
                        cand = read_ptr(tgt, t + 8)
                        if tgt.is_code(cand): found.append(cand)
                    t = tgt.mem.find(sva, t + 1)
                s = tgt.mem.find(b"\0" + n + b"\0", s + 1)
            if len(set(found)) == 1:
                votes[found[0]] += 1
            elif found:
                notes.append(f"native pair ambiguous: {[hex(c) for c in found]}")
            return votes, notes
        p = ref.mem.find(va, p + 1)
    return votes, notes


def resolve_fn(ref, tgt, rva):
    """derive_build's prologue / callers / vtable methods, plus: whole body, RIP-relative references to the function
    (delegate binding), vtable neighbours, and native pair tables."""
    votes, notes = db.resolve_function(ref, tgt, rva)
    hits, uniq = body_match(ref, tgt, rva)
    if uniq and len(hits) == 1:
        votes[hits[0]] += 1; notes.append(f"body -> {hits[0]:#x}")
    refs, rnotes = db.resolve_data(ref, tgt, rva, 1)
    for t in refs.elements(): votes[t] += 1
    notes += [n.replace("references", "address references") for n in rnotes]
    if sum(votes.values()) < 2:
        wv, wn = vtable_window_votes(ref, tgt, rva)
        votes.update(wv)
        notes += wn
        pv, pn = native_pair_votes(ref, tgt, rva)
        for t in pv: votes[t] += 1
        notes += pn + ([f"native pair -> {[hex(t) for t in pv]}"] if pv else [])
    # one candidate found by a single method (e.g. its only call site): its whole body matching the reference
    # (relative fields aside) is the second, independent check
    if len(votes) == 1 and sum(votes.values()) == 1:
        (c,) = votes
        if body_same(ref, tgt, rva, c)[0]:
            votes[c] += 1; notes.append(f"identical body at {c:#x}")
    return votes, notes


def resolve_vtable(ref, tgt, rva, anchors):
    """A vtable: its RIP-relative references, plus each anchor function (already derived) placed at the same slot."""
    votes, notes = db.resolve_data(ref, tgt, rva, 8)
    for field, ref_fn, tgt_fn in anchors:
        k = next((i for i in range(0, 600) if read_ptr(ref, rva + 8 * i) == ref_fn), None)
        if k is None or tgt_fn is None: continue
        for tslot in ptr_slots(tgt, tgt_fn, limit=16):
            votes[tslot - 8 * k] += 1
        notes.append(f"anchor {field} at slot {k}")
    return votes, notes


def body_same(ref, tgt, ref_rva, tgt_rva):
    """Whether the function's code is identical in both images, apart from the fields that move with the code
    (norm). Each side is cut at its own end (body_insns). Returns (same, bytes compared)."""
    a, t = body_sig(ref, ref_rva), body_sig(tgt, tgt_rva)
    return a == t, len(a)


def run(ref_path, tgt_path, name, json_out):
    ref, tgt = db.Image(ref_path), db.Image(tgt_path)
    if (ref.stamp, ref.size) != (db.REF_BUILD["stamp"], db.REF_BUILD["size"]):
        sys.exit(f"reference is not {db.REF_BUILD['name']} (stamp {ref.stamp:#x}, size {ref.size:#x})")
    print(f"reference {db.REF_BUILD['name']}: {ref_path}\ntarget    {name}: {tgt_path}")
    print(f"engine: {db.version_strings(tgt)}\n")
    result, all_ok, bodies_differ = {}, True, []
    # functions and data first: vtables are anchored on the functions stored in them
    ordered = [s for s in SYMBOLS if s[1] != "vtable"] + [s for s in SYMBOLS if s[1] == "vtable"]
    for field, kind, rva, extent in ordered:
        if kind in ("fn", "check"):
            votes, notes = resolve_fn(ref, tgt, rva)
        elif kind == "vtable":
            anchor = next(s for s in SYMBOLS if s[0] == extent)
            votes, notes = resolve_vtable(ref, tgt, rva, [(extent, anchor[2], result[extent]["rva"])])
        else:
            votes, notes = db.resolve_data(ref, tgt, rva, extent)
        addr, status = db.verdict(votes)
        entry = {"ref": rva, "rva": addr, "status": status, "notes": notes}
        same = None
        if kind in ("fn", "check") and addr is not None:
            same, n = body_same(ref, tgt, rva, addr)
            entry["body_same"], entry["body_bytes"] = same, n
            if not same: bodies_differ.append(field)
        all_ok &= status == "CONFIRMED"
        result[field] = entry
        body = "" if same is None else ("body same" if same else "BODY DIFFERS")
        if field in RETIRED: body += " (retired: not in the row)"
        print(f"{field:24} {rva:#9x} -> {('%#x' % addr) if addr is not None else '-':>10}  {status:22} {body}")
        if status != "CONFIRMED":
            for n_ in notes: print(f"{'':26}{n_}")
    print(f"\n{sum(e['status'] == 'CONFIRMED' for e in result.values())}/{len(SYMBOLS)} CONFIRMED; "
          f"bodies differing: {bodies_differ or 'none'}")
    print("\nhost_hook.cpp kBuildAddrs entry:")
    print(cpp_entry(name, result))
    print(f"\nOVERALL: {'all CONFIRMED' if all_ok else 'NOT all confirmed - resolve before adding'}"
          f"{'' if not bodies_differ else '; review the differing bodies for changed field offsets'}")
    if json_out:
        with open(json_out, "w") as f:
            json.dump({"name": name, "symbols": result, "ok": all_ok, "bodies_differ": bodies_differ}, f, indent=2)
    return all_ok


def cpp_entry(name, result):
    """A designated-initializer BuildAddrs row; array members are grouped. Check and retired symbols are left out."""
    groups, order = {}, []
    for field, kind, *_ in SYMBOLS:
        if kind == "check" or field in RETIRED: continue
        base = field.split("[")[0]
        if base not in groups: groups[base] = []; order.append(base)
        v = result[field]["rva"]
        groups[base].append(f"{v:#x}" if v is not None else "0 /* NOT FOUND */")
    parts = [f".{b} = {{{', '.join(groups[b])}}}" if len(groups[b]) > 1 or "[" in next(f for f, *_ in SYMBOLS if f.startswith(b))
             else f".{b} = {groups[b][0]}" for b in order]
    lines, cur = [], "            "
    for p in parts:
        if len(cur) + len(p) + 2 > 118:
            lines.append(cur.rstrip()); cur = "            "
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

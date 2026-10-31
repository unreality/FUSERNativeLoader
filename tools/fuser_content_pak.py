#!/usr/bin/env python3
"""
Packs FUSER custom content into a <id>_P.pak + .sig for Fuser/Content/Paks/custom_content/ (or for fuserd's server
content).

FUSERNativeLoader (src/custom_content.cpp) reads each pak's content.json at startup and registers the item; the
engine mounts the pak with the rest of Content/Paks and reads the item's files from it. Layout, mounted at
../../../Fuser/Content/:
    CustomContent/<id>/content.json
    CustomContent/<id>/<the item's files: a projector's <id>.mp4, an outfit part's cooked mesh, icon.png>

Content types:
  projector  a looping video for the Projector A/B/C stage screens.
             The stock screens are H.264 MP4s, 512x512, mostly 60 fps (some 30 or 24), seamless loops of about 6 s;
             a few carry an unused audio track. Any H.264 size, frame rate and length plays: the screens keep the
             video's aspect ratio and ignore its audio.
             The stage material recolours the video unless --use-original-video is given.
             --use-video-alpha chroma-keys a fixed dark purple, the background of the stock Corgi and BeachBalls
             videos (about RGB 80,12,75). Other background colours (black, white) are not keyed.
             --icon names a stock screen row (e.g. ColorGlow1, Corgi_ScreenItem) whose preview images the item
             shows in the menus; --icon-image replaces them with a PNG of its own.
             An encoder command that produces the stock format:
                 ffmpeg -i in.mov -vf "scale=512:512,fps=60" -t 6 -an -c:v libx264 -pix_fmt yuv420p -profile:v high out.mp4

  outfit_part
             a new character part (hat, glasses, ...) next to the stock ones: a static mesh cooked in the UE 4.24
             modkit at /Game/CustomContent/<id>/<mesh>, on a copy of a stock part (--part-template: its slot, hide
             rules and material slot names) with one colour variant copied from a stock variant (--variant-template:
             its material and palettes). The mesh's material slots must be named as the template part's meshes name
             theirs, including a "Hair" slot when the template has a HairMaterialSlot (the game puts the character's
             hair material on it, and on a one-slot mesh without it, on its only slot).
             --mesh-dir is the modkit's cooked folder holding <mesh>.uasset, .uexp (and .ubulk, if any).
             The variant's unlock row (DT_UnlocksOutfitPartVariants) is named <id>; the part is OP_<id>.

Preview icons (any type): --icon-image adds an 8-bit PNG as the item's menu image (content.json "icon_image").
The loader turns it into a texture at startup. The stock icons are 512x512 with a transparent background (outfit
part icons: 256x256, a light grey render on transparent).

Usage:
    python fuser_content_pak.py projector --id MyScreen --name "My Screen" --video my_screen.mp4
        [--use-video-alpha] [--use-original-video] [--tightness 0.0] [--icon ColorGlow1] [--icon-image icon.png]
        [--out-dir DIR]
    python fuser_content_pak.py outfit_part --id PumpkinLantern --name "Pumpkin Lantern" --mesh SM_Hat_PumpkinLantern
        --mesh-dir <modkit>/Saved/Cooked/WindowsNoEditor/<project>/Content/CustomContent/PumpkinLantern
        --part-template /Game/Character/Outfit/Hat/Baseball/OP_Hat_Baseball
        --variant-template /Game/Character/Outfit/Hat/Baseball/MV_Hat_Baseball_Palettet [--icon-image icon.png] [--out-dir DIR]
"""

import argparse
import json
import os
import re
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fuser_pak_create import create_pak  # noqa: E402

MOUNT = "../../../Fuser/Content/"
ICON_FILE = "icon.png"  # the icon's name in the item's folder
CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl"}


def mp4_boxes(data, start, end):
    """(type, payload_start, box_end) for each box in data[start:end]."""
    p = start
    while p + 8 <= end:
        size, kind = struct.unpack_from(">I4s", data, p)
        header = 8
        if size == 1:
            size = struct.unpack_from(">Q", data, p + 8)[0]
            header = 16
        elif size == 0:
            size = end - p
        if size < header or p + size > end:
            raise ValueError(f"malformed MP4 box at offset {p}")
        yield kind, p + header, p + size
        p += size


def mp4_info(path):
    """The first video track's codec, size and duration, and whether the file has other tracks."""
    with open(path, "rb") as f:
        data = f.read()
    info = {"codec": None, "width": None, "height": None, "seconds": None, "fps": None, "tracks": []}

    def walk(start, end, track):
        for kind, body, box_end in mp4_boxes(data, start, end):
            if kind == b"trak":
                t = {"handler": None, "codec": None}
                walk(body, box_end, t)
                info["tracks"].append(t)
                if t["handler"] == "vide" and info["codec"] is None:
                    info.update({k: t[k] for k in ("codec", "width", "height", "seconds", "fps") if k in t})
            elif kind in CONTAINERS:
                walk(body, box_end, track)
            elif kind == b"hdlr" and track is not None and track["handler"] is None:
                # mdia's handler (vide, soun) comes first; QuickTime files add a data handler (alis) in minf
                track["handler"] = data[body + 8:body + 12].decode("latin-1")
            elif kind == b"mdhd" and track is not None:
                version = data[body]
                if version == 1:
                    scale, duration = struct.unpack_from(">IQ", data, body + 20)
                else:
                    scale, duration = struct.unpack_from(">II", data, body + 12)
                track["timescale"] = scale
                track["seconds"] = duration / scale if scale else None
            elif kind == b"stsd" and track is not None:
                entry = body + 8  # version/flags, entry count
                track["codec"] = data[entry + 4:entry + 8].decode("latin-1")
                track["width"], track["height"] = struct.unpack_from(">HH", data, entry + 32)
            elif kind == b"stts" and track is not None:
                count = struct.unpack_from(">I", data, body + 4)[0]
                if count >= 1 and track.get("timescale"):
                    delta = struct.unpack_from(">I", data, body + 12)[0]
                    track["fps"] = track["timescale"] / delta if delta else None

    walk(0, len(data), None)
    return info


def check_video(path):
    try:
        info = mp4_info(path)
    except (ValueError, struct.error) as e:
        sys.exit(f"error: {path} is not a readable MP4 ({e})")
    if info["codec"] is None:
        sys.exit(f"error: {path} has no video track")
    if info["codec"] not in ("avc1", "avc3"):
        sys.exit(f"error: {path} is {info['codec']}, not H.264 (avc1): Windows Media Foundation in FUSER plays H.264")
    warnings = []
    notes = []
    if info["width"] != info["height"]:
        notes.append(f"{info['width']}x{info['height']} is not square: the screens keep its aspect ratio (stock: 512x512)")
    elif info["width"] != 512:
        notes.append(f"{info['width']}x{info['height']} differs from the stock 512x512")
    others = [str(t["handler"]) for t in info["tracks"] if t["handler"] != "vide"]
    if info["seconds"] and info["seconds"] > 30:
        warnings.append(f"{info['seconds']:.1f} s long: the screen loops it, and the whole file is held in memory while it plays")
    fps = f"{info['fps']:.0f} fps, " if info["fps"] else ""
    secs = f"{info['seconds']:.2f} s" if info["seconds"] else "unknown length"
    print(f"video: {info['codec']} {info['width']}x{info['height']}, {fps}{secs}, {os.path.getsize(path)} bytes")
    if others:
        notes.append(f"non-video tracks ({', '.join(others)}) are carried along; the screens play no sound")
    for n in notes:
        print(f"note: {n}")
    for w in warnings:
        print(f"warning: {w}")


def check_icon(path, stock_size=512):
    """An 8-bit PNG (the engine's PNG import takes 8 or 16 bits; 16-bit images show overbright in FUSER's menus)."""
    with open(path, "rb") as f:
        head = f.read(33)
    if len(head) < 33 or head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        sys.exit(f"error: {path} is not a PNG")
    width, height, depth, colour = struct.unpack(">IIBB", head[16:26])
    if depth != 8:
        sys.exit(f"error: {path} has {depth}-bit channels; save it as an 8-bit PNG")
    if colour == 3:
        sys.exit(f"error: {path} is a palette PNG; save it as 8-bit RGB or RGBA")
    print(f"icon: {width}x{height}, {'RGBA' if colour in (4, 6) else 'RGB'}")
    if (width, height) != (stock_size, stock_size):
        print(f"note: the stock icons are {stock_size}x{stock_size}; {width}x{height} is scaled by the menus")
    if colour not in (4, 6):
        print("note: no alpha channel - the stock icons have a transparent background")


def add_icon(a, manifest, files, stock_size=512):
    if a.icon_image:
        check_icon(a.icon_image, stock_size)
        manifest["icon_image"] = ICON_FILE
        files[ICON_FILE] = a.icon_image


def pack(content_id, manifest, files, out_dir):
    """files: {name in the item's folder: local path}"""
    os.makedirs(out_dir, exist_ok=True)
    stage = tempfile.mkdtemp(prefix="fuser_content_")
    try:
        folder = os.path.join(stage, "CustomContent", content_id)
        os.makedirs(folder)
        staged = []
        with open(os.path.join(folder, "content.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(manifest, f, indent=2, ensure_ascii=False)
            f.write("\n")
        staged.append(os.path.join(folder, "content.json"))
        for name, src in files.items():
            dst = os.path.join(folder, name)
            shutil.copyfile(src, dst)
            staged.append(dst)
        create_pak(os.path.join(out_dir, f"{content_id}_P.pak"), MOUNT, sorted(staged), root=stage)
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def cmd_projector(a):
    check_id(a.id)
    check_video(a.video)
    manifest = {
        "type": "projector",
        "id": a.id,
        "name": a.name or a.id,
        "video": f"{a.id}.mp4",
        "use_video_alpha": a.use_video_alpha,
        "use_original_video": a.use_original_video,
        "tightness": a.tightness,
        "icon": a.icon,
    }
    files = {f"{a.id}.mp4": a.video}
    add_icon(a, manifest, files)
    pack(a.id, manifest, files, a.out_dir)
    print(f"install: copy {a.id}_P.pak and {a.id}_P.sig into <FUSER>/Fuser/Content/Paks/custom_content/")


def check_id(content_id):
    if not re.fullmatch(r"[A-Za-z0-9_]{1,64}", content_id):
        sys.exit("error: --id must be 1-64 letters, digits or '_' (it becomes the item's table row name)")


def cmd_outfit_part(a):
    check_id(a.id)
    if len(a.id) > 61:
        sys.exit("error: an outfit part's --id is at most 61 characters (its part is OP_<id>)")
    if not re.fullmatch(r"[A-Za-z0-9_]+", a.mesh):
        sys.exit("error: --mesh is the cooked asset's name, without an extension")
    for flag, path in (("--part-template", a.part_template), ("--variant-template", a.variant_template)):
        if not re.fullmatch(r"/Game/[A-Za-z0-9_/]+", path):
            sys.exit(f"error: {flag} must be a stock /Game/ package path, e.g. /Game/Character/Outfit/Hat/Baseball/OP_Hat_Baseball")
    files = {}
    for ext, required in ((".uasset", True), (".uexp", True), (".ubulk", False)):
        src = os.path.join(a.mesh_dir, a.mesh + ext)
        if os.path.isfile(src):
            files[a.mesh + ext] = src
        elif required:
            sys.exit(f"error: {src} not found (cook the mesh in the modkit first)")
    # the package must have been cooked where the pak puts it, or the engine can't load it from there
    package = f"/Game/CustomContent/{a.id}/{a.mesh}".encode()
    with open(files[a.mesh + ".uasset"], "rb") as f:
        if package + b"\0" not in f.read():
            sys.exit(f"error: {a.mesh}.uasset was not cooked as {package.decode()}: import it there in the modkit and cook again")
    manifest = {
        "type": "outfit_part",
        "id": a.id,
        "name": a.name or a.id,
        "mesh": a.mesh,
        "part_template": a.part_template,
        "variant_template": a.variant_template,
    }
    add_icon(a, manifest, files, 256)
    for name, src in files.items():
        print(f"{name}: {os.path.getsize(src)} bytes")
    pack(a.id, manifest, files, a.out_dir)
    print(f"install: copy {a.id}_P.pak and {a.id}_P.sig into <FUSER>/Fuser/Content/Paks/custom_content/")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="type", required=True)
    pj = sub.add_parser("projector", help="a looping video for the Projector A/B/C stage screens")
    pj.add_argument("--id", required=True, help="unique item id (table row name): letters, digits, '_'")
    pj.add_argument("--name", help="display name in the menus (default: the id)")
    pj.add_argument("--video", required=True, help="H.264 MP4 to loop on the screens")
    pj.add_argument("--use-video-alpha", action="store_true", help="key out a dark purple (RGB ~80,12,75) background, as the stock Corgi screen does")
    pj.add_argument("--use-original-video", action="store_true", help="keep the video's own colours")
    pj.add_argument("--tightness", type=float, default=0.0, help="screen material Tightness (stock video screens: 0)")
    pj.add_argument("--icon", default="ColorGlow1", help="stock screen row whose preview images to show (default ColorGlow1)")
    pj.add_argument("--icon-image", help="8-bit PNG shown in the menus instead of the --icon row's images (stock: 512x512, transparent)")
    pj.add_argument("--out-dir", default=".", help="where to write <id>_P.pak and .sig")
    pj.set_defaults(func=cmd_projector)
    op = sub.add_parser("outfit_part", help="a new character part (hat, glasses, ...) with a cooked static mesh")
    op.add_argument("--id", required=True, help="unique item id (the variant's unlock row name): letters, digits, '_'")
    op.add_argument("--name", help="display name in the menus (default: the id)")
    op.add_argument("--mesh", required=True, help="the cooked static mesh's asset name (cooked at /Game/CustomContent/<id>/<mesh>)")
    op.add_argument("--mesh-dir", required=True, help="the modkit's cooked folder holding <mesh>.uasset / .uexp (/ .ubulk)")
    op.add_argument("--part-template", required=True, help="stock part to copy, e.g. /Game/Character/Outfit/Hat/Baseball/OP_Hat_Baseball")
    op.add_argument("--variant-template", required=True, help="stock colour variant to copy, e.g. /Game/Character/Outfit/Hat/Baseball/MV_Hat_Baseball_Palettet")
    op.add_argument("--icon-image", help="8-bit PNG shown in the character editor instead of the template part's icon")
    op.add_argument("--out-dir", default=".", help="where to write <id>_P.pak and .sig")
    op.set_defaults(func=cmd_outfit_part)
    a = p.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
FUSER (Harmonix, UE4) unencrypted override .pak writer.

Companion to fuser_pak_extract.py. Builds a plain, unencrypted UE4 pak format
version 8 file (FNameBasedCompressionMethod, no compression, no encryption) -
the exact shape already confirmed to be accepted by the game for override
content (matching the community FuserSongLoader tool's customSongsUnlocked_P.pak).
FUSERNativeLoader's build.ps1 uses it to pack each bundled mod's pak (<Mod>_P.pak).

Usage:
    python fuser_pak_create.py <output.pak> --mount <mount_point> [--root <dir>] <file1> [<file2> ...]

Example (mirrors an existing asset's own directory as the mount point, matching
the convention already used by the community's own override paks):
    python fuser_pak_create.py MyMod_P.pak \\
        --mount "../../../Fuser/Content/Gameplay/Hud/" \\
        WBP_UberHudList.uasset WBP_UberHudList.uexp

Each input file is stored under its own bare filename (no directory prefix),
resolved relative to --mount - matching how the game's real override paks are
laid out (one mount point per pak, flat file list underneath it).

Pass --root <dir> to instead store each file under its path relative to <dir>
(preserving subfolders) - needed when a single mount point's overrides span
multiple subdirectories, e.g. staging an entire Content/Gameplay/Hud/ override
tree (Hud/, Hud/SongList/SongItem/, Hud/Tabs/) into one pak.
"""

import argparse
import hashlib
import os
import struct
import sys
import zlib

PAK_MAGIC = 0x5A6F12E1
PAK_VERSION = 8

# --- .pak.sig companion file ---
#
# FUSER's shipping build has UE4 pak signature verification compiled in (confirmed via the
# exe's own "Signature check failed for pak file: %s" string), but the community's own, already-public, MIT-licensed FuserModDoorstop tool
# (github.com/Mettra/FuserSongLoader) - the winhttp.dll proxy DLL FUSERNativeLoader is built on -
# installs a runtime hook that unconditionally skips the actual
# verification (DoSignatureCheck_Hook just calls straight through to the success callback).
# That means the .sig file's cryptographic content is never actually checked; it only needs to
# exist and be structurally parseable. This is the exact format+bytes that tool already writes
# (PakSigFile::serialize in uasset.h, and the known_good_hash constant) - reused verbatim rather
# than invented, since the whole point is to match what's already running on this machine.
SIG_MAGIC = 0x73832DAA
SIG_VERSION = 1
SIG_CHUNK_SIZE = 64 * 1024

# Verbatim from FuserModDoorstop/src/uasset.h (MIT licensed) - a real signature blob whose
# actual cryptographic validity is irrelevant since the check that would read it is bypassed.
SIG_KNOWN_GOOD_HASH = bytes([
    0x98, 0xFE, 0xD5, 0x13, 0x09, 0xE7, 0xB9, 0x85, 0xB1, 0x93, 0x7F, 0x81, 0xC9, 0x20, 0x5F, 0x81,
    0x79, 0x9B, 0x3B, 0x2E, 0x05, 0x41, 0x77, 0x84, 0x52, 0xB0, 0xD2, 0xE7, 0x13, 0x77, 0x02, 0x9D,
    0x59, 0x23, 0xD3, 0x2F, 0x08, 0x06, 0x90, 0xEC, 0x16, 0xC1, 0x0B, 0x10, 0x9C, 0xC3, 0x1A, 0xEE,
    0x09, 0x32, 0x76, 0xEA, 0xB8, 0xE7, 0xAF, 0x9F, 0x19, 0xD0, 0x00, 0xA6, 0x36, 0x57, 0x7D, 0x87,
    0x71, 0x92, 0x86, 0xD5, 0x24, 0xE0, 0x22, 0x01, 0xED, 0xE5, 0xB6, 0xB3, 0x37, 0xBD, 0x58, 0x58,
    0xA5, 0xB7, 0x6F, 0x21, 0x60, 0xBD, 0x24, 0x71, 0x86, 0xB8, 0x6B, 0x92, 0xB8, 0x03, 0x27, 0x0C,
    0x70, 0xF0, 0x06, 0x99, 0x5D, 0x10, 0x23, 0xBE, 0x0B, 0x50, 0x07, 0x86, 0xAD, 0x18, 0x88, 0x12,
    0x49, 0xE1, 0x6A, 0xA6, 0xEC, 0x1D, 0x55, 0xAB, 0x1A, 0xC2, 0x32, 0x75, 0x54, 0x10, 0x7C, 0x1A,
    0x19, 0xE1, 0x29, 0xF4, 0xC3, 0x94, 0x0E, 0xB7, 0x8D, 0xA4, 0x0B, 0xC6, 0x89, 0xD9, 0x52, 0x83,
    0x0B, 0x94, 0xB8, 0x42, 0x5B, 0x0D, 0x14, 0xB2, 0x08, 0x69, 0x20, 0xA5, 0x09, 0x31, 0x83, 0x7D,
    0xBD, 0x86, 0xBC, 0x0B, 0x35, 0x3D, 0x57, 0x03, 0xBE, 0xAA, 0xEC, 0x26, 0xEE, 0xC0, 0x78, 0x3C,
    0x38, 0x99, 0x24, 0x7B, 0xCA, 0xC5, 0xA3, 0xBC, 0x65, 0x5A, 0x9B, 0x0E, 0x2C, 0x99, 0x30, 0x8B,
    0x8B, 0xD2, 0x19, 0xFD, 0x56, 0xD9, 0x6C, 0xF0, 0xF8, 0xFE, 0xB4, 0xF8, 0x7E, 0xD6, 0xBC, 0x6B,
    0xA2, 0xD3, 0xEF, 0x93, 0x3F, 0x88, 0x19, 0x73, 0xC1, 0x02, 0xD5, 0xF6, 0xCE, 0xBD, 0x7D, 0x2B,
    0x0E, 0x75, 0xF7, 0xB2, 0x09, 0x78, 0x1C, 0x1C, 0xD7, 0x32, 0x01, 0x8A, 0xE8, 0x16, 0x58, 0x48,
    0x94, 0x3A, 0x7B, 0x2C, 0x3F, 0x87, 0x3C, 0xC0, 0x4B, 0x5E, 0xE4, 0xE8, 0xB0, 0x6E, 0x1E, 0x44,
    0x5A, 0x1A, 0xBF, 0x72, 0x44, 0x27, 0x7A, 0x27, 0x49, 0xF2, 0x44, 0xDC, 0xBB, 0x87, 0x16, 0xFF,
    0xAF, 0xCD, 0x63, 0xD1, 0xAF, 0x48, 0xFF, 0x07, 0x80, 0x98, 0xEF, 0x22, 0x7D, 0xEE, 0x22, 0x5F,
    0xCB, 0x70, 0x51, 0x8B, 0x76, 0x06, 0x53, 0x94, 0xAE, 0x76, 0x32, 0x0A, 0x82, 0x6F, 0xB1, 0x7D,
    0xB2, 0x97, 0x06, 0x54, 0x9C, 0x63, 0x4A, 0x02, 0xD0, 0xBA, 0xD5, 0x59, 0x2C, 0x01, 0x9A, 0x16,
    0xA0, 0x0E, 0x57, 0x3F, 0xAF, 0xF4, 0xBD, 0xBE, 0x75, 0x07, 0x3A, 0x76, 0xD3, 0x38, 0x9A, 0x26,
    0xC6, 0x97, 0xBA, 0xCE, 0x23, 0x41, 0xC6, 0x8E, 0xD8, 0x0C, 0x25, 0xE0, 0x1E, 0xA1, 0x1A, 0x42,
    0x62, 0x50, 0xC2, 0x4D, 0xF2, 0x79, 0xB8, 0x32, 0xB9, 0xBD, 0x30, 0x9F, 0xE2, 0xFF, 0x62, 0xA6,
    0x72, 0x08, 0xDF, 0x44, 0xF8, 0xC2, 0xFA, 0x9B, 0xA0, 0xE9, 0x6B, 0x69, 0x80, 0xEF, 0x44, 0x91,
    0xBC, 0x8B, 0x94, 0x48, 0xD3, 0xB0, 0x56, 0xC7, 0x4D, 0x54, 0xEA, 0xF9, 0x32, 0x61, 0x4D, 0x9A,
    0x5B, 0x5D, 0x7F, 0x9D, 0x98, 0xA0, 0x9C, 0x9A, 0x6D, 0x11, 0x01, 0xBF, 0xA6, 0xEC, 0xD9, 0x7E,
    0x4E, 0x71, 0x6A, 0xC7, 0x32, 0xD5, 0x10, 0x26, 0x58, 0xC2, 0x3A, 0xBD, 0xA9, 0x4D, 0x78, 0x20,
    0x3F, 0xFE, 0xDB, 0x6E, 0xEA, 0x06, 0xAF, 0xA6, 0xAA, 0xAF, 0x56, 0xC6, 0x95, 0x3A, 0x0E, 0x89,
    0xA3, 0x52, 0x90, 0x09, 0xEC, 0x63, 0xDA, 0x75, 0x20, 0x65, 0xAF, 0xF9, 0xBC, 0x6C, 0xD9, 0x5F,
    0x00, 0xEE, 0x22, 0x2F, 0x55, 0x4C, 0x9C, 0x2E, 0xFE, 0x6A, 0x1F, 0x3D, 0xCC, 0xCF, 0x24, 0xD5,
    0x18, 0x25, 0x9B, 0x81, 0x05, 0x25, 0xAD, 0x24, 0x21, 0xC2, 0x6C, 0xC4, 0x14, 0x8B, 0x52, 0xEA,
    0x2B, 0x2C, 0x32, 0x57, 0x5D, 0xB4, 0xDE, 0x99, 0x35, 0x06, 0x49, 0xFB, 0x45, 0x7F, 0x3A, 0xCA,
])
assert len(SIG_KNOWN_GOOD_HASH) == 512


def build_sig_file(pak_bytes: bytes) -> bytes:
    chunks = []
    for start in range(0, len(pak_bytes), SIG_CHUNK_SIZE):
        chunk = pak_bytes[start:start + SIG_CHUNK_SIZE]
        chunks.append(zlib.crc32(chunk) & 0xFFFFFFFF)

    out = struct.pack('<II', SIG_MAGIC, SIG_VERSION)
    out += struct.pack('<I', len(SIG_KNOWN_GOOD_HASH)) + SIG_KNOWN_GOOD_HASH
    out += struct.pack('<I', len(chunks))
    for c in chunks:
        out += struct.pack('<I', c)
    return out


def write_fstring(s: str) -> bytes:
    """UE4 FString on-disk format: int32 length (positive = ANSI, includes null
    terminator), then bytes. Mount points and entry names in existing FUSER
    paks are plain ASCII, so we always use the positive/ANSI form."""
    data = s.encode('latin1') + b'\x00'
    return struct.pack('<i', len(data)) + data


def build_entry_header(size: int, uncompressed_size: int, file_hash: bytes) -> bytes:
    """53-byte no-name FPakEntry record (uncompressed, unencrypted). The Offset
    field is written as 0 - unused by the game's reader - the real position comes from the index's own Offset."""
    out = struct.pack('<qqq', 0, size, uncompressed_size)   # Offset, Size, UncompressedSize
    out += struct.pack('<I', 0)                              # CompressionMethodIndex = none
    out += file_hash.ljust(20, b'\x00')[:20]                 # Hash (20 bytes)
    out += struct.pack('<B', 0)                              # Flags = unencrypted
    out += struct.pack('<I', 0)                              # CompressionBlockSize
    assert len(out) == 53, len(out)
    return out


def build_index_entry(name: str, offset: int, size: int, uncompressed_size: int, file_hash: bytes) -> bytes:
    out = write_fstring(name)
    out += struct.pack('<qqq', offset, size, uncompressed_size)
    out += struct.pack('<I', 0)                # CompressionMethodIndex
    out += file_hash.ljust(20, b'\x00')[:20]    # Hash
    out += struct.pack('<B', 0)                 # Flags
    out += struct.pack('<I', 0)                 # CompressionBlockSize
    return out


def create_pak(out_path: str, mount_point: str, files, root: str = None):
    """files: list of local filesystem paths.

    If `root` is given, each file is stored under its path relative to `root`
    (forward slashes, e.g. "SongList/SongItem/WBP_SongItem_Horizontal_Diamond.uasset")
    - letting one pak with one mount point reproduce an entire override subtree exactly
    as the game's own FPakFile entry-name/mount-point concatenation expects (FPakEntry's
    `name` is a plain, unrestricted string, not required to be a bare filename).
    If `root` is omitted, falls back to the original bare-basename behavior (flat
    mount, matching this function's pre-existing single-directory use)."""
    if not mount_point.endswith('/'):
        mount_point += '/'

    entries = []  # (name, header_bytes, data_bytes, file_hash)
    for path in files:
        with open(path, 'rb') as f:
            data = f.read()
        file_hash = hashlib.sha1(data).digest()
        if root:
            name = os.path.relpath(path, root).replace(os.sep, '/')
        else:
            name = os.path.basename(path)
        header = build_entry_header(len(data), len(data), file_hash)
        entries.append((name, header, data, file_hash))

    with open(out_path, 'wb') as out:
        offsets = []
        for name, header, data, file_hash in entries:
            offsets.append(out.tell())
            out.write(header)
            out.write(data)

        index_offset = out.tell()
        index_body = write_fstring(mount_point)
        index_body += struct.pack('<i', len(entries))
        for (name, header, data, file_hash), off in zip(entries, offsets):
            index_body += build_index_entry(name, off, len(data), len(data), file_hash)

        out.write(index_body)
        index_size = len(index_body)
        index_hash = hashlib.sha1(index_body).digest()

        # FPakInfo (v8) serializes EncryptionKeyGuid (16 bytes) and bEncryptedIndex (1 byte) BEFORE
        # the magic. Omitting them makes the engine read the last 17 bytes of the
        # index as the key GUID; a non-zero GUID marks the pak as encrypted with an unregistered key,
        # so it is silently never mounted.
        out.write(b'\x00' * 16)                                   # EncryptionKeyGuid = none
        out.write(b'\x00')                                        # bEncryptedIndex = false
        footer = struct.pack('<II', PAK_MAGIC, PAK_VERSION)
        footer += struct.pack('<QQ', index_offset, index_size)
        footer += index_hash.ljust(20, b'\x00')[:20]
        assert len(footer) == 44, len(footer)
        out.write(footer)

        # Pak version 8 (FNameBasedCompressionMethod) appends a fixed CompressionMethods name
        # table AFTER the 44-byte core footer: MAX_NUM_COMPRESSION_METHODS (5) slots of 32 bytes
        # each. Confirmed empirically against a real working pak (customSongsUnlocked_P.pak) -
        # its tail has exactly 160 all-zero bytes past the core footer, matching "0 custom
        # compression methods" for a game where nothing is ever compressed. Omitting this entirely is what caused "Invalid pak file version (8)" - the
        # game's stricter runtime mount parser expects this table to be present for version 8.
        out.write(b'\x00' * (5 * 32))

    total = os.path.getsize(out_path)
    print(f"Wrote {out_path} ({total} bytes): mount={mount_point!r}, {len(entries)} entries")
    for name, header, data, file_hash in entries:
        print(f"  {name}  ({len(data)} bytes)")

    with open(out_path, 'rb') as f:
        pak_bytes = f.read()
    sig_bytes = build_sig_file(pak_bytes)
    sig_path = os.path.splitext(out_path)[0] + '.sig'
    with open(sig_path, 'wb') as f:
        f.write(sig_bytes)
    print(f"Wrote {sig_path} ({len(sig_bytes)} bytes, {len(pak_bytes) // SIG_CHUNK_SIZE + 1} chunk hashes)")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('output', help='path to the .pak file to create')
    p.add_argument('--mount', required=True, help='mount point, e.g. "../../../Fuser/Content/Gameplay/Hud/"')
    p.add_argument('--root', default=None,
                    help='if given, store each file under its path relative to this directory '
                         '(preserves subfolders under the mount point) instead of its bare filename')
    p.add_argument('files', nargs='+', help='local files to include')
    args = p.parse_args()
    create_pak(args.output, args.mount, args.files, root=args.root)


if __name__ == '__main__':
    main()

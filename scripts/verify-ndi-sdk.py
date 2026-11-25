#!/usr/bin/env python3
"""Verify an Android arm64-v8a libndi.so binary before bundling in a verification APK.

Checks:
  1. ELF64 AArch64 (EM_AARCH64 = 183) shared object.
  2. 16 KB page-size compatibility (every PT_LOAD p_align >= 16384).
  3. DT_NEEDED dependencies (warns on non-standard Android NDK system libs).
  4. Required NDI C API symbols exported in .dynsym.
"""

import struct
import sys
from pathlib import Path

EM_AARCH64 = 183
PT_LOAD = 1
PT_DYNAMIC = 2
DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
DT_SYMTAB = 6
SHT_DYNSYM = 11

ALLOWED_SYSTEM_LIBS = {
    "libc.so",
    "libm.so",
    "libdl.so",
    "liblog.so",
    "libz.so",
    "libandroid.so",
    "libmediandk.so",
    "libOpenSLES.so",
    "libGLESv2.so",
    "libGLESv3.so",
    "libEGL.so",
    "libc++_shared.so",
}

REQUIRED_NDI_SYMBOLS = [
    "NDIlib_initialize",
    "NDIlib_destroy",
    "NDIlib_find_create_v2",
    "NDIlib_find_destroy",
    "NDIlib_find_get_current_sources",
    "NDIlib_find_wait_for_sources",
    "NDIlib_recv_create_v3",
    "NDIlib_recv_destroy",
    "NDIlib_recv_connect",
    "NDIlib_recv_capture_v3",
    "NDIlib_recv_free_video_v2",
    "NDIlib_recv_free_audio_v3",
    "NDIlib_recv_free_metadata",
]


def read_cstr(blob: bytes, offset: int) -> str:
    end = blob.find(b"\x00", offset)
    if end < 0:
        end = len(blob)
    return blob[offset:end].decode("utf-8", errors="replace")


def check_libndi(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF":
        print(f"FAIL: {path} is not an ELF file")
        return 1
    ei_class, ei_data = data[4], data[5]
    if ei_class != 2 or ei_data != 1:
        print(f"FAIL: {path} is not ELF64 little-endian (class={ei_class}, data={ei_data})")
        return 1

    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    if e_machine != EM_AARCH64:
        print(f"FAIL: {path} e_machine={e_machine}, expected EM_AARCH64 ({EM_AARCH64})")
        return 1

    e_phoff, e_shoff = struct.unpack_from("<QQ", data, 32)
    e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from("<HHHH", data, 54)

    ok = True
    min_load_align = None
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, _p_flags, _p_off, _p_vaddr, _p_paddr, _p_filesz, _p_memsz, p_align = struct.unpack_from(
            "<IIQQQQQQ", data, off
        )
        if p_type == PT_LOAD:
            min_load_align = p_align if min_load_align is None else min(min_load_align, p_align)
            if p_align < 16384:
                print(f"FAIL: PT_LOAD[{i}] p_align={p_align} (< 16384; incompatible with 16 KB Android pages)")
                ok = False

    dynsym_names = set()
    needed = []
    dyn_entries = []
    dyn_strtab_off = None

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh = struct.unpack_from("<IIQQQQIIQQ", data, off)
        sections.append(sh)

    for sh in sections:
        sh_type, _flags, _addr, sh_offset, sh_size, sh_link, _info, _align, sh_entsize = sh[1:]
        if sh_type == SHT_DYNSYM and sh_entsize >= 24 and sh_link < len(sections):
            str_off = sections[sh_link][4]
            for pos in range(sh_offset, sh_offset + sh_size, sh_entsize):
                st_name, st_info, _other, st_shndx = struct.unpack_from("<IBBH", data, pos)
                if st_shndx != 0 and st_name > 0:
                    dynsym_names.add(read_cstr(data, str_off + st_name))
        elif sh_type == 6:  # SHT_DYNAMIC
            dyn_strtab_off = sections[sh_link][4] if sh_link < len(sections) else None
            for pos in range(sh_offset, sh_offset + sh_size, 16):
                d_tag, d_val = struct.unpack_from("<qQ", data, pos)
                if d_tag == DT_NULL:
                    break
                dyn_entries.append((d_tag, d_val))

    if dyn_strtab_off is not None:
        for d_tag, d_val in dyn_entries:
            if d_tag == DT_NEEDED:
                needed.append(read_cstr(data, dyn_strtab_off + d_val))

    missing = [s for s in REQUIRED_NDI_SYMBOLS if s not in dynsym_names]
    if missing:
        print(f"FAIL: missing required NDI symbols: {', '.join(missing)}")
        ok = False

    extra_needed = [lib for lib in needed if lib not in ALLOWED_SYSTEM_LIBS]
    print(f"File: {path} ({len(data)} bytes)")
    print(f"Arch: arm64-v8a (EM_AARCH64), min PT_LOAD p_align={min_load_align}")
    print(f"DT_NEEDED ({len(needed)}): {', '.join(needed) if needed else '(none)'}")
    if extra_needed:
        print(f"WARN: non-standard DT_NEEDED libraries: {', '.join(extra_needed)}")
    if ok:
        print("PASS: libndi.so passed ABI, 16 KB page alignment, and symbol checks.")
        return 0
    return 1


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <path/to/libndi.so>")
        return 2
    p = Path(sys.argv[1])
    if not p.is_file():
        print(f"FAIL: file not found: {p}")
        return 1
    return check_libndi(p)


if __name__ == "__main__":
    raise SystemExit(main())

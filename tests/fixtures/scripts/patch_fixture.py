#!/usr/bin/env python3
"""统一的 ELF fixture patch 入口。"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable, Iterable


class PatchError(RuntimeError):
    pass


def read_bytes(path: str | Path) -> bytearray:
    return bytearray(Path(path).read_bytes())


def write_bytes(path: str | Path, data: bytes | bytearray) -> None:
    Path(path).write_bytes(bytes(data))


def find_pattern(data: bytes | bytearray, pattern: Iterable[int], start: int = 0) -> int:
    pattern_bytes = bytes(pattern)
    return bytes(data).find(pattern_bytes, start)


def replace_first(data: bytearray, pattern: Iterable[int], replacement: Iterable[int], message: str) -> int:
    index = find_pattern(data, pattern)
    if index < 0:
        raise PatchError(message)
    data[index : index + len(bytes(pattern))] = bytes(replacement)
    return index


def u32(value: int) -> bytes:
    return struct.pack("<I", value)


def read_u32(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def write_u32(data: bytearray, offset: int, value: int) -> None:
    data[offset : offset + 4] = u32(value)


def run_tool(args: list[str], message: str) -> None:
    try:
        subprocess.run(args, check=True)
    except subprocess.CalledProcessError as exc:
        raise PatchError(message) from exc


def dump_sections(objcopy: str, input_path: str, temp_dir: Path, names: Iterable[str]) -> dict[str, Path]:
    paths: dict[str, Path] = {}
    command = [objcopy]
    for name in names:
        section_file = temp_dir / f"{name.removeprefix('.').replace('.', '_')}.bin"
        paths[name] = section_file
        command.extend(["--dump-section", f"{name}={section_file}"])
    command.append(input_path)
    run_tool(command, f"导出 {'/'.join(names)} 失败")
    return paths


def update_sections(objcopy: str, input_path: str, output_path: str, sections: dict[str, Path], message: str) -> None:
    command = [objcopy]
    for name, section_file in sections.items():
        command.extend(["--update-section", f"{name}={section_file}"])
    command.extend([input_path, output_path])
    run_tool(command, message)


def patch_addrx_to_gnu_addr_index(args: argparse.Namespace) -> None:
    data = read_bytes(args.input_path)
    replacements = 0
    index = 0
    while index <= len(data) - 3:
        if data[index] == 0x02 and data[index + 1] == 0xA1:
            data[index + 1] = 0xFB
            replacements += 1
            index += 3
        else:
            index += 1
    if replacements == 0:
        raise PatchError("未找到可替换的 DW_OP_addrx 模式")
    write_bytes(args.output_path, data)


def build_debug_sup_sidecar(args: argparse.Namespace) -> None:
    with tempfile.TemporaryDirectory(prefix="elf-static-view-debug-sup-") as temp:
        section_path = Path(temp) / "debug_sup.bin"
        file_name = Path(args.input_path).name.encode("ascii")
        checksum = b"fake checksum_content"
        # .debug_sup 固定构造 DWARF5 sidecar 引用，保持原 PowerShell fixture 语义。
        content = bytearray([0x02, 0x00, 0x01])
        content.extend(file_name)
        content.append(0x00)
        content.append(len(checksum) + 1)
        content.extend(checksum)
        content.append(0x00)
        write_bytes(section_path, content)
        run_tool(
            [args.objcopy_path, "--add-section", f".debug_sup={section_path}", args.input_path, args.output_path],
            "写入 .debug_sup section 失败",
        )


def patch_ranges_to_rnglistx(args: argparse.Namespace) -> None:
    with tempfile.TemporaryDirectory(prefix="elf-static-view-rnglistx-") as temp:
        sections = dump_sections(args.objcopy_path, args.input_path, Path(temp), [".debug_abbrev", ".debug_info", ".debug_rnglists"])
        abbrev = read_bytes(sections[".debug_abbrev"])
        match = replace_first(abbrev, [0x55, 0x17], [0x55, 0x23], "未找到 DW_AT_ranges + DW_FORM_sec_offset 模式")
        del match
        info = read_bytes(sections[".debug_info"])
        ranges_payload_offset = 0x59
        info[ranges_payload_offset : ranges_payload_offset + 4] = bytes([0x80, 0x80, 0x80, 0x00])
        rnglists = read_bytes(sections[".debug_rnglists"])
        if len(rnglists) < 0x0C:
            raise PatchError(".debug_rnglists 长度不足，无法补 offset-entry table")
        patched_rnglists = bytearray(len(rnglists) + 4)
        patched_rnglists[:8] = rnglists[:8]
        write_u32(patched_rnglists, 8, 1)
        write_u32(patched_rnglists, 12, 4)
        patched_rnglists[16:] = rnglists[12:]
        write_u32(patched_rnglists, 0, read_u32(rnglists, 0) + 4)
        write_bytes(sections[".debug_abbrev"], abbrev)
        write_bytes(sections[".debug_info"], info)
        write_bytes(sections[".debug_rnglists"], patched_rnglists)
        update_sections(
            args.objcopy_path,
            args.input_path,
            args.output_path,
            {".debug_abbrev": sections[".debug_abbrev"], ".debug_info": sections[".debug_info"], ".debug_rnglists": sections[".debug_rnglists"]},
            "回写 .debug_abbrev/.debug_info/.debug_rnglists 失败",
        )


def patch_rnglists_payload(args: argparse.Namespace, payload: bytes) -> None:
    with tempfile.TemporaryDirectory(prefix="elf-static-view-rnglists-") as temp:
        sections = dump_sections(args.objcopy_path, args.input_path, Path(temp), [".debug_rnglists"])
        rnglists = read_bytes(sections[".debug_rnglists"])
        if len(rnglists) < 0x25:
            raise PatchError(".debug_rnglists 长度不足")
        new_length = 16 + len(payload)
        patched = bytearray(new_length)
        patched[:16] = rnglists[:16]
        patched[16:] = payload
        write_u32(patched, 0, new_length - 4)
        write_bytes(sections[".debug_rnglists"], patched)
        update_sections(args.objcopy_path, args.input_path, args.output_path, {".debug_rnglists": sections[".debug_rnglists"]}, "回写 patch 后 section 失败")


def patch_rnglists_start_end(args: argparse.Namespace) -> None:
    patch_rnglists_payload(args, bytes([0x07, 0x00, 0x00, 0x00, 0x00, 0x18, 0x11, 0x40, 0x00, 0x00, 0x00, 0x00, 0x23, 0x11, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00]))


def patch_rnglists_offset_pair(args: argparse.Namespace) -> None:
    patch_rnglists_payload(args, bytes([0x05, 0x18, 0x23, 0x00]))


def patch_rnglists_base_addressx(args: argparse.Namespace) -> None:
    patch_rnglists_payload(args, bytes([0x02, 0x00, 0x05, 0x18, 0x23, 0x00]))


def patch_rnglists_startx_endx(args: argparse.Namespace) -> None:
    patch_rnglists_payload(args, bytes([0x03, 0x00, 0x01, 0x00]))


def patch_rnglists_startx_length(args: argparse.Namespace) -> None:
    patch_rnglists_payload(args, bytes([0x04, 0x00, 0x0B, 0x00]))

def insert_loclist_prefix(args: argparse.Namespace, insert: bytes, prefix: str) -> None:
    with tempfile.TemporaryDirectory(prefix=prefix) as temp:
        sections = dump_sections(args.objcopy_path, args.input_path, Path(temp), [".debug_loclists"])
        loclists = read_bytes(sections[".debug_loclists"])
        patched = bytearray(len(loclists) + len(insert))
        patched[:0x10] = loclists[:0x10]
        patched[0x10 : 0x10 + len(insert)] = insert
        patched[0x10 + len(insert) :] = loclists[0x10:]
        write_u32(patched, 0, read_u32(loclists, 0) + len(insert))
        write_bytes(sections[".debug_loclists"], patched)
        update_sections(args.objcopy_path, args.input_path, args.output_path, {".debug_loclists": sections[".debug_loclists"]}, "回写 patch 后 section 失败")


def patch_loclists_base_default(args: argparse.Namespace) -> None:
    insert_loclist_prefix(args, bytes([0x06, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x01, 0x50]), "elf-static-view-loclists-base-default-")


def patch_loclists_base_addressx(args: argparse.Namespace) -> None:
    insert_loclist_prefix(args, bytes([0x01, 0x00, 0x05, 0x01, 0x50]), "elf-static-view-loclists-base-addressx-")


def replace_loclist_entry(args: argparse.Namespace, replacement: bytes, message: str) -> None:
    with tempfile.TemporaryDirectory(prefix="elf-static-view-loclists-") as temp:
        sections = dump_sections(args.objcopy_path, args.input_path, Path(temp), [".debug_loclists"])
        loclists = read_bytes(sections[".debug_loclists"])
        if len(loclists) < 0x23:
            raise PatchError(message)
        replaced_length = 14
        delta = len(replacement) - replaced_length
        patched = bytearray(len(loclists) + delta)
        patched[:0x15] = loclists[:0x15]
        patched[0x15 : 0x15 + len(replacement)] = replacement
        patched[0x15 + len(replacement) :] = loclists[0x15 + replaced_length :]
        write_u32(patched, 0, read_u32(loclists, 0) + delta)
        write_bytes(sections[".debug_loclists"], patched)
        update_sections(args.objcopy_path, args.input_path, args.output_path, {".debug_loclists": sections[".debug_loclists"]}, "回写 patch 后 section 失败")


def patch_loclists_start_end(args: argparse.Namespace) -> None:
    replace_loclist_entry(args, bytes([0x07, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x75, 0x2A, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x1A, 0x9F]), ".debug_loclists 长度不足，无法改写成 start_end")


def patch_loclists_start_length(args: argparse.Namespace) -> None:
    replace_loclist_entry(args, bytes([0x08, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x0A, 0x75, 0x2A, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x1A, 0x9F]), ".debug_loclists 长度不足，无法改写成 start_length")


def patch_loclists_in_place(args: argparse.Namespace, transform: Callable[[bytearray], None], length_message: str, min_length: int = 0x23) -> None:
    with tempfile.TemporaryDirectory(prefix="elf-static-view-loclists-") as temp:
        sections = dump_sections(args.objcopy_path, args.input_path, Path(temp), [".debug_loclists"])
        loclists = read_bytes(sections[".debug_loclists"])
        if len(loclists) < min_length:
            raise PatchError(length_message)
        transform(loclists)
        write_bytes(sections[".debug_loclists"], loclists)
        update_sections(args.objcopy_path, args.input_path, args.output_path, {".debug_loclists": sections[".debug_loclists"]}, "回写 patch 后 section 失败")


def patch_loclists_startx_endx(args: argparse.Namespace) -> None:
    def transform(loclists: bytearray) -> None:
        loclists[0x15] = 0x02
        loclists[0x17] = 0x01
    patch_loclists_in_place(args, transform, ".debug_loclists 长度不足，无法改写成 startx_endx")


def patch_loclists_startx_length(args: argparse.Namespace) -> None:
    def transform(loclists: bytearray) -> None:
        loclists[0x15] = 0x03
    patch_loclists_in_place(args, transform, ".debug_loclists 长度不足，无法改写成 startx_length", 0x1B)


def patch_type_unit_abbrev_offset(args: argparse.Namespace) -> None:
    with tempfile.TemporaryDirectory(prefix="elf-static-view-type-unit-offset-") as temp:
        sections = dump_sections(args.objcopy_path, args.input_path, Path(temp), [".debug_abbrev", ".debug_info"])
        abbrev = read_bytes(sections[".debug_abbrev"])
        info = read_bytes(sections[".debug_info"])
        if len(info) < 12:
            raise PatchError(".debug_info 长度不足，无法修改 type unit header")
        new_abbrev_offset = len(abbrev)
        patched_abbrev = bytearray(len(abbrev) * 2)
        patched_abbrev[: len(abbrev)] = abbrev
        patched_abbrev[len(abbrev) :] = abbrev
        write_u32(info, 8, new_abbrev_offset)
        write_bytes(sections[".debug_abbrev"], patched_abbrev)
        write_bytes(sections[".debug_info"], info)
        update_sections(args.objcopy_path, args.input_path, args.output_path, {".debug_abbrev": sections[".debug_abbrev"], ".debug_info": sections[".debug_info"]}, "回写 patch 后 section 失败")
def splice_bytes(data: bytes | bytearray, offset: int, original_length: int, replacement: bytes) -> bytearray:
    return bytearray(data[:offset]) + bytearray(replacement) + bytearray(data[offset + original_length :])


def patch_dwarf5_strx_form(args: argparse.Namespace) -> None:
    modes = {
        "strx2": (0x26, bytes([0x06, 0x00])),
        "strx3": (0x27, bytes([0x06, 0x00, 0x00])),
        "strx4": (0x28, bytes([0x06, 0x00, 0x00, 0x00])),
    }
    if args.mode not in modes:
        raise PatchError("Mode 必须是 strx2/strx3/strx4")
    form_byte, new_payload = modes[args.mode]
    with tempfile.TemporaryDirectory(prefix="elf-static-view-dwarf5-strx-form-") as temp:
        sections = dump_sections(args.objcopy_path, args.input_path, Path(temp), [".debug_abbrev", ".debug_info"])
        abbrev = read_bytes(sections[".debug_abbrev"])
        pattern = bytes([0x36, 0x0B, 0x03, 0x25, 0x0B, 0x0B, 0x3A, 0x0B, 0x3B, 0x0B, 0x00, 0x00])
        match = find_pattern(abbrev, pattern)
        if match < 0:
            raise PatchError("未找到 type unit 中 structure_type 的 DW_FORM_strx1 模式")
        patched_abbrev = bytearray(abbrev[:match]) + bytearray([0x36, 0x0B, 0x03, form_byte, 0x0B, 0x0B, 0x3A, 0x0B, 0x3B, 0x0B, 0x00, 0x00]) + abbrev[match + len(pattern) :]
        original_info = read_bytes(sections[".debug_info"])
        if len(original_info) < 0x38:
            raise PatchError(".debug_info 长度不足，无法修改 type unit")
        payload_offset = 0x25
        patched_info = splice_bytes(original_info, payload_offset, 1, new_payload)
        delta = len(new_payload) - 1
        write_u32(patched_info, 0, read_u32(original_info, 0) + delta)
        if delta > 0:
            write_u32(patched_info, 0x2B + delta, 0x33 + delta)
        if 0x38 + delta + 12 > len(patched_info):
            raise PatchError("patch 后 compile unit header 越界")
        write_bytes(sections[".debug_abbrev"], patched_abbrev)
        write_bytes(sections[".debug_info"], patched_info)
        update_sections(args.objcopy_path, args.input_path, args.output_path, {".debug_abbrev": sections[".debug_abbrev"], ".debug_info": sections[".debug_info"]}, "回写 .debug_abbrev/.debug_info 失败")


def patch_dwarf5_addrx_form(args: argparse.Namespace) -> None:
    modes = {
        "addrx1": (0x29, bytes([0x01])),
        "addrx2": (0x2A, bytes([0x01, 0x00])),
        "addrx3": (0x2B, bytes([0x01, 0x00, 0x00])),
        "addrx4": (0x2C, bytes([0x01, 0x00, 0x00, 0x00])),
    }
    if args.mode not in modes:
        raise PatchError("Mode 必须是 addrx1/addrx2/addrx3/addrx4")
    form_byte, new_payload = modes[args.mode]
    with tempfile.TemporaryDirectory(prefix="elf-static-view-dwarf5-addrx-form-") as temp:
        sections = dump_sections(args.objcopy_path, args.input_path, Path(temp), [".debug_abbrev", ".debug_info"])
        abbrev = read_bytes(sections[".debug_abbrev"])
        pattern = bytes([0x11, 0x1B, 0x12, 0x06, 0x73, 0x17, 0x00, 0x00])
        match = find_pattern(abbrev, pattern)
        if match < 0:
            raise PatchError("未找到 compile_unit 中 DW_AT_low_pc 的 DW_FORM_addrx 模式")
        patched_abbrev = bytearray(abbrev[:match]) + bytearray([0x11, form_byte, 0x12, 0x06, 0x73, 0x17, 0x00, 0x00]) + abbrev[match + len(pattern) :]
        original_info = read_bytes(sections[".debug_info"])
        if len(original_info) < 0x65:
            raise PatchError(".debug_info 长度不足，无法修改 compile_unit 的 DW_AT_low_pc")
        payload_offset = 0x52
        patched_info = splice_bytes(original_info, payload_offset, 1, new_payload)
        delta = len(new_payload) - 1
        write_u32(patched_info, 0x38, read_u32(original_info, 0x38) + delta)
        if delta > 0:
            write_u32(patched_info, 0x5D + delta, 0x2F + delta)
            write_u32(patched_info, 0x7C + delta, 0x63 + delta)
            write_u32(patched_info, 0x87 + delta, 0x2F + delta)
            write_u32(patched_info, 0x97 + delta, 0x63 + delta)
        write_bytes(sections[".debug_abbrev"], patched_abbrev)
        write_bytes(sections[".debug_info"], patched_info)
        update_sections(args.objcopy_path, args.input_path, args.output_path, {".debug_abbrev": sections[".debug_abbrev"], ".debug_info": sections[".debug_info"]}, "回写 .debug_abbrev/.debug_info 失败")
def patch_abbrev_form(args: argparse.Namespace, pattern: bytes, offset: int, form: int, missing: str, fallback_copy: bool = False) -> None:
    with tempfile.TemporaryDirectory(prefix="elf-static-view-abbrev-form-") as temp:
        sections: dict[str, Path]
        try:
            sections = dump_sections(args.objcopy_path, args.input_path, Path(temp), [".debug_abbrev"])
        except PatchError:
            if fallback_copy:
                shutil.copyfile(args.input_path, args.output_path)
                return
            raise
        abbrev = read_bytes(sections[".debug_abbrev"])
        match = find_pattern(abbrev, pattern)
        if match < 0:
            raise PatchError(missing)
        abbrev[match + offset] = form
        write_bytes(sections[".debug_abbrev"], abbrev)
        update_sections(args.objcopy_path, args.input_path, args.output_path, {".debug_abbrev": sections[".debug_abbrev"]}, "回写 .debug_abbrev 失败")


def patch_gcc_ref4_to_ref_sup4(args: argparse.Namespace) -> None:
    patch_abbrev_form(args, bytes([0x05, 0x34, 0x00, 0x03, 0x08, 0x3A, 0x0B, 0x3B, 0x0B, 0x39, 0x0B, 0x49, 0x13, 0x02, 0x18, 0x00, 0x00]), 13, 0x2A, "未找到 gcc variable ref4 abbrev 模板", True)


def patch_gcc_ref4_to_ref_addr(args: argparse.Namespace) -> None:
    patch_abbrev_form(args, bytes([0x05, 0x34, 0x00, 0x03, 0x08, 0x3A, 0x0B, 0x3B, 0x0B, 0x39, 0x0B, 0x49, 0x13, 0x02, 0x18, 0x00, 0x00]), 13, 0x10, "未找到 gcc variable ref4 abbrev 模板")


def patch_gcc_ref8_to_ref_sup8(args: argparse.Namespace) -> None:
    patch_abbrev_form(args, bytes([0x05, 0x34, 0x00, 0x03, 0x08, 0x3A, 0x0B, 0x3B, 0x0B, 0x39, 0x0B, 0x49, 0x14, 0x02, 0x18, 0x00, 0x00]), 13, 0x2B, "未找到 gcc variable ref8 abbrev 模板")


def patch_gcc_ref4_to_small_ref(args: argparse.Namespace) -> None:
    modes = {
        "ref1": (0x11, bytes([0x74])),
        "ref2": (0x12, bytes([0x74, 0x00])),
        "ref_udata": (0x15, bytes([0x74])),
    }
    if args.mode not in modes:
        raise PatchError("Mode 必须是 ref1/ref2/ref_udata")
    form_byte, new_payload = modes[args.mode]
    with tempfile.TemporaryDirectory(prefix="elf-static-view-gcc-small-ref-") as temp:
        sections = dump_sections(args.objcopy_path, args.input_path, Path(temp), [".debug_abbrev", ".debug_info"])
        abbrev = read_bytes(sections[".debug_abbrev"])
        pattern = bytes([0x05, 0x34, 0x00, 0x03, 0x08, 0x3A, 0x0B, 0x3B, 0x0B, 0x39, 0x0B, 0x49, 0x13, 0x02, 0x18, 0x00, 0x00])
        match = find_pattern(abbrev, pattern)
        if match < 0:
            raise PatchError("未找到 gcc variable ref4 abbrev 模板")
        abbrev[match + 12] = form_byte
        debug_info = read_bytes(sections[".debug_info"])
        type_field_offset = 0x0000009E + 1 + 10 + 1 + 1 + 1
        patched_info = splice_bytes(debug_info, type_field_offset, 4, new_payload)
        write_u32(patched_info, 0, read_u32(patched_info, 0) - (4 - len(new_payload)))
        write_bytes(sections[".debug_abbrev"], abbrev)
        write_bytes(sections[".debug_info"], patched_info)
        update_sections(args.objcopy_path, args.input_path, args.output_path, {".debug_abbrev": sections[".debug_abbrev"], ".debug_info": sections[".debug_info"]}, "回写 .debug_abbrev/.debug_info 失败")


def patch_gcc_strp_to_strp_sup(args: argparse.Namespace) -> None:
    if not args.debug_sup_bytes_path:
        raise PatchError("缺少 DebugSupBytesPath")
    with tempfile.TemporaryDirectory(prefix="elf-static-view-gcc-strp-sup-") as temp:
        sections = dump_sections(args.objcopy_path, args.input_path, Path(temp), [".debug_abbrev"])
        abbrev = read_bytes(sections[".debug_abbrev"])
        pattern = bytes([0x01, 0x11, 0x00, 0x10, 0x17, 0x11, 0x01, 0x12, 0x0F, 0x03, 0x0E, 0x1B, 0x0E, 0x25, 0x0E, 0x13, 0x05, 0x00, 0x00])
        match = find_pattern(abbrev, pattern)
        if match < 0:
            raise PatchError("未找到 gcc 第二个 compile_unit 的 strp abbrev 模板")
        abbrev[match + 10] = 0x1D
        abbrev[match + 12] = 0x1D
        abbrev[match + 14] = 0x1D
        write_bytes(sections[".debug_abbrev"], abbrev)
        run_tool([args.objcopy_path, "--update-section", f".debug_abbrev={sections['.debug_abbrev']}", "--add-section", f".debug_sup={args.debug_sup_bytes_path}", args.input_path, args.output_path], "回写 .debug_abbrev/.debug_sup 失败")


def patch_ref_sig8_debug_types(args: argparse.Namespace) -> None:
    with tempfile.TemporaryDirectory(prefix="elf-static-view-refsig8-") as temp:
        sections = dump_sections(args.objcopy_path, args.input_path, Path(temp), [".debug_abbrev", ".debug_info"])
        abbrev = read_bytes(sections[".debug_abbrev"])
        old_abbrev = bytes([0x06, 0x34, 0x00, 0x03, 0x0E, 0x49, 0x13, 0x3A, 0x0B, 0x3B, 0x0B, 0x02, 0x18, 0x6E, 0x0E, 0x00, 0x00])
        new_abbrev = bytes([0x06, 0x34, 0x00, 0x03, 0x0E, 0x49, 0x20, 0x3A, 0x0B, 0x3B, 0x0B, 0x02, 0x18, 0x00, 0x00])
        match = find_pattern(abbrev, old_abbrev)
        if match < 0:
            raise PatchError("未找到待替换的 variable abbrev")
        patched_abbrev = bytearray(abbrev[:match]) + bytearray(new_abbrev) + abbrev[match + len(old_abbrev) :]
        info = read_bytes(sections[".debug_info"])
        global_index = -1
        signature_index = -1
        for index in range(0, len(info) - 24):
            if info[index] != 0x06:
                continue
            exprloc_length = info[index + 11]
            next_die_index = index + 12 + exprloc_length + 4
            if next_die_index + 8 >= len(info):
                continue
            if info[next_die_index] == 0x07:
                global_index = index
                signature_index = next_die_index + 1
                break
        if global_index < 0 or signature_index < 0:
            raise PatchError("未找到 global_value DIE 模式")
        signature = bytes(info[signature_index : signature_index + 8])
        exprloc_length = info[global_index + 11]
        patched_die = bytearray(12 + exprloc_length + 4)
        patched_die[0] = 0x06
        patched_die[1:5] = info[global_index + 1 : global_index + 5]
        patched_die[5:13] = signature
        patched_die[13] = info[global_index + 9]
        patched_die[14] = info[global_index + 10]
        patched_die[15] = exprloc_length
        patched_die[16 : 16 + exprloc_length] = info[global_index + 12 : global_index + 12 + exprloc_length]
        info[global_index : global_index + len(patched_die)] = patched_die
        write_bytes(sections[".debug_abbrev"], patched_abbrev)
        write_bytes(sections[".debug_info"], info)
        update_sections(args.objcopy_path, args.input_path, args.output_path, {".debug_abbrev": sections[".debug_abbrev"], ".debug_info": sections[".debug_info"]}, "回写 patch 后 section 失败")


def patch_debug_types_name_indirect(args: argparse.Namespace) -> None:
    with tempfile.TemporaryDirectory(prefix="elf-static-view-debug-types-indirect-") as temp:
        sections = dump_sections(args.objcopy_path, args.input_path, Path(temp), [".debug_abbrev", ".debug_types"])
        abbrev = read_bytes(sections[".debug_abbrev"])
        pattern = bytes([0x02, 0x13, 0x01, 0x36, 0x0B, 0x03, 0x0E, 0x0B, 0x0B, 0x3A, 0x0B, 0x3B, 0x0B, 0x00, 0x00])
        match = find_pattern(abbrev, pattern)
        if match < 0:
            raise PatchError("未找到 structure_type abbrev 模式")
        abbrev[match + 6] = 0x16
        types = read_bytes(sections[".debug_types"])
        unit_length = read_u32(types, 0)
        type_offset = read_u32(types, 19)
        die_offset = int(type_offset)
        if die_offset + 7 >= len(types):
            raise PatchError("type unit DIE 偏移越界")
        patched_types = bytearray(types[: die_offset + 2]) + bytearray([0x0E]) + types[die_offset + 2 :]
        write_u32(patched_types, 0, unit_length + 1)
        patched_count = 0
        for index in range(die_offset, len(patched_types) - 8):
            if patched_types[index] != 0x03:
                continue
            if read_u32(patched_types, index + 5) == 0x40:
                write_u32(patched_types, index + 5, 0x41)
                patched_count += 1
        if patched_count < 2:
            raise PatchError("未找到足够的 member ref4 目标用于修正偏移")
        write_bytes(sections[".debug_abbrev"], abbrev)
        write_bytes(sections[".debug_types"], patched_types)
        update_sections(args.objcopy_path, args.input_path, args.output_path, {".debug_abbrev": sections[".debug_abbrev"], ".debug_types": sections[".debug_types"]}, "回写 patch 后 section 失败")


def patch_gcc_gnu_altlink(args: argparse.Namespace) -> None:
    alt_file_name = args.alt_file_name or "gnu_alt_side.elf"
    with tempfile.TemporaryDirectory(prefix="elf-static-view-gcc-altlink-") as temp:
        temp_dir = Path(temp)
        sections = dump_sections(args.objcopy_path, args.input_path, temp_dir, [".debug_abbrev", ".debug_info"])
        altlink_path = temp_dir / "gnu_debugaltlink.bin"
        abbrev = read_bytes(sections[".debug_abbrev"])
        strp_pattern = bytes([0x01, 0x11, 0x00, 0x10, 0x17, 0x11, 0x01, 0x12, 0x0F, 0x03, 0x0E, 0x1B, 0x0E, 0x25, 0x0E, 0x13, 0x05, 0x00, 0x00])
        strp_replacement = bytes([0x01, 0x11, 0x00, 0x10, 0x17, 0x11, 0x01, 0x12, 0x0F, 0x03, 0xA1, 0x3E, 0x1B, 0xA1, 0x3E, 0x25, 0xA1, 0x3E, 0x13, 0x05, 0x00, 0x00])
        match = find_pattern(abbrev, strp_pattern)
        if match < 0:
            raise PatchError("未找到 gcc 第二个 compile_unit 的 strp abbrev 模板")
        abbrev = bytearray(abbrev[:match]) + bytearray(strp_replacement) + abbrev[match + len(strp_pattern) :]
        ref4_pattern = bytes([0x05, 0x34, 0x00, 0x03, 0x08, 0x3A, 0x0B, 0x3B, 0x0B, 0x39, 0x0B, 0x49, 0x13, 0x02, 0x18, 0x00, 0x00])
        ref4_replacement = bytes([0x05, 0x34, 0x00, 0x03, 0x08, 0x3A, 0x0B, 0x3B, 0x0B, 0x39, 0x0B, 0x49, 0xA0, 0x3E, 0x02, 0x18, 0x00, 0x00])
        match = find_pattern(abbrev, ref4_pattern)
        if match < 0:
            raise PatchError("未找到 gcc variable ref4 abbrev 模板")
        abbrev = bytearray(abbrev[:match]) + bytearray(ref4_replacement) + abbrev[match + len(ref4_pattern) :]
        debug_info = read_bytes(sections[".debug_info"])
        if len(debug_info) < 0x11B:
            raise PatchError(".debug_info 长度不足，无法修正后续 CU 的 abbrev_offset")
        write_u32(debug_info, 0x00E2, 0x6C)
        write_u32(debug_info, 0x010A, 0x83)
        content = bytearray(alt_file_name.encode("ascii"))
        content.append(0)
        while len(content) % 4 != 0:
            content.append(0)
        content.extend([0x22] * 20)
        write_bytes(sections[".debug_abbrev"], abbrev)
        write_bytes(sections[".debug_info"], debug_info)
        write_bytes(altlink_path, content)
        run_tool([args.objcopy_path, "--update-section", f".debug_abbrev={sections['.debug_abbrev']}", "--update-section", f".debug_info={sections['.debug_info']}", "--add-section", f".gnu_debugaltlink={altlink_path}", args.input_path, args.output_path], "回写 .debug_abbrev/.debug_info/.gnu_debugaltlink 失败")

COMMANDS: dict[str, Callable[[argparse.Namespace], None]] = {
    "patch_addrx_to_gnu_addr_index": patch_addrx_to_gnu_addr_index,
    "build_debug_sup_sidecar": build_debug_sup_sidecar,
    "patch_ranges_to_rnglistx": patch_ranges_to_rnglistx,
    "patch_rnglists_start_end": patch_rnglists_start_end,
    "patch_rnglists_offset_pair": patch_rnglists_offset_pair,
    "patch_rnglists_base_addressx": patch_rnglists_base_addressx,
    "patch_rnglists_startx_endx": patch_rnglists_startx_endx,
    "patch_rnglists_startx_length": patch_rnglists_startx_length,
    "patch_loclists_base_default": patch_loclists_base_default,
    "patch_loclists_base_addressx": patch_loclists_base_addressx,
    "patch_loclists_start_end": patch_loclists_start_end,
    "patch_loclists_start_length": patch_loclists_start_length,
    "patch_loclists_startx_endx": patch_loclists_startx_endx,
    "patch_loclists_startx_length": patch_loclists_startx_length,
    "patch_type_unit_abbrev_offset": patch_type_unit_abbrev_offset,
    "patch_dwarf5_strx_form": patch_dwarf5_strx_form,
    "patch_dwarf5_addrx_form": patch_dwarf5_addrx_form,
    "patch_gcc_ref4_to_ref_sup4": patch_gcc_ref4_to_ref_sup4,
    "patch_gcc_ref4_to_ref_addr": patch_gcc_ref4_to_ref_addr,
    "patch_gcc_ref8_to_ref_sup8": patch_gcc_ref8_to_ref_sup8,
    "patch_gcc_ref4_to_small_ref": patch_gcc_ref4_to_small_ref,
    "patch_gcc_strp_to_strp_sup": patch_gcc_strp_to_strp_sup,
    "patch_ref_sig8_debug_types": patch_ref_sig8_debug_types,
    "patch_debug_types_name_indirect": patch_debug_types_name_indirect,
    "patch_gcc_gnu_altlink": patch_gcc_gnu_altlink,
}


def normalize_script_name(script_file: str) -> str:
    name = Path(script_file).stem
    if name in COMMANDS:
        return name
    raise PatchError(f"暂未迁移脚本: {script_file}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Patch ELF fixtures for tests.")
    parser.add_argument("script_file", help="原 .ps1 脚本路径，用于兼容 CMake 调用面")
    parser.add_argument("-InputPath", "--input-path", dest="input_path", required=True)
    parser.add_argument("-OutputPath", "--output-path", dest="output_path", required=True)
    parser.add_argument("-ObjcopyPath", "--objcopy-path", dest="objcopy_path")
    parser.add_argument("-Mode", "--mode", dest="mode")
    parser.add_argument("-AltFileName", "--alt-file-name", dest="alt_file_name")
    parser.add_argument("-DebugSupBytesPath", "--debug-sup-bytes-path", dest="debug_sup_bytes_path")
    args = parser.parse_args(argv)
    args.command = normalize_script_name(args.script_file)
    return args


def main(argv: list[str]) -> int:
    try:
        args = parse_args(argv)
        COMMANDS[args.command](args)
        return 0
    except PatchError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))





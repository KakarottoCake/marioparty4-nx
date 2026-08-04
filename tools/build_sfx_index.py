#!/usr/bin/env python3
"""Build a small runtime index for the original MusyX sound-effect bank.

The index contains offsets and playback metadata only.  Audio remains in the
user-supplied mpgcsnd.msm file.
"""

from __future__ import annotations

import argparse
import struct
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


GROUP_INFO_OFS = 0x320
GROUP_INFO_SIZE = 0xFE0
GROUP_DATA_OFS = 0xA820
SAMPLE_DATA_OFS = 0x20F800
SE_OFS = 0x17E0
SE_SIZE = 0x9040
SE_ENTRY_SIZE = 0x10
INDEX_ENTRY_COUNT = SE_SIZE // SE_ENTRY_SIZE
INDEX_HEADER = struct.Struct(">4sHHII")
INDEX_ENTRY = struct.Struct(">IIIIBBHI")


@dataclass
class Group:
    data_ofs: int
    data_size: int
    sample_ofs: int
    sample_size: int


@dataclass
class Sample:
    sample_id: int
    offset: int
    length: int
    loop: int
    loop_length: int
    info: int
    extra: int


@dataclass
class Fx:
    macro: int
    volume: int
    pan: int


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def clamp_u8(value: int) -> int:
    return max(0, min(127, value))


def parse_groups(data: bytes) -> dict[int, Group]:
    groups: dict[int, Group] = {}
    for offset in range(GROUP_INFO_OFS, GROUP_INFO_OFS + GROUP_INFO_SIZE, 0x20):
        group_id = u16(data, offset)
        group = Group(
            data_ofs=u32(data, offset + 4),
            data_size=u32(data, offset + 8),
            sample_ofs=u32(data, offset + 12),
            sample_size=u32(data, offset + 16),
        )
        if group_id != 0xFFFF and group.data_size:
            groups[group_id] = group
    return groups


def parse_sound_effects(data: bytes, groups: dict[int, Group]):
    effects: dict[int, list[tuple[int, int, int, int]]] = defaultdict(list)
    for index in range(INDEX_ENTRY_COUNT):
        offset = SE_OFS + index * SE_ENTRY_SIZE
        group_id = u16(data, offset)
        fx_id = u16(data, offset + 2)
        if group_id in groups and fx_id != 0xFFFF:
            effects[group_id].append((index, fx_id, data[offset + 4], data[offset + 5]))
    return effects


def find_fx_table(data: bytes, base: int, size: int, fx_ids: set[int]) -> tuple[int, int]:
    candidates = []
    for relative in range(0, size - 4, 2):
        count = u16(data, base + relative)
        if u16(data, base + relative + 2) != 0:
            continue
        if count == 0 or count > 1024 or relative + 4 + count * 10 > size:
            continue
        ids = [u16(data, base + relative + 4 + i * 10) for i in range(count)]
        if len(set(ids) & fx_ids) != len(fx_ids):
            continue
        run = 0
        for fx_id in ids:
            if fx_id not in fx_ids:
                break
            run += 1
        # Prefer the compact exact table.  Some groups contain a few unused
        # FX entries in addition to the IDs referenced by the SE table.
        candidates.append((count - len(fx_ids), -run, count, relative))
    if not candidates:
        raise ValueError(f"FX table not found at group data 0x{base:x}")
    _, _, count, relative = min(candidates)
    return relative, count


def find_sample_directory(data: bytes, base: int, size: int, sample_size: int):
    best: list[Sample] | None = None
    for relative in range(0, size - 0x20, 2):
        if u32(data, base + relative + 0x0C) >> 16 != 0x3C00:
            continue
        position = relative
        samples: list[Sample] = []
        while position + 0x20 <= size and len(samples) < 4096:
            sample_id = u16(data, base + position)
            if sample_id == 0xFFFF:
                break
            sample = Sample(
                sample_id=sample_id,
                offset=u32(data, base + position + 4),
                length=u32(data, base + position + 0x10) & 0xFFFFFF,
                loop=u32(data, base + position + 0x14),
                loop_length=u32(data, base + position + 0x18),
                info=u32(data, base + position + 0x0C),
                extra=u32(data, base + position + 0x1C),
            )
            if (
                u32(data, base + position + 8) != 0
                or sample.offset >= sample_size
                or sample.length == 0
                or sample.extra >= size
            ):
                break
            samples.append(sample)
            position += 0x20
        if (
            len(samples) >= 1
            and position + 2 <= size
            and u16(data, base + position) == 0xFFFF
        ):
            if best is None or len(samples) > len(best):
                best = samples
    if best is None:
        raise ValueError(f"sample directory not found at group data 0x{base:x}")
    return best


def find_macro_samples(
    data: bytes, base: int, size: int, macro_ids: set[int], sample_ids: set[int]
):
    found: dict[int, list[int]] = {}
    for relative in range(0, size - 8, 4):
        first = u32(data, base + relative)
        macro_id = u32(data, base + relative + 4) >> 16
        if macro_id not in macro_ids or not 0x40 <= (first & 0x7F) <= 0x5F:
            continue
        samples: list[int] = []
        for position in range(relative, min(size - 8, relative + 0x180), 8):
            command = u32(data, base + position)
            opcode = command & 0x7F
            if opcode == 0:
                break
            if opcode == 0x10:
                sample_id = (command >> 8) & 0xFFFF
                if sample_id in sample_ids:
                    samples.append(sample_id)
        if samples and macro_id not in found:
            found[macro_id] = samples
    return found


def build_index(source: Path, destination: Path) -> tuple[int, int]:
    data = source.read_bytes()
    groups = parse_groups(data)
    effects = parse_sound_effects(data, groups)
    entries = [INDEX_ENTRY.pack(0, 0, 0, 0, 0, 64, 0, 0) for _ in range(INDEX_ENTRY_COUNT)]
    valid = 0
    unresolved = 0

    for group_id, sound_effects in effects.items():
        group = groups[group_id]
        data_base = GROUP_DATA_OFS + group.data_ofs
        fx_ids = {fx_id for _, fx_id, _, _ in sound_effects}
        fx_relative, fx_count = find_fx_table(data, data_base, group.data_size, fx_ids)
        fx_table: dict[int, Fx] = {}
        for i in range(fx_count):
            offset = data_base + fx_relative + 4 + i * 10
            fx_table[u16(data, offset)] = Fx(
                macro=u16(data, offset + 2),
                volume=data[offset + 6],
                pan=data[offset + 7],
            )

        directory = find_sample_directory(
            data, data_base, group.data_size, group.sample_size
        )
        samples = {sample.sample_id: sample for sample in directory}
        macros = find_macro_samples(
            data,
            data_base,
            group.data_size,
            {fx_table[fx_id].macro for fx_id in fx_ids},
            set(samples),
        )

        for effect_id, fx_id, se_volume, se_pan in sound_effects:
            fx = fx_table[fx_id]
            sample_ids = macros.get(fx.macro, [])
            sample = next((samples[sid] for sid in sample_ids if sid in samples), None)
            if sample is None:
                unresolved += 1
                continue
            data_bytes = ((sample.length + 13) // 14) * 8
            if sample.offset + data_bytes > group.sample_size:
                unresolved += 1
                continue
            coeff_offset = data_base + directory[0].extra
            # The extra field points from the first SDIR record to a 0x28-byte
            # Nintendo DSP coefficient block.
            coeff_offset = data_base + next(
                item.extra for item in directory if item.sample_id == sample.sample_id
            )
            mix_volume = clamp_u8((se_volume * fx.volume + 63) // 127)
            mix_pan = clamp_u8(se_pan + fx.pan - 64)
            entries[effect_id] = INDEX_ENTRY.pack(
                SAMPLE_DATA_OFS + group.sample_ofs + sample.offset,
                data_bytes,
                sample.length,
                coeff_offset,
                mix_volume,
                mix_pan,
                1,
                0,
            )
            valid += 1

    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as output:
        output.write(INDEX_HEADER.pack(b"SFXI", 1, INDEX_ENTRY.size, INDEX_ENTRY_COUNT, valid))
        for entry in entries:
            output.write(entry)
    return valid, unresolved


def write_c_source(index_path: Path, destination: Path) -> None:
    data = index_path.read_bytes()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("w", encoding="ascii", newline="\n") as output:
        output.write('#include "sfx_index_data.h"\n\n')
        output.write("const unsigned char g_switchSfxIndexData[] = {\n")
        for offset in range(0, len(data), 16):
            chunk = data[offset : offset + 16]
            output.write("    " + ", ".join(f"0x{value:02x}" for value in chunk) + ",\n")
        output.write("};\n")
        output.write("const unsigned int g_switchSfxIndexDataSize = sizeof(g_switchSfxIndexData);\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="path to the original mpgcsnd.msm")
    parser.add_argument("destination", type=Path, help="path for the generated index")
    parser.add_argument(
        "--append-msm",
        type=Path,
        help="also write a copy of the MSM with the index appended",
    )
    parser.add_argument(
        "--c-source",
        type=Path,
        help="also write the metadata index as a C source array",
    )
    args = parser.parse_args()
    valid, unresolved = build_index(args.source, args.destination)
    if args.append_msm:
        args.append_msm.parent.mkdir(parents=True, exist_ok=True)
        args.append_msm.write_bytes(args.source.read_bytes() + args.destination.read_bytes())
        print(f"Wrote appended bank {args.append_msm}")
    if args.c_source:
        write_c_source(args.destination, args.c_source)
        print(f"Wrote embedded index source {args.c_source}")
    print(f"Wrote {args.destination} ({valid} mapped effects, {unresolved} fallback effects)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

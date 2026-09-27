#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Encode synthetic PE images for tests.

No Windows binary is committed to this repository. This encoder writes
complete PE32 and PE32+ images with real section tables, import
descriptors and base-relocation blocks, from which the Wine runtime
manifest tests (tests/test_wine_runtime_manifest.py) build their inputs.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

DOS_MAGIC = 0x5A4D
NT_SIGNATURE = 0x00004550
OPT_MAGIC_PE32 = 0x010B
OPT_MAGIC_PE32PLUS = 0x020B
OPT_FIXED_PE32 = 0x60
OPT_FIXED_PE32PLUS = 0x70
MACHINE_I386 = 0x014C
MACHINE_AMD64 = 0x8664
NT_OFFSET = 0x40
FILE_HEADER_BYTES = 20
SECTION_HEADER_BYTES = 40
DIRECTORY_ENTRIES = 16
DIR_EXPORT = 0
DIR_IMPORT = 1
DIR_BASERELOC = 5
DIR_TLS = 9

SCN_CNT_CODE = 0x00000020
SCN_CNT_INITIALIZED_DATA = 0x00000040
SCN_CNT_UNINITIALIZED_DATA = 0x00000080
SCN_MEM_DISCARDABLE = 0x02000000
SCN_MEM_EXECUTE = 0x20000000
SCN_MEM_READ = 0x40000000
SCN_MEM_WRITE = 0x80000000

FILE_EXECUTABLE_IMAGE = 0x0002
FILE_32BIT_MACHINE = 0x0100
FILE_DLL = 0x2000
DLLCHAR_DYNAMIC_BASE = 0x0040
DLLCHAR_NX_COMPAT = 0x0100

RELOC_HIGHLOW = 3
RELOC_DIR64 = 10


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


@dataclass
class Section:
    name: str
    characteristics: int
    data: bytes = b""
    virtual_size: int = 0


@dataclass
class Import:
    dll: str
    names: tuple[str, ...] = ()
    ordinals: tuple[int, ...] = ()


@dataclass
class Export:
    """One export: real code at `rva`, or a forwarder string.

    A forwarder is encoded the way the PE format defines it - the function RVA
    points *inside* the export directory, at a "OTHERDLL.Function" string - so
    an importer that resolves it must follow the name rather than call it.
    """
    name: str
    rva: int | None = None
    forwarder: str | None = None


@dataclass
class Tls:
    """A PE32 TLS directory.

    `callbacks` are RVAs of functions the loader must call, terminated by a
    zero entry in the image; `zero_fill` is the template's zero-fill length and
    `index_rva` points at the loader-owned TLS index slot.
    """
    callbacks: tuple[int, ...] = ()
    zero_fill: int = 0
    index_rva: int | None = None


@dataclass
class Spec:
    name: str
    pe32plus: bool = True
    machine: int | None = None
    dll: bool = False
    image_base: int | None = None
    section_alignment: int = 0x1000
    file_alignment: int = 0x200
    entry_point_offset: int = 0
    sections: list[Section] = field(default_factory=list)
    imports: list[Import] = field(default_factory=list)
    exports: list[Export] = field(default_factory=list)
    tls: Tls | None = None
    relocate_data_pointer: bool = True


class _Blob:
    """Growable little-endian byte buffer."""

    def __init__(self) -> None:
        self.data = bytearray()

    def __len__(self) -> int:
        return len(self.data)

    def append(self, chunk: bytes) -> int:
        offset = len(self.data)
        self.data += chunk
        return offset

    def u16(self, value: int) -> int:
        return self.append(struct.pack("<H", value))

    def u32(self, value: int) -> int:
        return self.append(struct.pack("<I", value))

    def pad_to(self, alignment: int) -> None:
        self.data += b"\0" * (align_up(len(self.data), alignment) - len(self.data))


def _build_import_blob(spec: Spec, base_rva: int) -> bytes:
    """Descriptors, thunk tables, hint/name entries and DLL name strings."""
    width = 8 if spec.pe32plus else 4
    ordinal_flag = 1 << 63 if spec.pe32plus else 1 << 31
    pack = "<Q" if spec.pe32plus else "<I"

    descriptors_bytes = (len(spec.imports) + 1) * 20
    tail = _Blob()
    lookup_offsets: list[int] = []
    address_offsets: list[int] = []
    name_offsets: list[list[int]] = []
    dll_offsets: list[int] = []

    def tail_rva(offset: int) -> int:
        return base_rva + descriptors_bytes + offset

    for entry in spec.imports:
        name_offsets.append([])
        for name in entry.names:
            tail.pad_to(2)
            name_offsets[-1].append(len(tail))
            tail.u16(1)
            tail.append(name.encode("ascii") + b"\0")
    for entry in spec.imports:
        dll_offsets.append(len(tail))
        tail.append(entry.dll.encode("ascii") + b"\0")

    tail.pad_to(width)
    for index, entry in enumerate(spec.imports):
        thunks = [tail_rva(offset) for offset in name_offsets[index]]
        thunks += [ordinal_flag | ordinal for ordinal in entry.ordinals]
        for table in ("lookup", "address"):
            offsets = lookup_offsets if table == "lookup" else address_offsets
            offsets.append(len(tail))
            for value in thunks:
                tail.append(struct.pack(pack, value))
            tail.append(struct.pack(pack, 0))

    descriptors = _Blob()
    for index, entry in enumerate(spec.imports):
        descriptors.u32(tail_rva(lookup_offsets[index]))
        descriptors.u32(0)                                  # TimeDateStamp
        descriptors.u32(0)                                  # ForwarderChain
        descriptors.u32(tail_rva(dll_offsets[index]))
        descriptors.u32(tail_rva(address_offsets[index]))
    descriptors.append(b"\0" * 20)                          # terminator
    assert len(descriptors) == descriptors_bytes
    return bytes(descriptors.data + tail.data)


def _build_export_blob(spec: Spec, base_rva: int) -> bytes:
    """Export directory, address/name/ordinal tables and the strings.

    A forwarder is what the format says it is: the address entry points at a
    "OTHERDLL.Function" string inside the directory rather than at code, so an
    importer that resolves it must follow the name.
    """
    count = len(spec.exports)
    header_bytes = 40
    address_offset = header_bytes
    names_offset = address_offset + count * 4
    ordinals_offset = names_offset + count * 4
    tail_offset = ordinals_offset + count * 2
    tail = _Blob()
    forward_offsets: list[int] = []
    name_offsets: dict[str, int] = {}

    for entry in spec.exports:
        if entry.forwarder is not None:
            tail.pad_to(2)
            forward_offsets.append(len(tail))
            tail.append(entry.forwarder.encode("ascii") + b"\0")
        elif entry.rva is not None:
            forward_offsets.append(-1)
        else:
            raise ValueError(f"export {entry.name} has neither rva nor forwarder")
        tail.pad_to(2)
        name_offsets[entry.name] = len(tail)
        tail.append(entry.name.encode("ascii") + b"\0")
    tail.pad_to(2)
    dll_name_offset = len(tail)
    tail.append(spec.name.encode("ascii") + b"\0")

    image = bytearray(header_bytes + tail_offset + len(tail))
    ordered = sorted(range(count), key=lambda i: spec.exports[i].name)
    for index, entry in enumerate(spec.exports):
        value = (base_rva + tail_offset + forward_offsets[index]
                 if entry.forwarder is not None else int(entry.rva or 0))
        struct.pack_into("<I", image, address_offset + index * 4, value)
    for position, index in enumerate(ordered):
        entry = spec.exports[index]
        struct.pack_into("<I", image, names_offset + position * 4,
                         base_rva + tail_offset + name_offsets[entry.name])
        struct.pack_into("<H", image, ordinals_offset + position * 2, index)
    image[tail_offset:tail_offset + len(tail)] = bytes(tail.data)
    struct.pack_into("<I", image, 12, base_rva + tail_offset + dll_name_offset)
    struct.pack_into("<I", image, 16, 1)                    # ordinal base
    struct.pack_into("<I", image, 20, count)                # functions
    struct.pack_into("<I", image, 24, count)                # names
    struct.pack_into("<I", image, 28, base_rva + address_offset)
    struct.pack_into("<I", image, 32, base_rva + names_offset)
    struct.pack_into("<I", image, 36, base_rva + ordinals_offset)
    return bytes(image)


def _build_tls_blob(spec: Spec, base_rva: int) -> bytes:
    """The PE32 TLS directory plus its callback array, zero-terminated.

    The directory's fields and the entries of the callback array are *virtual
    addresses* in the image, which is what a loader dereferences; the caller
    supplies RVAs and the encoder adds the image base, so a caller cannot
    accidentally encode an RVA where a VA is required (which is exactly the
    mistake that made the gate refuse this fixture at the TLS stage).
    """
    assert spec.tls is not None
    image_base = spec.image_base
    if image_base is None:
        image_base = 0x140000000 if spec.pe32plus else 0x400000
    blob = _Blob()
    blob.u32(0)                                        # StartAddressOfRawData
    blob.u32(0)                                        # EndAddressOfRawData
    blob.u32(image_base + (spec.tls.index_rva or 0))   # AddressOfIndex
    blob.u32(image_base + base_rva + 24)               # AddressOfCallBacks
    blob.u32(spec.tls.zero_fill)                       # SizeOfZeroFill
    blob.u32(0)                                        # Characteristics
    for callback in spec.tls.callbacks:
        blob.u32(image_base + callback)
    blob.u32(0)                                        # terminating entry
    return bytes(blob.data)


def _build_reloc_blob(targets: list[tuple[int, int]], alignment: int) -> bytes:
    blob = _Blob()
    index = 0
    targets = sorted(targets)
    while index < len(targets):
        page = targets[index][0] & ~(alignment - 1)
        entries = []
        while index < len(targets) and (targets[index][0] & ~(alignment - 1)) == page:
            rva, kind = targets[index]
            entries.append((kind << 12) | (rva - page))
            index += 1
        if len(entries) % 2:
            entries.append(0)                               # ABSOLUTE padding
        blob.u32(page)
        blob.u32(8 + 2 * len(entries))
        for entry in entries:
            blob.u16(entry)
    return bytes(blob.data)


def build_pe(spec: Spec) -> bytes:
    """Encode one complete image and return its bytes."""
    if not spec.sections:
        raise ValueError("a PE image needs at least one section")
    machine = spec.machine
    if machine is None:
        machine = MACHINE_AMD64 if spec.pe32plus else MACHINE_I386
    image_base = spec.image_base
    if image_base is None:
        image_base = 0x140000000 if spec.pe32plus else 0x400000
    if image_base % spec.section_alignment:
        raise ValueError("image base must be section aligned")

    optional_fixed = OPT_FIXED_PE32PLUS if spec.pe32plus else OPT_FIXED_PE32
    optional_bytes = optional_fixed + 8 * DIRECTORY_ENTRIES
    sections = list(spec.sections)
    total_sections = len(sections) + len(
        [part for part in (spec.imports, spec.exports, spec.tls,
                           spec.relocate_data_pointer) if part]
    )
    header_bytes = align_up(
        NT_OFFSET + 4 + FILE_HEADER_BYTES + optional_bytes +
        total_sections * SECTION_HEADER_BYTES,
        spec.file_alignment,
    )

    # First pass: place the caller's sections so the generated ones can
    # reference real addresses.
    placed: list[dict[str, object]] = []
    next_rva = align_up(header_bytes, spec.section_alignment)
    for section in sections:
        virtual_size = section.virtual_size or len(section.data)
        if virtual_size == 0:
            raise ValueError(f"section {section.name} is empty")
        placed.append({
            "name": section.name,
            "characteristics": section.characteristics,
            "rva": next_rva,
            "virtual_size": virtual_size,
            "data": section.data,
        })
        next_rva += align_up(virtual_size, spec.section_alignment)

    data_rva = next(
        (int(entry["rva"]) for entry in placed if entry["name"] == ".data"),
        int(placed[0]["rva"]),
    )
    entry_point = int(placed[0]["rva"]) + spec.entry_point_offset

    if spec.imports:
        blob = _build_import_blob(spec, next_rva)
        placed.append({
            "name": ".idata",
            "characteristics": SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ |
                               SCN_MEM_WRITE,
            "rva": next_rva,
            "virtual_size": len(blob),
            "data": blob,
        })
        import_rva = next_rva
        import_size = (len(spec.imports) + 1) * 20
        next_rva += align_up(len(blob), spec.section_alignment)
    else:
        import_rva = import_size = 0

    if spec.exports:
        blob = _build_export_blob(spec, next_rva)
        placed.append({
            "name": ".edata",
            "characteristics": SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ,
            "rva": next_rva,
            "virtual_size": len(blob),
            "data": blob,
        })
        export_rva = next_rva
        export_size = len(blob)
        next_rva += align_up(len(blob), spec.section_alignment)
    else:
        export_rva = export_size = 0

    if spec.tls is not None:
        blob = _build_tls_blob(spec, next_rva)
        placed.append({
            "name": ".tls",
            "characteristics": SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ |
                               SCN_MEM_WRITE,
            "rva": next_rva,
            "virtual_size": len(blob),
            "data": blob,
        })
        tls_rva = next_rva
        tls_size = len(blob)
        next_rva += align_up(len(blob), spec.section_alignment)
    else:
        tls_rva = tls_size = 0

    if spec.relocate_data_pointer:
        kind = RELOC_DIR64 if spec.pe32plus else RELOC_HIGHLOW
        targets = [(data_rva, kind)]
        if spec.tls is not None:
            # The TLS directory's own fields are virtual addresses, and the
            # loader dereferences two of them: it *writes* the module's slot
            # index through AddressOfIndex and reads the callback list through
            # AddressOfCallBacks. An image that carries them without a
            # relocation writes to the address the linker baked in even when
            # the loader placed the image somewhere else - measured: with the
            # application's a.dll mapped away from its preferred base, ntdll's
            # store to AddressOfIndex landed outside every mapping this run
            # owns and the run stopped on the guard's classified write fault.
            # The callback array's entries are virtual addresses too, and the
            # loader *calls* each one: measured, with the entries left
            # unrelocated a run placed away from the preferred base took the
            # preferred-base address - a.dll's own callback, still encoded as
            # 0x10101010 - as a function pointer and the run stopped as
            # non-code at an address no mapping covers. A real linker
            # relocates each entry, so this encoder does the same.
            targets.append((tls_rva + 8, kind))
            targets.append((tls_rva + 12, kind))
            for index in range(len(spec.tls.callbacks)):
                targets.append((tls_rva + 24 + 4 * index, kind))
        blob = _build_reloc_blob(targets, spec.section_alignment)
        placed.append({
            "name": ".reloc",
            "characteristics": SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ |
                               SCN_MEM_DISCARDABLE,
            "rva": next_rva,
            "virtual_size": len(blob),
            "data": blob,
        })
        reloc_rva = next_rva
        reloc_size = len(blob)
        next_rva += align_up(len(blob), spec.section_alignment)
    else:
        reloc_rva = reloc_size = 0

    size_of_image = next_rva
    if len(placed) != total_sections:
        raise AssertionError("section accounting mismatch")

    # Second pass: raw offsets and the final byte stream.
    next_raw = header_bytes
    for entry in placed:
        data = bytes(entry["data"])                         # type: ignore[arg-type]
        raw_size = align_up(len(data), spec.file_alignment)
        entry["raw_size"] = raw_size
        entry["raw_offset"] = next_raw if raw_size else 0
        next_raw += raw_size

    image = bytearray(next_raw)
    struct.pack_into("<H", image, 0, DOS_MAGIC)
    struct.pack_into("<I", image, 0x3C, NT_OFFSET)
    struct.pack_into("<I", image, NT_OFFSET, NT_SIGNATURE)

    file_header = NT_OFFSET + 4
    characteristics = FILE_EXECUTABLE_IMAGE
    if spec.dll:
        characteristics |= FILE_DLL
    if not spec.pe32plus:
        characteristics |= FILE_32BIT_MACHINE
    struct.pack_into("<H", image, file_header, machine)
    struct.pack_into("<H", image, file_header + 2, total_sections)
    struct.pack_into("<H", image, file_header + 16, optional_bytes)
    struct.pack_into("<H", image, file_header + 18, characteristics)

    optional = file_header + FILE_HEADER_BYTES
    struct.pack_into("<H", image, optional,
                     OPT_MAGIC_PE32PLUS if spec.pe32plus else OPT_MAGIC_PE32)
    struct.pack_into("<I", image, optional + 0x10, entry_point)
    struct.pack_into("<I", image, optional + 0x14, int(placed[0]["rva"]))
    if spec.pe32plus:
        struct.pack_into("<Q", image, optional + 0x18, image_base)
    else:
        struct.pack_into("<I", image, optional + 0x18, data_rva)
        struct.pack_into("<I", image, optional + 0x1C, image_base)
    struct.pack_into("<I", image, optional + 0x20, spec.section_alignment)
    struct.pack_into("<I", image, optional + 0x24, spec.file_alignment)
    struct.pack_into("<H", image, optional + 0x30, 4)       # subsystem version
    struct.pack_into("<I", image, optional + 0x38, size_of_image)
    struct.pack_into("<I", image, optional + 0x3C, header_bytes)
    struct.pack_into("<H", image, optional + 0x44, 3)       # console
    struct.pack_into("<H", image, optional + 0x46,
                     DLLCHAR_DYNAMIC_BASE | DLLCHAR_NX_COMPAT)
    struct.pack_into("<I", image, optional + optional_fixed - 4,
                     DIRECTORY_ENTRIES)

    directories = optional + optional_fixed
    if import_size:
        struct.pack_into("<I", image, directories + 8 * DIR_IMPORT, import_rva)
        struct.pack_into("<I", image, directories + 8 * DIR_IMPORT + 4,
                         import_size)
    if export_size:
        struct.pack_into("<I", image, directories + 8 * DIR_EXPORT, export_rva)
        struct.pack_into("<I", image, directories + 8 * DIR_EXPORT + 4,
                         export_size)
    if tls_size:
        struct.pack_into("<I", image, directories + 8 * DIR_TLS, tls_rva)
        struct.pack_into("<I", image, directories + 8 * DIR_TLS + 4, tls_size)
    if reloc_size:
        struct.pack_into("<I", image, directories + 8 * DIR_BASERELOC,
                         reloc_rva)
        struct.pack_into("<I", image, directories + 8 * DIR_BASERELOC + 4,
                         reloc_size)

    table = optional + optional_bytes
    for index, entry in enumerate(placed):
        offset = table + index * SECTION_HEADER_BYTES
        name = str(entry["name"]).encode("ascii")[:8]
        image[offset:offset + len(name)] = name
        struct.pack_into("<I", image, offset + 8, int(entry["virtual_size"]))
        struct.pack_into("<I", image, offset + 12, int(entry["rva"]))
        struct.pack_into("<I", image, offset + 16, int(entry["raw_size"]))
        struct.pack_into("<I", image, offset + 20, int(entry["raw_offset"]))
        struct.pack_into("<I", image, offset + 36,
                         int(entry["characteristics"]))
        data = bytes(entry["data"])                         # type: ignore[arg-type]
        start = int(entry["raw_offset"])
        image[start:start + len(data)] = data

    return bytes(image)

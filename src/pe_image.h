/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Minimal PE reader: DOS header, NT headers, data directories and the
 * section table, parsed out of a read-only byte span with every field
 * bounds-checked before use. No operating system, no Windows headers and
 * no writes to the input.
 *
 * The parser answers "can these tables be read and are they self
 * consistent". Whether the image can be mapped is pe_layout's question.
 */
#ifndef PROSPERO_WIN_PE_IMAGE_H
#define PROSPERO_WIN_PE_IMAGE_H

#include "../include/prospero_win.h"

enum {
    PE_DOS_MAGIC = 0x5a4du,             /* "MZ" */
    PE_NT_SIGNATURE = 0x00004550u,      /* "PE\0\0" */
    PE_OPT_MAGIC_PE32 = 0x010bu,
    PE_OPT_MAGIC_PE32PLUS = 0x020bu,
    PE_OPT_FIXED_PE32 = 0x60u,          /* bytes before the data directories */
    PE_OPT_FIXED_PE32PLUS = 0x70u,
    PE_MACHINE_I386 = 0x014cu,
    PE_MACHINE_AMD64 = 0x8664u,
    PE_DOS_HEADER_BYTES = 0x40u,
    PE_FILE_HEADER_BYTES = 20u,
    PE_SECTION_HEADER_BYTES = 40u,
    PE_SECTION_NAME_BYTES = 8,
    PE_MAX_SECTIONS = 96,               /* the Windows image loader's own cap */
    PE_DIRECTORY_ENTRIES = 16,
};

/* Data-directory indices this project refers to by name. */
enum {
    PE_DIR_EXPORT = 0,
    PE_DIR_IMPORT = 1,
    PE_DIR_RESOURCE = 2,
    PE_DIR_EXCEPTION = 3,
    PE_DIR_SECURITY = 4,
    PE_DIR_BASERELOC = 5,
    PE_DIR_DEBUG = 6,
    PE_DIR_TLS = 9,
    PE_DIR_LOAD_CONFIG = 10,
    PE_DIR_BOUND_IMPORT = 11,
    PE_DIR_IAT = 12,
    PE_DIR_DELAY_IMPORT = 13,
    PE_DIR_CLR = 14,
};

/* Section characteristics. */
#define PE_SCN_CNT_CODE 0x00000020u
#define PE_SCN_CNT_INITIALIZED_DATA 0x00000040u
#define PE_SCN_CNT_UNINITIALIZED_DATA 0x00000080u
#define PE_SCN_LNK_NRELOC_OVFL 0x01000000u
#define PE_SCN_MEM_DISCARDABLE 0x02000000u
#define PE_SCN_MEM_NOT_CACHED 0x04000000u
#define PE_SCN_MEM_NOT_PAGED 0x08000000u
#define PE_SCN_MEM_SHARED 0x10000000u
#define PE_SCN_MEM_EXECUTE 0x20000000u
#define PE_SCN_MEM_READ 0x40000000u
#define PE_SCN_MEM_WRITE 0x80000000u

/* File characteristics. */
#define PE_FILE_EXECUTABLE_IMAGE 0x0002u
#define PE_FILE_32BIT_MACHINE 0x0100u
#define PE_FILE_DLL 0x2000u

/* DLL characteristics. */
#define PE_DLLCHAR_DYNAMIC_BASE 0x0040u
#define PE_DLLCHAR_NX_COMPAT 0x0100u

typedef struct PeSection {
    char name[PE_SECTION_NAME_BYTES + 1];
    uint32_t virtual_address;
    uint32_t virtual_size;
    uint32_t raw_offset;
    uint32_t raw_size;
    uint32_t characteristics;
} PeSection;

typedef struct PeDataDirectory {
    uint32_t virtual_address;
    uint32_t size;
} PeDataDirectory;

typedef struct PeImage {
    const uint8_t *bytes;
    size_t size;
    uint32_t nt_offset;
    uint16_t machine;
    uint16_t characteristics;
    uint16_t optional_magic;
    uint16_t subsystem;
    uint16_t dll_characteristics;
    uint16_t section_count;
    uint32_t optional_header_bytes;
    uint32_t size_of_headers;
    uint32_t size_of_image;
    uint32_t section_alignment;
    uint32_t file_alignment;
    uint32_t entry_point;               /* RVA; 0 is legal for a resource DLL */
    uint64_t image_base;
    /* The version, stack and checksum fields ntdll reports back through
     * NtQueryInformationProcess(ProcessImageInformation): they describe the
     * image the process is running, and both optional-header shapes carry
     * them at the same offsets except for the stack sizes. */
    uint32_t checksum;
    uint32_t stack_reserve;
    uint32_t stack_commit;
    uint16_t os_version_major;
    uint16_t os_version_minor;
    uint16_t subsystem_version_major;
    uint16_t subsystem_version_minor;
    uint32_t directory_count;
    PeDataDirectory directories[PE_DIRECTORY_ENTRIES];
    PeSection sections[PE_MAX_SECTIONS];
} PeImage;

int pe_image_parse(PeImage *image, const void *bytes, size_t size);

/*
 * The same parse for a caller that holds only an image's headers and reads its
 * data on demand - a file-backed section, whose bytes come from a service when
 * they are mapped rather than from the span being parsed. Every check
 * pe_image_parse makes on the headers and the section table still applies; only
 * the requirement that each section's raw bytes be present in the span is
 * dropped, and the summary it produces is the same.
 */
int pe_image_parse_headers(PeImage *image, const void *bytes, size_t size);

int pe_image_is_dll(const PeImage *image);

/*
 * True when the machine can execute directly on the console's Zen 2 cores
 * in 64-bit long mode. An i386 image parses and maps but cannot run without
 * instruction translation; see docs/TECHNICAL_DETAILS.md.
 */
int pe_image_machine_is_native(const PeImage *image);

const char *pe_image_machine_name(const PeImage *image);

const PeDataDirectory *pe_image_directory(const PeImage *image,
                                          unsigned index);

/* Section owning rva, or NULL. Zero-size sections never own an address. */
const PeSection *pe_image_section_for_rva(const PeImage *image, uint32_t rva);

/*
 * Translate rva..rva+bytes to an offset in the unmapped file span. Fails
 * when the range crosses a section boundary or lands in a part of a section
 * that has no backing bytes on disk.
 */
int pe_image_file_offset(const PeImage *image, uint32_t rva, uint32_t bytes,
                         size_t *file_offset);

/* Bounded reader for a NUL-terminated ASCII name at an RVA. */
int pe_image_read_name(const PeImage *image, uint32_t rva, char *out,
                       size_t out_bytes);

#endif

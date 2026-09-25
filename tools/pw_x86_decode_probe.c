/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Offline decoder-only census helper. It never invokes emitted host code. */
#include "../src/pw_x86_block.h"

#include <stdio.h>
#include <string.h>

typedef struct PrefixProbe {
    const char *name;
    uint8_t byte;
} PrefixProbe;

typedef struct OpcodeMap {
    const char *name;
    uint8_t bytes[3];
    unsigned length;
} OpcodeMap;

static const PrefixProbe prefixes[] = {
    { "none", 0u }, { "66", 0x66u }, { "67", 0x67u },
    { "f0", 0xf0u }, { "f2", 0xf2u }, { "f3", 0xf3u },
    { "64", 0x64u }, { "65", 0x65u },
};

static const OpcodeMap maps[] = {
    { "primary", { 0u, 0u, 0u }, 0u },
    { "0f", { 0x0fu, 0u, 0u }, 1u },
    { "0f38", { 0x0fu, 0x38u, 0u }, 2u },
    { "0f3a", { 0x0fu, 0x3au, 0u }, 2u },
};

static int consumes_candidate(const uint8_t *bytes, size_t length,
                              size_t candidate_offset)
{
    uint8_t output[4096];
    PwX86Block block;

    memset(&block, 0, sizeof(block));
    if (pw_x86_translate(bytes, length, 0x1000u, output, sizeof(output),
                         &block) != PW_OK)
        return 0;
    return block.instructions != 0u &&
           block.instruction_ends[0] > candidate_offset;
}

static unsigned probe(const PrefixProbe *prefix, const OpcodeMap *map,
                      unsigned opcode, unsigned memory)
{
    uint8_t bytes[15];
    size_t cursor = 0u;
    unsigned mask = 0u;

    memset(bytes, 0x90, sizeof(bytes));
    if (prefix->byte)
        bytes[cursor++] = prefix->byte;
    for (unsigned index = 0u; index < map->length; ++index)
        bytes[cursor++] = map->bytes[index];
    bytes[cursor++] = (uint8_t)opcode;
    for (unsigned group = 0u; group < 8u; ++group) {
        /* register form: mod=3, rm=eax; memory form: mod=0, rm=ebx */
        bytes[cursor] = (uint8_t)((memory ? 0u : 0xc0u) |
                                  (group << 3u) | (memory ? 3u : 0u));
        bytes[14] = 0xc3u; /* RET terminates any trailing NOP padding. */
        if (consumes_candidate(bytes, sizeof(bytes), cursor))
            mask |= 1u << group;
    }
    return mask;
}

int main(void)
{
    unsigned emitted = 0u;

    puts("{\"schema\":1,\"probe\":\"representative-decode-only\",\"rows\":[");
    for (unsigned prefix_index = 0u;
         prefix_index < sizeof(prefixes) / sizeof(prefixes[0]);
         ++prefix_index) {
        for (unsigned map_index = 0u;
             map_index < sizeof(maps) / sizeof(maps[0]); ++map_index) {
            for (unsigned opcode = 0u; opcode < 256u; ++opcode) {
                const unsigned reg = probe(&prefixes[prefix_index],
                                           &maps[map_index], opcode, 0u);
                const unsigned mem = probe(&prefixes[prefix_index],
                                           &maps[map_index], opcode, 1u);

                if (!reg && !mem)
                    continue;
                printf("%s{\"prefix\":\"%s\",\"map\":\"%s\","
                       "\"opcode\":%u,\"reg_candidate_mask\":%u,"
                       "\"mem_candidate_mask\":%u}",
                       emitted++ ? "," : "", prefixes[prefix_index].name,
                       maps[map_index].name, opcode, reg, mem);
            }
        }
    }
    puts("]}");
    return 0;
}

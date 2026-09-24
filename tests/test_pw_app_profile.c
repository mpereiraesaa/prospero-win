/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_app_profile.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t valid_profile[] =
    "; Pinball profile\r\n"
    "[application]\r\n"
    "id = space-cadet-pinball\r\n"
    "name = Space Cadet Pinball\r\n"
    "executable = C:\\Games\\Pinball\\PINBALL.EXE\r\n"
    "working_directory = C:\\Games\\Pinball\r\n"
    "arguments = /windowed -test\r\n"
    "prefix = pinball\r\n"
    "runtime = wine-i386-pinned\r\n"
    "architecture = PE32\r\n"
    "graphics = GDI\r\n";

static int parse_text(const char *text, PwAppProfile *profile)
{
    return pw_app_profile_parse((const uint8_t *)text, strlen(text), profile);
}

static void parse_example(const char *path,const char *expected_id,
                          const char *expected_executable)
{
    uint8_t bytes[PW_APP_PROFILE_MAX_BYTES];
    FILE *file=fopen(path,"rb");
    assert(file);
    size_t length=fread(bytes,1,sizeof(bytes),file);
    assert(!ferror(file));
    assert(fgetc(file)==EOF);
    assert(!ferror(file));
    assert(fclose(file)==0);
    PwAppProfile profile;
    assert(pw_app_profile_parse(bytes,length,&profile)==PW_OK);
    assert(strcmp(profile.id,expected_id)==0);
    assert(strcmp(profile.executable,expected_executable)==0);
    assert(strcmp(profile.runtime,"prospero-win-direct")==0);
    assert(profile.architecture==PW_APP_ARCH_PE32);
    assert(profile.graphics==PW_APP_GRAPHICS_GDI);
}

int main(void)
{
    PwAppProfile profile;
    PwAppProfile before;

    parse_example("examples/profiles/pinball.profile","space-cadet-pinball",
                  "C:\\Games\\Pinball\\PINBALL.EXE");
    parse_example("examples/profiles/paint.profile","paint",
                  "C:\\Windows\\System32\\mspaint.exe");

    memset(&profile, 0xa5, sizeof(profile));
    assert(pw_app_profile_parse(valid_profile, sizeof(valid_profile) - 1u,
                                &profile) == PW_OK);
    assert(strcmp(profile.id, "space-cadet-pinball") == 0);
    assert(strcmp(profile.name, "Space Cadet Pinball") == 0);
    assert(strcmp(profile.executable, "C:\\Games\\Pinball\\PINBALL.EXE") == 0);
    assert(strcmp(profile.working_directory, "C:\\Games\\Pinball") == 0);
    assert(strcmp(profile.arguments, "/windowed -test") == 0);
    assert(strcmp(profile.prefix, "pinball") == 0);
    assert(strcmp(profile.runtime, "wine-i386-pinned") == 0);
    assert(profile.architecture == PW_APP_ARCH_PE32);
    assert(profile.graphics == PW_APP_GRAPHICS_GDI);

    assert(parse_text(
        "[application]\n"
        "id=paint\nname=Paint\n"
        "executable=C:\\Windows\\System32\\mspaint.exe\n"
        "working_directory=C:\\Windows\\System32\n"
        "prefix=desktop\nruntime=wine-i386-pinned\n"
        "architecture=pe64\ngraphics=dxvk\n", &profile) == PW_OK);
    assert(profile.architecture == PW_APP_ARCH_PE64);
    assert(profile.graphics == PW_APP_GRAPHICS_DXVK);
    assert(profile.arguments[0] == '\0');

    before = profile;
    assert(parse_text(
        "[application]\nid=paint\nid=second\nname=Paint\n"
        "executable=C:\\paint.exe\nworking_directory=C:\\"
        "\nprefix=paint\nruntime=wine\narchitecture=pe32\n"
        "graphics=auto\n", &profile) == PW_ERR_MALFORMED);
    assert(memcmp(&profile, &before, sizeof(profile)) == 0);

    assert(parse_text(
        "[application]\nid=../paint\nname=Paint\n"
        "executable=C:\\paint.exe\nworking_directory=C:\\"
        "\nprefix=paint\nruntime=wine\narchitecture=pe32\n"
        "graphics=auto\n", &profile) == PW_ERR_MALFORMED);
    assert(parse_text(
        "[application]\nid=paint\nname=Paint\n"
        "executable=C:\\Games\\..\\paint.exe\n"
        "working_directory=C:\\Games\nprefix=paint\nruntime=wine\n"
        "architecture=pe32\ngraphics=auto\n", &profile) == PW_ERR_MALFORMED);
    assert(parse_text(
        "[application]\nid=paint\nname=Paint\n"
        "executable=paint.exe\nworking_directory=C:\\Games\n"
        "prefix=paint\nruntime=wine\narchitecture=pe32\n"
        "graphics=auto\n", &profile) == PW_ERR_MALFORMED);
    assert(parse_text(
        "[application]\nid=paint\nname=Paint\n"
        "executable=C:\\paint.txt\nworking_directory=C:\\Games\n"
        "prefix=paint\nruntime=wine\narchitecture=pe32\n"
        "graphics=auto\n", &profile) == PW_ERR_MALFORMED);
    assert(parse_text(
        "[application]\nid=paint\nname=Paint\n"
        "executable=C:\\paint.exe\nworking_directory=C:\\Games\n"
        "prefix=paint\nruntime=wine\narchitecture=arm64\n"
        "graphics=auto\n", &profile) == PW_ERR_UNSUPPORTED);
    assert(parse_text(
        "[application]\nid=paint\nname=Paint\n"
        "executable=C:\\paint.exe\nworking_directory=C:\\Games\n"
        "prefix=paint\nruntime=wine\narchitecture=pe32\n"
        "graphics=metal\n", &profile) == PW_ERR_UNSUPPORTED);
    assert(parse_text(
        "[application]\nid=paint\nname=Paint\n"
        "executable=C:\\paint.exe\nworking_directory=C:\\Games\n"
        "prefix=paint\nruntime=wine\narchitecture=pe32\n"
        "graphics=auto\n[extra]\n", &profile) == PW_ERR_MALFORMED);
    assert(pw_app_profile_parse(NULL, 0u, &profile) == PW_ERR_PRECONDITION);
    assert(pw_app_profile_parse(valid_profile, sizeof(valid_profile) - 1u,
                                NULL) == PW_ERR_PRECONDITION);
    {
        uint8_t oversized[PW_APP_PROFILE_MAX_BYTES + 1u] = { 0 };
        assert(pw_app_profile_parse(oversized, sizeof(oversized), &profile) ==
               PW_ERR_LIMIT);
    }
    return 0;
}

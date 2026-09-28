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

int main(void)
{
    PwAppProfile profile;
    PwAppProfile before;

    memset(&profile, 0xa5, sizeof(profile));
    assert(pw_app_profile_parse(valid_profile, sizeof(valid_profile) - 1u,
                                &profile) == PW_OK);
    assert(strcmp(profile.id, "space-cadet-pinball") == 0);
    assert(strcmp(profile.name, "Space Cadet Pinball") == 0);
    assert(strcmp(profile.executable, "C:\\Games\\Pinball\\PINBALL.EXE") == 0);
    assert(strcmp(profile.working_directory, "C:\\Games\\Pinball") == 0);
    assert(strcmp(profile.arguments, "/windowed -test") == 0);
    assert(profile.startup_command_id==0u);
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

    assert(parse_text(
        "[application]\nid=pinball\nname=Pinball\n"
        "executable=C:\\pinball.exe\nworking_directory=C:\\Games\n"
        "startup_command_id=65535\nprefix=pinball\nruntime=wine\n"
        "architecture=pe32\ngraphics=gdi\n", &profile) == PW_OK);
    assert(profile.startup_command_id==65535u);
    assert(profile.dll_overrides[0] == '\0');

    /* dll_overrides: optional, WINEDLLOVERRIDES syntax, no spaces or paths. */
#define OVERRIDES(line) "[application]\nid=dx\nname=DX\nexecutable=C:\\dx.exe\n" \
    "working_directory=C:\\\nprefix=default\nruntime=wine\narchitecture=pe64\ngraphics=dxvk\n" line
    assert(parse_text(OVERRIDES("dll_overrides = d3d11,dxgi=n;d3d9=n,b\n"), &profile) == PW_OK);
    assert(strcmp(profile.dll_overrides, "d3d11,dxgi=n;d3d9=n,b") == 0);
    assert(parse_text(OVERRIDES("dll_overrides=*d3d8.dll=\n"), &profile) == PW_OK);
    assert(parse_text(OVERRIDES("dll_overrides=d3d11 dxgi=n\n"), &profile) == PW_ERR_MALFORMED);
    assert(parse_text(OVERRIDES("dll_overrides=C:\\x.dll=n\n"), &profile) == PW_ERR_MALFORMED);
    assert(parse_text(OVERRIDES("dll_overrides=\n"), &profile) == PW_ERR_MALFORMED);
    assert(parse_text(OVERRIDES("dll_overrides=a=n\ndll_overrides=b=n\n"), &profile) == PW_ERR_MALFORMED);
    {
        char line[400] = "dll_overrides=";
        size_t used = strlen(line);
        memset(line + used, 'a', PW_APP_DLL_OVERRIDES_CAPACITY);
        strcpy(line + used + PW_APP_DLL_OVERRIDES_CAPACITY, "\n");
        char text[800];
        snprintf(text, sizeof(text), OVERRIDES("%s"), line);
        assert(parse_text(text, &profile) == PW_ERR_LIMIT);
    }
#undef OVERRIDES
    assert(parse_text(
        "[application]\nid=pinball\nname=Pinball\n"
        "executable=C:\\pinball.exe\nworking_directory=C:\\Games\n"
        "startup_command_id=65536\nprefix=pinball\nruntime=wine\n"
        "architecture=pe32\ngraphics=gdi\n", &profile) == PW_ERR_LIMIT);
    assert(parse_text(
        "[application]\nid=pinball\nname=Pinball\n"
        "executable=C:\\pinball.exe\nworking_directory=C:\\Games\n"
        "startup_command_id=7x\nprefix=pinball\nruntime=wine\n"
        "architecture=pe32\ngraphics=gdi\n", &profile) == PW_ERR_MALFORMED);
    assert(parse_text(
        "[application]\nid=pinball\nname=Pinball\n"
        "executable=C:\\pinball.exe\nworking_directory=C:\\Games\n"
        "startup_command_id=101\nstartup_command_id=102\n"
        "prefix=pinball\nruntime=wine\narchitecture=pe32\ngraphics=gdi\n",
        &profile) == PW_ERR_MALFORMED);

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

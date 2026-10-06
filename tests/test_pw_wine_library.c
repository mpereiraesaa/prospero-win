/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_wine_library.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define APP(id, name) "[application]\nid = " id "\nname = " name "\n" \
    "executable = C:\\Games\\" name "\\GAME.EXE\nworking_directory = C:\\Games\\" name "\n" \
    "prefix = default\nruntime = wine-wow64\narchitecture = pe32\ngraphics = gdi\n"

static char root[64];

static void write_file(const char *relative, const char *text)
{
    char path[256];
    FILE *file;

    snprintf(path, sizeof(path), "%s/%s", root, relative);
    assert((file = fopen(path, "w")) && fputs(text, file) >= 0 && !fclose(file));
}

static void test_load(void)
{
    char directory[128];
    PwWineLibrary library;
    PwGameInput input;

    snprintf(root, sizeof(root), "/tmp/pw-library-XXXXXX");
    assert(mkdtemp(root));
    /* No profiles directory, no index: nothing to offer. */
    assert(pw_wine_library_load(&library, root) == PW_ERR_NOT_FOUND && !library.count);
    assert(library.scan_error == ENOENT && !library.listed_by);

    snprintf(directory, sizeof(directory), "%s/profiles", root);
    assert(!mkdir(directory, 0755));
    snprintf(directory, sizeof(directory), "%s/input", root);
    assert(!mkdir(directory, 0755));
    write_file("profiles/pinball.profile", APP("pinball", "Pinball")
               "[display]\ndesktop = 800x600\nscaling = fit\n[input]\npreset = pinball\ncross = enter\n"
               "player2 = pinball-p2\n");
    write_file("profiles/solitaire.profile", APP("solitaire", "Solitaire")
               "[input]\nmouse = right_stick\nr2 = mouse_left\npreset = missing\n");
    write_file("profiles/broken.profile", "[application]\nid = broken\n");
    write_file("profiles/Upper.profile", APP("upper", "Upper"));
    write_file("profiles/notes.txt", "not a profile");
    write_file("input/pinball.input", "[input]\nl1 = z\nr1 = slash\ncross = space\n");
    write_file("input/pinball-p2.input", "[input]\nmode = xinput\ncross = s\n");

    assert(pw_wine_library_load(&library, root) == PW_OK);
    assert(library.listed_by == PW_WINE_LIBRARY_SCANNED && library.count == 3 && !library.scan_error);
    assert(!strcmp(library.entries[0].file, "broken.profile") && library.entries[0].status != PW_OK);
    assert(!strcmp(library.entries[1].file, "pinball.profile") && library.entries[1].status == PW_OK);
    assert(!strcmp(library.entries[2].file, "solitaire.profile") && library.entries[2].status == PW_OK);

    const PwGameProfile *pinball = pw_wine_library_find(&library, "pinball");
    assert(pinball && pinball->display.width == 800 && !strcmp(pinball->input.preset, "pinball"));
    assert(!pw_wine_library_find(&library, "broken") && !pw_wine_library_find(&library, "none"));
    assert(!pw_wine_library_find(NULL, "pinball") && !pw_wine_library_find(&library, NULL));

    /* The preset under the profile's own lines. */
    assert(pw_wine_library_input(pinball, root, &input) == PW_OK);
    assert(input.bindings[4].code == 'Z' && input.bindings[5].code == 0xbf);   /* l1, r1 */
    assert(input.bindings[0].code == 0x0d);                                     /* cross overridden */
    /* A missing preset leaves the profile's own lines. */
    const PwGameProfile *solitaire = pw_wine_library_find(&library, "solitaire");
    assert(pw_wine_library_input(solitaire, root, &input) == PW_ERR_NOT_FOUND);
    assert(input.mouse == PW_GAME_STICK_RIGHT && input.bindings[7].kind == PW_GAME_BIND_MOUSE);
    write_file("input/missing.input", "[input]\nl1 = teleport\n");
    assert(pw_wine_library_input(solitaire, root, &input) == PW_ERR_UNSUPPORTED);
    assert(input.mouse == PW_GAME_STICK_RIGHT);
    assert(pw_wine_library_input(NULL, root, &input) == PW_ERR_PRECONDITION);
    /* The second pad's preset, keys only even when it asks for XInput. */
    assert(pw_wine_library_player2_input(pinball, root, &input) == PW_OK);
    assert(input.bindings[0].code == 'S' && input.mode == PW_GAME_INPUT_KEYBOARD);
    assert(input.bindings[4].kind == PW_GAME_BIND_UNSET);                       /* not player 1's */
    assert(pw_wine_library_player2_input(solitaire, root, &input) == PW_ERR_NOT_FOUND);
    assert(pw_wine_library_player2_input(NULL, root, &input) == PW_ERR_PRECONDITION);
    assert(pw_wine_library_load(NULL, root) == PW_ERR_PRECONDITION);

    /* A file larger than any valid profile is refused as too large. */
    {
        static char big[PW_APP_PROFILE_MAX_BYTES + 64];
        memset(big, '#', sizeof(big) - 1);
        write_file("profiles/zbig.profile", big);
        write_file("input/big.input", big);
        write_file("profiles/zbigpreset.profile", APP("bigpreset", "Big") "[input]\npreset = big\n");
        assert(pw_wine_library_load(&library, root) == PW_OK && library.count == 5);
        assert(library.entries[3].status == PW_ERR_LIMIT);
        assert(pw_wine_library_input(&library.entries[4].profile, root, &input) == PW_ERR_LIMIT);
    }

    /* An unlistable directory falls back to its index. */
    write_file("profiles/profiles.lst", "# index\npinball.profile\nmissing.profile\n");
    snprintf(directory, sizeof(directory), "%s/profiles", root);
    assert(!chmod(directory, 0311));
    if (geteuid() != 0) {   /* root may list anything */
        assert(pw_wine_library_load(&library, root) == PW_OK);
        assert(library.listed_by == PW_WINE_LIBRARY_INDEXED && library.count == 2);
        assert(!strcmp(library.entries[0].file, "missing.profile") &&
               library.entries[0].status == PW_ERR_NOT_FOUND);
        assert(library.entries[1].status == PW_OK);
        write_file("profiles/profiles.lst", "../escape.profile\n");
        assert(pw_wine_library_load(&library, root) == PW_ERR_NOT_FOUND);
    }
    assert(!chmod(directory, 0755));
    {
        char command[128];
        snprintf(command, sizeof(command), "rm -rf '%s'", root);
        assert(!system(command));
    }
    char long_root[300];
    memset(long_root, 'a', sizeof(long_root) - 1);
    long_root[sizeof(long_root) - 1] = 0;
    assert(pw_wine_library_load(&library, long_root) == PW_ERR_LIMIT);
}

static char seen[8][32];
static int seen_count;

static void collect(const char *name, size_t length, void *context)
{
    (void)context;
    memcpy(seen[seen_count], name, length);
    seen[seen_count++][length] = 0;
}

/* One FreeBSD 11 directory record. */
static size_t record(uint8_t *out, uint8_t type, const char *name, uint16_t reclen)
{
    size_t length = strlen(name);
    memset(out, 0, reclen);
    out[0] = 1;
    memcpy(out + 4, &reclen, 2);
    out[6] = type;
    out[7] = (uint8_t)length;
    memcpy(out + 8, name, length);
    return reclen;
}

static void test_dirents(void)
{
    uint8_t buffer[256];
    size_t used = 0;

    used += record(buffer + used, 4, ".", 12);                 /* a directory: skipped */
    used += record(buffer + used, 8, "pinball.profile", 24);
    used += record(buffer + used, 0, "unknown.profile", 24);   /* type not filled in */
    used += record(buffer + used, 10, "link.profile", 24);     /* a symbolic link: skipped */
    assert(pw_wine_library_dirents(buffer, used, collect, NULL) == 4 && seen_count == 2);
    assert(!strcmp(seen[0], "pinball.profile") && !strcmp(seen[1], "unknown.profile"));
    assert(pw_wine_library_dirents(buffer, 0, collect, NULL) == 0);
    assert(pw_wine_library_dirents(buffer, 5, collect, NULL) == -1);
    record(buffer, 8, "x", 4);
    assert(pw_wine_library_dirents(buffer, 16, collect, NULL) == -1);          /* reclen < 8 */
    record(buffer, 8, "x", 64);
    assert(pw_wine_library_dirents(buffer, 16, collect, NULL) == -1);          /* past the end */
    record(buffer, 8, "abcdefghijklmnop", 16);
    assert(pw_wine_library_dirents(buffer, 16, collect, NULL) == -1);          /* name past reclen */
    assert(pw_wine_library_dirents(NULL, 16, collect, NULL) == -1);
    assert(pw_wine_library_dirents(buffer, 16, NULL, NULL) == -1);
}

int main(void)
{
    test_load();
    test_dirents();
    printf("wine library passed: scanned and indexed profiles, refused entries, sorted order, "
           "presets under profile lines, size limits, FreeBSD directory records\n");
    return 0;
}

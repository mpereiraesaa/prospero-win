/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_game_profile.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define APP "[application]\nid = pinball\nname = Space Cadet Pinball\n" \
            "executable = C:\\Games\\Pinball\\PINBALL.EXE\nworking_directory = C:\\Games\\Pinball\n" \
            "prefix = default\nruntime = wine-wow64\narchitecture = pe32\ngraphics = gdi\n"

static int parse(const char *text, PwGameProfile *profile)
{
    return pw_game_profile_parse((const uint8_t *)text, strlen(text), profile);
}

static int preset(const char *text, PwGameInput *input)
{
    return pw_game_input_parse((const uint8_t *)text, strlen(text), input);
}

enum { CROSS, CIRCLE, SQUARE, TRIANGLE, L1, R1, L2, R2, L3, R3, UP, DOWN, LEFT, RIGHT, OPTIONS,
       CREATE, TOUCHPAD };

static void test_profile(void)
{
    PwGameProfile p;

    assert(parse(APP, &p) == PW_OK);
    assert(!strcmp(p.app.id, "pinball") && !strcmp(p.app.runtime, "wine-wow64"));
    assert(!p.display.width && p.display.scaling == PW_GAME_SCALING_FIT);
    assert(!p.input.set && p.input.bindings[CROSS].kind == PW_GAME_BIND_UNSET);

    assert(parse(APP "; comments and blank lines\n\n[display]\ndesktop = 1024x768\nscaling = Integer\n"
                 "[input]\npreset = pinball\nmode = keyboard\nmouse = right_stick\nmouse_speed = 900\n"
                 "cross = space\nL1 = z\nr1 = slash\nr2 = mouse_left\nl2 = mouse_right\n"
                 "r3 = mouse_middle\noptions = f3\nsquare = F12\ntriangle = vk:0x5B\n"
                 "circle = 7\ntouchpad = none\nup = up\n", &p) == PW_OK);
    assert(p.display.width == 1024 && p.display.height == 768);
    assert(p.display.scaling == PW_GAME_SCALING_INTEGER);
    assert(!strcmp(p.input.preset, "pinball") && p.input.mouse == PW_GAME_STICK_RIGHT);
    assert(p.input.mouse_speed == 900 && p.input.mode == PW_GAME_INPUT_KEYBOARD);
    assert(p.input.bindings[CROSS].kind == PW_GAME_BIND_KEY && p.input.bindings[CROSS].code == 0x20);
    assert(p.input.bindings[CROSS].mask == 0x4000u && p.input.bindings[L1].code == 'Z');
    assert(p.input.bindings[R1].code == 0xbf && p.input.bindings[OPTIONS].code == 0x72);
    assert(p.input.bindings[SQUARE].code == 0x7b && p.input.bindings[TRIANGLE].code == 0x5b);
    assert(p.input.bindings[CIRCLE].code == '7' && p.input.bindings[UP].code == 0x26);
    assert(p.input.bindings[R2].kind == PW_GAME_BIND_MOUSE && p.input.bindings[R2].code == 0);
    assert(p.input.bindings[L2].code == 1 && p.input.bindings[R3].code == 2);
    assert(p.input.bindings[TOUCHPAD].kind == PW_GAME_BIND_NONE);
    assert(p.input.bindings[DOWN].kind == PW_GAME_BIND_UNSET);

    assert(parse(APP "[input]\nmode = xinput\n[display]\nscaling = stretch\n", &p) == PW_OK);
    assert(p.input.mode == PW_GAME_INPUT_XINPUT && p.display.scaling == PW_GAME_SCALING_STRETCH);
    assert(p.display.view == PW_GAME_VIEW_WINDOW && p.display.show_fps == 1);
    assert(parse(APP "[display]\nview = desktop\n", &p) == PW_OK && p.display.view == PW_GAME_VIEW_DESKTOP);
    /* show_fps: the backend's frame-rate counter, on unless turned off */
    assert(parse(APP "[display]\nshow_fps = true\n", &p) == PW_OK && p.display.show_fps == 1);
    assert(parse(APP "[display]\nshow_fps = True\nview = desktop\n", &p) == PW_OK && p.display.show_fps == 1);
    assert(parse(APP "[display]\nshow_fps = false\n", &p) == PW_OK && p.display.show_fps == 0);
    /* refresh: 60 unless the profile asks for 120 */
    assert(parse(APP "[display]\nshow_fps = false\n", &p) == PW_OK && p.display.refresh == 60);
    assert(parse(APP "[display]\nrefresh = 120\n", &p) == PW_OK && p.display.refresh == 120);
    assert(parse(APP "[display]\nrefresh = 60\nshow_fps = true\n", &p) == PW_OK && p.display.refresh == 60);
    assert(parse(APP "[display]\nview = Window\n", &p) == PW_OK && p.display.view == PW_GAME_VIEW_WINDOW);
    assert(parse(APP "[display]\ndesktop = 320x200\n[input]\nmouse = left_stick\n", &p) == PW_OK);
    assert(p.display.width == 320 && p.input.mouse == PW_GAME_STICK_LEFT);
    assert(parse(APP "[display]\ndesktop = 3840x2160\n[input]\nmouse = none\ncross = enter\n", &p) == PW_OK);
    assert(p.input.mouse == PW_GAME_STICK_NONE && p.input.bindings[CROSS].code == 0x0d);

    /* [debug] winedebug: a game's own Wine channels, anywhere after [application]. */
    assert(!p.winedebug[0]);
    assert(parse(APP "[debug]\nwinedebug = +seh,warn+module,-all\n[display]\nview = desktop\n", &p) == PW_OK);
    assert(!strcmp(p.winedebug, "+seh,warn+module,-all") && p.display.view == PW_GAME_VIEW_DESKTOP);
    assert(parse(APP "[debug]\nwinedebug = trace+d3d.9,err=all\n", &p) == PW_OK);
}

static void test_refusals(void)
{
    static const char *const bad[] = {
        "[display]\ndesktop = 800x600\n" APP,                  /* application not first */
        APP "[display]\n[display]\n",                          /* duplicate section */
        APP "[input]\n[input]\n",
        APP "[sound]\n",                                       /* unknown section */
        APP "[display]\nshow_fps = yes\n",                     /* only true or false */
        APP "[display]\nshow_fps =\n",
        APP "[display]\nshow_fps = true\nshow_fps = false\n",   /* twice */
        APP "[display]\nrefresh = 90\n",                      /* 60 or 120 only */
        APP "[display]\nrefresh =\n",
        APP "[display]\nrefresh = 120\nrefresh = 60\n",         /* twice */
        APP "[debug]\nwinedebug = +seh\n[debug]\n",            /* duplicate section */
        APP "[debug]\nwinedebug =\n",                         /* empty */
        APP "[debug]\nwinedebug = +seh;rm\n",                  /* not a channel list */
        APP "[debug]\nwinedebug = +seh all\n",
        APP "[debug]\nwinedebug = $HOME\n",
        APP "[debug]\nwinedebug = +seh\nwinedebug = +relay\n",  /* twice */
        APP "[debug]\nrelay = on\n",                           /* unknown key */
        APP "[debug]\nwinedebug = +aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n",   /* too long */
        APP "[display\n",
        APP "[]\n",
        APP "[display]\ncolour = 32\n",                        /* unknown key */
        APP "[display]\ndesktop = 800x600\ndesktop = 640x480\n",
        APP "[display]\ndesktop = 800\n",
        APP "[display]\ndesktop = 100x100\n",
        APP "[display]\ndesktop = 4000x2000\n",
        APP "[display]\ndesktop = 800x3000\n",
        APP "[display]\ndesktop = 8a0x600\n",
        APP "[display]\ndesktop = 800x\n",
        APP "[display]\nscaling = zoom\n",
        APP "[display]\nscaling = fit\nscaling = fit\n",
        APP "[display]\nview = screen\n",
        APP "[display]\nview = window\nview = window\n",
        APP "[input]\nmode = gamepad\n",
        APP "[input]\nmode = keyboard\nmode = keyboard\n",
        APP "[input]\nmouse = both_sticks\n",
        APP "[input]\nmouse = none\nmouse = none\n",
        APP "[input]\nmouse_speed = 0\n",
        APP "[input]\nmouse_speed = 20001\n",
        APP "[input]\nmouse_speed = fast\n",
        APP "[input]\nmouse_speed = 10\nmouse_speed = 10\n",
        APP "[input]\nmouse_speed = 1234567890\n",
        APP "[input]\ncross = space\ncross = enter\n",
        APP "[input]\ncross = hyperspace\n",
        APP "[input]\ncross = f0\n",
        APP "[input]\ncross = f25\n",
        APP "[input]\ncross = fx\n",
        APP "[input]\ncross = !\n",
        APP "[input]\ncross = vk:0x00\n",
        APP "[input]\ncross = vk:0xZZ\n",
        APP "[input]\nstart = space\n",
        APP "[input]\npreset = Pinball\n",
        APP "[input]\npreset = a/b\n",
        APP "[input]\npreset =\n",
        APP "[input]\npreset = a\npreset = b\n",
        APP "[input]\npreset = 0123456789012345678901234567890123456789012345678901234567890123456789\n",
        APP "[input]\ncross space\n",
        APP "[input]\n = space\n",
        "[application]\nid = x\n",                           /* pw_app_profile refuses */
        "id = pinball\n",
    };
    PwGameProfile p;

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (parse(bad[i], &p) == PW_OK) { fprintf(stderr, "accepted bad profile %zu\n", i); assert(0); }
    }
    assert(pw_game_profile_parse(NULL, 1, &p) == PW_ERR_PRECONDITION);
    assert(pw_game_profile_parse((const uint8_t *)APP, 0, &p) == PW_ERR_PRECONDITION);
    assert(pw_game_profile_parse((const uint8_t *)APP, strlen(APP), NULL) == PW_ERR_PRECONDITION);
    assert(pw_game_profile_parse((const uint8_t *)APP, PW_APP_PROFILE_MAX_BYTES + 1, &p) == PW_ERR_LIMIT);
}

static void test_presets(void)
{
    PwGameInput input, overrides;
    PwGameProfile p;

    pw_game_input_init(&input);
    assert(input.mouse_speed == PW_GAME_MOUSE_SPEED_DEFAULT && input.mode == PW_GAME_INPUT_KEYBOARD);
    assert(input.bindings[L1].mask == 0x400u && input.bindings[L1].kind == PW_GAME_BIND_UNSET);
    assert(preset("# shared pinball keys\n[input]\nl1 = z\nr1 = slash\ncross = space\n"
                  "mouse = right_stick\n", &input) == PW_OK);
    assert(input.bindings[L1].code == 'Z' && input.mouse == PW_GAME_STICK_RIGHT);
    assert(input.mouse_speed == PW_GAME_MOUSE_SPEED_DEFAULT);

    /* The profile's own lines override the preset, the rest stays. */
    assert(parse(APP "[input]\npreset = pinball\ncross = enter\nmouse_speed = 400\n", &p) == PW_OK);
    pw_game_input_overlay(&input, &p.input);
    assert(input.bindings[CROSS].code == 0x0d && input.bindings[L1].code == 'Z');
    assert(input.mouse_speed == 400 && input.mouse == PW_GAME_STICK_RIGHT);
    overrides = p.input;
    overrides.set = 0;
    pw_game_input_overlay(&input, &overrides);
    assert(input.mouse_speed == 400);

    /* A preset holds only [input] and cannot chain another preset. */
    assert(preset("[input]\npreset = other\n", &input) == PW_ERR_MALFORMED);
    assert(preset("[display]\ndesktop = 800x600\n", &input) == PW_ERR_MALFORMED);
    assert(preset(APP, &input) == PW_ERR_MALFORMED);
    assert(preset("; nothing\n", &input) == PW_ERR_MALFORMED);
    assert(preset("l1 = z\n", &input) == PW_ERR_MALFORMED);
    assert(preset("[input]\nl1 = warp\n", &input) == PW_ERR_UNSUPPORTED);
    assert(input.bindings[L1].code == 'Z');             /* refused files change nothing */
    assert(pw_game_input_parse(NULL, 1, &input) == PW_ERR_PRECONDITION);
    assert(pw_game_input_parse((const uint8_t *)"[input]\n", 8, NULL) == PW_ERR_PRECONDITION);
    assert(pw_game_input_parse((const uint8_t *)"[input]\n", PW_APP_PROFILE_MAX_BYTES + 1, &input) ==
           PW_ERR_LIMIT);
    pw_game_input_init(NULL);
    pw_game_input_overlay(NULL, &input);
    pw_game_input_overlay(&input, NULL);

    assert(pw_game_button_mask(CREATE) == 0x1u && pw_game_button_mask(TOUCHPAD) == 0x100000u);
    assert(pw_game_button_mask(PW_GAME_BUTTON_COUNT) == 0);
}

/* Nothing bound and no mode set: xinput, so the DualSense reaches games
 * that read a gamepad while the keyboard and mouse still work. */
static void test_default_mode(void)
{
    PwGameInput input;
    PwGameProfile p;

    pw_game_input_init(&input);
    pw_game_input_default_mode(&input);
    assert(input.mode == PW_GAME_INPUT_XINPUT);

    assert(parse(APP "[display]\ndesktop = 800x600\n", &p) == PW_OK);
    pw_game_input_init(&input);
    pw_game_input_overlay(&input, &p.input);
    pw_game_input_default_mode(&input);
    assert(input.mode == PW_GAME_INPUT_XINPUT);

    /* A binding, a pointer stick or an explicit mode keeps keyboard mode. */
    assert(parse(APP "[input]\ncross = space\n", &p) == PW_OK);
    pw_game_input_init(&input);
    pw_game_input_overlay(&input, &p.input);
    pw_game_input_default_mode(&input);
    assert(input.mode == PW_GAME_INPUT_KEYBOARD);
    assert(parse(APP "[input]\nmouse = left_stick\n", &p) == PW_OK);
    pw_game_input_init(&input);
    pw_game_input_overlay(&input, &p.input);
    pw_game_input_default_mode(&input);
    assert(input.mode == PW_GAME_INPUT_KEYBOARD);
    assert(parse(APP "[input]\nmode = keyboard\n", &p) == PW_OK);
    pw_game_input_init(&input);
    pw_game_input_overlay(&input, &p.input);
    pw_game_input_default_mode(&input);
    assert(input.mode == PW_GAME_INPUT_KEYBOARD);

    /* A preset's bindings count too. */
    pw_game_input_init(&input);
    assert(preset("[input]\nl1 = z\n", &input) == PW_OK);
    pw_game_input_default_mode(&input);
    assert(input.mode == PW_GAME_INPUT_KEYBOARD);
    pw_game_input_default_mode(NULL);
}

/* The forms the published profiles use (the prospero-win-profiles
 * repository): a preset gives Pinball its keys, Minesweeper shares the
 * mouse preset, the 7-Zip benchmark passes an arguments line, and the
 * gamepad preset binds nothing. */
static void test_published_forms(void)
{
    PwGameProfile p;
    PwGameInput input;

    assert(parse(APP "\n[display]\ndesktop = 800x600\nscaling = fit\n\n[input]\npreset = pinball\n",
                 &p) == PW_OK);
    assert(!strcmp(p.app.id, "pinball") && !strcmp(p.input.preset, "pinball"));
    assert(p.display.width == 800 && p.display.height == 600 && p.display.scaling == PW_GAME_SCALING_FIT);
    pw_game_input_init(&input);
    assert(preset("; comment\n[input]\nmode = keyboard\nl1 = z\nr1 = slash\ncross = space\n"
                  "left = x\nright = period\nup = up\noptions = f3\nsquare = f2\n"
                  "mouse = right_stick\nmouse_speed = 900\nr2 = mouse_left\nl2 = mouse_right\n",
                  &input) == PW_OK);
    pw_game_input_overlay(&input, &p.input);
    assert(input.bindings[L1].code == 'Z' && input.bindings[R1].code == 0xbf);
    assert(input.bindings[CROSS].code == 0x20 && input.mouse == PW_GAME_STICK_RIGHT);
    assert(input.bindings[R2].kind == PW_GAME_BIND_MOUSE && input.bindings[R2].code == 0);

    assert(parse("[application]\nid = minesweeper\nname = Minesweeper\n"
                 "executable = C:\\windows\\system32\\winemine.exe\n"
                 "working_directory = C:\\windows\\system32\nprefix = default\n"
                 "runtime = wine-wow64\narchitecture = pe64\ngraphics = gdi\n\n[input]\npreset = mouse\n",
                 &p) == PW_OK);
    assert(!strcmp(p.input.preset, "mouse") && p.app.architecture == PW_APP_ARCH_PE64);
    pw_game_input_init(&input);
    assert(preset("[input]\nmode = keyboard\nmouse = left_stick\nmouse_speed = 900\n"
                  "cross = mouse_left\ncircle = mouse_right\nsquare = mouse_middle\n"
                  "r2 = mouse_left\nl2 = mouse_right\n", &input) == PW_OK);
    assert(input.mouse == PW_GAME_STICK_LEFT && input.bindings[CROSS].kind == PW_GAME_BIND_MOUSE);
    assert(input.bindings[CIRCLE].code == 1 && input.bindings[SQUARE].code == 2);

    assert(parse("[application]\nid = sevenzip-bench\nname = 7-Zip benchmark\n"
                 "executable = C:\\Tools\\7za.exe\nworking_directory = C:\\Tools\n"
                 "arguments = b -mmt1 -md22\nprefix = default\nruntime = wine-wow64\n"
                 "architecture = pe32\ngraphics = gdi\n", &p) == PW_OK);
    assert(!strcmp(p.app.id, "sevenzip-bench") && !strcmp(p.app.arguments, "b -mmt1 -md22"));
    assert(p.app.architecture == PW_APP_ARCH_PE32 && !p.input.preset[0]);

    pw_game_input_init(&input);
    assert(preset("[input]\nmode = xinput\nmouse = none\n", &input) == PW_OK);
    assert(input.mode == PW_GAME_INPUT_XINPUT && input.mouse == PW_GAME_STICK_NONE);
    for (size_t i = 0; i < PW_GAME_BUTTON_COUNT; i++) assert(input.bindings[i].kind == PW_GAME_BIND_UNSET);
}

int main(void)
{
    test_published_forms();
    test_profile();
    test_refusals();
    test_presets();
    test_default_mode();
    printf("game profile passed: application plus display and input, every binding kind, "
           "refusals, shared presets overridden by the profile\n");
    return 0;
}

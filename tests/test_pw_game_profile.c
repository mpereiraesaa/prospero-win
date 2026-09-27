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
    assert(parse(APP "[display]\ndesktop = 320x200\n[input]\nmouse = left_stick\n", &p) == PW_OK);
    assert(p.display.width == 320 && p.input.mouse == PW_GAME_STICK_LEFT);
    assert(parse(APP "[display]\ndesktop = 3840x2160\n[input]\nmouse = none\ncross = enter\n", &p) == PW_OK);
    assert(p.input.mouse == PW_GAME_STICK_NONE && p.input.bindings[CROSS].code == 0x0d);
}

static void test_refusals(void)
{
    static const char *const bad[] = {
        "[display]\ndesktop = 800x600\n" APP,                  /* application not first */
        APP "[display]\n[display]\n",                          /* duplicate section */
        APP "[input]\n[input]\n",
        APP "[sound]\n",                                       /* unknown section */
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

/* The shipped Wine examples parse, and the preset gives Pinball its keys. */
static void test_examples(void)
{
    static uint8_t text[PW_APP_PROFILE_MAX_BYTES + 1];
    PwGameProfile p;
    PwGameInput input;
    FILE *file;
    size_t length;

    assert((file = fopen("examples/wine/profiles/pinball.profile", "rb")));
    length = fread(text, 1, sizeof(text), file);
    assert(!fclose(file) && length < sizeof(text));
    assert(pw_game_profile_parse(text, length, &p) == PW_OK);
    assert(!strcmp(p.app.id, "pinball") && !strcmp(p.input.preset, "pinball"));
    assert(p.display.width == 800 && p.display.height == 600 && p.display.scaling == PW_GAME_SCALING_FIT);
    assert((file = fopen("examples/wine/input/pinball.input", "rb")));
    length = fread(text, 1, sizeof(text), file);
    assert(!fclose(file) && length < sizeof(text));
    pw_game_input_init(&input);
    assert(pw_game_input_parse(text, length, &input) == PW_OK);
    pw_game_input_overlay(&input, &p.input);
    assert(input.bindings[L1].code == 'Z' && input.bindings[R1].code == 0xbf);
    assert(input.bindings[CROSS].code == 0x20 && input.mouse == PW_GAME_STICK_RIGHT);
    assert(input.bindings[R2].kind == PW_GAME_BIND_MOUSE && input.bindings[R2].code == 0);

    /* Minesweeper shares the generic mouse preset: left stick, Cross clicks. */
    assert((file = fopen("examples/wine/profiles/minesweeper.profile", "rb")));
    length = fread(text, 1, sizeof(text), file);
    assert(!fclose(file) && length < sizeof(text));
    assert(pw_game_profile_parse(text, length, &p) == PW_OK && !strcmp(p.input.preset, "mouse"));
    assert(p.app.architecture == PW_APP_ARCH_PE64);
    assert((file = fopen("examples/wine/input/mouse.input", "rb")));
    length = fread(text, 1, sizeof(text), file);
    assert(!fclose(file) && length < sizeof(text));
    pw_game_input_init(&input);
    assert(pw_game_input_parse(text, length, &input) == PW_OK);
    assert(input.mouse == PW_GAME_STICK_LEFT && input.bindings[CROSS].kind == PW_GAME_BIND_MOUSE);
    assert(input.bindings[CIRCLE].code == 1 && input.bindings[SQUARE].code == 2);
}

int main(void)
{
    test_examples();
    test_profile();
    test_refusals();
    test_presets();
    printf("game profile passed: application plus display and input, every binding kind, "
           "refusals, shared presets overridden by the profile\n");
    return 0;
}

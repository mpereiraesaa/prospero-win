/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_game_profile.h"
#include "../src/pw_wine_start.h"
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

static void test_graphics_env(void)
{
    PwGameProfile p;
    PwGameEnv env[PW_GAME_GRAPHICS_ENV_MAX], cpu[PW_GAME_CPU_ENV_MAX], runtime[PW_GAME_RUNTIME_ENV_MAX];
    assert(parse(APP, &p) == PW_OK);
    assert(pw_game_graphics_env(NULL, env) == 0 && pw_game_graphics_env(&p, NULL) == 0);
    for (int graphics = PW_APP_GRAPHICS_AUTO; graphics <= PW_APP_GRAPHICS_ZINK; graphics++) {
        for (int fps = 0; fps < 2; fps++) {
            p.app.graphics = (PwAppGraphics)graphics; p.display.show_fps = fps;
            p.display.refresh = 120; p.display.opengl_thread = 1;
            p.runtime.thread_scheduling = 1; p.runtime.shared_input = 1; p.runtime.fast_clock = 1;
            p.runtime.cpu = PW_GAME_CPU_NATIVE; /* worst-case explicit override */
            size_t count = pw_game_graphics_env(&p, env);
            assert(count <= PW_GAME_GRAPHICS_ENV_MAX);
            /* Fixed6, desktop/override/XInput3, graphics, runtime, clock and CPU. */
            assert(6 + 3 + count + pw_game_cpu_env(&p, cpu) + pw_game_runtime_env(&p.runtime, runtime) +
                   PW_GAME_CLOCK_ENV_MAX <= PW_WINE_START_MAX_ENV);
            if (graphics == PW_APP_GRAPHICS_ZINK) {
                assert(count == (size_t)(1 + fps));
                assert(!strcmp(env[0].name, "GALLIUM_DRIVER") && !strcmp(env[0].value, "zink"));
                if (fps) assert(!strcmp(env[1].name, "GALLIUM_HUD"));
            } else if (graphics == PW_APP_GRAPHICS_OPENGL) {
                assert(count == (size_t)(3 + fps));
                assert(!strcmp(env[0].name, "WINE_PS5_OPENGL"));
                assert(!strcmp(env[count - 2].name, "WINE_PS5_GL_REFRESH"));
                assert(!strcmp(env[count - 1].name, "PS5_GLTHREAD"));
            } else {
                assert(count == (size_t)fps);
                if (fps) assert(!strcmp(env[0].name, "DXVK_HUD"));
            }
        }
    }
}

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
    /* opengl_thread: off unless the profile asks for it */
    assert(parse(APP "[display]\nshow_fps = false\n", &p) == PW_OK && p.display.opengl_thread == 0);
    assert(parse(APP "[display]\nopengl_thread = true\n", &p) == PW_OK && p.display.opengl_thread == 1);
    assert(parse(APP "[display]\nopengl_thread = false\nrefresh = 120\n", &p) == PW_OK &&
           p.display.opengl_thread == 0 && p.display.refresh == 120);
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

/* [runtime] cpu: native by default for 32-bit games, the translator for
 * OpenGL ones, either chosen explicitly; native always brings batching. */
#define APP_ARCH(arch, gfx) "[application]\nid = g\nname = G\nexecutable = C:\\g.exe\n" \
    "working_directory = C:\\\nprefix = default\nruntime = wine-wow64\narchitecture = " arch \
    "\ngraphics = " gfx "\n"
static void test_cpu(void)
{
    PwGameProfile p;
    PwGameEnv env[PW_GAME_CPU_ENV_MAX];

    assert(parse(APP, &p) == PW_OK && p.runtime.cpu == PW_GAME_CPU_DEFAULT);
    assert(pw_game_cpu_native(&p) == 1);                       /* pe32 GDI */
    assert(pw_game_cpu_env(&p, env) == 2);
    assert(!strcmp(env[0].name, "WINE_PS5_WOW64_CPU") && !strcmp(env[0].value, "wow64native.dll"));
    assert(!strcmp(env[1].name, "PW_VK_BATCH") && !strcmp(env[1].value, "1"));

    assert(parse(APP_ARCH("pe32", "dxvk"), &p) == PW_OK && pw_game_cpu_native(&p) == 1);
    assert(parse(APP_ARCH("pe32", "auto"), &p) == PW_OK && pw_game_cpu_native(&p) == 1);
    assert(parse(APP_ARCH("pe32", "opengl"), &p) == PW_OK && pw_game_cpu_native(&p) == 0);
    assert(pw_game_cpu_env(&p, env) == 0);
    assert(parse(APP_ARCH("pe64", "dxvk"), &p) == PW_OK && pw_game_cpu_native(&p) == 0);

    assert(parse(APP_ARCH("pe32", "zink"), &p) == PW_OK && pw_game_cpu_native(&p) == 1);
    assert(pw_game_cpu_env(&p, env) == 2 && !strcmp(env[1].name, "PW_VK_BATCH"));
    assert(parse(APP_ARCH("pe64", "zink"), &p) == PW_OK && pw_game_cpu_env(&p, env) == 0);
    assert(parse(APP_ARCH("pe32", "zink") "[runtime]\ncpu=translator\n", &p) == PW_OK &&
           pw_game_cpu_env(&p, env) == 0);

    /* explicit choices override the default both ways */
    assert(parse(APP_ARCH("pe32", "opengl") "[runtime]\ncpu = native\n", &p) == PW_OK);
    assert(p.runtime.cpu == PW_GAME_CPU_NATIVE && pw_game_cpu_native(&p) == 1 &&
           pw_game_cpu_env(&p, env) == 2);
    assert(parse(APP_ARCH("pe32", "dxvk") "[runtime]\nCPU = Translator\n", &p) == PW_OK);
    assert(p.runtime.cpu == PW_GAME_CPU_TRANSLATOR && pw_game_cpu_native(&p) == 0 &&
           pw_game_cpu_env(&p, env) == 0);
    /* a 64-bit game never uses the WoW64 CPU, even when asked */
    assert(parse(APP_ARCH("pe64", "dxvk") "[runtime]\ncpu = native\n", &p) == PW_OK);
    assert(pw_game_cpu_native(&p) == 0 && pw_game_cpu_env(&p, env) == 0);
    /* with thread scheduling, in either order */
    assert(parse(APP "[runtime]\ncpu = native\nthread_scheduling = 1\n", &p) == PW_OK);
    assert(p.runtime.thread_scheduling == 1 && p.runtime.cpu == PW_GAME_CPU_NATIVE);
    assert(pw_game_cpu_native(NULL) == 0 && pw_game_cpu_env(NULL, env) == 0 &&
           pw_game_cpu_env(&p, NULL) == 0);
}

/* [runtime]: opt-in settings that reach Wine's environment, off unless set. */
static void test_runtime(void)
{
    PwGameEnv env[PW_GAME_RUNTIME_ENV_MAX];
    PwGameProfile p;

    assert(parse(APP, &p) == PW_OK);
    assert(!p.runtime.thread_scheduling && pw_game_runtime_env(&p.runtime, env) == 0);
    assert(parse(APP "[runtime]\nthread_scheduling = true\n", &p) == PW_OK);
    assert(p.runtime.thread_scheduling == 1);
    assert(pw_game_runtime_env(&p.runtime, env) == 1);
    assert(!strcmp(env[0].name, "WINE_PS5_SCHED") && !strcmp(env[0].value, "1"));
    /* Among the other sections, in any case. */
    assert(parse(APP "[display]\nview = desktop\n[runtime]\nThread_Scheduling = 1\n"
                 "[debug]\nwinedebug = +seh\n", &p) == PW_OK);
    assert(p.runtime.thread_scheduling == 1);
    assert(p.display.view == PW_GAME_VIEW_DESKTOP && !strcmp(p.winedebug, "+seh"));
    /* Turned off explicitly: nothing reaches the environment. */
    assert(parse(APP "[runtime]\nthread_scheduling = FALSE\n", &p) == PW_OK);
    assert(!p.runtime.thread_scheduling && pw_game_runtime_env(&p.runtime, env) == 0);
    assert(pw_game_runtime_env(NULL, env) == 0 && pw_game_runtime_env(&p.runtime, NULL) == 0);

    /* shared_input reaches Wine; fast_clock is only a flag, the title
     * measures the TSC and sets the clock variables itself. */
    assert(parse(APP, &p) == PW_OK && !p.runtime.shared_input && !p.runtime.fast_clock);
    assert(parse(APP "[runtime]\nshared_input = true\n", &p) == PW_OK);
    assert(p.runtime.shared_input == 1 && !p.runtime.fast_clock);
    assert(pw_game_runtime_env(&p.runtime, env) == 1);
    assert(!strcmp(env[0].name, "PW_INPUT_SHARED_FAST") && !strcmp(env[0].value, "1"));
    assert(parse(APP "[runtime]\nfast_clock = 1\n", &p) == PW_OK);
    assert(p.runtime.fast_clock == 1 && !p.runtime.shared_input && pw_game_runtime_env(&p.runtime, env) == 0);
    assert(parse(APP "[runtime]\nfast_clock = false\nshared_input = 0\n", &p) == PW_OK);
    assert(!p.runtime.fast_clock && !p.runtime.shared_input);
    assert(parse(APP "[runtime]\nthread_scheduling = 1\nshared_input = 1\nfast_clock = true\ncpu = native\n",
                 &p) == PW_OK);
    assert(p.runtime.fast_clock && p.runtime.cpu == PW_GAME_CPU_NATIVE);
    assert(pw_game_runtime_env(&p.runtime, env) == PW_GAME_RUNTIME_ENV_MAX);
    assert(!strcmp(env[0].name, "WINE_PS5_SCHED") && !strcmp(env[1].name, "PW_INPUT_SHARED_FAST"));
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
        APP "[display]\nopengl_thread = yes\n",                /* true or false only */
        APP "[display]\nopengl_thread = true\nopengl_thread = false\n", /* twice */
        APP "[debug]\nwinedebug = +seh\n[debug]\n",            /* duplicate section */
        APP "[debug]\nwinedebug =\n",                         /* empty */
        APP "[debug]\nwinedebug = +seh;rm\n",                  /* not a channel list */
        APP "[debug]\nwinedebug = +seh all\n",
        APP "[debug]\nwinedebug = $HOME\n",
        APP "[debug]\nwinedebug = +seh\nwinedebug = +relay\n",  /* twice */
        APP "[debug]\nrelay = on\n",                           /* unknown key */
        APP "[debug]\nwinedebug = +aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n",   /* too long */
        APP "[runtime]\n[runtime]\n",                          /* duplicate section */
        APP "[runtime]\nthread_scheduling = yes\n",             /* true/false or 1/0 only */
        APP "[runtime]\nthread_scheduling = 2\n",
        APP "[runtime]\nthread_scheduling =\n",
        APP "[runtime]\nthread_scheduling = on\n",
        APP "[runtime]\nthread_scheduling = 0\nthread_scheduling = 1\n",
        APP "[runtime]\nshared_input = yes\n",                  /* true/false or 1/0 only */
        APP "[runtime]\nshared_input =\n",
        APP "[runtime]\nshared_input = 1\nshared_input = 0\n",   /* once */
        APP "[runtime]\nfast_clock = on\n",
        APP "[runtime]\nfast_clock = 2\n",
        APP "[runtime]\nfast_clock = true\nfast_clock = true\n",
        APP "[display]\nfast_clock = 1\n",                      /* another section's key */
        APP "[runtime]\ntrust_code_pages = 1\n",                /* unknown key */
        APP "[runtime]\ncpu = dbt\n",                           /* native or translator only */
        APP "[runtime]\ncpu =\n",
        APP "[runtime]\ncpu = native\ncpu = translator\n",     /* once */
        APP "[runtime]\nwinedebug = +seh\n",                    /* another section's key */
        APP "[display]\nthread_scheduling = 1\n",
        "[runtime]\nthread_scheduling = 1\n" APP,               /* application not first */
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
    test_graphics_env();
    test_published_forms();
    test_profile();
    test_runtime();
    test_cpu();
    test_refusals();
    test_presets();
    test_default_mode();
    printf("game profile passed: application plus display and input, every binding kind, "
           "runtime settings and their environment, the CPU backend choice, refusals, shared presets overridden by the profile\n");
    return 0;
}

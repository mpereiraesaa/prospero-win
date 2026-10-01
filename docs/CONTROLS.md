# Controls

A game can be played with the DualSense, with a USB keyboard and mouse, or
both at once. How the DualSense behaves is set per game by an input preset.

## USB keyboard and mouse

Plug them into the PS5 and they work in every game, alongside the
controller. You can plug them in after the game has started; the app looks
for them every few seconds. Keys reach the game just as they would on a PC,
modifiers included (Ctrl, Shift, Alt, the Windows keys), and the mouse moves
the same pointer the DualSense's stick does. The mouse wheel doesn't work
yet.

## The DualSense

Each game's profile names a preset, a small text file in
`/data/prospero-win/input/`. There are two kinds.

### Keyboard and mouse games

Most older PC games expect a keyboard and mouse, so the preset turns the
DualSense into one: each button becomes a key or a mouse button, and a stick
can move the pointer. This is Warcraft III's (`warcraft3.input`):

```ini
; Warcraft III on the DualSense. Comments go on their own lines.
[input]
mode = keyboard
; the left stick moves the pointer, at the default speed (up to 20000)
mouse = left_stick
mouse_speed = 1200
cross = mouse_left
circle = mouse_right
; attack and stop
square = a
triangle = s
r1 = tab
l1 = alt
; the d-pad scrolls the map
up = up
down = down
left = left
right = right
; the game menu
options = f10
create = escape
touchpad = space
```

- **Buttons:** `cross`, `circle`, `square`, `triangle`, `l1`, `r1`, `l2`,
  `r2`, `l3`, `r3`, `up`, `down`, `left`, `right`, `options`, `create`,
  `touchpad`.
- **What they can send:**
  - a letter or a digit;
  - `f1` to `f24`;
  - `space`, `enter`, `escape`, `tab`, `backspace`, `shift`, `ctrl`, `alt`,
    `pause`, `pageup`, `pagedown`, `home`, `end`, `insert`, `delete`, the
    arrows (`up`, `down`, `left`, `right`), and `semicolon`, `equals`,
    `comma`, `minus`, `period`, `slash`, `backquote`, `lbracket`,
    `backslash`, `rbracket`, `quote`;
  - any Windows key code as `vk:0xNN`;
  - `mouse_left`, `mouse_right` or `mouse_middle`;
  - `none`.
- **`mouse`:** `left_stick`, `right_stick` or `none`.

`mouse.input` is a starting point for games played with the mouse alone.

### Xbox controller games

Games from the Xbox 360 era and later read a controller through XInput. With
`mode = xinput` the DualSense is the game's first Xbox controller:

| DualSense | Xbox |
| --- | --- |
| Cross, Circle, Square, Triangle | A, B, X, Y |
| L1, R1 | LB, RB |
| L2, R2 | LT, RT (analog) |
| L3, R3 | stick clicks |
| Options | Start |
| Create | Back |
| sticks, d-pad | sticks, d-pad |

Rumble works: the game's vibration drives the DualSense's motors. The
`gamepad.input` preset is exactly this. You can still add button lines in an
XInput preset to send keys as well.

A profile that binds no button, moves no pointer and sets no `mode` gets
`mode = xinput` on its own: a game with gamepad support reads the DualSense,
and the keyboard and mouse keep working. Games built on SDL2 (Half-Life's
25th-anniversary build, for one) look for controllers through raw input
first, which Wine on the PS5 doesn't have, and then skip XInput; in xinput
mode the app sets `SDL_JOYSTICK_RAWINPUT=0` for them, and they find the
DualSense as an XInput controller. On the console, XInput reports both
sticks over their full range, the triggers and the buttons, and Half-Life's
SDL2 opens it as a game controller.

### DirectInput

Games that read a controller through DirectInput (many games from before
2005 or so) don't see the DualSense yet. Play them with a keyboard-and-mouse
preset instead. Keyboard and mouse input through DirectInput should work,
since it comes from the same place as normal Windows input, but it hasn't
been tested.

## Closing a game

Hold **Options + Create** for a second on the DualSense. The game is asked
to close as with Alt+F4, and you return to the launcher; if it doesn't close
within a few seconds, the app leaves it. From a keyboard, Alt+F4 or the
game's own quit option do the same.

## Writing a preset

A profile can name a preset (`preset = warcraft3` in its `[input]` section)
and change parts of it: any line in the profile's own `[input]` section wins
over the preset's. Put new presets in `/data/prospero-win/input/`, and
consider sharing them in
[prospero-win-profiles](https://github.com/mpereiraesaa/prospero-win-profiles).

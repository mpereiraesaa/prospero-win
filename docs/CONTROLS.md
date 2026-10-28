# Controls

A game can be played with the DualSense, with a USB keyboard and mouse, or
both at once. How the DualSense behaves is set per game by an input preset.

## USB keyboard and mouse

Plug them into the PS5 and they work in every game, alongside the
controller. You can plug them in after the game has started; the app looks
for them every few seconds. Keys reach the game just as they would on a PC,
modifiers included (Ctrl, Shift, Alt, the Windows keys), and the mouse moves
the same pointer the DualSense's stick does. The vertical mouse wheel is
forwarded as Windows wheel input (one detent is 120 units). Horizontal scrolling and Logitech-specific gestures are not
yet supported.

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
| touchpad click, left half | Back |
| touchpad click, right half | Guide (the Xbox button) |
| sticks, d-pad | sticks, d-pad |

The touchpad's left half gives Back without Create, whose press also opens
the PS5's capture menu; the half is the one the click began on. Rumble
works: the game's vibration drives the DualSense's motors. The
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

### A second player

A game that two people play on one PC keyboard, each with their own keys,
can give the second player a DualSense too. The profile names a second
keyboard preset in its `[input]` section:

```ini
[input]
preset = atomic-bomberman
player2 = atomic-bomberman-p2
```

The second DualSense has to be signed in as another PS5 user (a second
account or a guest): the app opens the pad of a signed-in user other than
the one who started it, and looks again every few seconds while there is
none, so the second player can sign in after the game has started. With
more users signed in, a pick that shows no controller for a few seconds,
or loses it, makes way for the next user. Its buttons send keys through
`player2`'s preset only: it never acts as an Xbox controller, the preset's
mouse buttons and `mouse` stick are ignored, and closing the game with
Options + Create stays the first player's.

## Closing a game

Hold **Options + Create** for three seconds on the DualSense (games use
Start + Select together, so a short press never closes). The game is asked
to close as with Alt+F4, and you return to the launcher; if it doesn't close
within a few seconds, the app leaves it. From a keyboard, Alt+F4 or the
game's own quit option do the same.

## The in-game menu

A profile with `menu = true` in its `[display]` section opens prospero-win's
menu over the game when you press **L3 + R3** (both sticks) together. While
it is open the game sees the DualSense idle: the d-pad moves the cursor,
Cross picks a line and Circle or L3 + R3 closes it. Buttons still held as it
closes stay away from the game until you let go of them. Closing the game
with Options + Create works with the menu open.

DXVK's HUD draws the menu, so it appears in Direct3D games only, and only
with the DXVK build that has the `menu` item (`2.6.2-prospero3` and later;
an older DXVK draws nothing, and the pad is still the menu's while it is
open). Its lines are **Keyboard input**, which opens the console's keyboard
(below), and **Close**.

## The console's keyboard

Click the touchpad with **two fingers** in a game and the PS5's own
on-screen keyboard opens over it, the same one the console shows for a
search box. Write with the DualSense or with a USB keyboard, accept, and
the line is typed into the game as key presses, with Enter at the end — a
chat line or a console command arrives as it would from a PC keyboard.
Cancelling types nothing.

The two-finger click never reaches the game, not even for a moment: the
pad says how many fingers made a click in the click itself, so nothing
has to wait for a second button, and a one-finger click is the game's as
before (Back or Guide, or the preset's key). The game sees the DualSense
idle while the keyboard is up, and the buttons still down when it closes
stay away from it until you let go of them. A USB keyboard types into the
console's keyboard while it is up, not into the game.

The line is typed a key at a time, each held down for one tick (16 ms), so
a game that reads its keyboard once a frame sees every key, and a letter
typed twice in a row is let go in between: a 20-character line takes about
a third of a second, the longest (255) about four. Characters a US
keyboard has no key for (accents, for instance) are skipped: the rest of
the line is still typed. The in-game menu's "Keyboard input" opens the same
keyboard; it is shown only when the console's keyboard is available.

The keyboard is the system's, drawn and driven by the console. Its module
is loaded when the app starts (system modules have to be, before the app
gets its data folder), and the log's `preload libSceImeDialog` line says
how much flexible memory that took; the rest of the dialog's setup waits
for the first time the keyboard is opened. A firmware that refuses it to
this app leaves everything else as it is; the run's log says `osk=` and
what the system answered.

## Writing a preset

A profile can name a preset (`preset = warcraft3` in its `[input]` section)
and change parts of it: any line in the profile's own `[input]` section wins
over the preset's. Put new presets in `/data/prospero-win/input/`, and
consider sharing them in
[prospero-win-profiles](https://github.com/mpereiraesaa/prospero-win-profiles).

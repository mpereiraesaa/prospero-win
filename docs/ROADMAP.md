# Where it's going

- **More games.** Warcraft III runs through DXVK on RADV; the next step is a
  wider set of Direct3D 8–11 games, fixing what they need in Wine's PS5
  drivers and patches rather than game by game.
- **More OpenGL games.** Game profiles can select the PS5 OpenGL backend
  alongside DXVK. Broaden compatibility and reduce draw overhead in the SDK;
  its legacy WGL support is awaiting upstream review.
- **DirectInput controllers.** Today the DualSense reaches games as a
  keyboard and mouse, or as an Xbox controller through XInput.
- **Speed.** The x86 translator reaches 85–93% of native speed on the
  benchmarks; startup and frame pacing come next, measured against native
  runs.

Pinball and Minesweeper stay as regression tests for every change.

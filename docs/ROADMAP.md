# Where it's going

- **More games.** Warcraft III runs through DXVK on RADV; the next step is a
  wider set of Direct3D 8–11 games, fixing what they need in Wine's PS5
  drivers and patches rather than game by game.
- **Other firmwares.** Everything so far was tested on 12.02. Reports from
  other firmwares tell us what depends on the firmware.
- **OpenGL games**, through the PS5's own OpenGL, alongside DXVK.
- **DirectInput controllers.** Today the DualSense reaches games as a
  keyboard and mouse, or as an Xbox controller through XInput.
- **Speed.** The x86 translator reaches 85–93% of native speed on the
  benchmarks; startup and frame pacing come next, measured against native
  runs.

Pinball and Minesweeper stay as regression tests for every change.

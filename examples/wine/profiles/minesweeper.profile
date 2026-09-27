; Wine's own Minesweeper (winemine), a mouse-only game that ships with the
; prefix: a quick check of the pointer and both mouse buttons.
[application]
id = minesweeper
name = Minesweeper
executable = C:\windows\system32\winemine.exe
working_directory = C:\windows\system32
prefix = default
runtime = wine-wow64
architecture = pe64
graphics = gdi

[display]
desktop = 800x600
scaling = fit

[input]
preset = mouse

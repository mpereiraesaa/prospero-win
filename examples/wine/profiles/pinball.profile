; Space Cadet Pinball through Wine. Copy examples/wine/ to /data/prospero-win/
; on the console; the game itself goes in the prefix at C:\Games\Pinball.
[application]
id = pinball
name = Space Cadet Pinball
executable = C:\Games\Pinball\PINBALL.EXE
working_directory = C:\Games\Pinball
prefix = default
runtime = wine-wow64
architecture = pe32
graphics = gdi

[display]
; Wine's desktop, scaled to the whole screen keeping its aspect ratio.
desktop = 800x600
scaling = fit

[input]
; Flippers, plunger and nudges are shared with other Pinball profiles.
preset = pinball

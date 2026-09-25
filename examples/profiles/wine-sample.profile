; Synthetic PE32 fixture used to validate the native Wine bootstrap.
[application]
id = wine-bootstrap-sample
name = Wine Bootstrap Sample
executable = C:\app.exe
working_directory = C:\
arguments = --bootstrap-test
prefix = bootstrap
runtime = wine-i386-pinned
architecture = pe32
graphics = gdi

; 7-Zip's built-in benchmark on one thread, the yardstick FEX and box64
; publish: it measures the i386 translator (docs/DBT_BENCHMARK.md). Copy the
; i386 7za.exe from the official 7-Zip "extra" package into the prefix's
; drive_c/Tools; it is not shipped here. The report arrives in ps5log as
; STDOUT lines, and the program exits by itself.
[application]
id = sevenzip-bench
name = 7-Zip benchmark
executable = C:\Tools\7za.exe
working_directory = C:\Tools
arguments = b -mmt1 -md22
prefix = default
runtime = wine-wow64
architecture = pe32
graphics = gdi

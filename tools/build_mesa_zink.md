# Pinned Mesa WGL/Zink builds

`build_mesa_zink.sh` builds PE32 and PE64 WGL frontends and the Zink Gallium
backend on Linux. OpenGL games on the console draw through them: the package
carries both architectures (`tools/package_release.sh --mesa-zink`), and the
launcher installs the game's copy into its prefix (`graphics = opengl`).

Install Git, tar, xz, Ninja, Python 3 with venv support, and the pinned Python
tools in an isolated environment:

```sh
python3 -m venv build/mesa-python
build/mesa-python/bin/python -m pip install -r tools/mesa_zink_requirements.txt
git clone https://github.com/mpereiraesaa/PS5_Mesa.git build/mesa-source
```

Download `llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64.tar.xz` from
[the llvm-mingw 20260922 release](https://github.com/mstorsjo/llvm-mingw/releases/tag/20260922).
Its SHA-256 is
`bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21`.
The script checks this digest before creating the build directory and exports
Mesa commit `e4607ef697fe3df8d1ebc329bf1917d041843306` from the supplied clone.
The source checkout is not modified. Subsequent runs validate the exported
source against a fresh Git archive, including archive export attributes.

```sh
MESON="$PWD/build/mesa-python/bin/meson" \
PATH="$PWD/build/mesa-python/bin:$PATH" \
bash tools/build_mesa_zink.sh --work build/mesa-zink \
  --mesa build/mesa-source \
  --llvm-mingw /path/to/llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64.tar.xz \
  --jobs 1
```

Both architectures are built sequentially. Keep sufficient memory for the
release link; the first dual build was validated with one job and an 8 GiB
memory limit. Reuse the same work directory only for this pinned recipe.
Ninja 1.11.1 and Python 3.12 were used for validation.

Each architecture produces `opengl32.dll` and `libgallium_wgl.dll`. Imported
compiler runtimes, including their transitive compiler runtime dependencies,
are copied beside them. Windows and UCRT APIs are provided by the Wine runtime.
The script strips separate output copies, retaining unstripped DLLs in the
build directories. `artifacts/manifest.json` records the source/compiler pins,
PE architectures, imports, raw and stripped DLL hashes, and licence hashes.
Source copyright/permission notices and component licence texts accompany the
artifacts. The manifest explicitly records that console validation is pending.

The optional release packaging interface is supplied by a separate change.

# Third-party software in the app folder

The `PPSA99995` folder you install on the console is more than prospero-win.
It also carries Wine, a font engine, a graphics driver and a few other
pieces, each under its own licence. This file lists them. The licence texts
are in the folder's `LICENSES/` directory, and `SOURCES.txt` names the exact
source revision of each part, so you can get the source that matches the
binaries you have.

prospero-win's own code is LGPL-2.1-or-later (`LICENSE`).

## The title: `eboot.bin`, `sce_module/libc.prx`

The title is built with
[BlackBearReloaded's PS5 Native App Boilerplate](https://github.com/mpereiraesaa/ps5-native-app-boilerplate)
and the public [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk),
both GPL-3.0-or-later. The boilerplate's startup code is linked into
`eboot.bin`, and `sce_module/libc.prx` is the boilerplate's own runtime
library. Because the title combines their code with prospero-win's, it is
distributed as a whole under the GPL, version 3 (`LICENSES/GPL-3.0.txt`);
prospero-win's part stays available under the LGPL on its own.

The title also contains a tile-address permutation adapted from SDL's PS5
video backend, Copyright (C) 2026 John Törnblom, under the zlib licence. The
full notice is in prospero-win's `native/pw_videoout_ps5.c`.

## The privilege helper: `lapy.elf`

`lapy.elf` is the prebuilt
[PS5 Lapy JB Daemon](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon),
Copyright (c) 2026 Arksama (Team PHU), under the MIT licence
(`LICENSES/Lapy-MIT.txt`, which also holds its authors' disclaimer).

## Wine: `win/wine/`

Every module under `win/wine/lib/wine/`, the NLS files and the fonts come
from [Wine](https://gitlab.winehq.org/wine/wine), LGPL-2.1-or-later, with
prospero-win's patches from its `wine/patches` directory applied.
`wowprospero.dll` and `wowprospero.prx` are prospero-win's own code under
the same licence.

Wine includes code from other projects (zlib, libpng, FAudio, the LLVM C++
runtime, Unicode data and more). `LICENSES/wine/` holds Wine's `LICENSE`,
`COPYING.LIB`, `AUTHORS` and `NOTICES.md` and the licence files of the
libraries Wine bundles (`LICENSES/wine/libs/`), copied from the Wine source
tree the package was built from.

The PS5 modules (`win/wine/lib/wine/x86_64-unix/*.prx`) are built with the
same payload SDK as the title and link parts of LLVM's runtime from it:
libunwind, compiler-rt's emulated TLS and, where a driver needs C++,
libc++ and libc++abi. LLVM is under the Apache License 2.0 with LLVM
Exceptions (`LICENSES/Apache-2.0-WITH-LLVM-exception.txt`, libunwind's copy;
the other three carry the same terms).

## FreeType: `libfreetype.prx`

Wine draws text with [FreeType](https://freetype.org), used here under the
FreeType License (`LICENSES/freetype/FTL.TXT`; `LICENSES/freetype/LICENSE.TXT`
explains the choice of licences). Portions of this software are copyright
© 2024 The FreeType Project (www.freetype.org). All rights reserved.

FreeType's gzip support is its own copy of zlib, and its hash functions come
from the BDF driver. Their notices, from the FreeType source:

```
  Copyright (C) 1995-2023 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu
```

```
 * Copyright 2000 Computing Research Labs, New Mexico State University
 * Copyright 2001-2015
 *   Francesco Zappa Nardelli
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COMPUTING RESEARCH LAB OR NEW MEXICO STATE UNIVERSITY BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT
 * OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
 * THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## The Vulkan driver: `libvulkan.prx`

`libvulkan.prx` is RADV, Mesa's Vulkan driver for AMD GPUs, from the
[PS5_Mesa](https://github.com/mpereiraesaa/PS5_Mesa) port. Mesa is mostly
MIT-licensed (`LICENSES/Mesa-MIT.txt`); the copyright holders are named in
each source file, and individual files may carry other permissive terms
(see Mesa's `docs/license.rst` and its `licenses/` directory at the
revision in `SOURCES.txt`). It is linked for the console by
[PS5_Vulkan](https://github.com/mpereiraesaa/PS5_Vulkan) with the
[PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) fork's platform
layer, both GPL-3.0-or-later, so `libvulkan.prx` as a whole is distributed
under the GPL, version 3.

## OpenGL, when included

A package built with OpenGL support links the
[ps5-opengl](https://github.com/mpereiraesaa/ps5-opengl) SDK, itself built
on Mesa, into `win32u.prx`. That SDK is GPL-3.0-or-later, so in such a
package `win32u.prx` is distributed under the GPL, version 3, and its Mesa
parts keep their MIT notices as above. `SOURCES.txt` says whether your
package includes it.

## Getting the source

Every part above is open source, and `SOURCES.txt` gives the repository and
revision of each. prospero-win's own source, including the Wine patches, is
at <https://github.com/mpereiraesaa/prospero-win>.

## Optional Mesa WGL/Zink: `opengl32.dll`, `libgallium_wgl.dll`

When supplied with `--mesa-zink`, the package includes Mesa's Windows WGL
frontend and Zink under `win/mesa-zink/i386-windows/` and
`win/mesa-zink/x86_64-windows/`. They use the installed
Vulkan driver. Packaging these DLLs does not select them for a game or
remove the existing OpenGL backend. Wine's builtin modules remain unchanged.

Mesa is primarily MIT-licensed; component-specific licence texts and
Mesa's licence overview are copied from the pinned source into
`LICENSES/mesa/`. The llvm-mingw C++ runtime and MinGW runtime notices
are in `LICENSES/llvm-mingw/`. `SOURCES.txt` and
`mesa-zink-manifest.json` record the source, compiler archive and DLL hashes.

The optional WGL/Zink packager validates every consumed manifest field before
changing the output package, including compiler version, hashes, imports and
licence metadata. Input is a trusted, immutable local builder artifact set;
this is not manifest authentication. Limits are 1 MiB for the manifest,
32 DLLs per architecture (128 MiB each), 256 imports per DLL, and 256 licence
files (16 MiB each). Provider basenames are ASCII and at most 127 bytes;
symlink inputs and duplicate case aliases are refused. Copying after successful
preflight remains nontransactional, so output must not be published if copying
fails. Existing builtin OpenGL remains unchanged.

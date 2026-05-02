# Building obs-st2110 (the OBS plugin)

The plugin has zero non-OBS dependencies (libst2110rx is built in-tree).
What you need:

1. **OBS Studio source or dev SDK.** OBS doesn't ship dev headers in the
   standard installer. Two options:
   - **Plugin template approach** (recommended for first build): clone
     `https://github.com/obsproject/obs-studio.git` at a tag matching your
     installed OBS version (currently `32.1.2`), build it once locally with
     CMake. The build produces `libobs.lib` and exposes
     `libobsConfig.cmake`.
   - **Bring your own SDK**: any directory containing
     `cmake/libobsConfig.cmake` + the libobs headers + `libobs.lib`. The
     golivebro `obs-plugin/native` build accepts paths via
     `OBS_INCLUDE_DIRS` + `OBS_LIBRARIES` — that pattern works here too if
     you prefer manual paths.

2. **A C compiler.** MSVC (Visual Studio 2022 Build Tools) on Windows is
   what OBS itself ships against. MinGW works for the library but not
   recommended for the plugin DLL (ABI risk against MSVC-built obs.lib).

3. **CMake 3.20+** and **Ninja** (already installed at
   `C:\Program Files\CMake\bin\cmake.exe`).

## Build steps (Windows, MSVC)

```powershell
# From the obs-st2110 repo root
cmake -B build -G Ninja `
  -DBUILD_OBS_PLUGIN=ON `
  -Dlibobs_DIR="<path-to-obs-studio-build>/libobs" `
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --target obs-st2110
```

Output: `build/obs-plugin/obs-st2110.dll` plus a `data/` tree.

## Install

```powershell
# Run as Administrator
copy build\obs-plugin\obs-st2110.dll "C:\Program Files (x86)\obs-studio\obs-plugins\64bit\"
xcopy obs-plugin\data\* "C:\Program Files (x86)\obs-studio\data\obs-plugins\obs-st2110\" /s /e /i
```

Restart OBS. The source should appear in the **+ Source** menu as
"SMPTE ST 2110-20".

## Default settings (matches the camera bridge sender)

| Field | Value |
|-------|-------|
| Multicast group | `239.10.21.20` |
| Port | `5000` |
| Interface IP | `10.0.0.1` (or blank for any) |
| Width | `1920` |
| Height | `1080` |
| Socket buffer (MiB) | `128` |

These are the plugin's built-in defaults so the source should produce
video as soon as you add it without any property changes.

## Verifying the library independently

Before debugging plugin glue, make sure the library decodes the stream:

```bash
cmake -B build -G Ninja
cmake --build build
build/libst2110rx/st2110rx-cli --addr 239.10.21.20 --port 5000 \
  --iface 10.0.0.1 --width 1920 --height 1080 --count 60 --stats 30
```

Expected: `frames=60 dropped=0 packets=~250000 lost=0`. Validated on
sender-host against the real bridge stream on 2026-05-02.

## Known limitations (this MVP)

- 10-bit RFC 4175 4:2:2 only (matches camera bridge sender output). 8-bit
  inputs would need a separate conversion path.
- Color space hardcoded to BT.709 limited range.
- No NMOS IS-04 discovery or IS-05 control yet — user enters multicast
  address manually.
- No SDP file parsing — same.
- Single sender per source instance. Add multiple OBS sources to consume
  multiple senders.

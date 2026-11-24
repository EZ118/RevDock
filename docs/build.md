# Build

## Windows (verified)

Tested with w64devkit:

- GCC 16.2.0 (`g++`)
- CMake 4.4.4
- Ninja 1.13.2

```powershell
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
ctest --preset windows-mingw-release --output-on-failure
.\build\windows-mingw-release\bin\revdock.exe
```

Install a portable tree:

```powershell
cmake --install build\windows-mingw-release --prefix dist
.\dist\revdock.exe
```

## Linux (X11)

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
  libx11-dev libxext-dev libxrandr-dev libxfixes-dev libxi-dev \
  libxcursor-dev libxss-dev libwayland-dev libxkbcommon-dev \
  wayland-protocols libdecor-0-dev

cmake --preset linux-x11-release
cmake --build --preset linux-x11-release
ctest --preset linux-x11-release --output-on-failure
./build/linux-x11-release/bin/revdock
```

If X11 development headers are not available:

```bash
cmake -S . -B build/linux-no-x11 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DREVDOCK_ENABLE_X11=OFF
cmake --build build/linux-no-x11
```

## Dependencies

The repository vendors the exact versions used by the build. To refresh:

```powershell
pwsh tools/fetch_deps.ps1 -Force
python tools/trim_deps.py
python tools/verify_deps.py
```

The fetch script uses `https://cdn.gh-proxy.org/` by default; change `-Proxy`
if direct GitHub access is preferred.

## Troubleshooting

- **No transparent window**: the desktop compositor may not support
  `SDL_WINDOW_TRANSPARENT`; the dock falls back to a non-transparent window.
- **No taskbar icon**: expected; dock/menu/grid/stack use utility windows.
- **Missing assets**: run from the build tree (CMake copies `assets/` next to
  the executable) or use `cmake --install`.
- **Settings saved to the wrong place**: remove `revdock.portable` from the
  executable directory to use platform config paths.
- **Windows work-area reservation moves windows**: this is the experimental
  AppBar path. The Dock reserves space automatically when auto-hide is off.

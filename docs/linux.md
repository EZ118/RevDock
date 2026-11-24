# Linux notes

## X11 (primary)

- Dock window: `_NET_WM_WINDOW_TYPE_DOCK`, `_NET_WM_STATE_ABOVE`,
  `_NET_WM_STATE_SKIP_TASKBAR`, `_NET_WM_STATE_SKIP_PAGER`.
- Work area: `_NET_WM_STRUT_PARTIAL` (only when “Reserve screen space” is on).
- Window placement: positioned at the bottom/left/right of the display bounds.
- Global mouse position: `SDL_GetGlobalMouseState` for autohide edge detection.
- Application list: `.desktop` files from `$XDG_DATA_HOME/applications` and
  `$XDG_DATA_DIRS/applications`.
- Icons: freedesktop icon theme search in
  `$XDG_DATA_HOME/icons`, `$XDG_DATA_DIRS/icons`, `~/.icons`, `/usr/share/pixmaps`;
  supports PNG and SVG (`IMG_LoadSizedSVG_IO`).
- Launch: `gio launch <desktop file>` when available, otherwise a safe
  `fork/exec` of the parsed `Exec=` argv (no shell).
- Running apps: `/proc/<pid>/comm` comparison against the desktop entry's
  executable basename. This is intentionally cheap and does not require
  XComposite.
- Trash: opens `$XDG_DATA_HOME/Trash/files`; emptying tries
  `gio trash --empty`.

## Wayland (degraded)

SDL3 will use the Wayland backend when no X11 display is available. In that
case RevDock:

- does not reserve work area (no strut),
- cannot position itself at an absolute screen edge,
- cannot poll the global cursor when the pointer is outside its surface,
- does not enumerate other applications' windows.

The dock remains usable as a floating bottom panel. For a native
`wlr-layer-shell` implementation, a later milestone can add the layer-shell
protocol and a dedicated Wayland surface role.

Backdrop blur is currently Windows-only: the Windows backend captures the
screen behind the dock with `BitBlt` while excluding the dock, then applies a
box blur. On X11/Wayland the dock uses a translucent light/dark material, and
the application grid uses a dim background instead of blurred wallpaper.
Capture-based blur can be added per compositor later (XComposite/sway effects),
but it must remain optional to avoid compositor-specific dependencies.

## Build dependencies

Ubuntu/Debian:

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
  libx11-dev libxext-dev libxrandr-dev libxfixes-dev libxi-dev \
  libxcursor-dev libxss-dev libwayland-dev libxkbcommon-dev \
  wayland-protocols libdecor-0-dev
```

Fedora:

```bash
sudo dnf install gcc-c++ cmake ninja-build pkg-config \
  libX11-devel libXext-devel libXrandr-devel libXfixes-devel \
  libXi-devel libXcursor-devel libXScrnSaver-devel \
  wayland-devel libxkbcommon-devel wayland-protocols-devel libdecor-devel
```

If X11 headers are unavailable:

```bash
cmake -S . -B build/linux-no-x11 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DREVDOCK_ENABLE_X11=OFF
```

The dock will still build and run, but no strut/input-shape integration is
compiled in.

## Asset path

Installed layout:

```text
/usr/local/bin/revdock
/usr/local/share/revdock/assets/fonts/Inter/*.ttf
```

`Paths` checks `<exe>/assets`, `<exe>/../share/revdock/assets`, then the
compile-time source asset directory.

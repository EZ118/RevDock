# Current status

## Verified on this machine

- Windows 10/11, MinGW-w64 GCC 16.2.0, CMake 4.4.4, Ninja 1.13.2
- Clean configure + build from an empty build directory: success
- `ctest`: 5 test cases / 20 assertions, all passing
- Application starts, dock renders, app grid opens, folder stack opens,
  settings window opens, single-instance guard works
- Idle CPU measured at ~0.3–1% of one core after startup; working set ~30 MB
- Installed tree (`cmake --install`) includes executable, assets/fonts, project
  license, third-party licenses and notices

## Visual checks performed

- transparent dock bar at bottom center
- hover magnification, dynamic-width hover label
- context menu with left-aligned text and dock-matching material
- app grid with blurred wallpaper background and Windows shortcut icons
- Downloads folder stack
- `--settings` dialog and live controls
- autohide hides the Dock completely off-screen; only the last screen pixel
  row reveals it, and hovering the empty window area no longer shows it
- single-instance second launch no longer overwrites the config
- no taskbar icon for dock/menu/grid/stack
- drag from app grid to Dock and drag a persistent tile out to remove it
- 1 px separator with equal top/bottom insets; icon gutters match top padding
- click-through outside the visible dock bar/icon area
- shadowless Dock
- dynamic blurred desktop backdrop (Windows capture + blur + tint, ~70 ms
  refresh while the scene changes and 150 ms when it is static)
- live blur follows windows dragged under the Dock without mouse movement
- neutral backdrop colours: captured pixels are stored as RGBA and uploaded as
  `RGBA32`, so the red/blue channels are no longer swapped (this was the source
  of the blue cast on the Dock, Launchpad and popovers)
- popover hairline border composited with its own alpha instead of being baked
  as opaque white
- blur-backed right-click menus and tooltips with centered downward triangle
- Launchpad/Trash tooltips and hover magnification
- blurred wallpaper behind the application grid; Dock remains above it
- Launchpad/Folder artwork and Trash/pileArrow/Open-in-Finder icons come from the
  user-supplied Dock resources (ssets/icons/); no GPL icon collection vendored
- full-screen Launchpad wallpaper blur (including the hidden taskbar strip)
- 7×5 Launchpad grid (the reverse-engineered `springboard-columns`/`springboard-rows` defaults), icon size derived from the smaller cell (20–112 effective, multiple of 8), with room reserved for the top search field and the Dock above the page dots; fade in/out, blank-click dismissal
- first-frame placeholders + budgeted icon loading so Launchpad appears immediately
- Dock anchored to the physical screen bottom (4px inset)
- Windows taskbar hidden while the Dock runs and restored on normal exit
- menu rows tightened (24 px) and separators compacted to 9 px
- the Dock material (blur + tint + 1 px border) is baked into one texture, so
  the border follows the rounded edge exactly, corners included
- the separator is shorter than the icon height and has a 5 px hit cell
- only the separator / Dock-background menu offers "Dock Settings..."; icon
  menus no longer do, and the separator menu opens above the bar
- dragging a tile moves the tile itself (no ghost copy), hides it from its old
  slot, and opens a gap at the drop position; passing over separators works and
  releasing outside restores the order
- dragging from Launchpad previews the drop in the Dock, opens the gap, and
  draws the dragged icon above the Dock bar
- every application icon (Dock, newly added tint, Launchpad) is converted to a
  macOS-style icon: transparent/trim, background analysis (none/solid/complex),
  dominant-colour gradient or flat background, a standard circular-corner
  rounded square (radius = 22.37% of the 80.5% artwork tile) and a baked drop
  shadow. Corner masks use analytic signed-distance coverage, and every icon
  texture is drawn with bilinear filtering, so corners stay smooth when the
  Dock magnifies them. Solid-colour plate icons (most UWP apps, e.g. Settings,
  Terminal, Realtek Audio Console) keep their plate colour but the central
  glyph is lifted, centred and scaled to a macOS-like inset size with clear
  margins from the rounded border instead of being stretched edge to edge
- the Dock starts with a pinned Finder tile (the supplied `finder.png`
  artwork) at the far left, followed by Launchpad; both can be moved but never
  removed/unpinned, and the separator/Trash are fixed. Finder is the only file
  manager icon on Windows/Linux (Explorer/Nautilus/etc. entries are filtered
  from Launchpad and the recent area), shows its running state, and has no
  close/quit action
- Finder and Launchpad artwork is trimmed and normalised into one common box
  so both special tiles have the same visible size and shape
- Launchpad tile uses the supplied `launchpad_dark.icns` / `launchpad_light.icns`
  artwork (`assets/icons/`), picked by theme
- icon conversion follows the updated reference rules: neutral gradients for
  near-white/near-black artwork, tinted background kept for saturated or
  mid-tone artwork, tiny sources and full-bleed squares masked without a new
  background
- unsharp-mask sharpening on the source artwork (recovers detail from 32 px
  shell bitmaps), so lines are crisper at dock sizes
- launch bounce is cancelled as soon as the process/window is detected; the
  one-second running poll no longer waits for an "at rest" bounce phase (which
  could keep an app hopping indefinitely while its running dot was already on)
- UWP running state is read from the window's `PKEY_AppUserModel_ID` property
  store first (ApplicationFrameHost can hide the hosted app's process AUMID),
  then from the process AUMID, so hosted UWP apps get their running dot
- running state also consults every process image path, so console programs
  hosted by another terminal (cmd.exe / PowerShell inside Windows Terminal)
  show their dot and stop bouncing; visible-window detection still supplies
  AUMIDs for packaged apps
- Office 2013/2016 Click-to-Run shortcuts point at Windows Installer icon stubs
  (`wordicon.exe`, `xlicons.exe`, ...). They are resolved to the real
  `WINWORD.EXE`/`EXCEL.EXE`/... through the App Paths registry and launched
  directly, so the running dot appears and the bounce stops; if an app still
  starts under another process name, the first visible window/foreground
  process spawned by the launch is recorded as a persisted process alias
- the Dock running dot and every Launchpad/stack page dot are anti-aliased
  circles baked at the display's device resolution (4-5 px dots stay round
  instead of being a stretched corner mask); rounded rectangles snap to whole
  device pixels and the settled expanded-folder panel is baked as one mask, so
  there is no soft seam or right/bottom "border"
- launching an app from Launchpad adds its tile to the Dock immediately and
  bounces it; the bounce follows the reverse-engineered folded-parabola curve
  (300 ms half period) and always finishes at rest
- Launchpad hides installers/uninstallers (`msiexec`, `uninst*`) and, on Linux,
  non-`Application` entries and shell wrappers
- `.url` shortcuts are scanned too: app-registered protocols (`steam://`,
  `bilibili://`, ...) are listed with their own icons, while `http(s)`/`ftp`/
  `file` URLs are skipped
- folder tiles use the user-supplied macOS folder artwork
  (`folder_empty.png` / `folder_nonempty.png`, both alpha-trimmed to 452x384);
  the non-empty artwork is chosen from the folder's actual contents and both
  are fitted by aspect ratio
- folder stacks open as a **vertical fan** (the reverse-engineered
  `DOCKStackFanLayer::layout` arc): items are stacked one icon apart, higher
  items curve to the right on a 3000 pt arc, and every file name sits in a
  rounded pill to the **left** of its icon; the bottom icon lines up with the
  clicked Dock tile and the top row is an arrow labelled "Open in Finder"
  which opens the folder itself. Folders with more items than fit the screen
  height fall back to the paged grid
- fan icons are the shell's original icons (no macOS squircle conversion, no
  rounded icon plate); they keep their aspect ratio and are fitted to one
  common box so none looks oversized. File names are rotated with the same arc
  angle, placed on the fan tangent so their right edge meets the icon centre,
  centred in a tooltip-style pill with real padding and an 8 px corner radius
  (no stretched capsule). Hovering a fan icon does not enlarge it, and the
  "Open in Finder" arrow is scaled to the same visible size as a file icon
- the fan unfolds upward with a 160 ms animation and **collapses back into the
  tile with a 160 ms fade/arc-close animation** when dismissed (Esc,
  transparent-gap click, outside click)
- folder tiles magnify on hover like every other Dock icon; while the fan is
  open it follows the tile's current position/size as neighbours magnify
- grid stacks use the same captured/desaturated/blurred material as the Dock
  and tooltip, including the 1 px border; the material is refreshed when the
  panel opens or its size/theme changes
- while a folder stack is open its Dock tile switches to the supplied
  `pileArrow` artwork (rounded square with a down chevron), so the active stack
  is visible at a glance
- folders default to **stack mode**: the Dock tile shows up to three of the
  folder's items stacked toward the lower-right, and clicking opens the
  fan/grid. The right-click menu has Display as → Folder / Stack; in Folder
  mode the tile shows the folder artwork and clicking opens the folder in
  Explorer/Files instead of the stack. Empty folders show the empty-folder
  artwork and an empty stack.
- the grid stack uses the same frosted material (8 px blur, radius 9, same
  tint) as the right-click menus, keeps each file's original aspect ratio
  (like the fan), and no longer draws a bright rounded hover plate around the
  icon under the pointer
- with auto-hide on, an open folder stack is treated as part of the Dock:
  the bar stays visible while the pointer is over the Dock or the stack, and
  moving outside both for ~200 ms folds the stack first, after which the Dock
  hides normally. The stack no longer keeps the bar pinned open until the
  user clicks elsewhere.
- fan files can be dragged out: over the Dock the app tracks the pointer
  without reordering, dims the app/folder/Trash target under it, and after
  500 ms activates a running app and flashes its tile twice; releasing opens
  the file with that app, recycles it on Trash, moves it into a folder, or adds
  it at the drop position. The fan folds back down with its 160 ms animation
  before the native OLE drag is handed to the desktop/other apps
- files/folders already pinned on the Dock follow the same rule when dragged:
  they never open a gap or reorder, they dim an app/folder/Trash target, and
  releasing on an app opens the file with it; the dim is a tint of the icon
  artwork itself (no larger square plate)
- dropping an application/shell/UWP `.lnk` or a `.url` shortcut on the Dock
  pins the real application (with the standard rounded-square icon transform),
  including in the empty padding left of the first icon
- Dock order uses two fixed hairlines dividing three regions: Finder/Launchpad
  + pinned apps | recently used apps (all open unpinned apps, padded with
  recently used closed apps to at least three entries) | folders/files,
  minimized-window thumbnails and Trash. The stored separator is no longer
  rendered; the two region separators cannot be moved or removed
- minimized windows get compact Dock tiles (at most 2 px larger than an app
  icon in either direction, window screenshot fitted inside with its original
  aspect, app icon fallback for failed/black captures, app-icon badge flush to
  the bottom-right) between folders and Trash; clicking restores the window,
  right-click can restore or close it
- minimizing a window plays a Genie animation by default: the minimized-window
  tile is inserted immediately (so the adjacent Dock icons move apart and the
  Dock makes room), and a grid mesh is pulled into that tile, not the app icon,
  with the top rows lagging, a mid-animation neck pinch and a short fade.
  `mineffect` can be set to `scale` for the older straight tween; the window is
  captured from the foreground snapshot cache (refreshed ~900 ms) or WM_PRINT
  before minimizing. Minimized tiles are ordered by minimize time so the
  placeholder does not jump when the periodic poll sees it
- restore uses the same Genie in reverse: the minimized tile's screenshot grows
  back into the window's normal rectangle and the real window is restored at
  the end
- auto-hide reveals only from the last physical pixel row/column after a 100 ms
  dwell; the bar is shifted an extra 6 px when hidden so no sliver remains
- the Dock backdrop is sampled at the fully-visible bar position even while
  hidden, so revealing it never flashes a black material; capture keeps the
  exclusion affinity and the GDI DIB surface cached, skips `CAPTUREBLT`, and
  retries every frame while the scene changes (measured ~40 blur updates/s with
  a window moving across the bar; the same-signature back-off only kicks in
  after several unchanged frames)
- the Genie overlay is shown only while an animation runs. A full-screen
  always-on-top sheet that stays visible has to be layered to let clicks reach
  other applications, which proved unreliable; hiding it while idle keeps the
  Dock and the desktop fully clickable. The overlay is still pre-created and
  the first frame is presented synchronously when it appears
- restoring a minimized window removes its tile from the bar immediately and
  the reverse Genie grows from that tile rect to the window's normal rect; the
  real window is restored at the end of the animation
- a normal window close (WM_CLOSE/Alt+F4 on the Dock) runs the full shutdown,
  which also restores the taskbar state, work area and DWM transition settings
- while a Dock context menu is open the magnification envelope is frozen, so
  moving the pointer from the icon into the menu no longer shrinks the icon
- the menu window is owned by the Dock (always above the Launchpad grid), and a
  menu focus loss no longer dismisses it (the grid used to steal focus)
- clicking the Launchpad icon while Launchpad is open closes it
- unpinned running apps in the middle region can be drag-reordered; the recent
  app list is rewritten to match the dropped order
- inserting/removing a minimized-window tile animates the neighbouring tiles
  into their new slots over ~200 ms instead of snapping
- pinned-icon drag reordering inserts at the preview slot index without the
  extra forward decrement (the old code dropped one slot too far left)
- only the Dock keeps `WDA_EXCLUDEFROMCAPTURE` permanently; menus/popovers
  restore their capturability after their one-shot backdrop capture
- Launchpad pages follow a two-finger horizontal touchpad swipe. On Windows the
  input arrives as `SDL_EVENT_MOUSE_WHEEL` with `wheel.x` (WM_MOUSEHWHEEL); the
  handler accumulates the deltas with the same edge resistance as a wallpaper
  drag and snaps after the gesture pauses. Precision touchpads send
  ultra-high-precision deltas (120 events per notch), so deltas move 0.034 page per notch (roughly one page per 8 cm of finger
travel); the visual
  offset follows the accumulator every frame with a ~28 ms time constant, so
  slow swipes glide continuously instead of stepping once per wheel message.
  Wheel gestures use the same 18%-of-screen flip threshold as a wallpaper
  drag, and commit the page as soon as the threshold is crossed (not on
  release), so consecutive swipes page immediately one after another and the
  page dots move with the gesture. At the first/last page the overscroll is
  limited to a 10%-of-screen rubber band instead of a full screen width.
  Overscroll at the first/last page settles only 50 ms after the last wheel
  event and rebounds with a 60 ms ease-out (a below-threshold mid-page swipe
  uses 120 ms / 100 ms), so hitting the edge recovers almost instantly. If the
  driver keeps streaming release/inertia events while the page is held at the
  edge, the gesture is force-settled after 300 ms (long enough to see the
  rubber band) and further outward events
  are swallowed until the user reverses direction or the stream goes quiet for
  250 ms. A fast flick therefore rebounds once and cannot twitch repeatedly in
  the swipe direction or stay stuck in the overscroll position.
  Wheel deltas are inverted so the page moves with the fingers. Devices that
  report `SDL_EVENT_FINGER_*` (touchscreens, or a macOS trackpad with
  `SDL_HINT_TRACKPAD_IS_TOUCH_ONLY`) use the same threshold and immediate page
  commit with two or more fingers, so chained swipes do not wait for the page
  animation. The hint itself is only read by SDL's Cocoa backend, so it is a
  no-op on the Windows/Linux builds.
- on Windows the native DWM minimize/maximize transition is disabled for
  top-level windows while the Dock runs (`DWMWA_TRANSITIONS_FORCEDISABLED`) and
  restored on normal exit; newly opened windows are picked up by a periodic
  refresh. On Linux the minimize effect belongs to the compositor/WM, which has
  no portable per-app switch, so the Dock's overlay draws on top and the
  desktop animation settings are left untouched. The WinEvent callback wakes
  the SDL event loop immediately, the overlay is created at startup, and the
  first Genie frame is presented synchronously, so the animation starts on the
  next frame instead of after the idle timeout
- entering/leaving the Dock animates the magnification envelope over 160 ms
  instead of snapping, including when the pointer moves between the Dock and an
  open folder fan or Launchpad
- the gap between neighbouring tiles is 25% tighter than the outer gutters
- the Dock sits 1 px lower than before; its reserved work area ends exactly at
  the Dock's top edge (0 gap for maximized windows). The “Reserve screen space”
  option is gone: auto-hide off reserves the Dock edge, auto-hide on reserves
  nothing
- while running, the Windows taskbar is temporarily set to auto-hide so its
  work-area reservation disappears (including the maximized-window gap); the
  original taskbar state and work area are restored on exit
- the "Open in Finder" row uses the supplied `openinfinder` circle-arrow
  artwork; the Trash tile uses the supplied light/dark empty/full trash
  resources and switches automatically with the Recycle Bin contents
- the Dock stays above Launchpad through window ownership (set while the grid
  is visible, cleared before it is hidden), so it no longer flickers or
  disappears when the pointer moves between them
- with Launchpad open the Dock uses the same rounded shape/border as the normal
  material, translucent instead of blurred; the Dock is re-raised on grid
  activation so it does not flash
- Launchpad pages swipe horizontally: dragging the wallpaper follows the
  pointer, shows the neighbour page, and settles to the nearest page with a
  cosine-eased 220 ms animation; a plain wallpaper click still dismisses
- wheel/touchpad and arrow keys slide to the neighbouring page with a quick
  160 ms cosine animation (new page enters from the side)
- Launchpad icons are pre-warmed in the background: one icon per 100 ms while
  the Dock is idle (no pointer over it, no menu/stack/settings/Launchpad), so
  the first open draws cached textures instead of extracting ~40 shell icons
  (measured: 40/40 first-page icons cached)
- Dock magnification freezes while a context menu is open and resumes when it
  closes
- Dock context menus follow `DockMenus.plist`: dynamic Turn Hiding On/Off,
  dynamic Turn Magnification On/Off, Position on Screen (Left/Bottom/Right,
  checked), Minimize Using (Genie/Scale, checked), Dock Preferences…, and a
  platform-only Quit RevDock. Submenus open in a separate owned popup with a
  chevron, accent-blue hover, checkmarks and section headers; rows are 24 px.
- Application menus start with application-provided items, then the standard
  process menu: Options (Keep in Dock / Remove from Dock / Open at Login /
  Show in Explorer|Files), Show All Windows, Hide/Hide Others/Show, Quit and
  Force Quit when the app is unresponsive. Trash is disabled when empty;
  folders get Sort By and View Content As submenus.
- Firefox's Dock menu has New Tab, New Window, New Private Window and its
  recent visits (read from the profile's `places.sqlite`, with the URL icon and
  host as secondary text). Visual Studio Code has New Window and its recent
  workspaces (read from `workspaceStorage`, with shell file/folder icons).
  Other Windows apps fall back to recent destinations from their jump list.
- Menu items can show a leading icon, right-aligned detail text and a submenu
  chevron. Tasks launch the app with the profile's command-line arguments
  (`-new-tab`, `-new-window`, `-private-window`, `--new-window`,
  `--reuse-window <path>`); generic recent files open with the application.
  Open at Login toggles a Startup shortcut on Windows or an XDG autostart entry
  on Linux. An opaque panel sits under the frosted capture so menus remain
  readable without screen-capture permission.
- Launchpad page dots are clickable/draggable; the active dot is larger and
  updates as soon as a wheel/touchpad or two-finger gesture commits a page.
  Two consecutive swipes can flip two pages without waiting for the first
  animation.
- Launchpad data is a persisted tree of pages and folders (`launchpad` in
  `config.json`): drag an icon to a new slot (a live gap shows the insertion
  point), across a screen edge after a short dwell, onto another application
  to create a folder, onto/out of a folder, or onto the Dock. Empty folders
  disappear automatically.
- Launchpad folders use a borderless light translucent squircle with the same
  mask and size as an application icon (transparency equals the expanded
  panel), a 3×3 preview and the same faint app-icon shadow. Opening one grows
  that rounded body into a fixed-width, content-height light translucent panel;
  the larger folder title sits above it, is single-click editable in a rounded
  field, and there is no close button — clicking outside the panel folds it
  without closing Launchpad.
- Launchpad drag feedback: the lifted icon is removed from its old cell, leaving
  a live gap; app/folder targets keep that original hole so icons do not rush
  aside until the pointer crosses ~3/4 of the target icon. Hovering another app
  dims it and creates a folder on release, hovering a folder dims it twice and
  then expands it after ~0.7 s (a quick release drops into the folder instead),
  holding a dragged icon outside the expanded folder for 0.5 s folds it, and a
  direct release outside flies the icon back to its original slot. All
  refill/gap/return movement is animated at frame rate (the main loop keeps the
  4 ms animation tick while the grid moves); dropping an app onto a folder
  shrinks it into the folder tile instead of making it vanish; dragging a folder
  never merges folders.
- The Launchpad search field (`ECTextInputLayer`/`SBSearchPage`) filters every
  installed app, ranks exact/prefix/word-prefix/substring matches, shows the
  temporary result pages without the pager dots, starts from any typed text,
  clears with Escape, and opens the first hit with Return. Typing no longer
  steals room from the grid: the first row starts below the field.
- Search and folder-title editing support typing, Backspace/Delete,
  Ctrl+C/X/V clipboard operations and CJK IME input. On Windows the grid window
  captures the IME's `GCS_RESULTSTR` natively, so committed Chinese text arrives
  even when SDL's keyboard focus lags behind the Dock's no-activate activation
  path; SDL's per-character TEXT_INPUT duplicates are suppressed for the same
  commit window, and printable key presses have a delayed fallback when no
  TEXT_INPUT event follows. The expanded folder title uses 44 px text with a
  larger gap above the light panel.
- On Windows, Dock folder stacks and their previews hide `desktop.ini`,
  `Thumbs.db` and `.DS_Store` instead of listing shell bookkeeping files.
- launching an app that is not running bounces its tile until the process is
  detected (20 s safety timeout)
- packaged/UWP apps are enumerated from the virtual `shell:AppsFolder` (not
  only Start Menu `.lnk` files), so Settings, Terminal, Realtek Audio Console,
  Windows Security and other UWP apps appear in Launchpad; they are launched
  and activated by AUMID, and `ActivateForFile` is used when a file is dropped
  on a UWP app tile
- dropping a `.lnk` (including IDList-only UWP shortcuts) on the Dock pins its
  application instead of adding a generic file tile; the AUMID is resolved from
  `System.Link.TargetParsingPath`
- Launchpad lists programs only: shortcuts to documents/URLs/help files are
  skipped, while shortcuts that start a program (with arguments) stay
- Launchpad has no "Applications" title and no bottom hint text; the page dots
  sit above the Dock
- Dock tiles slide to their new slots with an interruptible animation while
  reordering (including Launchpad drag previews); only the preview displacement
  is animated, so magnification and fast pointer sweeps stay pixel-exact
- launch bounce and slide animations invalidate the frame every tick (they used
  to advance only on the ~70 ms backdrop timer, which looked choppy)
- faint shell-icon frames (1 px at ~27% alpha) are ignored, so small logos on
  a 256 px canvas are trimmed and scaled up instead of sitting in a white plate
- non-square artwork is scaled to fit rather than cover-cropped
- an open context menu hides the icon tooltip
- Dock stays visible above Launchpad after the pointer leaves the Dock area
- icon gutters equal to the top inset

- first run without a persisted `launchpad` section builds pages from the
  Start Menu folder tree: only the outermost folder name is used, one-app
  folders stay standalone, and UWP apps are never grouped
- config writes are coalesced: nothing is written while a drag or swipe is in
  progress; once interaction stops the pending change is flushed after a 2 s
  idle window, at most once per 3 s, and on normal exit
- activation first restores an app's hidden/tray window (tiny 16x16 tray
  helper windows are ignored), so QQ-like apps that close to the tray are
  brought back instead of spawning a second instance; apps with no restorable
  window are relaunched through their single-instance handoff
- the Settings window has grouped cards, an accent header with app mark,
  pill toggles, value-labelled sliders, segmented pickers, hover feedback and a
  compact footer

- the Dock's Quit menu item now quits the whole application instead of just
  closing its windows: every top-level window (including hidden tray windows)
  is asked to end the session (WM_QUERYENDSESSION + WM_ENDSESSION) so the app
  can save or veto, with WM_CLOSE as a final graceful fallback.  Nothing is
  terminated from Quit; Force Quit remains the explicit hard kill
## Not yet verified

- Linux X11 runtime (code and CI workflow are present, but no Linux machine in
  the development environment)
- Wayland runtime
- high-DPI (125%/150%) visual calibration
- drag-reorder under all running-app combinations
- work-area reservation under multi-monitor/AppBar combinations
- Windows UWP activation and exotic shell shortcuts

## Known limitations

See `docs/parity.md`. Most importantly:

- no Mission Control/Exposé/Spaces/hot corners,
- folder stacks above the screen-height fan limit use the paged grid,
- primary display only,
- Wayland degrades to a floating panel.

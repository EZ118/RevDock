# Architecture

## Layers

```text
ui/Application, DockWindow, Menu, AppGrid, Stack, Settings
        │
render/Renderer, TextRenderer, IconCache
        │
layout/DockLayout + anim/AnimatedValue
        │
model/DockModel + config/Settings + apps/AppIndex
        │
platform/Platform  (Windows / Linux)
        │
SDL3 + SDL3_ttf + SDL3_image + FreeType
```

## Data flow

1. `Settings` loads `config.json` settings; `DockModel` loads persisted items.
2. `AppIndex` loads `cache/apps.json` or scans the platform application list.
3. `visibleItems_` = persistent items + running, unpinned applications.
4. `DockLayout` computes tile rectangles from Tile-size units, pointer position,
   orientation, and available window length.
5. `Application` updates autohide/moving values, then renders only when dirty or
   animating.
6. Config writes are debounced and atomic; app scans are cached for 24 hours.

## Key implementation choices

- **Logical points**: SDL renderer scale is set from the window display scale;
  layout and hit tests use points, textures are created at pixel size.
- **Fixed dock window**: the window is anchored to the display edge; autohide
  translates content inside the window instead of moving the window every frame.
- **Event-driven loop**: `SDL_WaitEventTimeout` is 4 ms only while dragging or
  animating, otherwise 50 ms. Running-state polling is 1 Hz; cursor polling is
  ~33 Hz and only needed for autohide/edge detection.
- **Input region**: Windows `WM_NCHITTEST` returns `HTTRANSPARENT` outside the
  union of the dock bar and the current icon cells, so the large transparent
  area above the dock does not block clicks.
- **Blur backdrop**: on Windows the dock is temporarily excluded from screen
  capture, the area behind it is captured with `BitBlt`, downscaled with a box
  filter to one third/quarter size, box-blurred, given a rounded alpha mask,
  and drawn under the Dock tint. The GPU upscales the small texture with
  bilinear filtering and the texture is reused between frames. Each frame is
  compared with the previous one; unchanged content skips the blur and upload
  and the capture cadence backs off from 70 ms to 150 ms while the scene is
  static. `SDL_Renderer` exposes no custom shaders, and the remaining cost is
  Windows' screen read-back (~7 ms per `BitBlt`), not the blur (~0.7 ms).
  Menus and tooltips are separate `buildPopoverTexture` captures with a baked
  hairline border and downward triangle. Captured pixels are RGBA and uploaded
  as `SDL_PIXELFORMAT_RGBA32`; using `ARGB8888` swapped the red/blue channels
  and produced the blue cast.
  Linux falls back to a translucent material.
- **Launchpad backdrop**: the wallpaper is pre-scaled/blurred at startup and
  drawn full physical-screen; icon textures are loaded a few per frame after
  the first placeholder frame, so the grid fades in immediately instead of
  blocking on Shell icon extraction. The Dock is raised above the grid.
- **Taskbar hide**: `Shell_TrayWnd`/secondary taskbars are hidden during
  runtime, so display bounds are full-screen. The Dock uses the physical
  bottom edge with a 4px inset; the grid covers the former taskbar strip.
  The taskbar is only touched when its visibility actually changes, so the
  shell is not re-shuffled every two seconds.
- **Dock material**: the blurred capture, tint and the 1 px border are baked
  into a single RGBA texture with a signed-distance rounded mask. The border is
  therefore part of the same shape as the fill (no inset ring), and the texture
  is drawn over the exact bar rect.
- **Autohide**: `dockHideOffset()` slides the bar by its full height plus the
  4 px inset, so nothing remains on screen. Only the last physical pixel row
  (or column) triggers reveal; `mouseInside_` is limited to the bar and the
  magnified icon area. With no hit-test rects the invisible window is
  click-through.
- **Icon conversion**: `IconCache` runs every app icon through the same
  pipeline as the reference web converter: trim transparent padding, classify
  the border band (transparent / solid colour / complex artwork), pick a
  dominant colour and build an HSL gradient (or reuse the solid colour), then
  place the artwork and mask it with a standard rounded rectangle (circular
  corners, radius 22.37% of the artwork tile) occupying 80.5% of the canvas,
  with a baked drop shadow. The mask is computed analytically from the rounded
  box signed distance (smooth 1 px coverage) and textures use bilinear
  filtering, avoiding faceted corners under magnification. For a solid plate
  whose border-connected background is the dominant colour, the plate is kept
  and the remaining central body is drawn at `0.82 * shape`, centred, so UWP
  icons get the same inset-glyph layout as native macOS icons. Documents and other plain files skip this pipeline
  entirely and keep their original shell icon and aspect ratio; Finder and
  Launchpad artwork is trimmed and rescaled into the same normalised box. Near-white and
  near-black plates get neutral light/dark gradients instead of a hue; faint
  1 px frames (alpha < 43%) are ignored when trimming, so a shell icon that
  draws a small logo on a 256 px canvas is cropped to the logo and scaled up.
  For neutral plates, dense artwork that covers more than half of the canvas
  (a console window, a full-bleed square) is kept whole, while thin artwork
  (a page outline, a logo) is lifted onto a gradient. Tiny sources (< 64 px)
  are upscaled as-is. Artwork whose aspect differs by more than 12% is scaled
  to fit instead of being cover-cropped. Shape masks are cached per pixel
  size. Set `mac-icons` to false in the config (or use the Dock menu) to draw
  the original icons.
- **Tile spacing**: the layout subtracts one artwork margin (9.75% of the tile
  size) from the inter-icon gap, so the visible gap matches the visible inset
  between the icon tops and the bar's top edge.
- **Bounce**: the launch bounce uses the reference curve: a folded parabola
  (`amp = 1 - phase²/period²`, 300 ms half period), repeated while the process
  is starting and stopped at a rest phase.
- **Launching tiles**: `launching_` applications are included in the visible
  list immediately, so a Launchpad launch shows its Dock tile (and bounce)
  before the 1 Hz running poll sees the process.
- **Icon sharpening**: the source icon is upscaled to the working size and run
  through a premultiplied unsharp mask, recovering edges from small shell
  bitmaps.
- **Folder stacks**: `ui/StackLayout.h` holds the pure fan geometry (stacking
  distance, 3000 pt arc, `asin` rotation, hit testing) so it is unit-tested.
  The fan has one extra top row for "Open in Finder" (its circle-arrow is
  scaled to the same visible size as a file icon); labels sit to the left of
  the original shell icons on the fan tangent, so their rotated right edge
  meets the icon centre. The window itself has no panel.
  Stack windows do not close on focus loss — an explicit global outside-click
  poll dismisses them, which avoids Windows swallowing the next Dock click
  after a focus change. While a stack is open the Dock tile switches to the
  supplied `pileArrow` artwork (`StackState::itemId`); opening/closing uses a
  160 ms spread/fade animation, and `updateStackAnchor` keeps the window on the
  tile as it magnifies or moves. Labels are rotated with the icon and drawn on
  a tooltip-style pill. Dragging a fan item first runs an internal drag over
  the Dock (dim/flash/activate app icons without reordering); leaving the Dock
  hands off to a native OLE file drag (`Platform::startFileDrag`) for the
  desktop/other applications. Drops back onto the Dock arrive as
  `SDL_EVENT_DROP_FILE` and are routed to an app, the Trash, or insertion.
  File/folder tiles already on the Dock use the same internal hover path and
  never open an insertion gap; only app targets dim/open, folder targets move,
  and the Trash recycles.
- **Folder tile appearance**: `DockItem::stack` (persisted per item, default
  true) selects between the macOS stack and a plain folder tile. Stack tiles
  read the first few directory entries (cached for 1 s) and draw up to three
  shell icons offset toward the lower-right; empty folders use the empty
  folder artwork. `Display as → Folder/Stack` in the folder menu changes the
  persisted value. Clicking a stack opens the fan/grid, while a folder tile
  opens the directory in Explorer/Files. The folder artwork is
  `folder_empty.png` / `folder_nonempty.png`, alpha-trimmed and fitted by
  aspect ratio. Grid stacks share the right-click menu's frosted material
  (8 px blur, radius 9) and fit each shell icon by aspect ratio without a
  hover plate. With auto-hide enabled an open stack is a logical extension of
  the Dock: the bar stays visible while the pointer is over the Dock or the
  stack, and `update()` folds the stack after the pointer has been outside
  both for ~200 ms; only then does `updateAutohide` hide the bar. This avoids
  dragging the stack down with the hidden bar and matches macOS' dismiss-on-
  leave behavior.
- **Auto-hide**: only the last physical screen row/column triggers a reveal,
  and it must be held for 100 ms; the hidden offset includes an extra 6 px so
  the bar is fully off-screen. The Dock samples its backdrop at the final
  visible position, so the slide-in reuses a valid blur instead of a black
  material. The backdrop capture keeps `WDA_EXCLUDEFROMCAPTURE` and the GDI
  DIB/memory DC cached and retries every frame while the scene changes, so a
  moving window updates the blur at roughly 40 fps instead of the old single
  digits.
- **Input safety**: the full-screen Genie sheet is hidden between animations.
  `WS_EX_TRANSPARENT` alone does not make a non-layered top-level window
  click-through across processes, and a layered always-visible sheet was not
  reliable for input, so the overlay is only shown for the duration of a
  minimize/restore animation (and is layered so its transparent pixels never
  take input while shown).
- **Work area**: on Windows the taskbar is switched to `ABS_AUTOHIDE` while the
  Dock runs, which removes its work-area reservation; when auto-hide is off the
  Dock AppBar reserves exactly its bar height + bottom inset, when on it
  reserves nothing. The previous taskbar state and work area are restored on
  exit. The old “reserve screen space” setting was removed.
- **Dock categories**: `rebuildVisibleItems` builds three regions separated by
  two synthesized hairlines: Finder/Launchpad + pinned apps; all open unpinned
  apps in recency order, padded with closed recent apps to at least three
  entries; then folders/files, minimized windows and Trash. The stored
  separator item is ignored for layout. `noteRecentApp` persists recent app
  ids in settings.
  `WindowsPlatform::minimizedWindows` enumerates iconic top-level windows and
  attaches the foreground thumbnail captured on `EVENT_SYSTEM_MINIMIZESTART`.
  Minimized tiles keep a square slot (2 px larger than an app icon) and fit the
  window screenshot inside it, with the owning app's processed icon as a
  bottom-right badge. Finder (leftmost, movable but never removable) and
  Launchpad are special pinned items.
- **Magnification envelope**: `DockLayout::compute` takes a `magEnvelope`
  factor (0..1) driven by a 160 ms `AnimatedValue`, so entering/leaving the Dock
  eases instead of snapping. Grid-stack material uses the same captured blur
  and border as popovers.
- **Window ownership**: while Launchpad is visible the Dock window is given as
  an owned window of the grid (`GWLP_HWNDPARENT`), which keeps it above the
  owner regardless of activation. The owner is cleared before the grid is
  hidden, because Windows hides owned windows together with a hidden owner.
- **Shortcut scanning**: `.lnk` targets are classified as programs/documents;
  `.url` files are parsed for `URL=` and `IconFile=`, and only app-registered
  schemes are kept (web/file schemes are skipped).
- **Minimize animation**: `WindowsPlatform::queueMinimize` captures the window
  from the foreground screenshot cache (or WM_PRINT) when MINIMIZESTART fires;
  `Application::renderMinimizeAnimation` draws the captured texture on a
  full-screen transparent overlay. Genie mode builds a vertex grid
  (`SDL_RenderGeometry`), composes per-row eased progress so the top rows lag,
  pinches the neck toward the tile centre mid-way and fades as it reaches the
  minimized-window tile (the pending tile is injected into `rebuildVisibleItems`
  at minimize start, so the neighbouring icons shift before the animation).
  `mineffect=scale` keeps the simple rectangle tween. Windows also sets
  `DWMWA_TRANSITIONS_FORCEDISABLED` on top-level windows while running and
  restores it in `shutdown`; Linux leaves compositor animation settings to the
  desktop.
- **Context menus**: the popup is a separate window owned by the Dock, so it
  renders above both the Dock and the Launchpad grid. Opening a menu freezes
  the magnification envelope (the pointer leaves the Dock to enter the menu)
  and the menu is not dismissed on focus loss, because the grid takes focus
  while Launchpad is open. The backdrop capture only keeps
  `WDA_EXCLUDEFROMCAPTURE` on the Dock; one-shot captures (menus, stacks)
  restore the window's capturability afterwards. An opaque rounded panel is
  drawn underneath the frosted capture so the menu stays readable if a
  compositor refuses the screen capture.
- **Menu model**: menu entries support separators, non-clickable section
  headers, checkmarks, leading icons, right-aligned detail text and one level of
  submenus (chevron on the right, separate owned popup window). Hover uses the
  macOS accent-blue highlight; separators are 9 px and rows 24 px. The standard
  Dock menu mirrors `DockMenus.plist`: dynamic Turn Hiding On/Off, dynamic Turn
  Magnification On/Off, Position on Screen (Left/Bottom/Right, checked),
  Minimize Using (Genie/Scale, checked), Dock Preferences… and a platform-only
  Quit RevDock.
- **Application-provided menus**: a running application's menu starts with
  `applicationDockMenu:`-style items, then the standard process menu from
  `DockMenus.plist` (Options, Show All Windows, Hide/Hide Others/Show, Quit,
  Force Quit when unresponsive). Options contains Keep in Dock / Remove from
  Dock, Open at Login, and Show in Finder/Explorer/Files. Firefox contributes
  New Tab, New Window and New Private Window plus recent visits from
  `places.sqlite`; Visual Studio Code contributes New Window plus recent
  workspaces from `workspaceStorage`. On Windows, other applications fall back
  to recent destinations from `IApplicationDocumentLists` (the jump list).
- **Application menu actions**: tasks and recent items become command-line
  arguments per application profile (`-new-tab`, `-new-window`,
  `-private-window`, `--new-window`, `--reuse-window path`). Generic recent
  files are opened through the platform's "open with app" path. Open at Login
  creates/removes a Startup-folder shortcut on Windows and an
  `~/.config/autostart/*.desktop` entry on Linux.
- **Launchpad swipe input**: page navigation is driven by the same
  `swipeOffset`/`settleGridSwipe` state for the mouse blank-area drag, the
  horizontal touchpad wheel stream (`wheel.x`) and multi-finger gestures
  (`SDL_EVENT_FINGER_*`, two or more fingers). Wheel ticks are accumulated and
  animated; after a short pause the offset either flips the page or snaps back
  using the same 18%-of-screen threshold for wheel gestures and wallpaper
  drags alike, so the two input methods feel identical; the page is committed
  the moment that threshold is crossed, so consecutive swipes page one after
  another without waiting for the animation or the dots. On Windows precision
  touchpads the OS reports ultra-high-precision pans as WM_MOUSEHWHEEL
  (delta = 1 per event, 120 per notch), so the handler scales deltas by
  0.034 page per notch (about one page per 8 cm), uses the same 18% flip
  threshold as a wallpaper drag, and caps edge overscroll to 10% of the screen. Instead of starting a tween per wheel message (which stepped at
  slow finger speeds), the visual offset follows the accumulator every frame
  with a ~28 ms time constant, so slow swipes glide continuously. No extra SDL
  hint is needed: `SDL_HINT_TRACKPAD_IS_TOUCH_ONLY` is Cocoa-only in SDL 3.4.
- **Layout transitions**: when the visible item set changes, the previous
  screen positions of the surviving tiles are recorded; the next layout frame
  seeds a decaying per-tile offset (200 ms, ease-in-out) so neighbours slide
  into their new slots around an inserted/removed minimized tile. Recent-app
  drag reordering rewrites the `recent-apps` history list in the dropped order.
- **Running detection**: `runningExecutables` enumerates visible windows and
  inserts both the process path and the window's AppUserModelID (window
  property store first, process AUMID as fallback); `runningProcessPaths`
  additionally enumerates every process image through Toolhelp32 so console
  programs hosted by another terminal (cmd.exe under Windows Terminal) count
  as running. `AppIndex::isRunning` matches executables, executable stems,
  desktop ids, AUMIDs and persisted process aliases. Launch bounce is stopped
  on the first poll that sees the app running.
- **Launcher stubs**: shortcut targets under `%WINDIR%\Installer\{...}` whose
  names are Office icon stubs are resolved through the App Paths registry to
  the real executable and launched directly (`AppInfo::launchExec`). If a
  launched app still shows up under a different process name, the first new
  visible window process (or the changed foreground process) within the launch
  watch is stored as an app alias in the application cache.
- **UWP apps**: `scanPackagedApps` enumerates `shell:AppsFolder` and keeps
  AUMID-style entries (`PackageFamilyName!AppId`). `IApplicationActivationManager`
  launches/activates them, and `ActivateForFile` opens dropped files with a
  packaged app. `.lnk` files that resolve to an AUMID through
  `PKEY_Link_TargetParsingPath` are matched back to the app when dropped.
- **Icon preload**: after startup the app feeds the Launchpad icon cache one
  icon per 100 ms, but only while the Dock is idle. Shell extraction costs
  10-20 ms per icon, so spreading it prevents hitches and the first Launchpad
  open finds the first pages fully cached. `--grid-delay <seconds>` opens the
  Launchpad later for testing this path.
- **Launchpad pages**: the reverse-engineered `springboard-columns` /
  `springboard-rows` default to 7x5. Pages are virtual columns; the renderer
  draws the current page plus one neighbour at `pageOffset ± width`. A
  blank-area drag updates `swipeOffset` (with rubber-banding at the first/last
  page), and the release chooses the page by an 18% width threshold and eases
  the offset back with a 220 ms cosine (`easeInOut`). Wheel/touchpad and
  two-finger gestures commit at the same threshold while the gesture is still
  running, so chained swipes page immediately and the dots follow the committed
  page. The page indicator is clickable/draggable (`ECPagerControlLayer`); the
  active dot is larger and opaque. A blank click without motion still closes
  the Launchpad.
- **Launchpad model**: `ui/LaunchpadLayout.h` holds the pure page/folder tree
  and its operations (next-available insertion, overflow cascading,
  create-folder, move in/out, folder reorder, cleanup, JSON round-trip,
  reconcile and search ranking); it is unit-tested without SDL. The UI keeps
  the tree in `GridState::layout` and persists it as `launchpad` in
  `config.json`. On open it reconciles against `AppIndex` so removed apps
  disappear, newly installed apps fill the next free root slot, and folder
  contents stay ordered.
- **Launchpad folders and search**: folder tiles draw the same squircle mask as
  an app icon (`IconCache::shapeTexture`) in a light translucent tint (96, the
  same alpha as the expanded body), a 3x3 mini-icon preview and the same baked
  soft drop shadow as application icons — no plate, border or captured blur. `showGroup:` expands that
  rounded body into a fixed-width panel whose height follows the folder row
  count, with the larger title above the body (single-click to edit in a rounded
  field), its own pager and no close button. The expansion interpolates the
  panel rect from the original tile body; clicking outside folds it. Search is a
  temporary page list driven by `ECTextInputLayer`: any text input filters all
  apps (exact/prefix/word/substring ranking), the dots are hidden while
  searching, Return launches the first hit, Escape clears the query and a second
  Escape closes Launchpad. The grid reserves the search field above and the Dock
  below, so the last row and the page dots stay visible.
  Text editing routes all `KEY_DOWN` events to the grid (the old global Escape
  handler used to swallow Backspace/Delete/clipboard keys), supports Ctrl+C/X/V,
  and uses a delayed printable-key fallback when no TEXT_INPUT arrives. On
  Windows `WindowsPlatform::setTextCommitHandler` subclasses the grid HWND and
  captures `WM_IME_COMPOSITION`/`GCS_RESULTSTR` directly, so CJK commits do not
  depend on SDL's keyboard focus.  SDL may afterwards deliver the same commit as
  one TEXT_INPUT string or one per character; any TEXT_INPUT contained in the
  native result during the 350 ms commit window is ignored, avoiding doubled
  characters. The folder title above the panel uses 44 px text and a 20 px gap.
- **Launchpad drag state**: `ECSBBeginningDragState` / `MoveItemDragState` /
  `ChangePageDragState` / `OpenGroupDragState` map onto `GridState` source and
  target fields. The lifted cell is always removed from the page (never drawn
  twice); for app/folder targets its original hole stays open, so other icons do
  not rush aside. Reordering only starts after the pointer crosses about 3/4 of
  the target icon in the drag direction; inside the middle 50% the tile dims
  (app/folder target) or does not react (folder drag / in-folder reorder).
  `GridState::cellAnims` eases every affected icon from its old to new cell (and
  a pending `returnFrom` position animates a cancelled drag back to its slot).
  Hovering an app dims it and targets folder creation; hovering a folder dims it
  twice and opens it after ~0.7 s (quick release drops into the folder); the
  screen edge flips pages after 550 ms; holding a dragged icon outside the
  expanded folder for 0.5 s folds it (the drag continues), while a direct
  outside release returns it to its original slot. Dropping an app onto a
  folder tile starts `GridState::folderDrop`: the released app icon shrinks from
  the pointer position into the folder body over 240 ms instead of disappearing.
  Dragging a folder never creates/merges folders. Every structural change marks
  the Launchpad config dirty.
- **Grid-state material**: while Launchpad is open the Dock draws a cached
  signed-distance material texture (same radius and border as the baked blur,
  no capture), so the corners match and no capture/exclusion happens.
- **Drag preview**: while dragging, the layout list is replaced by a copy with
  the dragged item removed and an empty slot inserted at the drop index, so the
  other tiles make room. The dragged tile is rendered last, above the bar. The
  same path serves Launchpad drags via `gridDragOverDock_`; the Launchpad stops
  drawing its own copy so the icon is not duplicated behind the Dock.
- **No taskbar icon**: dock/menu/grid/stack use `SDL_WINDOW_UTILITY` plus
  `WS_EX_TOOLWINDOW`; settings is a utility window while open.
- **Taskbar**: on Windows, `Shell_TrayWnd` and secondary taskbars are hidden at
  startup and restored during normal shutdown. A forced kill therefore leaves
  the taskbar hidden until Explorer or the taskbar is shown again.
- **Topmost ordering**: the Dock periodically reasserts `HWND_TOPMOST`, except
  while a context menu/stack/settings window is open; the grid raises the Dock
  afterward. Menus are raised after the Dock so they cannot be covered by it.
- **No tray**: RevDock intentionally does not create a tray icon.

## Layout and animation

The formulas are ported from `reverse/spec/metrics-animation.md`:

- cosine ease: `0.5 - 0.5 * cos(pi * t / d)`
- magnification envelope: bell curve over `tilesize * 3`
- size-change duration: `log(|delta| / 2 + 1) * 42 + 1`
- launch bounce: damped sine (window-based approximation in `bounce_`)
- autohide: progress 0..1 with configurable delay/speed

The current magnification implementation keeps each tile's base order and
scales tiles around the pointer; exact per-edge displacement can be refined
during visual calibration.

## Initial Launchpad layout

When the config has no `launchpad` object, `Layout::buildInitial` receives each
scanned app's outermost Start Menu folder (`AppInfo::menuGroup`, the first
component below the Programs known folder; deeper nesting collapses into it).
Groups with two or more apps become a folder; single-app groups and UWP apps
(whose group is empty) stay standalone. Items are sorted by folder/app name and
paginated with `springboard-columns` × `springboard-rows`. Once the generated
layout is written, the normal `reconcile` rules apply.

## Config persistence

`markConfigDirty` timestamps the last change; `saveConfig` runs from the main
loop but refuses to write while a drag or swipe is in progress. After 2 s
without further changes (and at most once per 3 s) it writes
`config.json.tmp` and atomically replaces `config.json`, so a drag across many
slots produces a single write. `shutdown` forces a final write, so normal exits
never lose the pending layout.

## Window activation

`activateApplication` accepts any top-level window of the app's process with a
real size (at least 80x60 and 12000 px²), skipping owned/tiny helper windows
such as `tray_icon_app`, `Base_PowerMessageWindow` and
`Electron_NotifyIconHostWindow`. A visibly titled window wins; otherwise the
largest hidden window is shown and brought forward (using the Alt
foreground-lock workaround because the Dock is no-activate). Apps with no
restorable window fall through to launching the shortcut, which lets
single-instance apps such as Rayburst reopen their UI.

## Graceful quit

The Dock menu's Quit is an application quit, not a window close.
`WindowsPlatform::quitApplication` collects every top-level window of the app
(including hidden tray windows) and, on a short worker thread, sends
`WM_QUERYENDSESSION` with `ENDSESSION_CLOSEAPP`. A veto, save dialog or
unresponsive window cancels the request. Otherwise `WM_ENDSESSION` is sent so
the app exits itself, followed by `WM_CLOSE` for apps that ignore the session
message. `TerminateProcess` is never used here; the separate Force Quit action
is the only hard kill.

## Settings registry

`Settings` exposes typed getters/setters and stores keys with macOS names
(`tilesize`, `largesize`, `magnification`, `orientation`, `autohide`,
`autohide-delay`, `autohide-time-modifier`, `mineffect`, ...). The settings
dialog is generated from a fixed control layout, and every change immediately
updates the dock and marks the config dirty. This keeps the future settings
dialog and config migration independent from layout code.

## Extension points

- `Platform` interface: application discovery, icons, launching, activation,
  trash, cursor, window hints, work-area reservation.
- Minimized-window tiles and thumbnails are implemented (see Dock categories above).
- Next plugin host can expose a redesigned NSDockTile-like ABI in a separate
  process; it must not run in the dock process.

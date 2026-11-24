# Feature parity

| macOS Dock capability | RevDock | Notes |
|---|---|---|
| Pinned applications | ✅ | Windows `.lnk` / Linux `.desktop`; icons are converted to macOS style |
| Running applications added automatically | ✅ | appended before Trash |
| Running indicator | ✅ | 1 Hz window + process-image poll; crisp device-resolution dot; console apps hosted by another terminal and launcher-stub apps supported |
| Launch / activate | ✅ | restores hidden/tray windows first, then ShellExecuteEx / gio + exec |
| Magnification | 🟡 | bell curve approximation; frozen while a context menu is open |
| Auto-hide + delay + speed | ✅ | fully off-screen; revealed by the last screen pixel row |
| Launch bounce | 🟡 | repeating hop until the process is detected (20 s cap) |
| Labels/tooltips | ✅ | hover label |
| Context menus | 🟡 | self-drawn blur material, downward arrow; `DockMenus.plist`-style Dock/process/folder/trash menus, checkmarks, submenus and icons; Dock Preferences on the separator/background menu |
| Drag reorder | ✅ | the tile itself follows the pointer; preview gap at the drop slot |
| Drag out of Dock | ✅ | release outside removes a persistent item; Finder/Launchpad stay pinned; files/folders can be dropped onto a folder or the Trash |
| Drag from Launchpad to Dock | ✅ | gap preview in the Dock, dragged icon drawn above the bar, insert at the drop slot |
| Drop files/folders onto Dock | ✅ | adds file/folder tiles |
| Folder stacks | 🟡 | stack artwork on the Dock tile (up to 3 items, empty/non-empty folder artwork, Display as Folder/Stack), vertical fan (3000 pt arc, rotated labels, aspect-preserving icons, Open in Finder row, unfold/close animations, `pileArrow` while open), grid fallback with the same frosted material as context menus and aspect-preserving icons (no hover plate) |
| App grid / Launchpad | 🟡 | 7x5 grid (configurable `springboard-columns`/`springboard-rows`), persisted pages/folders, drag reorder with live gap and edge paging, drop-on-app folder creation, borderless translucent 3x3 folder tiles (app-icon squircle, no blur plate), content-height expanded folder body with title above and single-click rename, hover-dim folder expansion, animated gap/refill/cancel return, ranked search field with Return-to-open, clickable/draggable page dots, paging from mouse/touchpad/two-finger gestures, full-screen blurred wallpaper, fade, blank-click close, drag to Dock, right-click add/remove; icons use the macOS-style pipeline with standard rounded corners |
| Launcher background | ✅ | desktop wallpaper blurred, fade transition, Dock kept above |
| Dock always-on-top | ✅ | periodic `HWND_TOPMOST` reassert; menu/stack/settings stay above while open |
| Windows taskbar | ✅ | hidden at startup and restored on normal exit |
| Dock backdrop | ✅ | Windows screen capture + Gaussian blur + light/dark tint; X11 falls back to translucent material |
| Trash | ✅ | open/empty on Windows; open on Linux |
| Recents | ✅ | up to three recent apps persisted in `recent-apps`, plus other open unpinned apps |
| Separators/spacers | 🟡 | separator implemented and not draggable; spacer types reserved |
| Minimized-window tiles | ✅ | compact icon-sized tiles between folders and Trash, window-aspect screenshot fitted inside, bottom-right app badge, app-icon fallback for failed captures, click restore and right-click restore/close |
| Genie minimize animation | ✅ | mesh warp into the app tile (top rows lag, mid-animation pinch, fade); Scale fallback via `mineffect`; restore animation not yet |
| Mission Control / Exposé | ❌ | out of scope for M1 |
| Spaces bar / hot corners | ❌ | out of scope for M1 |
| NSDockTile plugin ABI | ❌ | optional later, separate process |
| Accessibility (UIA/AT-SPI) | ❌ | future platform layer |
| Settings UI | 🟡 | card-based settings window with grouped controls; core dock preferences; application-provided Dock menu items (Firefox/VS Code tasks and recents), Open at Login, Hide/Hide Others/Force Quit |
| i18n | 🟡 | English UI; locale fallback from app names |
| Multi-display | ❌ | primary display only |
| Wayland native layer-shell | ❌ | degraded floating panel |
| Work-area reservation | 🟡 | Windows AppBar/X11 strut; experimental |

Legend: ✅ implemented, 🟡 partial/approximate, ❌ not implemented.

| Application Dock menus | 🟡 | Firefox tasks + recent visits, VS Code New Window + recent workspaces, Windows jump-list fallback, icons/detail text/submenus, Open at Login |

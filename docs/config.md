# Configuration

## Files

`config.json`:

```json
{
  "version": 1,
  "settings": {
    "tilesize": 48.0,
    "largesize": 64.0,
    "magnification": true,
    "orientation": "bottom",
    "autohide": false,
    "autohide-delay": 0.0,
    "autohide-time-modifier": 1.0,
    "launchanim": true,
    "show-process-indicators": true,
    "labels": true,
    "show-recents": true,
    "show-recent-count": 3,
    "mineffect": "genie",
    "static-only": false,
    "showhidden": false,
    "theme": "dark",
    "reduce-motion": false,
    "launchpad-swipe-scale": 0.034,
    "launchpad-swipe-threshold": 0.18,
    "springboard-columns": 7,
    "springboard-rows": 5
  },
  "items": [
    {"type": "finder", "id": "special:finder", "label": "Finder"},
    {"type": "app-grid", "id": "special:app-grid", "label": "Applications"},
    {"type": "separator", "id": "special:separator:1"},
    {"type": "folder", "id": "folder:C:\\Users\\me\\Downloads",
     "path": "C:\\Users\\me\\Downloads", "label": "Downloads", "stack": true,
     "showAs": 1},
    {"type": "trash", "id": "special:trash", "label": "Trash"}
  ]
}
```

Item types: `application`, `folder`, `file`, `url`, `separator`, `app-grid`,
`finder`, `trash`. `finder` and `app-grid` can be reordered but never removed;
`separator` and `trash` are fixed positions. Folder items also have `stack`:
`true` (default) draws up to three stacked contents and opens the fan/grid on
click; `false` draws the folder artwork and opens the directory in the file
manager. `showAs` remains the stack content layout (`0` automatic, `1` fan,
`2` grid).

## macOS key mapping

The following keys keep their original names so that behavior can be compared
directly with `reverse/spec/data-formats.md`:

| Key | Meaning |
|---|---|
| `tilesize` | base icon size, 16–128 in the UI |
| `largesize` | magnification size, up to 192 |
| `magnification` | hover magnification |
| `orientation` | `bottom`, `left`, `right` |
| `autohide` | auto-hide the dock |
| `autohide-delay` | seconds before hiding |
| `autohide-time-modifier` | show/hide speed multiplier |
| `launchanim` | launch bounce |
| `show-process-indicators` | running dots |
| `show-recents` | reserved for recents stack |
| `mineffect` | `genie` (default) or `scale` minimize animation |
| `static-only` | do not append running apps |
| `showhidden` | show dotfiles in folder stacks |
| `labels` | hover labels |
| `theme` | `dark`, `light`, `system` |
| `reduce-motion` | disables bounce/large motion |
| `launchpad-swipe-scale` | touchpad swipe distance per wheel notch, in page widths (default `0.034` ≈ one page per 8 cm) |
| `launchpad-swipe-threshold` | fraction of the screen a wheel swipe must travel before it flips a page (default `0.18`, the same as a wallpaper drag) |
| `springboard-columns` | Launchpad columns per page (default `7`, clamped to 2..20) |
| `springboard-rows` | Launchpad rows per page (default `5`, clamped to 2..20) |

## Write behavior

- Changes mark a dirty flag; writes are deferred while a drag or swipe is in
  progress. After the last change the pending config is flushed once the UI is
  idle for 2 s, with a 3 s minimum interval between writes; a single
  drag/reorder therefore produces one write.
- Normal exit forces a final write. Writes use `config.json.tmp` then replace,
  so a crash cannot leave a partial file.
- Unknown keys inside `settings` are preserved when the file is rewritten.
- Application cache (`cache/apps.json`) is written after a scan and reused for
  24 hours. `Refresh Applications` forces a scan.

## Paths

Windows:

- config: `%APPDATA%\RevDock\config.json`
- cache: `%LOCALAPPDATA%\RevDock\cache\`
- state/logs: `%LOCALAPPDATA%\RevDock\state\`, `%LOCALAPPDATA%\RevDock\logs\`

Linux:

- config: `${XDG_CONFIG_HOME:-~/.config}/revdock/config.json`
- cache: `${XDG_CACHE_HOME:-~/.cache}/revdock/`
- state/logs: `${XDG_STATE_HOME:-~/.local/state}/revdock/`

Portable mode: create a file named `revdock.portable` next to the executable;
all data goes to `<exe>/data/`.

Environment overrides: `REVDOCK_CONFIG_DIR`, `REVDOCK_CACHE_DIR`.


## Launchpad layout

The reverse-engineered Launchpad defaults are 7 columns by 5 rows (35 items
per page). `springboard-columns` and `springboard-rows` are read whenever the
Launchpad opens, so changing them does not require a restart. The icon size is
derived from the smaller cell dimension:

```
icon = clamp(cell - (labelFontSize + 11 + 22), 20, 112)
icon = floor(icon / 8) * 8
```

The usable area starts below the search field and stops above the Dock, so the
last row and the page dots never slide under the bar. The current page is a
slightly larger opaque page dot; the other pages are smaller and translucent.
Clicking or dragging the dots switches pages.

### Persisted pages and folders

The Launchpad tree is stored in the config file next to `settings`/`items`:

```json
"launchpad": {
  "version": 1,
  "pages": [
    {"items": [
      {"type": "app", "app": "C:\\...\\Firefox.lnk"},
      {"type": "folder", "id": "folder-123-0", "title": "Utilities",
       "apps": ["C:\\...\\Terminal.lnk", "C:\\...\\Calculator.lnk"]}
    ]}
  ]
}
```

`pages` is ordered; `items` in a page are ordered. Folder contents are a flat
ordered list and paginate with the same `columns x rows` capacity. On startup
the layout is reconciled with the installed applications: known ids keep their
position, missing apps are appended to the next free slot, stale ids and empty
folders are dropped, and duplicate entries are collapsed.

When no `launchpad` object exists (first run), the layout is generated from the
Start Menu tree instead: apps are grouped by the outermost folder below
`Programs` (deeper nesting collapses into that name), a folder with only one app
is shown as a normal tile, and UWP/packaged apps are never placed in a folder.

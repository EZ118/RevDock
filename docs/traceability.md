# Traceability to the reverse-engineering archive

The reverse-engineering artifacts live under `reverse/`. They are analysis-only;
the runtime does not read them.

| RevDock area | Reverse reference | Key items |
|---|---|---|
| Tile layout | `reverse/spec/metrics-animation.md` §3–4 | `xNow/squeezNow/scaleNow`, baseline, fit-to-display |
| Magnification | `reverse/spec/metrics-animation.md` §5 | `magRange = tilesize * 3`, sine bell, 550 ms envelope |
| Easing | §2 | `0.5 - 0.5*cos(pi*t)` |
| Size animation | §6 | `log(|Δ|/2+1)*42+1` |
| Launch bounce | §9 | 500 ms segments, 0.65 decay |
| Auto-hide | §7 | trigger strip, delay, cosine progress |
| Context commands | `reverse/analysis/12_menu_command_map.md` | 1000–1044, 3001–3007, 6001–6004, 7001–7002 |
| Preference keys | `reverse/spec/data-formats.md` | `tilesize`, `largesize`, `autohide*`, `mineffect` |
| App grid geometry | `reverse/analysis/subsystems/launchpad.md` | 7×5, icon size formula, 8 px rounding |
| Folder fan formula | `reverse/analysis/subsystems/stacks.md` | `R=3000`, `atan`, 0.15 s (future) |
| NSDockTile ABI | `reverse/analysis/11_public_api_nsdocktile.md` | future plugin host |
| Windows/Linux mapping | `reverse/spec/porting-notes.md` | AppBar/strut, icon providers, DnD |

## Intentional deviations

- Magnification uses a centered bell curve over base tile centers instead of
  the exact per-edge displacement; this keeps the dock centered while the
  pointer moves and avoids unstable total widths. Parameters remain adjustable.
- Folder stacks currently use a grid popup; the reversed fan `R=3000` formula is
  recorded for the next stack-animation pass.
- Running apps are appended before Trash rather than inserted in Apple’s exact
  “pinned / running / others” order.
- Window thumbnails and Genie/Scale/Suck are deferred; no platform API can
  reproduce `CGSSetWindowWarp` directly.

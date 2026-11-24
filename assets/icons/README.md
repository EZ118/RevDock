# Icon artwork sources

- `launchpad_search_bg.png`, `launchpad_search_loupe.png`,
  `launchpad_search_close.png`: the Launchpad search field, loupe and clear
  button, exported from Dock.app's `ectl_search_*` resources.
- `folder_empty.png`, `folder_nonempty.png`: the user-supplied macOS folder
  artwork (empty / containing files), alpha-trimmed to 452x384. Used for Dock
  folder tiles and folder entries inside stacks.
- `launchpad_dark.icns`, `launchpad_light.icns`: user-supplied Launchpad artwork.
- `trash_empty_light.png`, `trash_empty_dark.png`, `trash_full_light.png`,
  `trash_full_dark.png`: copied from the user-supplied
  `samples/Dock.app/Contents/Resources` (`trashempty@2x`, `trashempty2@2x`,
  `trashfull@2x`, `trashfull2@2x`).
- `open_in_finder.png`: copied from the user-supplied Dock resources
  (`openinfinder@2x.png`) and used for the stack's "Open in Finder" row.
- `pile_arrow.png`: copied from the user-supplied Dock resources
  (`pileArrow@2x.png`) and used as the Dock folder's open-stack state.

The macOS application icons rendered in the Dock/Launchpad are extracted from
the user's installed applications at runtime; no Apple application code is
used.

- `url.png`: copied from the user-supplied Dock resources (`url@2x.png`) and used as the favicon placeholder for recent web pages in the application Dock menu.

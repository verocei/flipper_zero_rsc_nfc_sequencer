# RSC NFC Sequencer

Flipper Zero app that cycles through saved `.nfc` tags and emulates each one
for a settable interval — for testing a UID-based access-control reader
unattended.

Runs on **stock (OFW)** and **Momentum** firmware (shared OFW NFC API only).

## Build

```bash
# one-time: install the micro Flipper Build Tool
python3 -m pip install --upgrade ufbt

# in this project folder:
ufbt            # builds dist/rsc_nfc_sequencer.fap
ufbt launch     # builds, uploads to a connected Flipper, and runs it
```

By default `ufbt` targets the **stock** SDK. To build against Momentum's SDK:

```bash
ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json
ufbt
```

**Windows: no `!` in the project path.** ufbt splits the compiler command line
with `mslex`, which rejects `!` as a CMD metacharacter, so a path like
`C:\!projects\...` fails with `MSLexError: Unquoted CMD metacharacters`. Build
through a junction whose path has no `!`:

```powershell
New-Item -ItemType Junction -Path $HOME\flipper -Target "C:\!projects\flipper"
cd $HOME\flipper\RSC-NFC-Sequencer
ufbt
```

The `.fap` is firmware-API-version specific, so build once per target and
copy the matching `.fap` to each device (`/ext/apps/NFC/`).

## Set up the tag library

1. On the Flipper, read each card with the built-in **NFC** app and **Save**.
2. Saved files land in `/ext/nfc/`. Move or copy the ones you want to cycle
   into:

   ```
   /ext/apps_data/rsc_nfc_sequencer/tags/
   ```

   (the app creates this folder on first run). Up to 64 `.nfc` files.

## Use

The app opens on a menu (the app name is the menu header):

- **Start** — begin emulating; the screen shows the current tag name (large)
  and its UID, the mode/delay/position, and a countdown. **OK** or **Back**
  stops and returns to the menu.
- **Settings** — a scrolling list; **←/→** changes a value, **Back** saves and
  returns. Settings persist across launches.
  - **Mode**: SEQ (directory order) · ABC (alphabetical) · RND (random each step)
  - **Delay**: 1–60 s per tag
- **About** — short info.

The blue LED blinks on every tag change.

## Scope / known limits

- Built and intended for **UID-based** readers. The Flipper emulates the UID
  of any saved ISO14443-A tag faithfully.
- Full MIFARE Classic *content* emulation (sectors/keys) is reader-dependent
  on the Flipper hardware and is **not** what this app is for — a failed
  unlock there may be the Flipper's timing, not your reader or this app.
- `.nfc` files that fail to load are skipped and shown with `!!` on screen.

## Roadmap ideas

- Saved named sequences (ordered playlists), not just whole-folder cycling
- Per-tag dwell time
- On-device add/remove tags
- Shuffle-without-repeat for RND
- CSV log of what was emulated when (pairs well with reader-side logs)

## License

[MIT](LICENSE) © 2026 Athos / RSC

You are free to use, copy, modify and redistribute this app, including in
your own projects and firmware bundles, as long as the copyright notice and
the license text are kept with it. It comes with no warranty.

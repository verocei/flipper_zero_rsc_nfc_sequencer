# RSC NFC Sequencer

Flipper Zero app that cycles through saved `.nfc` tags and emulates each one
for a settable interval — for testing a UID-based access-control reader
unattended.

Runs on **stock (OFW)** and **Momentum** firmware (shared OFW NFC API only).

> Magyar használati útmutató: [HASZNALAT.md](HASZNALAT.md).

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

   (the app creates this folder on first run). Up to 128 `.nfc` files.

## Use

The app opens on a menu (the app name is the menu header):

- **Start** — begin emulating; the screen shows the current tag name (large)
  and its UID, plus a counter. **OK** or **Back** stops and returns to the menu.
- **Settings** — a scrolling list; **←/→** changes a value, **Back** saves and
  returns. Settings persist across launches.
- **Generate tags** — create random test tags (see below).
- **Delete generated** — remove every `GEN-*.nfc` from the app folder (asks first).
- **About** — short info.

### Settings

- **Order**: SEQ (directory order) · ABC (alphabetical) · RND (random each step)
- **Timing** — each code is presented for the **Hold** time (field on), then the
  field drops, so a reader sees each code once (like a tap) instead of reading the
  same one repeatedly.
  - **Fixed** — a code every **Delay** seconds (1–60); **Delay** is the time
    between taps.
  - **Burst** — **Codes/period** codes per **Period**, at random moments, but
    never closer together than **Min gap**.
- **Hold** — how long each code is transmitted (0.1–1 s). Both modes.
- **Total** — stop after N emissions (Off = run forever).
- **Source** — ←/→ selects where tags come from:
  - **App** — the app's own `tags/` folder (your own / sample tags).
  - **Generated** — the `tags/generated/` subfolder (the `GEN-*.nfc` files).
  - a **folder name** — a folder you picked. Press **OK** on this row to open the
    file browser; the chosen file's folder becomes the custom source.
- **LED** — Off, or Red / Green / Blue / Yellow / Cyan / Magenta / White. The LED
  stays lit for as long as the tag is being emulated (continuously in Fixed mode,
  during each brief presentation in Burst mode), and turns red if a tag fails to
  load. (A steady LED while on USB is the charge indicator, not this app.)
- **Vibro** — Off / On: a short buzz on each emission.
- **Beep** — Off / On: a short beep on each emission (lower tone on a load error).

## Generate tags

**Menu → Generate tags.**

- **↑ / ↓** set the count (tap +1, hold +5, longer hold +10, up to 100).
- **← / →** choose **Data** — what to write into each tag (see below).
- **OK** makes them (for *Text* / *URL* it first opens an on-screen keyboard).

The app writes `GEN-1.nfc`, `GEN-2.nfc`, … into the **`tags/generated/`**
subfolder, continuing the numbering so nothing is overwritten, shows a
confirmation, and sets **Source → Generated**. **Delete generated** removes them
again. Generation and deletion only ever touch the app's own folder, never a
folder you picked under **Source**.

### Data (what a reader sees)

- **Off** — a plain **UID-only** tag (random 4-byte ISO14443-3A). A reader sees
  only the UID. This is the classic access-control test tag.
- **Name** — the tag also carries an **NDEF text record** equal to its own name
  (`GEN-1`, `GEN-2`, …).
- **Text** — an NDEF text record with a text you type.
- **URL** — an NDEF URI record with a URL you type (a phone offers to open it).

For **Name / Text / URL** the generated tag is a **Mifare Classic 1K** with the
NDEF data in its sectors (MAD + standard NDEF keys). Mifare Classic is used on
purpose: the Flipper emulates it reliably for phones, whereas its NTAG/Ultralight
emulation often gives a phone only the UID. An NDEF reader app (NFC Tools,
TagInfo) shows the record; a phone's **default** handler usually pops up only for
a **URL** (URI record), not for a bare text record, so pick **URL** if you want
the phone to react on its own. Either way a **UID-based access reader still only
reads the UID** — the NDEF data does not change what it sees. The data is
embedded only in newly generated tags, not in saved tags you copied in yourself.

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

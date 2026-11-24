# CDJ-850 Firmware Patcher

[Open the patcher](https://fruitylemonade.github.io/cdj-850-mods/)

Custom firmware for the Pioneer CDJ-850. Pick the mods you want in the browser and download a ready-to-install update file.

> [!WARNING]
> Custom firmware can cause playback problems or leave the deck unable to boot. The INFO rating display, BPM lock, Phase meter, Zoomed scrolling waveform and Browse history &amp; sort have been tested in the emulator and still need verification on a physical deck. Keep the original update for recovery.

## How to use it

1. Get the original Pioneer CDJ-850 **v1.13** update file, `C850MAIN.UPD`.
2. Open the [patcher](https://fruitylemonade.github.io/cdj-850-mods/) and choose that file.
3. Tick the mods you want (see the table below).
4. Enter a version number in `D.DD` format. Use `0.13` if you want to reinstall modded builds with the same label and restore stock directly from the original `1.13` file. The patcher defaults to `1.14`. The USB updater skips a matching `1.xx` version.
5. Download `C850MAIN.UPD` and copy it to the root of a FAT32 USB stick. Power on the deck while holding **USB SELECT + RELOOP**. Insert the stick when prompted. Once the display shows **COMPLETE!**, turn the deck off and back on.

Your firmware file never leaves your computer. All the patching happens in the browser.

## Available mods

All mods are off until you tick them, and you can combine them freely.

| Mod | What it does | How to use it on the deck |
|---|---|---|
| **USB WIDE tempo** | Gives you ±100% tempo for USB tracks. | Press **TEMPO** to step through ±6%, ±10%, ±16% and **WIDE**, the same cycle as on a CD. |
| **USB FLAC playback** | Plays stereo FLAC from USB at 16 or 24 bits and 44.1 or 48 kHz. The deck resamples 48 kHz audio to 44.1 kHz and uses the upper 16 bits of 24-bit audio, as it does for WAV. | Load and seek through a FLAC track as you would any other track. |
| **Hot cues** *unfinished* | Turns **BEAT 1**, **2** and **4** into hot cues A, B and C. Reads the hot cues you set in rekordbox, and saves new ones back to the USB stick. The beat LEDs show which slots are set (lit) and which are empty (blinking). With **Quantized loops** on, a new cue lands on the nearest beat. | Press **BEAT SELECT + RELOOP** to switch hot-cue mode on or off (the display shows HOT CUE ON/OFF, and the setting is saved). Press a pad to jump to its cue, or to set one if the slot is empty. Hold **BEAT 8** and press a pad to delete its cue. *Not ready for live use yet: a jump takes a few seconds, because the deck first reads about 5 seconds of audio from the stick (the same wait as calling a memory cue).* |
| **Quantized loops** | Makes loops snap to the nearest beat of the rekordbox grid, behind or ahead: auto beat loops, LOOP IN and LOOP OUT. Loops stay in time however many times they repeat. Needs USB tracks analysed in rekordbox. | Press **BEAT SELECT + LOOP IN** to switch quantize on or off (the display shows QUANTIZE ON/OFF, and the setting is saved). |
| **Loop resize** | Halves or doubles a playing loop, following the beat grid when the track has one. | While a loop plays, press **CUE/LOOP CALL ◀** to halve it or **CALL ▶** to double it. Outside a loop the buttons work as normal. |
| **BPM up to 999** | Raises the BPM limit from 180 to 999, for both rekordbox beat grids and the BPM display. Useful for very fast genres. | Nothing to do. Fast tracks just show and play with their real BPM. |
| **BPM decimals** | Shows the actual BPM to two decimal places, for example `132.25`, in place of the rekordbox BPM. | Open the **INFO** view on a track with a beat grid. |
| **BPM lock** | Enables the CDJ-350's BPM Lock across a ±100% range, matching loaded tracks to a chosen Master BPM and auto-switching to **WIDE** past ±16%. Shows Master/locked BPM on the 7-segment display and status flashes on the dot matrix. | Hold **BEAT SELECT + MASTER TEMPO** for 1 s to enter Master BPM edit mode and set the BPM with the rotary selector (press **BACK** or wait 10 s to exit). Short-press **BEAT SELECT + MASTER TEMPO** to toggle lock on or off. |
| **Phase meter** | Displays the current beat in the bar (`1/4`–`4/4`), playing BPM, 4-beat boxes and a sub-beat phase indicator on **INFO** Page 2, shifting subsequent **INFO** pages down by one page. Works with BPM Lock on or off. | Press **INFO** on a USB track and turn the rotary selector one page down to view Page 2. |
| **INFO screen order** | Shows the playing BPM, Camelot key and rekordbox rating under the USB track title (for example `128.23 12A *****`). One asterisk represents one star. With **MASTER TEMPO** off, the key follows the pitch to the nearest semitone. With **BPM decimals** enabled and a beat grid available, the playing BPM follows the current beat's tempo. | Press **INFO** on a USB track with BPM metadata. The first page shows the title, the combined row and the waveform. Turn the jog for Artist, Rating and Key, then Album and Comment. |
| **Zoomed scrolling waveform** | Streams a live 50 Hz zoomed scrolling half-waveform in the 100×5 **INFO** Page 1 strip while keeping rows 1–2 untouched (compatible with **INFO screen order** and **Phase meter**). Samples the 32-second PCM ring buffer and falls back to the 400-column ANLZ `PWAV` buffer or 100-column overview outside the buffered window; works on rekordbox USB, non-rekordbox USB, and Audio CD tracks. | On **INFO** Page 1 (top cursor), turn the rotary selector counterclockwise to zoom out through three levels: close (4 s) → medium (16 s) → whole-track overview. Turn clockwise to zoom back in before scrolling to Page 2. Hold **MENU/UTILITY** and turn past **VERSION No.** to **WAVE PLAYHEAD**, then press the selector to toggle **CENTER** (default) vs **LEFT** (saved with UTILITY settings unless the HISTORY name exceeds 30 characters). |
| **Browse history &amp; sort** | Prefixes played rekordbox USB tracks with `*` in **BROWSE** after more than 20 seconds of playback (tracking up to 1,024 track IDs per mount across pauses, loops and reloads; cleared on USB stop, dismount or power-off), and lets you sort the **TRACK** list by BPM or key. | Play a track for >20 s to mark it. Tap **MENU** in **BROWSE** to cycle through **BPM**, **Key** and **Default** (the display briefly shows the chosen order), then open the **TRACK** category to apply it. |
| **20-minute auto standby** | Puts the deck into standby after 20 minutes without use, instead of 4 hours. | Turn on **AUTO STANDBY** in the **UTILITY** menu. |

## Going back to stock firmware

Install the original `C850MAIN.UPD` (v1.13) using the USB update procedure above. If the deck already shows `1.13` for a modded build, the updater will skip the original file. Open the patcher with the original v1.13 file, leave every mod unticked, and export with a different label such as `1.14`. Repacking with zero mods preserves the original firmware and changes the version label.

## How it works

The browser checks the SHA-256 of your file to make sure it is the original v1.13 update. It then applies the patches in a Web Worker and offers the result as a download. The worker loads a pinned Pyodide runtime from jsDelivr and compresses the firmware with a JavaScript port of the reference LZSS encoder.

## Development

The mods are developed in the [CDJ-850 emulator project](https://github.com/FruityLemonade/cdj-850-emulator). Go there to write a new mod, change an existing one, test in the emulator or report a mod bug. This repository only packages those mods into the browser patcher.

### Building the patcher site

Install the [CDJ-850 SH-2A toolchain](https://github.com/FruityLemonade/cdj-850-emulator/blob/main/scripts/build/sh_elf_toolchain.sh). Put the `sh-elf-*` tools on your PATH or point `CDJ850_SH_ELF` at the toolchain prefix, for example `/opt/sh-elf/bin/sh-elf-`.

```sh
python3 sync_vendor.py ../cdj-850-emulator   # optional: take the mods from a local emulator checkout
python3 build.py
python3 -m http.server 8000 --directory dist
```

Open `http://localhost:8000/`. The build compiles the selectable combinations of mod payloads using the existing compiler driver. It needs no firmware file. GitHub Pages serves the static build from the `gh-pages` branch.

The tools and patch definitions in `vendor/` are copies from the [CDJ-850 emulator project](https://github.com/FruityLemonade/cdj-850-emulator); `vendor/UPSTREAM` names the emulator commit. The [pages workflow](.github/workflows/pages.yml) checks the emulator's `main` branch every hour, commits any change to `vendor/`, rebuilds the payloads and publishes the site, so the patcher always ships the newest mods. A new CDJ-850 mod in the emulator shows up as a workflow warning until it is added to `OPTIONS` in `patcher.py`, the browser adapter. See [LICENSE](LICENSE).

### Enabling the repack counter

The footer total counts each verified firmware repack, including stock repacks. It starts at zero and increments when the download is ready. The browser sends an empty request without cookies or firmware data. The total is an estimate because the service receives reports from the browser.

1. From the repository root, deploy the counter with `npx wrangler deploy --config counter/wrangler.toml` using a Cloudflare account.
2. Set the GitHub Actions repository variable `PATCH_COUNTER_URL` to the deployed Worker URL followed by `/patches`.
3. Run the `pages` workflow on `main` to publish the site with the configured endpoint.

For local development, run `npx wrangler dev --config counter/wrangler.toml --var ALLOWED_ORIGIN:http://localhost:8000` and build the site with `PATCH_COUNTER_URL=http://localhost:8787/patches`. Builds without the variable leave the counter off.

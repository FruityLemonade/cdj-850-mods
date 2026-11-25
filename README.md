# CDJ-850 Firmware Patcher

[Open the patcher](https://fruitylemonade.github.io/cdj-850-mods/)

Custom firmware for the Pioneer CDJ-850. Choose a build in the browser and download a ready-to-install update file.

> [!WARNING]
> Custom firmware can cause playback problems or leave the deck unable to boot. Every mod has been tested on a CDJ-850; Hot cues, the zoomed waveform and Loop roll still have minor known issues and are only in the Experimental build. Keep the original update for recovery.

## How to use it

1. Get the original Pioneer CDJ-850 **v1.13** update file, `C850MAIN.UPD`.
2. Open the [patcher](https://fruitylemonade.github.io/cdj-850-mods/) and choose that file. The patcher checks that it is the unmodified v1.13 update.
3. Choose a build: **Standard**, **Experimental** or **Restore stock** (see below).
4. Enter an output version in `D.DD` format. The default is `1.14`. The USB updater skips a file whose version matches the one already installed, so give each new install a different number. Use `0.13` if you want to reinstall modded builds under the same label and restore stock straight from the original `1.13` file.
5. Download `C850MAIN.UPD` and copy it to the root of a FAT32 USB stick. Power on the deck while holding **USB SELECT + RELOOP**. Insert the stick when prompted. Once the display shows **COMPLETE!**, turn the deck off and back on.

Your firmware file never leaves your computer. All the patching happens in the browser.

## Builds

| Build | What you get |
|---|---|
| **Standard** | Every mod tested on a CDJ-850 with no known issues. The core mods are always on; the optional ones are switched on or off on the deck. |
| **Experimental** | Everything in Standard, plus Hot cues, the zoomed scrolling waveform and Slip mode. They are hardware tested too, but still have minor known issues. |
| **Restore stock** | The unmodified Pioneer v1.13 firmware with a new version number, to roll back over a modded build. |

### Switching mods on the deck

All optional mods appear in the **UTILITY** menu after **VERSION No.** Hold **MENU/UTILITY**, turn the rotary selector to the item and press it to change the value. Optional mods start **ON**.

Only **UTILITY** menu settings are saved to `PIONEER/MODSET.DAT` on the USB stick (or flash), so they travel with the stick. **BEAT SELECT** performance combos (Quantize, Hot cues, Slip mode) toggle only in RAM without writing to the USB stick mid-playback; setting a mod to **OFF** in **UTILITY** disables it and its combo cannot arm it.

## Mods

### Always on (Standard and Experimental)

| Mod | What it does | How to use it on the deck |
|---|---|---|
| **Version display** | **VERSION No.** shows the custom version followed by the base firmware, for example `Ver. 1.14 (1.13)`. | Open **UTILITY** and turn to **VERSION No.** |
| **BPM up to 999** | Raises the BPM limit from 180 to 999, for both rekordbox beat grids and the BPM display. Useful for very fast genres. | Nothing to do. Fast tracks show and play with their real BPM. |
| **USB FLAC playback** | Plays stereo FLAC from USB at 16 or 24 bits and 44.1 or 48 kHz. The deck resamples 48 kHz audio to 44.1 kHz and uses the upper 16 bits of 24-bit audio, as it does for WAV. | Load and seek through a FLAC track as you would any other track. |
| **USB WIDE tempo** | Gives you ±100% tempo for USB tracks. | Press **TEMPO** to step through ±6%, ±10%, ±16% and **WIDE**, the same cycle as on a CD. |
| **Loop resize** | Halves or doubles a playing loop, following the beat grid when the track has one. Doubling (`CALL ▶`) and first-half halving continue smoothly without jumping back to Loop-In; halving past the new out wraps immediately to Loop-In. | While a loop plays, press **CUE/LOOP CALL ◀** to halve it or **CALL ▶** to double it. Outside a loop the buttons work as normal. |
| **Auto standby** | Lets you choose a standby timeout of 20, 40, 60 or 120 minutes instead of 4 hours. Also works on USB with a stick mounted (stock firmware never goes to standby there): idle time counts while browsing, paused or cued, and any key, rotary or tempo fader move restarts it. Playing or looping a track keeps the deck awake. | Turn on **AUTO STANDBY** in **UTILITY**, then set **STANDBY TIME** (default 20 MIN). |

### Switchable (Standard and Experimental)

| Mod | UTILITY item | What it does | How to use it on the deck |
|---|---|---|---|
| **Quantized loops** | **QUANTIZE**, **QUANTIZE SIZE** | Makes auto beat loops, LOOP IN and LOOP OUT snap to the nearest quantize point of the rekordbox grid, behind or ahead. Loops wrap on the deck's 1.9 ms audio blocks taken straight from the grid, so every pass is within a block of the grid length and loops never drift, however many times they repeat. RELOOP, memory and hot-cue loops get the same. Needs USB tracks analysed in rekordbox. | Enable or disable **QUANTIZE** (`ON`/`OFF`, saved) and choose **1 BEAT**, **1/2 BEAT** or **1/4 BEAT** in **UTILITY > QUANTIZE SIZE**. Temporarily arm or disarm quantize in RAM with **BEAT SELECT + LOOP IN** (`QUANTIZE ON`/`OFF`). |
| **BPM decimals** | **BPM DECIMALS** | Shows the actual BPM to two decimal places, for example `132.25`, in place of the rekordbox BPM. | Open the **INFO** view on a track with a beat grid. |
| **BPM lock** | **BPM LOCK** | Brings the CDJ-350's BPM Lock across a ±100% range: loaded tracks match a chosen Master BPM, switching to **WIDE** past ±16%. Shows the Master or locked BPM on the 7-segment display and status messages on the dot matrix. | Hold **BEAT SELECT + MASTER TEMPO** for 1 s to edit the Master BPM with the rotary selector (`MASTER XXX.X` stays up until confirmed/exited with **ENTER** or **BACK**). Short-press **BEAT SELECT + MASTER TEMPO** to switch lock on or off. |
| **Phase meter** | **PHASE METER** | Shows the beat in the bar (`1/4`–`4/4`), the playing BPM, four beat boxes and a sub-beat phase indicator on **INFO** page 2. Later **INFO** pages move down one page. Works with BPM lock on or off. | Press **INFO** on a USB track and turn the rotary selector one page down. |
| **INFO screen order** | **INFO ORDER** | Shows the playing BPM, Camelot key and rekordbox rating under the USB track title, for example `128.23 12A *****` (one asterisk per star). With **MASTER TEMPO** off, the key follows the pitch to the nearest semitone. | Press **INFO** on a USB track. The first page shows the title, the combined row and the waveform. Turn the jog for Artist, Rating and Key, then Album and Comment. |
| **Browse history &amp; sort** | **BROWSE MODS** | Marks rekordbox USB tracks played for more than 20 seconds with `*` in **BROWSE** (up to 1,024 tracks per stick, cleared when the stick is stopped or removed, or the deck is switched off), and sorts the **TRACK** list by BPM or key. | Tap **MENU** in **BROWSE** to cycle **BPM**, **Key** and **Default**, then open the **TRACK** category. |

### Minor known issues (Experimental only)

| Mod | UTILITY item | What it does | How to use it on the deck |
|---|---|---|---|
| **Hot cues** | **HOT CUES** | Turns **BEAT 1**, **2** and **4** into hot cues A, B and C. Reads the hot cues set in rekordbox and saves new ones to the USB stick. Lit beat LEDs mark set slots, blinking ones empty slots, and **BEAT 8** blinks while active. Triggering a hot cue auto-plays immediately even from pause or cue. With quantize on, a new cue lands on the nearest beat. Jumps are instant from anywhere in the track, even on the first press: after a track loads, the first 2 seconds of each hot cue are decoded into memory in the background while it plays. Until a cue is ready (the first seconds after loading) a jump prefills 2.4 s instead of 5.3 s. | Enable in **UTILITY > HOT CUES**. Press **BEAT SELECT + RELOOP** to arm or disarm hot cues in RAM (`HOT CUE ON`/`OFF`). Press a pad to jump and play, or set an empty slot. Hold **BEAT 8** and press a pad to delete its cue. |
| **Zoomed scrolling waveform** | **WAVEFORM**, **WAVE PLAYHEAD** | Replaces the **INFO** page 1 waveform strip with a live zoomed scrolling waveform. In close/medium zoom, clears the whole-track bottom cue bar to show only the zoomed active cue, and draws memory cues/loops, hot cues A–C, pending Loop-In red markers, and active loops on the top red strip. | On **INFO** page 1, turn the rotary selector counter-clockwise to zoom out: close (4 s), medium (16 s), whole track. Turn clockwise to zoom back in. **WAVE PLAYHEAD** places the playhead at **LEFT 1/3** (default), **CENTER** or **LEFT**. |
| **Slip mode** | **SLIP MODE** | Keeps a shadow playhead running at the tempo-fader speed during held beat-button loop rolls, **REV** reverse, **VINYL** scratch/backspin, and pause, then jumps back to where the track would have been (or the next quantize point with quantize on). The jog **VINYL** indicator blinks while slip mode is active. Holding **REV** for 1.5 s triggers momentary slip-reverse until released even when slip mode isn't armed; normal **REV** presses stay stock unless armed. | Enable in **UTILITY > SLIP MODE**. Press **BEAT SELECT + LOOP OUT** to arm/disarm slip mode in RAM (`SLIP ON`/`SLIP OFF`). Hold **BEAT 1**, **2**, **4** or **8** to roll, hold **REV** >= 1.5 s for momentary slip-reverse, or use **REV**, jog touch (**VINYL**), or **PLAY/PAUSE** while armed. |

## Going back to stock firmware

Choose **Restore stock** in the patcher with the original v1.13 file and export it with a version number the deck does not already show, such as `1.15`, then install it with the USB update procedure above. If the deck shows `0.13`, you can instead install the original `C850MAIN.UPD` directly.

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

Open `http://localhost:8000/`. The build compiles the mod payloads for each build using the existing compiler driver. It needs no firmware file. GitHub Pages serves the static build from the `gh-pages` branch.

The tools and patch definitions in `vendor/` are copies from the [CDJ-850 emulator project](https://github.com/FruityLemonade/cdj-850-emulator); `vendor/UPSTREAM` names the emulator commit. The [pages workflow](.github/workflows/pages.yml) checks the emulator's `main` branch every hour, commits any change to `vendor/`, rebuilds the payloads and publishes the site, so the patcher always ships the newest mods. A new CDJ-850 mod in the emulator shows up as a workflow warning until it is added to one of the build lists (`ALWAYS_ON`, `SWITCHABLE` or `KNOWN_ISSUES`) in `patcher.py`, the browser adapter. See [LICENSE](LICENSE).

### Enabling the repack counter

The footer total counts each verified firmware repack, including stock repacks. It starts at zero and increments when the download is ready. The browser sends an empty request without cookies or firmware data. The total is an estimate because the service receives reports from the browser.

1. From the repository root, deploy the counter with `npx wrangler deploy --config counter/wrangler.toml` using a Cloudflare account.
2. Set the GitHub Actions repository variable `PATCH_COUNTER_URL` to the deployed Worker URL followed by `/patches`.
3. Run the `pages` workflow on `main` to publish the site with the configured endpoint.

For local development, run `npx wrangler dev --config counter/wrangler.toml --var ALLOWED_ORIGIN:http://localhost:8000` and build the site with `PATCH_COUNTER_URL=http://localhost:8787/patches`. Builds without the variable leave the counter off.

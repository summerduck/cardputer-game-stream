# Game Boy for Cardputer ADV

Game Boy and Game Boy Color emulator ([Walnut-CGB](https://github.com/Mr-PauI/Walnut-CGB) core) with two
extras for the small screen:

- **Readable screen modes.** `zoom ×1.5` fills the full 240 px width with no dropped pixel columns and
  shows 90 of the 144 lines. Hold `[` or `]` to scroll the view up or down. `1:1` shows 135 lines
  pixel for pixel. `full frame` is the stock squeeze.
- **Stream to a phone.** The Cardputer starts a Wi-Fi network, and Safari on the phone shows the game
  full screen. You play on the Cardputer keyboard or on the phone's touch buttons.

ROMs (`.gb`, `.gbc`) go on the SD card, in any folder. Saves are written next to the ROM as `<name>.sav`:
when you open the menu, when you quit, and every 30 s if the game changed them.

## Keys

| Key | Game Boy |
|---|---|
| `;` `,` `.` `/` (arrows, also with `fn`) | D-pad |
| `X` | A |
| `Z` | B |
| `Enter` | Start |
| `Space` | Select |
| `\` | screen mode: zoom ×1.5 → 1:1 → whole frame → screen off |
| `[` `]` (hold) | scroll the zoomed view up / down |
| `-` `=` | volume |
| `` ` `` | menu: screen, brightness, colours for original Game Boy games, sound, stream, keys, restart, quit |

The Game Boy buttons can be moved to other keys in the pause menu under **Keys**: pick a button, press
Enter, then press the new key. A key already used by another button swaps with it. `` ` `` `\` `[` `]`
`-` `=` `F` stay reserved. **Reset to default** restores the table above.

The pause menu also has a **CPU** profile. `compatible` is the default and uses the conservative
Walnut-CGB execution path. `fast*` uses Walnut-CGB dual-fetch dispatch; it can be about 8% faster
on the ESP32-S3, but is experimental because some ROMs are known to crash with it. If that happens,
the emulator saves the game and automatically returns to `compatible` for the next launch.

## Streaming to an iPhone

1. In the game list, press `P` to open the **Phone** screen (also in the pause menu), then Enter. The
   stream stays on until you turn it off there. Turn it on from the game list, before starting a game:
   a running game can hold too much memory for Wi-Fi to start from its menu.
   The top right corner of the game list, the pause menu and the Phone screen shows `stream off`,
   `waiting` (Wi-Fi is on, no page open) or `1 connected`.
2. On the iPhone, join the Wi-Fi network **Cardputer-GB** (password `gameboy123`). iOS warns that the
   network has no internet. Stay connected anyway.
3. Open `http://192.168.4.1` in Safari. For a real full-screen view, use **Share → Add to Home Screen**
   and open it from there.
4. **Buttons** in the corner shows touch buttons on the phone.

The stream sends only the lines that changed, at up to 30 fps, in messages of at most 4 KB: 6–80 KB/s in
Zelda, about 220 KB/s in fast games like Donkey Kong Country. If the phone falls behind, it gets fewer frames. Set the Cardputer screen to `off` to give the emulator more time.
The page can't keep the phone awake over plain `http`, so set Auto-Lock to a longer time while playing.

## Memory

The Cardputer ADV has no PSRAM, and with Wi-Fi on only about 68 KB of RAM is left. So the game is
copied from the SD card into the 4 MB `gbrom` flash partition (once per game, a few seconds) and read
from there, mapped into memory: no RAM cache, no SD reads while playing. The partition is created by
`gb-emulator-install.bin` (see below).

4 MB games (Harry Potter, DKC, Rayman) fit. A bigger ROM, or an install with the older 3 MB partition
or none, falls back to reading 2 KB pages from the SD card into a RAM cache. Page misses per frame, measured on the host over
90 s of scripted play:

| Cache | Oracle of Seasons | Harry Potter 2 | Rayman | Link's Awakening (GB) |
|---|---|---|---|---|
| 48 KB | 15.7 | 10.6 | 11.0 | 0.03 |
| 64 KB | 9.9 | 4.6 | 1.2 | 0.01 |
| 80 KB | 0.34 | 0.40 | 0.20 | 0.01 |
| 128 KB | 0.05 | 0.08 | 0.07 | 0 |

Game Boy Color games need about 80 KB of cache, which leaves no room for Wi-Fi.

## Core changes

- `WALNUT_GB_32BIT_DMA` is off. 32-bit HDMA garbles Game Boy Color tiles (Zelda Oracle, DKC).
- `gb_run_frame()` is used instead of the dual-fetch runner, which crashes Harry Potter (invalid opcode).
  It is about 8% slower on the host.

## Build, test, install

Install through M5Launcher (WUI → OTA Update). For the first install, use `gb-emulator-install.bin`,
which also creates the `gbrom` partition. Later updates can use the plain `firmware.bin`.
M5Launcher keeps an existing `gbrom` at its old size, so to go from the 3 MB partition to 4 MB, first
remove `gbrom` in Launcher's **PMan** (partition manager), then install `gb-emulator-install.bin`.

```sh
pio run
tools/make_installer.py .pio/build/cardputer-adv/firmware.bin dist/gb-emulator-install.bin
test/run_tests.sh                        # codec tests (host, sanitizers)
test/run_tests.sh game.gbc 96            # also runs a ROM: cache misses, stream size, frames in test/out/
tools/replay_server.py test/out/<game>.stream   # the phone page against a recorded stream, port 8080

The host run checks that every streamed frame decodes back exactly.
```

Every 5 s the serial log (115200) prints fps, µs per emulated frame, page misses, free heap and the
largest free block, the screen task's unused stack, and stream frames/messages.

Licenses: Walnut-CGB and minigb_apu are MIT (`src/core/LICENSE-*`). PT Sans is under OFL.

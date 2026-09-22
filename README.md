# Cardputer ADV apps

Firmware for the [M5Stack Cardputer ADV](https://docs.m5stack.com/en/core/Cardputer-Adv), installed through [M5Launcher](https://github.com/bmorcelli/Launcher).

| App | What it does |
|---|---|
| [`rsvp-reader`](rsvp-reader/) | Speed reader: shows a book one word at a time with the focus letter highlighted (RSVP). Reads `.txt`, `.epub` and `.fb2`, including Cyrillic. |
| [`samsung-remote`](samsung-remote/) | Infrared remote for Samsung TVs / The Freestyle projector, plus a Bluetooth keyboard for typing on them. |

Ready-made `.bin` files are on the [Releases](../../releases) page.

## Installing with M5Launcher

1. On the Cardputer, open Launcher and choose **WUI → AP mode**.
2. Connect your computer to the Wi-Fi network **Launcher** and open `http://192.168.4.1` (login `admin` / `launcher`).
3. Choose **OTA Update**, pick the `.bin` file and press **Start Update**.

## Building

Both apps are [PlatformIO](https://platformio.org/) projects:

```sh
cd rsvp-reader   # or samsung-remote
pio run          # -> .pio/build/cardputer-adv/firmware.bin
```

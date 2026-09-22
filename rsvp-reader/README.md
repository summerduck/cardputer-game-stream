# RSVP Reader for Cardputer ADV

Reads books one word at a time in a fixed spot, with the optimal recognition point (focus letter) in red, at 100–1000 words per minute.

- Formats: `.txt` (UTF-8 or Windows-1251), `.epub`, `.fb2`. Russian and English out of the box (PT Sans font with Cyrillic).
- Books live on the SD card in `/books`, or without a card in a 2 MB internal flash partition.
- Upload over Wi-Fi: press **W**, join the `Cardputer-Books` network on your phone and open `192.168.4.1`.
- Remembers the position in every book and the reading speed.
- Chapters from EPUB/FB2 headings (and "Глава 5" / "Chapter 5" lines in plain text), with time left in the chapter and in the book.
- While playing only the word is on screen; everything else shows on pause.

## Keys

| Key | Library | Reading |
|---|---|---|
| `;` `.` | select book | faster / slower |
| Enter | open book | start / pause |
| Space | | start / pause |
| `,` `/` | | 10 words back / forward |
| `[` `]` | | previous / next chapter |
| C | | chapter list |
| W | Wi-Fi upload | |
| B | brightness | brightness |
| H | key help | key help |
| fn + Backspace | delete book | |
| `` ` `` | | back to library |

## Install files

- `rsvp-reader-install.bin`: first install. Besides the app it asks Launcher to create the 2 MB `books` flash partition. Don't install it again later, because rewriting the partition can damage books stored there.
- `rsvp-reader-update.bin`: the app only. Use it for updates.

`tools/make_installer.py` builds the install file from `firmware.bin`.

## Code layout

- `src/core/`: portable C++ (no Arduino): EPUB/ZIP, FB2 and HTML to clean text, cp1251 detection, chapter detection, the word reader, ORP and timing rules.
- `src/storage.*`: SD / LittleFS, per-book cache (`/rsvp/<id>.txt`, `.c` chapters, `.s` reading time, `.p` position).
- `src/upload.*`: Wi-Fi access point and upload page.
- `src/main.cpp`: screens and keys.
- `tools/make_vlw.py`: renders the fonts (`src/font_*.h`) from `fonts/*.ttf`.

## Tests

`test/run_tests.sh` builds the core on a Mac/Linux with AddressSanitizer (using [miniz](https://github.com/richgel999/miniz) instead of the ESP32 ROM inflater) and runs it on every book in `test/books/`.

## Licenses

PT Sans is © ParaType, under the SIL Open Font License (`fonts/OFL.txt`).

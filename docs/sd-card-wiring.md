# Wiring the microSD card

The ride log goes to a microSD card wired to four free header pins on the Autosport Labs ESP32-CAN-X2 (ESP32-S3). The card runs in SPI mode on the chip's SPI3 bus at 20 MHz. These are the firmware's default pins. `make menuconfig` (or `idf.py menuconfig`) → emcool → microSD card changes them, and setting any of them to -1 turns the ride log off.

A build keeps the pins it was first configured with. If the boot log's `sd: no card (cs=GPIO…)` names a pin that isn't the one in the table below, the build predates that default: run `make defaults`, then build and flash again.

## Pins

The card pins are the microSD standard's numbering. The GPIOs are plain numbers: find them on headers SV1 and SV2 in the board pinout, Figure 7 on the [ESP32-CAN-X2 wiki](https://wiki.autosportlabs.com/ESP32-CAN-X2). Power and ground are on SV2: 3.3 V on pins 12–13, 5 V on 14–15, GND on 16–18.

| microSD pin | Card signal | SPI role | ESP32-CAN-X2 |
| --- | --- | --- | --- |
| 1 | DAT2 | not used | 10 kΩ to 3.3 V |
| 2 | CD/DAT3 | CS | GPIO4 |
| 3 | CMD | MOSI (DI) | GPIO5 |
| 4 | VDD | power | 3.3 V, SV2-12 (or SV2-13) |
| 5 | CLK | SCK | GPIO15 |
| 6 | VSS | ground | GND, SV2-16 (to 18) |
| 7 | DAT0 | MISO (DO) | GPIO16 |
| 8 | DAT1 | not used | 10 kΩ to 3.3 V |

Which pad is pin 1 depends on the socket: check its datasheet.

Nothing else in the firmware uses the card's chip select (GPIO4). To free it, move CS to another pin in menuconfig.

## A bare socket needs pull-ups

ESP-IDF requires a 10 kΩ pull-up to 3.3 V on CMD and DAT0-DAT3 in SPI mode, so on CS, MOSI and MISO, and on the two unused pins, 1 and 8 (ESP-IDF, "SD Pull-up Requirements"). The firmware doesn't switch on the ESP32-S3's internal pull-ups, which are too weak for this anyway. A ready-made module usually has the resistors on it already.

## A ready-made module

Wire CS, SCK, MOSI, MISO and GND to the GPIOs and header pins above. Power depends on the module:

- **A plain socket board, no regulator:** VCC to 3.3 V (SV2-12).
- **A module with a regulator** (a small 3-pin chip, often marked AMS1117, usually sold as "5 V"): VCC to +5 V (SV2-14). On 3.3 V the regulator leaves the card under-powered, and it fails now and then.

## Things to watch

- **Keep the card off the strapping pins** (GPIO0, 3, 45 and 46). The card's pull-ups would hold one at reset, and the board might not boot. Don't use the USB pins (GPIO19 and 20), which carry the console, or the flash pins (GPIO26–32). The N8R8's octal PSRAM takes GPIO33–37 too. On the ESP32-CAN-X2, GPIO10–13 and GPIO3 belong to its second CAN controller (the MCP2515), and GPIO6 and 7 to CAN1, so the card can't go there.
- **Keep the wires short**, ideally under 10 cm, since the card runs at 20 MHz.
- **Format the card FAT32 first.** The firmware never formats a card. See [Preparing a card](ride-log-format.md#preparing-a-card).

To check the wiring, watch the status report on the serial console: its `ride log:` line says `card mounted` once the card works, and `card ABSENT` until then. `card mounted but not recording` means the card works but the ride log couldn't start on it (the error just before says why, often a full or damaged card). The card commands (`dir`, `cp`, …) still work on it, and the ride log tries again every 30 s.

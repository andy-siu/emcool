# Wiring the microSD card

The ride log goes to a microSD card on the board's SPI header. The card runs in SPI mode on the ESP32's SPI3 bus at 20 MHz. These are the firmware's default pins; `idf.py menuconfig` → emcool → microSD card changes them, and setting any of them to -1 turns the ride log off.

## Pins

The card pins are the microSD standard's numbering. The header pins are on the Copperhill / SK Pang ESP32 CAN-BUS board rev B (see [Hardware](../README.md#hardware)).

| microSD pin | Card signal | SPI role | Connect to |
| --- | --- | --- | --- |
| 1 | DAT2 | not used | 10 kΩ to 3.3 V |
| 2 | CD/DAT3 | CS | GPIO17, JP2-6 |
| 3 | CMD | MOSI (DI) | GPIO18, JP2-8 |
| 4 | VDD | power | 3.3 V, JP2-1 (or JP3-2) |
| 5 | CLK | SCK | GPIO5, JP2-7 |
| 6 | VSS | ground | GND, JP2-2 (or JP3-3) |
| 7 | DAT0 | MISO (DO) | GPIO19, JP2-9 |
| 8 | DAT1 | not used | 10 kΩ to 3.3 V |

Which pad is pin 1 depends on the socket: check its datasheet.

GPIO17 is the card's chip select and nothing else in the firmware uses it. To free it, move CS to another pin in menuconfig.

## A bare socket needs pull-ups

ESP-IDF requires a 10 kΩ pull-up to 3.3 V on CMD and DAT0-DAT3 in SPI mode, so on CS, MOSI and MISO, and on the two unused pins, 1 and 8 (ESP-IDF, "SD Pull-up Requirements"). The firmware doesn't switch on the ESP32's internal pull-ups, which are too weak for this anyway. A ready-made module usually has the resistors on it already.

## A ready-made module

Wire CS, SCK, MOSI, MISO and GND to the GPIOs and header pins above. Power depends on the module:

- **A plain socket board, no regulator:** VCC to 3.3 V (JP2-1).
- **A module with a regulator** (a small 3-pin chip, often marked AMS1117, usually sold as "5 V"): VCC to +5 V (JP3-1). On 3.3 V the regulator leaves the card under-powered, and it fails now and then.

## Things to watch

- **Never use GPIO12 (JP3-5) for MISO.** The card's pull-up holds that strapping pin high at reset, and the board won't boot.
- **Keep the wires short**, ideally under 10 cm, since the card runs at 20 MHz.
- **Format the card FAT32 first.** The firmware never formats a card. See [Preparing a card](ride-log-format.md#preparing-a-card).

To check the wiring, watch the status report on the serial console: its `ride log:` line says `card mounted` once the card works, and `card ABSENT` until then.

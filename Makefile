# Shortcuts for idf.py. Run them from a shell where ESP-IDF is set up
# (`. ~/esp/esp-idf/export.sh`, or the ESP-IDF PowerShell on Windows).
#
#   make            build the firmware
#   make flash      build if needed, then flash
#   make monitor    open the serial monitor (Ctrl-] to quit)
#
# idf.py finds the serial port on its own. To choose one, set PORT:
#   make flash PORT=/dev/cu.usbserial-0001
#   make monitor PORT=COM5

IDF ?= idf.py
PORT ?=

PORT_ARG := $(if $(PORT),-p $(PORT))

# build is also the name of idf.py's output directory, so all three must be phony.
.PHONY: build flash monitor

build:
	$(IDF) build

flash:
	$(IDF) $(PORT_ARG) flash

monitor:
	$(IDF) $(PORT_ARG) monitor

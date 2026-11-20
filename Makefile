# Shortcuts for idf.py. Run them from a shell where ESP-IDF is set up
# (`. ~/esp/esp-idf/export.sh`, or the ESP-IDF PowerShell on Windows).
#
#   make              build the firmware
#   make flash        build if needed, then flash
#   make monitor      open the serial monitor (Ctrl-] to quit)
#   make menuconfig   change the build's options, such as the pins (emcool menu)
#   make defaults     throw away the build's options and start again from the defaults
#
# It builds for the ESP32-S3 (CMakePresets.json's one preset), into build/esp32s3.
#
# idf.py finds the serial port on its own. To choose one, set PORT:
#   make flash PORT=/dev/cu.usbmodem1101
#   make monitor PORT=COM5
#
# A build keeps the options it was first configured with in build/esp32s3/sdkconfig, so a changed
# default (in sdkconfig.defaults* or main/Kconfig.projbuild, such as a pin) doesn't reach a build
# that already exists. `make defaults` deletes that file, which also drops anything set in
# menuconfig, and the next build makes it again from the defaults.

IDF ?= idf.py
PYTHON ?= python
CHIP := esp32s3
PORT ?=

PORT_ARG := $(if $(PORT),-p $(PORT))
IDF_ARGS := --preset $(CHIP) $(PORT_ARG)
SDKCONFIG := build/$(CHIP)/sdkconfig

# build is also the name of idf.py's output directory, so every target must be phony.
.PHONY: build flash monitor menuconfig defaults

build:
	$(IDF) --preset $(CHIP) build

flash:
	$(IDF) $(IDF_ARGS) flash

monitor:
	$(IDF) $(IDF_ARGS) monitor

menuconfig:
	$(IDF) --preset $(CHIP) menuconfig

# Python rather than rm, so it works in the Windows PowerShell too.
defaults:
	$(PYTHON) -c "import os, sys; p = sys.argv[1]; e = os.path.exists(p); e and os.remove(p); print(('removed ' if e else 'no ') + p + ': the next build starts from the defaults')" $(SDKCONFIG)

# SPDX-FileCopyrightText: 2021 Jeff Epler
#
# SPDX-License-Identifier: GPL-3.0-only

FIRMWARE = firmware/cwwvb.ino.elf
all: decoder $(FIRMWARE) run-tests

decoder: decoder.cpp Makefile decoder.h
	$(CXX) -Wall -g -Og -o $@ $< -DMAIN

.PHONY: arduino
arduino: $(FIRMWARE)

$(FIRMWARE): cwwvb.ino decoder.cpp Makefile decoder.h
	arduino-cli compile --verbose -b adafruit:samd:adafruit_feather_m4 --output-dir firmware

PORT := /dev/ttyACM0
.PHONY: flash
flash: $(FIRMWARE)
	arduino-cli upload  -b adafruit:samd:adafruit_feather_m4 -i $(FIRMWARE:.elf=.hex) -p $(PORT)

.PHONY: clean
clean:
	rm -rf *.o decoder firmware

.PHONY: run-tests
run-tests: tests
	./tests

tests: decoder.cpp decoder.h Makefile tests.cpp
	$(CXX) -Wall -g -Og -o $@ $(filter %.cpp, $^)

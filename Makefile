# SPDX-FileCopyrightText: 2021 Jeff Epler
#
# SPDX-License-Identifier: GPL-3.0-only

all: size-decoder decoder

decoder: decoder.cc | Makefile
	g++ -Wall -g -Og -o $@ $< -DMAIN


.PHONY: size-%
size-%: %.cc
	arm-none-eabi-g++ -mcpu=cortex-m4 -Os -c $< && size $*.o && nm -C --print-size --size-sort $*.o

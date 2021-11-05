// SPDX-FileCopyrightText: 2021 Jeff Epler
//
// SPDX-License-Identifier: GPL-3.0-only

#ifndef ARDUINO

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "decoder.h"

circular_bit_array<6> cba;
circular_symbol_array<6, 4> csa;

TEST_CASE("test bit array") {
    for (int i = 0; i < 6; i++)
        cba.put(0);

    // The next puts need to read out six zeros
    for (int i = 0; i < 6; i++)
        CHECK(cba.put(1) == 0);

    // The next puts need to read out six ones
    for (int i = 0; i < 6; i++)
        CHECK(cba.put(1) == 1);

    cba.put(0);
    // content now 111110

    CHECK(cba.at(0) == 1);
    CHECK(cba.at(1) == 1);
    CHECK(cba.at(2) == 1);
    CHECK(cba.at(3) == 1);
    CHECK(cba.at(4) == 1);
    CHECK(cba.at(5) == 0);
}

TEST_CASE("test symbol array") {
    for (int i = 0; i < 6; i++)
        csa.put(0);

    // The next puts need to read out six zeros
    for (int i = 0; i < 6; i++)
        CHECK(csa.put(i) == 0);

    // The next puts need to read out the values above
    for (int i = 0; i < 6; i++)
        CHECK(csa.put(1) == i);

    csa.put(0);
    csa.put(1);
    csa.put(2);
    csa.put(3);
    csa.put(4);
    csa.put(5);
    // content now 012345

    CHECK(csa.at(0) == 0);
    CHECK(csa.at(1) == 1);
    CHECK(csa.at(2) == 2);
    CHECK(csa.at(3) == 3);
    CHECK(csa.at(4) == 4);
    CHECK(csa.at(5) == 5);
}

TEST_CASE("test leap second") {
    struct wwvb_time ww = {
        .yday = 366,
        .year = 16,
        .hour = 23,
        .minute = 59,
        .second = 59,
        .ls = 1,
        .ly = 1,
        .dst = 0,
        .dut1 = -4,
    };

    ww.advance_seconds();
    CHECK(ww.second == 60);
    CHECK(ww.ls);
    CHECK(ww.dut1 == -4);

    ww.advance_seconds();
    CHECK(ww.second == 0);
    CHECK(!ww.ls);
    CHECK(ww.dut1 == 6);
}

TEST_CASE("test dst next day") {
    struct wwvb_time ww = {
        .yday = 73,
        .year = 21,
        .hour = 23,
        .minute = 59,
        .second = 59,
        .ls = 1,
        .ly = 1,
        .dst = 2,
    };

    ww.advance_seconds();
    CHECK(ww.second == 0);
    CHECK(ww.dst == 3);
}

TEST_CASE("test std next day") {
    struct wwvb_time ww = {
        .yday = 311,
        .year = 21,
        .hour = 23,
        .minute = 59,
        .second = 59,
        .ls = 1,
        .ly = 1,
        .dst = 1,
    };

    ww.advance_seconds();
    CHECK(ww.second == 0);
    CHECK(ww.dst == 0);
}

#endif

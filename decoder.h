// SPDX-FileCopyrightText: 2021 Jeff Epler
//
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>

template <int N> int mod_diff(int a, int b) {
    int c = a - b;
    if (c > N / 2)
        c -= N;
    if (c < -N / 2)
        c += N;
    return c;
}

template <int N> int mod_between(int lo, int hi, int val) {
    return mod_diff<N>(lo, val) < 0 && mod_diff<N>(val, hi) < 0;
}

// An efficient circular buffer of N bits. This is used to store the sampled
// WWVB signal for statistical purposes.  It's also used as the basis of the
// circular_symbol_array, which handles multi-bit values
template <int N> struct circular_bit_array {
    bool at(int i) const {
        assert(i >= 0 && i < N);
        i += shift;
        if (i > N)
            i -= N;
        int j = i % 32;
        i /= 32;
        return data[i] & (1 << j);
    }

    bool put(bool b) {
        int i = shift / 32;
        int j = shift % 32;

        bool result = data[i] & (1 << j);

        if (b) {
            data[i] |= (1 << j);
        } else {
            data[i] &= ~(1 << j);
        }

        shift += 1;
        if (shift == N)
            shift = 0;

        return result;
    }

    uint32_t data[(N + 31) / 32];
    uint16_t shift;
};

// An efficient circular buffer of N M-bit values. This is used to store the
// decoded WWVB signals, which can be 0, 1, or 2 (mark).
template <int N, int M> struct circular_symbol_array {
    int at(int i) const {
        int result = 0;
        for (int j = 0; j < M; j++) {
            result = (result << 1) | data.at(i * M + j);
        }
        return result;
    }

    bool put(int v) {
        int result = 0;
        for (int j = 0; j < M; j++) {
            result = (result << 1) | data.put(v & (1 << (M - 1)));
            v <<= 1;
        }
        return result;
    }

    circular_bit_array<M * N> data;
};

// The second is divided into units of SUBSEC
constexpr size_t SUBSEC = 50;
// This many WWVB symbols are accumulated
constexpr size_t SYMBOLS = 60;
// This many raw samples are accumulated.  A slight code-size-savings is had
// if it is 2*SYMBOLS.  But, it must also be a multiple of SUBSEC, so that the
// sample that 'falls out' of the buffer each time is the correct one to
// subtract from the 'counts array!  However, only ~5 seconds is too little
// history and it makes SoS wander too much, so go for 60 seconds instead.
constexpr size_t BUFFER = SUBSEC * 40;
static_assert(BUFFER % SUBSEC == 0);

typedef circular_symbol_array<SYMBOLS, 2> symbol_buffer;
// Raw samples from the receiver
extern circular_bit_array<BUFFER> d;
// Statistical information about the raw samples
extern int16_t counts[SUBSEC], edges[SUBSEC];
// subsec counts the position modulo SUBSEC; sos is the start-of-second modulo
// SUBSEC
extern int16_t subsec, sos;
// Decoded symbols
extern symbol_buffer symbols;

extern size_t tss; // time since second
// Receive a sample `b` from the receiver and process:
//  * update statistics (counts and edges) incrementally
//  * check all edges values to update the start-of-second value
// Returns true if it is the START of a new WWVB second
inline bool update(bool b) {
    // Put the new bit & extract the old bit
    auto ob = d.put(b);

    // Update the counts array
    if (b && !ob) {
        counts[subsec]++;
    } else if (!b && ob) {
        counts[subsec]--;
    }

    // Update the edges array
    auto subsec1 = subsec == SUBSEC - 1 ? 0 : subsec + 1;
    edges[subsec] = counts[subsec1] - counts[subsec];

    // Check for sharpest edge
    // can this be done without a whole array scan?
    int bi = 0, best = 0;
    for (size_t i = 0; i < SUBSEC; i++) {
        if (edges[i] > best) {
            bi = i;
            best = edges[i];
        }
    }
    int osos = sos;
    sos = bi == SUBSEC - 1 ? 0 : bi + 1;

    subsec = subsec1;

    bool result = false;
    // If it's been a long time since the last second, fake one.
    if (tss > SUBSEC) {
        result = true;
    } else if (tss > SUBSEC / 2) {
        // Otherwise, sos may be wandering, so don't repeat a second too soon
        result = subsec == sos || subsec == osos;
    }

    // either reset or increment time-since-second
    if (result)
        tss = 0;
    else
        tss++;

    return result;
}

// Return how many items from i..j in the raw data array are true
// (true represents the reduced-carrier state)
inline int count(int i, int j) {
    int result = 0;
    for (; i < j; i++) {
        result += d.at(i);
    }
    return result;
}

// We're informed that a new second _just started_, so
// d.at(BUFFER-1) is the first sample of the new second. and
// d.at(BUFFER-SUBSEC-1) is the
inline int decode_symbol() {
    constexpr auto OFFSET = BUFFER - SUBSEC - 1;
    int count_a = count(OFFSET + 10, OFFSET + 25);
    int count_b = count(OFFSET + 25, OFFSET + 40);

    if (count_b > 7)
        return 2;
    if (count_a > 7)
        return 1;
    return 0;
}

// I guess we need to represent time...
struct wwvb_minute {
    int16_t yday;
    int8_t year, hour, minute;
    int8_t ly, dst;
    int8_t month, mday;
};

// Simple BCD-decoder
extern bool bcderr;
inline int decode_bcd(symbol_buffer const &s, int d, int c = -1, int b = -1,
                      int a = -1) {
    int r = (a >= 0 ? (s.at(SYMBOLS - 60 + a) * 8) : 0) +
            (b >= 0 ? (s.at(SYMBOLS - 60 + b) * 4) : 0) +
            (c >= 0 ? (s.at(SYMBOLS - 60 + c) * 2) : 0) +
            symbols.at(SYMBOLS - 60 + d) * 1;
    if (r > 9)
        bcderr = true;
    return r;
}

template <class... Ints>
int decode_bcd(symbol_buffer const &s, int d, int c, int b, int a,
               Ints... rest) {
    return decode_bcd(s, d, c, b, a) + 10 * decode_bcd(s, rest...);
}

// barebones decoding of some minute-fields
inline bool decode_minute(symbol_buffer const &s, wwvb_minute &m) {
    for (int i = 0; i < 60; i++) {
        int sym = s.at(SYMBOLS - 60 + i);
        bool is_mark = (i == 0) || (i % 10 == 9);
        if (is_mark != (sym == 2))
            return false;
        bool is_zero = (i % 10 == 4) || i == 10 || i == 11 || i == 20 ||
                       i == 21 || i == 35;
        if (is_zero && sym != 0)
            return false;
    }

    bcderr = false;
    m.year = decode_bcd(s, 53, 52, 51, 50, 48, 47, 46, 45);
    m.yday = decode_bcd(s, 33, 32, 31, 30, 28, 27, 26, 25, 23, 22);
    m.hour = decode_bcd(s, 18, 17, 16, 15, 13, 12);
    m.minute = decode_bcd(s, 8, 7, 6, 5, 3, 2, 1);
    m.ly = decode_bcd(s, 55);
    m.dst = decode_bcd(s, 58, 57);

    return !bcderr;
}

time_t wwvb_to_utc(const wwvb_minute &m);
struct tm apply_zone_and_dst(const wwvb_minute &m, int zone_offset,
                             bool observe_dst);

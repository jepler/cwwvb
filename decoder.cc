// SPDX-FileCopyrightText: 2021 Jeff Epler
//
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MAIN
#define NDEBUG
#endif

#define _DEFAULT_SOURCE

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>

// An efficient circular buffer of N bits. This is used to store the sampled
// WWVB signal for statistical purposes.  It's also used as the basis of the
// circular_symbol_array, which handles multi-bit values
template <int N> struct circular_bit_array {
    bool at(int i) {
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
    int at(int i) {
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

// Raw samples from the receiver
circular_bit_array<BUFFER> d;
// Statistical information about the raw samples
int16_t counts[SUBSEC], edges[SUBSEC];
// subsec counts the position modulo SUBSEC; sos is the start-of-second modulo
// SUBSEC
int16_t subsec = 0, sos = 0;
// Decoded symbols
circular_symbol_array<SYMBOLS, 2> symbols;

size_t tss; // time since second
// Receive a sample `b` from the receiver and process:
//  * update statistics (counts and edges) incrementally
//  * check all edges values to update the start-of-second value
// Returns true if it is the START of a new WWVB second
bool update(bool b) {
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
    sos = bi;

    subsec = subsec1;

    bool result = false;
    // If it's been a long time since the last second, fake one.
    if (tss > SUBSEC + 2) {
        result = true;
    } else if (tss > SUBSEC / 2) {
        // Otherwise, sos may be wandering, so don't repeat a second too soon
        result = subsec == sos;
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
int count(int i, int j) {
    int result = 0;
    for (; i < j; i++) {
        result += d.at(i);
    }
    return result;
}

// We're informed that a new second _just started_, so
// d.at(BUFFER-1) is the first sample of the new second. and
// d.at(BUFFER-SUBSEC-1) is the
int decode_symbol() {
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
bool bcderr;
int decode_bcd(int d, int c = -1, int b = -1, int a = -1) {
    int r = (a >= 0 ? (symbols.at(SYMBOLS - 60 + a) * 8) : 0) +
            (b >= 0 ? (symbols.at(SYMBOLS - 60 + b) * 4) : 0) +
            (c >= 0 ? (symbols.at(SYMBOLS - 60 + c) * 2) : 0) +
            symbols.at(SYMBOLS - 60 + d) * 1;
    if (r > 9)
        bcderr = true;
    return r;
}

template <class... Ints>
int decode_bcd(int d, int c, int b, int a, Ints... rest) {
    return decode_bcd(d, c, b, a) + 10 * decode_bcd(rest...);
}

// barebones decoding of some minute-fields
bool decode_minute(wwvb_minute &m) {
    for (int i = 0; i < 60; i++) {
        int sym = symbols.at(SYMBOLS - 60 + i);
        bool is_mark = (i == 0) || (i % 10 == 9);
        if (is_mark != (sym == 2))
            return false;
        bool is_zero = (i % 10 == 4) || i == 10 || i == 11 || i == 20 ||
                       i == 21 || i == 35;
        if (is_zero && sym != 0)
            return false;
    }

    bcderr = false;
    m.year = decode_bcd(53, 52, 51, 50, 48, 47, 46, 45);
    m.yday = decode_bcd(33, 32, 31, 30, 28, 27, 26, 25, 23, 22);
    m.hour = decode_bcd(18, 17, 16, 15, 13, 12);
    m.minute = decode_bcd(8, 7, 6, 5, 3, 2, 1);
    m.ly = decode_bcd(55);
    m.dst = decode_bcd(58, 57);

    return !bcderr;
}

time_t wwvb_to_utc(const wwvb_minute &m) {
    struct tm tm = {};
    tm.tm_year = 2000 + m.year;
    tm.tm_mday = 1;
    time_t t = mktime(&tm);
    t += (m.yday - 1) * 86400 + m.hour * 3600 + m.minute * 60;
    return t;
}

struct tm apply_zone_and_dst(const wwvb_minute &m, int zone_offset,
                             bool observe_dst) {
    auto t = wwvb_to_utc(m);
    t -= zone_offset * 3600;

    struct tm tm;
    gmtime_r(&t, &tm);

    switch (m.dst) {
    case 0: // Standard time in effect
        observe_dst = false;

        break;
    case 1: // DST ending at
    {
        struct tm tt = tm;
        tt.tm_hour = 1; // DST ends at 2AM *DST* which is 1AM *standard*
        auto u = mktime(&tt);
        if (t >= u)
            observe_dst = false;
    }

    break;
    case 2: // DST starting at 2AM local time this day
    {
        struct tm tt = tm;
        tt.tm_hour = 2;
        auto u = mktime(&tt);
        if (t < u)
            observe_dst = false;
    }
    }

    if (observe_dst)
        t += 3600;

    gmtime_r(&t, &tm);
    return tm;
}

#if MAIN
#include <iostream>
using namespace std;

int main() {

    static char zone[] = "TZ=UTC";
    putenv(zone);
    tzset();

    int i = 0, si = 0, d = 0;
    for (int c; (c = cin.get()) != EOF;) {
        if (c != '_' && c != '#') {
            continue;
        }
        if (update(c == '_')) {
            int sym = decode_symbol();
            symbols.put(sym);
            si++;
            // std::cout << sym;
            // if(si % 60 == 0) std::cout << "\n";
            if (sym == 2) {
                // This mark could be the minute-ending mark, so try to
                // decode a minute
                wwvb_minute m;
                if (decode_minute(m)) {
                    d++;
                    time_t t = wwvb_to_utc(m);
                    struct tm tt;
                    gmtime_r(&t, &tt);
                    printf("[%7.2f] %4d-%02d-%02d %2d:%02d %d %d\n", i / 50.,
                           tt.tm_year, tt.tm_mon + 1, tt.tm_mday, tt.tm_hour,
                           tt.tm_min, m.ly, m.dst);
                    tt = apply_zone_and_dst(m, 6, true);
                    printf("          %4d-%02d-%02d %2d:%02d\n", tt.tm_year,
                           tt.tm_mon + 1, tt.tm_mday, tt.tm_hour, tt.tm_min);
                    printf("          %4d-%03d   %2d:%02d\n", m.year + 2000,
                           m.yday, m.hour, m.minute);
                }
            }
        }
        i++;
    }
    printf("Samples: %8d Symbols: %7d Minutes: %6d\n", i, si, d);
}
#endif

// SPDX-FileCopyrightText: 2021 Jeff Epler
//
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
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

    std::array<uint32_t, (N + 31) / 32> data{};
    uint16_t shift{};
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

    int put(int v) {
        int result = 0;
        for (int j = 0; j < M; j++) {
            result = (result << 1) | data.put(v & (1 << (M - 1)));
            v <<= 1;
        }
        return result;
    }

    circular_bit_array<M * N> data{};
};

// I guess we need to represent time...
struct wwvb_time {
    int16_t yday;
    int8_t year, hour, minute, second;
    int8_t ls, ly, dst, dut1;

    time_t to_utc() const;
    struct tm apply_zone_and_dst(int zone_offset, bool observe_dst) const;

    int seconds_in_minute() const;
    void advance_seconds(int n = 1);
    void advance_minutes(int n = 1);

    bool operator==(const wwvb_time &other) const;
};

template <size_t SUBSEC_ = 50, size_t SYMBOLS_ = 60, size_t HISTORY_ = 40>
struct WWVBDecoder {
    // The second is divided into units of SUBSEC
    static constexpr size_t SUBSEC = SUBSEC_;

    // This many WWVB symbols are accumulated
    static constexpr size_t SYMBOLS = SYMBOLS_;

    // This many whole seconds of symbols are accumulated for statistics.
    // 5 seconds is too little history, 60 is plenty.  40 seems okay.
    static constexpr size_t HISTORY = HISTORY_;
    static constexpr size_t BUFFER = SUBSEC * HISTORY_;

    typedef circular_symbol_array<SYMBOLS, 2> symbol_buffer_type;
    typedef circular_bit_array<BUFFER> signal_buffer_type;

    // Total number of samples ever received
    size_t sample_count{};

    // Total number of symbols ever decoded
    size_t symbol_count{};

    // Raw samples from the receiver
    signal_buffer_type signal{};

    // Statistical information about the raw samples
    std::array<int16_t, SUBSEC> counts{};
    std::array<int16_t, SUBSEC> edges{};

    // Statistical information about the symbols
    int health{};
    std::array<uint8_t, SYMBOLS> health_history{};

    // subsec counts the position modulo SUBSEC; sos is the start-of-second
    // modulo SUBSEC.  tss is the time in ticks since the last second.
    uint16_t subsec{}, sos{}, tss{};

    // Decoded symbols
    symbol_buffer_type symbols{};

    // Receive a sample `b` from the receiver and process:
    //  * update statistics (counts and edges) incrementally
    //  * check all edges values to update the start-of-second value
    //  * updates the symbols buffer at the start of a new second
    // Returns true if it is the START of a new WWVB second

    bool update(bool b) {
        // Put the new bit & extract the old bit
        sample_count++;
        auto ob = signal.put(b);

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
            // Otherwise, sos may be wandering, so don't repeat a second too
            // soon
            result = subsec == sos || subsec == osos;
        }

        // either reset or increment time-since-second
        if (result) {
            tss = 0;
            decode_symbol();
        } else {
            tss++;
        }

        return result;
    }

    // Return how many items from i..j in the raw data array are true
    // (true represents the reduced-carrier state)
    int count(int i, int j) {
        int result = 0;
        for (; i < j; i++) {
            result += signal.at(i);
        }
        return result;
    }

    static constexpr int ms_in_subsec(int ms) {
        return (ms * SUBSEC + SUBSEC / 2) / 1000;
    }

    static constexpr auto p0 = ms_in_subsec(0);
    static constexpr auto p1 = ms_in_subsec(200);
    static constexpr auto p2 = ms_in_subsec(500);
    static constexpr auto p3 = ms_in_subsec(800);
    static constexpr auto p4 = ms_in_subsec(1000);

    static constexpr auto la = p1 - p0;
    static constexpr auto lb = p2 - p1;
    static constexpr auto lc = p3 - p2;
    static constexpr auto ld = p4 - p3;

    static constexpr auto MAX_HEALTH = SYMBOLS * SUBSEC;
    // In around 300 hours of logs from the WWVB observatory, the current
    // algorithm decoded 16004 minutes (at all, not back-checked for
    // correctness).  Of those, minutes about 86% had health above 97%.  That
    // makes 97% a plausible threshold for a healthy signal.
    static constexpr auto HEALTH_97PCT = MAX_HEALTH * 97 / 100;

    int check_health(int count, int length, int expect) {
        return expect ? count : length - count;
    }

    // A second just concluded, so signal.at(BUFFER-1) is the last sample of
    // the second, and signal.at(BUFFER-SUBSEC) is the first sample of the
    // second
    void decode_symbol() {
        constexpr auto OFFSET = BUFFER - SUBSEC;
#if 0
        for(size_t i=0; i<SUBSEC; i++) {
            printf("%c", signal.at(OFFSET + i) ? '_' : '#');
        }
#endif
        int count_a = count(OFFSET + p0, OFFSET + p1);
        int count_b = count(OFFSET + p1, OFFSET + p2);
        int count_c = count(OFFSET + p2, OFFSET + p3);
        int count_d = count(OFFSET + p3, OFFSET + p4);

        int result = 0;

        if (count_c > lc / 2) {
            if (count_b > lb / 2) {
                result = 2;
            } else {
                result = 3; // a nonsense symbol
            }
        } else if (count_b > lb / 2) {
            result = 1;
        }

        int h = 0;
        if (result != 3) {
            h += check_health(count_a, la, 1);
            h += check_health(count_b, lb, result != 0);
            h += check_health(count_c, lc, result == 2);
            h += check_health(count_d, ld, 0);
        }

        int sc = symbol_count++;
        int si = sc % SYMBOLS;
        int oh = health_history[si];
        health_history[si] = h;
        health += (h - oh);

        symbols.put(result);

#if 0
        printf(" %d\n", result);
        for(size_t i=0; i<SYMBOLS; i++) {
            printf("%d", symbols.at(i));
        }
        printf("\n", result);
#endif
    }

    mutable bool bcderr;

    // Simple BCD-decoder
    int decode_bcd(int d, int c = -1, int b = -1, int a = -1) const {
        int r = (a >= 0 ? (symbols.at(SYMBOLS - 60 + a) * 8) : 0) +
                (b >= 0 ? (symbols.at(SYMBOLS - 60 + b) * 4) : 0) +
                (c >= 0 ? (symbols.at(SYMBOLS - 60 + c) * 2) : 0) +
                symbols.at(SYMBOLS - 60 + d) * 1;
        if (r > 9)
            bcderr = true;
        return r;
    }

    template <class... Ints>
    int decode_bcd(int d, int c, int b, int a, Ints... rest) const {
        return decode_bcd(d, c, b, a) + 10 * decode_bcd(rest...);
    }

    // barebones decoding of some minute-fields
    inline bool decode_minute(wwvb_time &m) const {
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
        m.ls = decode_bcd(56);
        m.dst = decode_bcd(58, 57);
        m.second = 0;
        int abs_dut1 = decode_bcd(43, 42, 41, 40);
        int dut1_sign = decode_bcd(38, 37, 36);
        switch (dut1_sign) {
        case 2:
            m.dut1 = -abs_dut1;
            break;
        case 5:
            m.dut1 = abs_dut1;
            break;
        default:
            bcderr = true;
        }
        return !bcderr;
    }
};

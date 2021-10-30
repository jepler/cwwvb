// SPDX-FileCopyrightText: 2021 Jeff Epler
//
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <atomic>
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

    bool put(int v) {
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
    int8_t ly, dst;
    int8_t month, mday;

    time_t to_utc() const;
    struct tm apply_zone_and_dst(int zone_offset, bool observe_dst) const;

    int seconds_in_minute() const;
    int advance_second();
    int advance_minute();

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
    std::atomic<size_t> sample_count{};

    // Total number of symbols ever decoded
    std::atomic<size_t> symbol_count{};

    // Raw samples from the receiver
    signal_buffer_type signal{};

    // Statistical information about the raw samples
    std::array<int16_t, SUBSEC> counts{};
    std::array<int16_t, SUBSEC> edges{};

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
        sample_count.fetch_add(1);
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
            symbols.put(decode_symbol());
            symbol_count.fetch_add(1);
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

    constexpr int offset_ms(int ms) {
        return (ms + SUBSEC / 2) * SUBSEC / 1000;
    }

    // We're informed that a new second _just started_, so
    // signal.at(BUFFER-1) is the first sample of the new second. and
    // signal.at(BUFFER-SUBSEC-1) is the
    int decode_symbol() {
        constexpr auto OFFSET = BUFFER - SUBSEC - 1;
        int count_a = count(OFFSET + offset_ms(200), OFFSET + offset_ms(500));
        int count_b = count(OFFSET + offset_ms(500), OFFSET + offset_ms(800));

        if (count_b > 7)
            return 2;
        if (count_a > 7)
            return 1;
        return 0;
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
        m.dst = decode_bcd(58, 57);
        m.second = 0;
        return !bcderr;
    }

    // Consistently snapshot *this into other, including if update() is called
    // from an interrupt
    void snapshot(WWVBDecoder &other) const {
        int count;
        do {
            count = sample_count.load();
            other.symbols = symbols;
            other.counts = counts;
            other.edges = edges;
            other.sos = sos;
            other.tss = tss;
        } while (count != sample_count.load());
    }
};

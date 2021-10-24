#include <cstddef>
#include <cstdint>
#include <cassert>

// An efficient circular buffer of N bits. This is used to store the sampled
// WWVB signal for statistical purposes.  It's also used as the basis of the
// circular_symbol_array, which handles multi-bit values
template<int N>
struct circular_bit_array {
    bool at(int i) {
        assert(i >= 0 && i < N);
        i += shift;
        if(i > N) i -= N;
        int j = i % 32;
        i /= 32;
        return data[i] & (1<<j);
    }

    bool put(bool b) {
        int i = shift / 32;
        int j = shift % 32;

        bool result = data[i] & (1<<j);

        if (b) {
            data[i] |= (1<<j);
        } else {
            data[i] &= ~(1<<j);
        }

        shift += 1;
        if(shift == N) shift = 0;

        return result;
    }

    uint32_t data[(N+31)/32];
    uint16_t shift;
};

// An efficient circular buffer of N M-bit values. This is used to store the
// decoded WWVB signals, which can be 0, 1, or 2 (mark).
template<int N, int M>
struct circular_symbol_array {
    int at(int i) {
        int result = 0;
        for(int j=0; j<M; j++) {
            result = (result << 1) | data.at(i*M+j);
        }
        return result;
    }

    bool put(int v0) {
        int result = 0;
        int v = v0;
        for(int j=0; j<M; j++) {
            result = (result << 1) | data.put(v & (1<<(M-1)));
            v <<= 1;
        }
        return result;
    }

    circular_bit_array<M*N> data;
};

// The second is divided into units of SUBSEC
constexpr size_t SUBSEC = 50;
// This many WWVB symbols are accumulated
constexpr size_t SYMBOLS = 121;
// This many raw samples are accumulated.  A slight code-size-savings is had
// if it is 2*SYMBOLS.
constexpr size_t BUFFER = 2*SYMBOLS;

// Raw samples from the receiver
circular_bit_array<BUFFER> d;
// Statistical information about the raw samples
int16_t counts[SUBSEC], edges[SUBSEC];
// subsec counts the position modulo SUBSEC; sos is the start-of-second modulo SUBSEC
int16_t subsec = 0, sos = 0;
// Decoded symbols
circular_symbol_array<SYMBOLS, 2> symbols;


// Receive a sample `b` from the receiver and process:
//  * update statistics (counts and edges) incrementally
//  * check all edges values to update the start-of-second value
// Returns true if it is the START of a new WWVB second
bool update(bool b) {
    // Put the new bit & extract the old bit
    auto ob = d.put(b);

    // Update the counts array
    if(b && !ob) {
        counts[subsec] ++;
    } else if(!b && ob) {
        counts[subsec] --;
    }

    // Update the edges array
    auto subsec1 = subsec == SUBSEC-1 ? 0 : subsec+1;
    edges[subsec] = counts[subsec1] - counts[subsec];

    // Check for sharpest edge
    // can this be done without a whole array scan?
    int bi = 0, best = 0;
    for(size_t i=0; i<SUBSEC; i++) {
        if(edges[i] > best) {
            bi = i; best = edges[i];
        }
    }
    sos = bi;

    subsec = subsec1;

    return subsec == sos;
}

// Return how many items from i..j in the raw data array are true
// (true represents the reduced-carrier state)
int count(int i, int j) {
    int result = 0;
    for(;i<j;i++) {
        result += d.at(i);
    }
    return result;
}

// We're informed that a new second _just started_, so
// d.at(BUFFER-1) is the first sample of the new second. and
// d.at(BUFFER-SUBSEC-1) is the 
int decode_symbol() {
    constexpr auto OFFSET = BUFFER-SUBSEC-1;
    int count_a = count(OFFSET+10, OFFSET+25);
    int count_b = count(OFFSET+25, OFFSET+40);

    if(count_b > 7)
        return 2;
    if(count_a > 7)
        return 1;
    return 0;
}

// We know that the symbol just seen is a 2, which is a minute-terminating
// symbol.  Check if all 2-markers of a minute are present.
bool check_minute() {
    return symbols.at(SYMBOLS-60+0) == 2 &&
        symbols.at(SYMBOLS-60+9) == 2 &&
        symbols.at(SYMBOLS-60+19) == 2 &&
        symbols.at(SYMBOLS-60+29) == 2 &&
        symbols.at(SYMBOLS-60+39) == 2 &&
        symbols.at(SYMBOLS-60+49) == 2;
}

// I guess we need to represent time...
struct wwvb_minute {
    int16_t yday;
    int8_t year, hour, minute;
    int8_t ly, dst;
};

// Simple BCD-decoder
int decode_bcd(int d, int c=-1, int b=-1, int a=-1) {
    return
        (a >= 0 ? (symbols.at(SYMBOLS-60+a) * 8) : 0) +
        (b >= 0 ? (symbols.at(SYMBOLS-60+b) * 4) : 0) +
        (c >= 0 ? (symbols.at(SYMBOLS-60+c) * 2) : 0) +
        symbols.at(SYMBOLS-60+d) * 1;
}

template<class ...Ints>
int decode_bcd(int d, int c, int b, int a, Ints... rest) {
    return decode_bcd(d, c, b, a) + 10 * decode_bcd(rest...);
}

// barebones decoding of some minute-fields, doesn't track bad BCD codes
bool decode_minute(wwvb_minute &m) {
    for(int i=0; i<60; i++) {
        int sym = symbols.at(SYMBOLS-60+i);
        bool is_mark = (i == 0) || (i % 10 == 9);
        if (is_mark != (sym == 2)) return false;
    }

    m.year = decode_bcd(53, 52, 51, 50, 48, 47, 46, 45);
    m.yday = decode_bcd(33, 32, 31, 30, 28, 27, 26, 25, 23, 22);
    m.hour = decode_bcd(18, 17, 16, 15, 13, 12);
    m.minute = decode_bcd(8, 7, 6, 5, 3, 2, 1);
    m.ly = decode_bcd(55);
    m.dst = decode_bcd(58, 57);
    return true;
}

#if MAIN
#include <iostream>
using namespace std;

int main() {

    for(int c, i=-1; (c = cin.get()) != EOF; i++) {
        if(update(c == '_')) {
            int sym = decode_symbol();
            symbols.put(sym);
            if(sym == 2 && check_minute()) {
                wwvb_minute m;
                if (decode_minute(m)) {
                    std::cerr << "[" << i/50. << "] " << (2000+m.year) << " " << m.yday << " " << +m.hour << ":" << +m.minute << " " << +m.ly << +m.dst << "\n";
                }
            }
        }
    }
}
#endif

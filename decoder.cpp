// SPDX-FileCopyrightText: 2021 Jeff Epler
//
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MAIN
#define NDEBUG
#endif

#include "decoder.h"

time_t wwvb_time::to_utc() const {
    struct tm tm = {};
    tm.tm_year = 100 + year;
    tm.tm_mday = 1;
    time_t t = mktime(&tm);
    t += (yday - 1) * 86400 + hour * 3600 + minute * 60;
    t += (second == 60) ? 59 : second;
    return t;
}

struct tm wwvb_time::apply_zone_and_dst(int zone_offset,
                                        bool observe_dst) const {
    auto t = to_utc();
    t -= zone_offset * 3600;

    struct tm tm;
    gmtime_r(&t, &tm);

    switch (dst) {
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

    if (second == 60) {
        tm.tm_sec++;
    }
    return tm;
}

#if MAIN
#include <iostream>
using namespace std;

int main() {
    WWVBDecoder<> dec;

    static char zone[] = "TZ=UTC";
    putenv(zone);
    tzset();

    int i = 0, si = 0, d = 0;
    for (int c; (c = cin.get()) != EOF;) {
        if (c != '_' && c != '#') {
            continue;
        }
        if (dec.update(c == '_')) {
            si++;
            auto sym = dec.symbols.at(dec.SYMBOLS - 1);
            // std::cout << sym;
            // if(si % 60 == 0) std::cout << "\n";
            if (sym == 2) {
                // This mark could be the minute-ending mark, so try to
                // decode a minute
                wwvb_time m;
                if (dec.decode_minute(m)) {
                    d++;
                    time_t t = m.to_utc();
                    struct tm tt;
                    gmtime_r(&t, &tt);
                    printf("[%7.2f] %4d-%02d-%02d %2d:%02d %d %d\n", i / 50.,
                           1900 + tt.tm_year, tt.tm_mon + 1, tt.tm_mday,
                           tt.tm_hour, tt.tm_min, m.ly, m.dst);
                    tt = m.apply_zone_and_dst(6, true);
                    printf("          %4d-%02d-%02d %2d:%02d\n",
                           1900 + tt.tm_year, tt.tm_mon + 1, tt.tm_mday,
                           tt.tm_hour, tt.tm_min);
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

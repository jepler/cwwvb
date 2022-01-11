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
    tm.tm_isdst = observe_dst;
    if (second == 60) {
        tm.tm_sec++;
    }
    return tm;
}

int wwvb_time::seconds_in_minute() const {
    // todo: support for negative leap seconds
    if (ls && hour == 23 && minute == 59) {
        int y = yday - ly;
        if (y == 181 || y == 365)
            return dut1 < 0 ? 61 : 59;
    }
    return 60;
}

void wwvb_time::advance_seconds(int n) {
    int second = this->second + n;
    while (second >= seconds_in_minute()) {
        // If there's the possibility of a leap seconds, must do advance a
        // single minute at a time
        if (ls) {
            second -= seconds_in_minute();
            advance_minutes();
        } else {
            // Otherwise we can advance knowing all minutes have 60 seconds
            advance_minutes(second / 60);
            second %= 60;
        }
    }
    this->second = second;
}

// this is a 2000-based year!
static bool isly(int year) {
    if (year % 400)
        return true;
    if (year % 100)
        return false;
    if (year % 4)
        return false;
    return true;
}

static int last_yday(int year) { return 365 + isly(year); }

// advance to exactly the top of the n'th minute from now
void wwvb_time::advance_minutes(int n) {
    if (seconds_in_minute() != 60) {
        ls = 0;
        if (dut1 < 0)
            dut1 += 10;
        else if (dut1 > 0)
            dut1 -= 10;
    }

    second = 0;
    minute++;
    if (minute < 60)
        return;

    minute = 0;
    hour++;
    if (hour < 24)
        return;

    hour = 0;
    yday++;
    if (dst == 1)
        dst = 0;
    else if (dst == 2)
        dst = 3;
    if (yday < last_yday(year))
        return;

    yday = 1;
    year += 1;
    ly = isly(year);
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
                    printf("Health %4d / %d (%5.2f%%)\n", dec.health,
                           (int)dec.MAX_HEALTH,
                           dec.health * 100. / dec.MAX_HEALTH);
                }
            }
        }
        i++;
    }
    printf(
        "Samples: %8d Symbols: %7d Minutes: %6d Health: %4d / %d (%5.2f%%)\n",
        i, si, d, dec.health, (int)dec.MAX_HEALTH,
        dec.health * 100. / dec.MAX_HEALTH);
}
#endif

// SPDX-FileCopyrightText: 2021 Jeff Epler
//
// SPDX-License-Identifier: GPL-3.0-only

#define PIN_OUT (12)
#define PIN_PDN (10)
#define PIN_MON (5)
#include <algorithm>
#include <atomic>

#include "SAMDTimerInterrupt.h"
#include "SAMD_ISR_Timer.h"

#include "decoder.h"

#define MONITOR_LL (0)
#define MONITOR_SYM (0)

#define AUTO_STEERING (1)

// SAMD51 Hardware Timer only TC3
// We override this below, but the value puts the predivider into a good range
// for us (/8 prescaler)
constexpr int TIMER0_INTERVAL_MS = 20;
SAMDTimer ITimer0(TIMER_TC3);

int cc;

WWVBDecoder<> dec;
constexpr int CENTRAL_COUNT = 3000000 / dec.SUBSEC;
static_assert(CENTRAL_COUNT <= 65535);

struct Critical {
    Critical() { noInterrupts(); }
    ~Critical() { interrupts(); }
};

typedef void (*work)();
struct Work {
    int head, tail;
    static constexpr int size = 6;
    work todo[size];

    void put(work fun) {
        todo[head] = fun;
        head = (head + 1) % size;
        if (head == tail)
            abort();
    }

    work take() {
        Critical _;
        work result = nullptr;
        if (tail != head) {
            result = todo[tail];
            tail = (tail + 1) % size;
        }
        return result;
    }

    bool empty() {
        Critical _;
        return tail == head;
    }
} wq;

static void tick();
static void try_decode();

std::atomic<int> introduced_error;

void TimerHandler0(void) {
    int i = digitalRead(PIN_OUT);
    digitalWrite(PIN_MON, HIGH);
    digitalWrite(PIN_MON, LOW);
    TC3->COUNT16.CC[0].reg = cc;
#if MONITOR_LL
    putc(i ? '#' : '_');
#endif
    if (introduced_error.load()) {
        introduced_error.fetch_sub(1);
        return;
    }
    if (dec.update(i)) {
        wq.put(try_decode);
    }
    auto subsec = mod_diff<dec.SUBSEC>(dec.sos, dec.SUBSEC - 5);
    if (subsec == 0) {
        wq.put(tick);
    }
}

void moveto(int x, int y) { printf("\033[%d;%dH", y, x); }

int ss_I, ss_P;

void set_tc(int n, bool hold) {
    moveto(1, 25);
    // max 1% adjustment
    if (n > CENTRAL_COUNT / 100)
        n = CENTRAL_COUNT / 100;
    if (n < -(int)CENTRAL_COUNT / 100)
        n = -(int)CENTRAL_COUNT / 100;
    cc = CENTRAL_COUNT + n;
    fprintf(stderr, "Steer %+4d CC = %5d I = %+5d P=%+5d %.4s\n", n, cc, ss_I,
            ss_P, hold ? "HOLD" : "INTG");
}

void steer_tc(int delta) { set_tc(cc - CENTRAL_COUNT + delta, false); }

bool ever_set;
wwvb_time w;

void display_time() {
    time_t now = w.to_utc();

    struct tm tm;
    gmtime_r(&now, &tm);

    moveto(1, 1);
    char buf[32];
    strftime(buf, sizeof(buf), "%FT%TZ", &tm);
    printf("%s ", buf);

    tm = w.apply_zone_and_dst(6, true);
    strftime(buf, sizeof(buf), "%FT%T", &tm);
    printf("%s C%cT ls=%d ly=%d dst=%d dut1=%+2d", buf, tm.tm_isdst ? 'D' : 'S',
           w.ls, w.ly, w.dst, w.dut1);
}

void loop() {
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c == 'x') {
            introduced_error.fetch_add(5);
        }
#if !AUTO_STEERING
        if (c == '+' || c == '=') {
            steer_tc(1);
        }
        if (c == '-') {
            steer_tc(-1);
        }
        if (c == '0') {
            set_tc(0);
        }
#endif
    }

    {
        Critical _;

        if (wq.empty()) {
            __WFI();
            return;
        }
    }

    digitalWrite(PIN_LED, HIGH);
    wq.take()();
    digitalWrite(PIN_LED, LOW);
}

void try_decode() {
    decltype(dec) snapshot;
    {
        Critical _;
        snapshot = dec;
    }

#if AUTO_STEERING
    // Try to steer the start-of-subsec to the "0" value
    // This is a simple PI control, which should
    // settle with almost no phase error. (or, more likely, oscillate
    // around two nearby values)
    bool hold = snapshot.health < snapshot.HEALTH_97PCT;
    ss_P = mod_diff<snapshot.SUBSEC>(snapshot.sos, 0);
    if (!hold) {
        ss_I += ss_P;
    }
    set_tc(ss_P * 4 + ss_I / 20, hold);
#endif

    {
#define ROWS (22)
        char screen[ROWS][snapshot.SUBSEC];
        memset(screen, ' ', sizeof(screen));

        int max_counts = 0;
        int max_edges = 0;
        for (int i = 0; i < snapshot.SUBSEC; i++) {
            max_counts = std::max(max_counts, (int)snapshot.counts[i]);
            max_edges = std::max(max_edges, (int)abs(snapshot.edges[i]));
        }
        for (int i = 0; i < ROWS; i++) {
            screen[i][snapshot.sos] = '.';
        }
        for (int i = 0; i < snapshot.SUBSEC; i++) {
            int j = i;
            while (j >= snapshot.SUBSEC)
                j -= snapshot.SUBSEC;

            {
                int r =
                    ROWS / 2 - snapshot.edges[j] * (ROWS - 1) / max_edges / 2;
                screen[r][i] = '_';
            }

            {
                int r = ROWS - 1 -
                        snapshot.counts[j] * ROWS /
                            (1 + snapshot.BUFFER / snapshot.SUBSEC);
                screen[r][i] = '#';
            }
        }

        moveto(1, 2);
        for (int i = 0; i < ROWS; i++) {
            printf("%.*s|\n", snapshot.SUBSEC, screen[i]);
        }
    }

    {
        char buf[snapshot.SYMBOLS];
        for (int i = 0; i < sizeof(buf); i++) {
            buf[i] = '0' + snapshot.symbols.at(i);
        }
        moveto(1, 25);
        printf("%.*s health=%3d%%", sizeof(buf), buf,
               snapshot.health * 100 / snapshot.MAX_HEALTH);
    }

    if (snapshot.symbols.at(snapshot.SYMBOLS - 1) == 2) {
        if (snapshot.decode_minute(w)) {
            w.advance_minutes();
            ever_set = true;
        }
    }

    if (ever_set) {
        tick();
    }
}

void tick() {
    w.advance_seconds();
    display_time();
}

extern "C" int write(int file, char *ptr, int len);
void setup() {
    pinMode(PIN_PDN, OUTPUT);
    pinMode(PIN_MON, OUTPUT);
    pinMode(PIN_LED, OUTPUT);
    pinMode(PIN_OUT, INPUT_PULLUP);
    digitalWrite(PIN_PDN, 0);
    Serial.begin(115200);
    while (!Serial) { /* wait for connect */
    }

    ITimer0.attachInterruptInterval(TIMER0_INTERVAL_MS * 1000, TimerHandler0);

    // 59999 counts [the value you get with the above interval] is about
    // 300ppm off on my HW.
    // 300ppm _slow_ on my HW.  Each adjustment step is about 16.67ppm.
    // Making the number _BIGGER_ makes the `sos` value decrease over time
    // (because it makes the interrupt _slower_)
    // and making it _SMALLER_ makes the `sos` value incrase over time.
    // (making the interrupt _faster_)
    set_tc(0, false);
    printf("\033[2J");
}

// This bridges from stdio output to Serial.write
#include <errno.h>
#undef errno
extern int errno;
extern "C" int _write(int file, char *ptr, int len);
int _write(int file, char *ptr, int len) {
    if (file < 1 || file > 3) {
        errno = EBADF;
        return -1;
    }

    if (file == 3) { // File 3 does not do \n -> \r\n transformation
        Serial.write(ptr, len);
        return len;
    }

    // color stderr bold
    static bool red;
    bool is_stderr = (file == 2);
    if (is_stderr != red) {
        if (is_stderr) {
            Serial.write("\033[95m");
        } else {
            Serial.write("\033[0m");
        }
        red = is_stderr;
    }

    int result = len;
    for (; len--; ptr++) {
        int c = *ptr;
        if (c == '\n')
            Serial.write('\r');
        Serial.write(c);
    }
    return result;
}

extern "C" int write(int file, char *ptr, int len);
int write(int file, char *ptr, int len) __attribute__((alias("_write")));

// SPDX-FileCopyrightText: 2021 Jeff Epler
//
// SPDX-License-Identifier: GPL-3.0-only

#define PIN_OUT (12)
#define PIN_PDN (10)

#include <atomic>

#include "SAMDTimerInterrupt.h"
#include "SAMD_ISR_Timer.h"

#include "decoder.h"

// SAMD51 Hardware Timer only TC3
constexpr int TIMER0_INTERVAL_MS = 20;
SAMDTimer ITimer0(TIMER_TC3);

std::atomic<int> symbol_count;

void TimerHandler0(void) {
    int i = digitalRead(PIN_OUT);
    Serial.write(i ? '#' : '_');
    if (update(i)) {
        int sym = decode_symbol();
        symbols.put(sym);
        symbol_count.fetch_add(1);
    }
}

int last_count = 0;
void loop() {
    while (Serial.available() > 0)
        Serial.read();

    int new_count = symbol_count.load();
    if (new_count == last_count) {
        return;
    }

    int delta = new_count - last_count;

    circular_symbol_array<SYMBOLS, 2> symbols_snapshot;
    int16_t counts_snapshot[SUBSEC], edges_snapshot[SUBSEC];
    int sos_snapshot;
    noInterrupts();
    memcpy(&symbols_snapshot, &symbols, sizeof(symbols_snapshot));
    memcpy(counts_snapshot, counts, sizeof(counts_snapshot));
    memcpy(edges_snapshot, edges, sizeof(edges_snapshot));
    sos_snapshot = sos;
    interrupts();

    Serial.write('\t');
    for (int i = 0; i < delta; i++) {
        Serial.write('0' + symbols_snapshot.at(SYMBOLS - delta + i));
    }
    Serial.write(" ");
    Serial.print(sos_snapshot, DEC);
    Serial.write("\r\n");
    if (symbols_snapshot.at(SYMBOLS - 1) == 2) {
        wwvb_minute w;
        if (decode_minute(symbols_snapshot, w)) {
            Serial.write(" ");
            Serial.print(w.year + 2000, DEC);
            Serial.print("-");
            Serial.print(w.yday, DEC);
            Serial.print("T");
            Serial.print(w.hour, DEC);
            Serial.print(":");
            Serial.print(w.minute, DEC);
            Serial.print(" ");

            time_t now = wwvb_to_utc(w);
            Serial.print((unsigned)(now >> 32), HEX);
            Serial.print((unsigned)(now), HEX);
            Serial.print(" ");

            struct tm tm;
            gmtime_r(&now, &tm);

            char buf[32];
            strftime(buf, sizeof(buf), "%FT%RZ", &tm);
            Serial.println(buf);
        }
    }
    last_count = new_count;
}

void setup() {
    pinMode(PIN_PDN, OUTPUT);
    pinMode(PIN_OUT, INPUT_PULLUP);
    digitalWrite(PIN_PDN, 0);
    Serial.begin(115200);
    while (!Serial) { /* wait for connect */
    }
    Serial.println("hello world");

    wwvb_minute w = {};
    w.year = 21;
    w.yday = 301;
    w.hour = 19;
    w.minute = 20;
    time_t now = wwvb_to_utc(w);
    Serial.print((unsigned)(now >> 32), HEX);
    Serial.print("|");
    Serial.print((unsigned)(now), HEX);
    Serial.print(" ");

    struct tm tm;
    gmtime_r(&now, &tm);

    char buf[32];
    strftime(buf, sizeof(buf), "%FT%RZ", &tm);
    Serial.println(buf);

    ITimer0.attachInterruptInterval(TIMER0_INTERVAL_MS * 1000, TimerHandler0);
}

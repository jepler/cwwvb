<!--
SPDX-FileCopyrightText: 2021 Jeff Epler

SPDX-License-Identifier: GPL-3.0-only
-->

# CWWVB: A WWVB signal decoder in C++

WWVB is a radio time signal broadcast from near Fort Collins, Colorado, USA. It
can be received across most of the continental USA with a ferrite antenna and a
dedicated receiver board.  However, the receiver's output is simply the
amplitude modulation converted to logic level, and it's necessary to decode it
first into symbols (0 / 1 / Mark) and then into timestamps.

CWWVB implements a modestly CPU-hungry decoder that is targeted at e.g.,
Cortex-M microcontrollers in wall-powered environments.  It aims to
successfully and reliably decode the time signal whenever there is good quality
data from the receiver, and to reliably *NOT* perform incorrect decoding when
the signal is poor.

The CWWVB algorithm contrasts to other algorithms I've read about (&
implemented myself) as it uses statistics to continuously track the estimated
WWVB start of second (SoS), rather than working by 'pulsein' (strictly
measuring the length of pulses) or having separate modes for aligning to SoS &
actually decoding WWVB symbols.

# Technique

Decoding takes place in several steps.  First, the start-of-second is recovered:

 * The AM signal is sampled 50 times per second.  The samples are stored in a circular buffer.
   Arbitrarily, the "reduced carrier" is indicated by storing a 1, and the "full carrier" by storing a 0.
 * A 50-bucket array of counts is maintained by adding the most recently
   received sample to its bucket, and subtracting the signal that 'fell out'.
 * A 50-bucket array of edge-counts is likewise maintained, holding the difference between the previous bucket's count
   and this bucket's count.

The index of the edge-counts array with the highest (positive) value indicates the start of the WWVB second.
Currently this index is found by simply looping over all elements of the edge
counts array, though perhaps a more efficient algorithm could be employed.
(something like a binary tree structure to point to the larger child element,
but can be incrementally updated)

This means CWWVB doesn't find the onset of a second with more than about 40ms of
accuracy, but as the common receiver introduces a phase shift that varies
between 40ms and 80ms (typical), trying to do better is actually counterproductive.

Next, when a new second is reached, CWWVB looks back at the previous second's
data and decode a 0/1/Mark.  The Amplitude signal divides the second into 4 pieces:
 * 200ms when the carrier amplitude is always reduced
 * 300ms when the carrier amplitude is restored for 0, but remains reduced for 1 and Mark (call this 300ms "A")
 * 300ms when the carrier amplitude is restored for 0 or 1, but remains reduced for Mark (call this 300ms "B")
 * 200ms when the carrier amplitide is always restored

This translates to the following algorithm:
 * If the number of reduced-carrier samples during "B" is above a threshold, receive a "M"
 * Else if the number of reduced-carrier samples during "A" is above a threshold, receive a "1"
 * Else receive 0

The Mark symbols are placed at set positions within a minute, and the 59th second of a minute is one of the marks.
So anytime a mark is received, it's possible a WWVB minute has completed.  When this happens, the next task is to
look for the other 6 marks.  If those are all present, then a one-minute signal can be decoded.

... and that's what is implemented so far in `decoder.cc`.

(note that there's nothing special about 1/50s, it's simply the value I chose
in the [WWVB
Observatory](https://github.com/wwvb-observatory/wwvb-observatory). This means
I can feed my test program WWVB Observatory data and analyze its performance.)

# Next steps

 * If a time estimate is known, the received minute can be compared against it for plausibility
 * If no time estimate is known, two consecutive minutes can be compared for plausibility
 * The `edges` values can be used as a quality indicator. In particular, the number of positive transitions that are not +/-2 buckets from the start-of-second increase as the signal gets noisier
 * Add full checking for must-be-zero bits, invalid BCD values, etc.
 * Add local timekeeping code & a display

# Potential problems

The start-of-second bucket will naturally move by +-1 bucket during normal reception, both because the local microcontroller's "1/50s" sample rate will have accumulating error and because signal strength fluctuations and the 0/1/M balance alter the phase shift of the receiver.

This seems likely to be able to introduce & remove extra "start of second" signals. For instance, say that SoS is currently
40 but receiving a new sample into bucket 41 moves it to 41. Now, two "start of second" signals will have been issued 1/50s
apart. Similarly, it seems possible to miss a whole second when SoS decreases (e.g., from 41 to 40).

(Finally, if the signal is ALL noise, then SoS could change by an arbitrary distance, as all the counts would be close to equal and all the edge counts close to 0)

This is fine while decoding WWVB signals; the resulting 60 WWVB symbols would not have valid marker bits. However, if it's desired to use SoS for local timekeeping this would need to be addressed.

# Application to similar time signals

Similar AM time signals in the 40-100kHz range include MSF (Britian), JJY40/60
(Japan), and DCF77 (Germany).  The SoS-recovery code should work with all these
signals, but the further layers of symbol-decoding and minute-decoding would
differ.

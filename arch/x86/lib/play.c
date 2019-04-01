/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Aural kernel panic - x86 implementation
 *
 * Copyright (C) 2019 Matteo Croce <mcroce@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/delay.h>
#include <linux/timex.h>
#include <linux/i8253.h>
#include <linux/play.h>
#include <asm/io.h>

#define CONTROL_WORD_REG	0x43
#define COUNTER2		0x42
#define SPEAKER_PORT		0x61

void arch_play_one(unsigned int ms, unsigned int hz)
{
	unsigned long flags;
	u8 p61;

	/* filter out non audible by humans freqs */
	if (hz >= 16 && hz <= 22000) {
		unsigned int count = PIT_TICK_RATE / hz;

		raw_spin_lock_irqsave(&i8253_lock, flags);

		/* set buzzer
		 * 0xB6
		 * 1 0		Counter 2
		 * 1 1		2xRD/2xWR bits 0..7, 8..15 of counter value
		 * 0 1 1	Mode 3: Square Wave
		 * 0		Counter is a 16 bit binary counter
		 */
		outb_p(0xB6, CONTROL_WORD_REG);

		/* select desired HZ with two writes in counter 2, port 42h */
		outb_p(count, COUNTER2);
		outb_p(count >> 8, COUNTER2);

		/* start beep
		 * set bit 0-1 (0: SPEAKER DATA; 1: OUT2) of GATE2 (port 61h)
		 */
		p61 = inb_p(SPEAKER_PORT);
		if ((p61 & 3) != 3)
			outb_p(p61 | 3, SPEAKER_PORT);

		raw_spin_unlock_irqrestore(&i8253_lock, flags);
	}

	msleep(ms * 9 / 10);

	raw_spin_lock_irqsave(&i8253_lock, flags);

	/* stop beep
	 * clear bit 0-1 of port 61h
	 */
	p61 = inb_p(SPEAKER_PORT);
	if (p61 & 3)
		outb(p61 & 0xFC, SPEAKER_PORT);

	raw_spin_unlock_irqrestore(&i8253_lock, flags);

	msleep(ms / 10);
}

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * play.h - Definitions and headers for the aural error reporting framework.
 *
 * Copyright (C) 2019 Matteo Croce <mcroce@redhat.com>
 *
 * This file is released under the GPLv2.
 */

#ifndef _LINUX_PLAY_H
#define _LINUX_PLAY_H

#ifdef CONFIG_PLAY_LIB

struct note {
	unsigned int freq;
	unsigned int dur;
};

void arch_play_one(unsigned int ms, unsigned int hz);

#define play(notes, len) do {						\
		int i;							\
		for (i = 0; i < len; i++)				\
			arch_play_one(notes[i].dur, notes[i].freq);	\
	} while (0)

#else

#define play(n, l)

#endif

#endif

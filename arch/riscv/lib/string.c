// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/riscv/lib/string.c
 *
 * Copyright (C) 2021 Matteo Croce
 *
 * string functions optimized for 64 bit hardware which doesn't
 * handle unaligned memory accesses efficiently.
 *
 * May be freely distributed as part of Linux.
 */

#include <linux/types.h>
#include <linux/module.h>

union types {
	u8 *u8;
	u16 *u16;
	u32 *u32;
	u64 *u64;
	uintptr_t ptr;
};

union ctypes {
	const u8 *u8;
	const u16 *u16;
	const u32 *u32;
	const u64 *u64;
	uintptr_t ptr;
};

/**
 * memcpy - Copy one area of memory to another
 * @dest: Where to copy to
 * @src: Where to copy from
 * @count: The size of the area.
 *
 * You should not use this function to access IO space, use memcpy_toio()
 * or memcpy_fromio() instead.
 */
void *memcpy(void *dest, const void *src, size_t count)
{
	static const void *labels[] = {
		&&u64, &&u8, &&u16, &&u8,
		&&u32, &&u8, &&u16, &&u8,
	};
	union types d = { .u8 = dest };
	union ctypes s = { .u8 = src };
#ifdef HAVE_EFFICIENT_UNALIGNED_ACCESS
	int distance = 0;
#else
	const int mask = BITS_PER_LONG / 8 - 1;
	int distance = (src - dest) & 7;

	for (; count && d.ptr & s.ptr & mask; count--)
		*d.u8++ = *s.u8++;
#endif

	goto *labels[distance];

u64:
#if BITS_PER_LONG == 64
	for (; count >= 8; count -= 8)
		*d.u64++ = *s.u64++;
#endif

u32:
	for (; count >= 4; count -= 4)
		*d.u32++ = *s.u32++;

u16:
	for (; count >= 2; count -= 2)
		*d.u16++ = *s.u16++;

u8:
	while (count--)
		*d.u8++ = *s.u8++;

	return dest;
}
EXPORT_SYMBOL(memcpy);

void *__memcpy(void *dest, const void *src, size_t count)
{
	return memcpy(dest, src, count);
}
EXPORT_SYMBOL(__memcpy);

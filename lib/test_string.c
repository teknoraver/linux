// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>

static __init int memset16_selftest(void)
{
	unsigned i, j, k;
	u16 v, *p;

	p = kmalloc(256 * 2 * 2, GFP_KERNEL);
	if (!p)
		return -1;

	for (i = 0; i < 256; i++) {
		for (j = 0; j < 256; j++) {
			memset(p, 0xa1, 256 * 2 * sizeof(v));
			memset16(p + i, 0xb1b2, j);
			for (k = 0; k < 512; k++) {
				v = p[k];
				if (k < i) {
					if (v != 0xa1a1)
						goto fail;
				} else if (k < i + j) {
					if (v != 0xb1b2)
						goto fail;
				} else {
					if (v != 0xa1a1)
						goto fail;
				}
			}
		}
	}

fail:
	kfree(p);
	if (i < 256)
		return (i << 24) | (j << 16) | k | 0x8000;
	return 0;
}

static __init int memset32_selftest(void)
{
	unsigned i, j, k;
	u32 v, *p;

	p = kmalloc(256 * 2 * 4, GFP_KERNEL);
	if (!p)
		return -1;

	for (i = 0; i < 256; i++) {
		for (j = 0; j < 256; j++) {
			memset(p, 0xa1, 256 * 2 * sizeof(v));
			memset32(p + i, 0xb1b2b3b4, j);
			for (k = 0; k < 512; k++) {
				v = p[k];
				if (k < i) {
					if (v != 0xa1a1a1a1)
						goto fail;
				} else if (k < i + j) {
					if (v != 0xb1b2b3b4)
						goto fail;
				} else {
					if (v != 0xa1a1a1a1)
						goto fail;
				}
			}
		}
	}

fail:
	kfree(p);
	if (i < 256)
		return (i << 24) | (j << 16) | k | 0x8000;
	return 0;
}

static __init int memset64_selftest(void)
{
	unsigned i, j, k;
	u64 v, *p;

	p = kmalloc(256 * 2 * 8, GFP_KERNEL);
	if (!p)
		return -1;

	for (i = 0; i < 256; i++) {
		for (j = 0; j < 256; j++) {
			memset(p, 0xa1, 256 * 2 * sizeof(v));
			memset64(p + i, 0xb1b2b3b4b5b6b7b8ULL, j);
			for (k = 0; k < 512; k++) {
				v = p[k];
				if (k < i) {
					if (v != 0xa1a1a1a1a1a1a1a1ULL)
						goto fail;
				} else if (k < i + j) {
					if (v != 0xb1b2b3b4b5b6b7b8ULL)
						goto fail;
				} else {
					if (v != 0xa1a1a1a1a1a1a1a1ULL)
						goto fail;
				}
			}
		}
	}

fail:
	kfree(p);
	if (i < 256)
		return (i << 24) | (j << 16) | k | 0x8000;
	return 0;
}

static __init int strchr_selftest(void)
{
	const char *test_string = "abcdefghijkl";
	const char *empty_string = "";
	char *result;
	int i;

	for (i = 0; i < strlen(test_string) + 1; i++) {
		result = strchr(test_string, test_string[i]);
		if (result - test_string != i)
			return i + 'a';
	}

	result = strchr(empty_string, '\0');
	if (result != empty_string)
		return 0x101;

	result = strchr(empty_string, 'a');
	if (result)
		return 0x102;

	result = strchr(test_string, 'z');
	if (result)
		return 0x103;

	return 0;
}

static __init int strnchr_selftest(void)
{
	const char *test_string = "abcdefghijkl";
	const char *empty_string = "";
	char *result;
	int i, j;

	for (i = 0; i < strlen(test_string) + 1; i++) {
		for (j = 0; j < strlen(test_string) + 2; j++) {
			result = strnchr(test_string, j, test_string[i]);
			if (j <= i) {
				if (!result)
					continue;
				return ((i + 'a') << 8) | j;
			}
			if (result - test_string != i)
				return ((i + 'a') << 8) | j;
		}
	}

	result = strnchr(empty_string, 0, '\0');
	if (result)
		return 0x10001;

	result = strnchr(empty_string, 1, '\0');
	if (result != empty_string)
		return 0x10002;

	result = strnchr(empty_string, 1, 'a');
	if (result)
		return 0x10003;

	result = strnchr(NULL, 0, '\0');
	if (result)
		return 0x10004;

	return 0;
}

static __init int strspn_selftest(void)
{
	static const struct strspn_test {
		const char str[16];
		const char accept[16];
		const char reject[16];
		unsigned a;
		unsigned r;
	} tests[] __initconst = {
		{ "foobar", "", "", 0, 6 },
		{ "abba", "abc", "ABBA", 4, 4 },
		{ "abba", "a", "b", 1, 1 },
		{ "", "abc", "abc", 0, 0},
	};
	const struct strspn_test *s = tests;
	size_t i, res;

	for (i = 0; i < ARRAY_SIZE(tests); ++i, ++s) {
		res = strspn(s->str, s->accept);
		if (res != s->a)
			return 0x100 + 2*i;
		res = strcspn(s->str, s->reject);
		if (res != s->r)
			return 0x100 + 2*i + 1;
	}
	return 0;
}

#ifdef CONFIG_STRING_SELFTEST_BENCHMARK

#define MEMCPY_SIZE (4 * 1024 * 1024)
#define MEMCPYS	100

static __init int memcpy_selftest_align(bool unalign)
{
	ktime_t start, end;
	s64 elapsed_ns, total_ns = 0;
	char *buf1;
	char *buf2;
	int ret = 0;

	buf1 = kzalloc(MEMCPY_SIZE, GFP_KERNEL);
	if (!buf1)
		return -ENOMEM;

	buf2 = kzalloc(MEMCPY_SIZE, GFP_KERNEL);
	if (!buf2) {
		ret = -ENOMEM;
		goto out_free;
	}

	for (int i = 0; i < MEMCPYS; i++) {
		preempt_disable();
		start = ktime_get();
		memcpy(buf1 + unalign, buf2, MEMCPY_SIZE - unalign);
		end = ktime_get();
		preempt_enable();
		elapsed_ns = ktime_to_ns(ktime_sub(end, start));
		total_ns += elapsed_ns;
		cond_resched();
	}

	printk(KERN_INFO "memcpy: %saligned copy of %lu MBytes in %lld msecs (%lld MB/s)\n",
	       unalign ? "un" : "",
	       (unsigned long)(MEMCPYS * MEMCPY_SIZE) / (1024 * 1024),
	       total_ns / 1000000,
	       ((s64)MEMCPYS * MEMCPY_SIZE * 1000000000LL / max(total_ns, 1LL)) / (1024 * 1024));

	kfree(buf2);

out_free:
	kfree(buf1);

	return ret;
}

static __init int memcpy_selftest(void)
{
	int ret = 0;

	ret = memcpy_selftest_align(false);
	if (ret)
		return ret;

	return memcpy_selftest_align(true);
}

#define OVERLAP 1024

static __init int memmove_selftest_align(bool unalign)
{
	ktime_t start, end;
	s64 elapsed_ns, total_ns = 0;
	char *buf;
	int ret = 0;

	buf = kzalloc(MEMCPY_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (int i = 0; i < MEMCPYS; i++) {
		preempt_disable();
		start = ktime_get();
		memmove(buf + OVERLAP + unalign, buf, MEMCPY_SIZE - OVERLAP - unalign);
		end = ktime_get();
		preempt_enable();
		elapsed_ns = ktime_to_ns(ktime_sub(end, start));
		total_ns += elapsed_ns;
		cond_resched();
	}

	printk(KERN_INFO "memmove: %saligned move of %lu MBytes in %lld msecs (%lld MB/s)\n",
	       unalign ? "un" : "",
	       (unsigned long)(MEMCPYS * (MEMCPY_SIZE - OVERLAP)) / (1024 * 1024),
	       total_ns / 1000000,
	       ((s64)MEMCPYS * (MEMCPY_SIZE - OVERLAP) * 1000000000LL / max(total_ns, 1LL)) / (1024 * 1024));

	kfree(buf);

	return ret;
}

static __init int memmove_selftest(void)
{
	int ret = 0;

	ret = memmove_selftest_align(false);
	if (ret)
		return ret;

	return memmove_selftest_align(true);
}

static __init int memset_selftest_align(bool unalign)
{
	ktime_t start, end;
	s64 elapsed_ns, total_ns = 0;
	char *buf;
	int ret = 0;

	buf = kzalloc(MEMCPY_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (int i = 0; i < MEMCPYS * 4; i++) {
		preempt_disable();
		start = ktime_get();
		memset(buf + unalign, 0, MEMCPY_SIZE - unalign);
		end = ktime_get();
		preempt_enable();
		elapsed_ns = ktime_to_ns(ktime_sub(end, start));
		total_ns += elapsed_ns;
		cond_resched();
	}

	printk(KERN_INFO "memset: %saligned set of %lu MBytes in %lld msecs (%lld MB/s)\n",
	       unalign ? "un" : "",
	       (unsigned long)(MEMCPYS * 4 * MEMCPY_SIZE) / (1024 * 1024),
	       total_ns / 1000000,
	       ((s64)MEMCPYS * 4 * MEMCPY_SIZE * 1000000000LL / max(total_ns, 1LL)) / (1024 * 1024));

	kfree(buf);

	return ret;
}

static __init int memset_selftest(void)
{
	int ret = 0;

	ret = memset_selftest_align(false);
	if (ret)
		return ret;

	return memset_selftest_align(true);
}
#endif

static __exit void string_selftest_remove(void)
{
}

static __init int string_selftest_init(void)
{
	int test, subtest;

	test = 1;
	subtest = memset16_selftest();
	if (subtest)
		goto fail;

	test = 2;
	subtest = memset32_selftest();
	if (subtest)
		goto fail;

	test = 3;
	subtest = memset64_selftest();
	if (subtest)
		goto fail;

	test = 4;
	subtest = strchr_selftest();
	if (subtest)
		goto fail;

	test = 5;
	subtest = strnchr_selftest();
	if (subtest)
		goto fail;

	test = 6;
	subtest = strspn_selftest();
	if (subtest)
		goto fail;

#ifdef CONFIG_STRING_SELFTEST_BENCHMARK
	test = 7;
	subtest = memcpy_selftest();
	if (subtest)
		goto fail;

	test = 8;
	subtest = memmove_selftest();
	if (subtest)
		goto fail;

	test = 9;
	subtest = memset_selftest();
	if (subtest)
		goto fail;
#endif

	pr_info("String selftests succeeded\n");
	return 0;
fail:
	pr_crit("String selftest failure %d.%08x\n", test, subtest);
	return 0;
}

module_init(string_selftest_init);
module_exit(string_selftest_remove);
MODULE_LICENSE("GPL v2");

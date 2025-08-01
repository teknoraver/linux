/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARCH_X86_KVM_REVERSE_CPUID_H
#define ARCH_X86_KVM_REVERSE_CPUID_H

#include <uapi/asm/kvm.h>
#include <asm/cpufeature.h>
#include <asm/cpufeatures.h>

/*
 * Define a KVM-only feature flag.
 *
 * For features that are scattered by cpufeatures.h, __feature_translate() also
 * needs to be updated to translate the kernel-defined feature into the
 * KVM-defined feature.
 *
 * For features that are 100% KVM-only, i.e. not defined by cpufeatures.h,
 * forego the intermediate KVM_X86_FEATURE and directly define X86_FEATURE_* so
 * that X86_FEATURE_* can be used in KVM.  No __feature_translate() handling is
 * needed in this case.
 */
#define KVM_X86_FEATURE(w, f)		((w)*32 + (f))

/* Intel-defined SGX sub-features, CPUID level 0x12 (EAX). */
#define KVM_X86_FEATURE_SGX1		KVM_X86_FEATURE(CPUID_12_EAX, 0)
#define KVM_X86_FEATURE_SGX2		KVM_X86_FEATURE(CPUID_12_EAX, 1)
#define KVM_X86_FEATURE_SGX_EDECCSSA	KVM_X86_FEATURE(CPUID_12_EAX, 11)

/* Intel-defined sub-features, CPUID level 0x00000007:1 (EDX) */
#define X86_FEATURE_AVX_VNNI_INT8       KVM_X86_FEATURE(CPUID_7_1_EDX, 4)
#define X86_FEATURE_AVX_NE_CONVERT      KVM_X86_FEATURE(CPUID_7_1_EDX, 5)
#define X86_FEATURE_AMX_COMPLEX         KVM_X86_FEATURE(CPUID_7_1_EDX, 8)
#define X86_FEATURE_AVX_VNNI_INT16      KVM_X86_FEATURE(CPUID_7_1_EDX, 10)
#define X86_FEATURE_PREFETCHITI         KVM_X86_FEATURE(CPUID_7_1_EDX, 14)
#define X86_FEATURE_AVX10               KVM_X86_FEATURE(CPUID_7_1_EDX, 19)

/* Intel-defined sub-features, CPUID level 0x00000007:2 (EDX) */
#define X86_FEATURE_INTEL_PSFD		KVM_X86_FEATURE(CPUID_7_2_EDX, 0)
#define X86_FEATURE_IPRED_CTRL		KVM_X86_FEATURE(CPUID_7_2_EDX, 1)
#define KVM_X86_FEATURE_RRSBA_CTRL	KVM_X86_FEATURE(CPUID_7_2_EDX, 2)
#define X86_FEATURE_DDPD_U		KVM_X86_FEATURE(CPUID_7_2_EDX, 3)
#define KVM_X86_FEATURE_BHI_CTRL	KVM_X86_FEATURE(CPUID_7_2_EDX, 4)
#define X86_FEATURE_MCDT_NO		KVM_X86_FEATURE(CPUID_7_2_EDX, 5)

/* Intel-defined sub-features, CPUID level 0x00000024:0 (EBX) */
#define X86_FEATURE_AVX10_128		KVM_X86_FEATURE(CPUID_24_0_EBX, 16)
#define X86_FEATURE_AVX10_256		KVM_X86_FEATURE(CPUID_24_0_EBX, 17)
#define X86_FEATURE_AVX10_512		KVM_X86_FEATURE(CPUID_24_0_EBX, 18)

/* CPUID level 0x80000007 (EDX). */
#define KVM_X86_FEATURE_CONSTANT_TSC	KVM_X86_FEATURE(CPUID_8000_0007_EDX, 8)

/* CPUID level 0x80000022 (EAX) */
#define KVM_X86_FEATURE_PERFMON_V2	KVM_X86_FEATURE(CPUID_8000_0022_EAX, 0)

/* CPUID level 0x80000021 (ECX) */
#define KVM_X86_FEATURE_TSA_SQ_NO	KVM_X86_FEATURE(CPUID_8000_0021_ECX, 1)
#define KVM_X86_FEATURE_TSA_L1_NO	KVM_X86_FEATURE(CPUID_8000_0021_ECX, 2)

struct cpuid_reg {
	u32 function;
	u32 index;
	int reg;
};

static const struct cpuid_reg reverse_cpuid[] = {
	[CPUID_1_EDX]         = {         1, 0, CPUID_EDX},
	[CPUID_8000_0001_EDX] = {0x80000001, 0, CPUID_EDX},
	[CPUID_8086_0001_EDX] = {0x80860001, 0, CPUID_EDX},
	[CPUID_1_ECX]         = {         1, 0, CPUID_ECX},
	[CPUID_C000_0001_EDX] = {0xc0000001, 0, CPUID_EDX},
	[CPUID_8000_0001_ECX] = {0x80000001, 0, CPUID_ECX},
	[CPUID_7_0_EBX]       = {         7, 0, CPUID_EBX},
	[CPUID_D_1_EAX]       = {       0xd, 1, CPUID_EAX},
	[CPUID_8000_0008_EBX] = {0x80000008, 0, CPUID_EBX},
	[CPUID_6_EAX]         = {         6, 0, CPUID_EAX},
	[CPUID_8000_000A_EDX] = {0x8000000a, 0, CPUID_EDX},
	[CPUID_7_ECX]         = {         7, 0, CPUID_ECX},
	[CPUID_8000_0007_EBX] = {0x80000007, 0, CPUID_EBX},
	[CPUID_7_EDX]         = {         7, 0, CPUID_EDX},
	[CPUID_7_1_EAX]       = {         7, 1, CPUID_EAX},
	[CPUID_12_EAX]        = {0x00000012, 0, CPUID_EAX},
	[CPUID_8000_001F_EAX] = {0x8000001f, 0, CPUID_EAX},
	[CPUID_7_1_EDX]       = {         7, 1, CPUID_EDX},
	[CPUID_8000_0007_EDX] = {0x80000007, 0, CPUID_EDX},
	[CPUID_8000_0021_EAX] = {0x80000021, 0, CPUID_EAX},
	[CPUID_8000_0022_EAX] = {0x80000022, 0, CPUID_EAX},
	[CPUID_7_2_EDX]       = {         7, 2, CPUID_EDX},
	[CPUID_24_0_EBX]      = {      0x24, 0, CPUID_EBX},
	[CPUID_8000_0021_ECX] = {0x80000021, 0, CPUID_ECX},
};

/*
 * Reverse CPUID and its derivatives can only be used for hardware-defined
 * feature words, i.e. words whose bits directly correspond to a CPUID leaf.
 * Retrieving a feature bit or masking guest CPUID from a Linux-defined word
 * is nonsensical as the bit number/mask is an arbitrary software-defined value
 * and can't be used by KVM to query/control guest capabilities.  And obviously
 * the leaf being queried must have an entry in the lookup table.
 */
static __always_inline void reverse_cpuid_check(unsigned int x86_leaf)
{
	BUILD_BUG_ON(NR_CPUID_WORDS != NCAPINTS);
	BUILD_BUG_ON(x86_leaf == CPUID_LNX_1);
	BUILD_BUG_ON(x86_leaf == CPUID_LNX_2);
	BUILD_BUG_ON(x86_leaf == CPUID_LNX_3);
	BUILD_BUG_ON(x86_leaf == CPUID_LNX_4);
	BUILD_BUG_ON(x86_leaf == CPUID_LNX_5);
	BUILD_BUG_ON(x86_leaf >= ARRAY_SIZE(reverse_cpuid));
	BUILD_BUG_ON(reverse_cpuid[x86_leaf].function == 0);
}

/*
 * Translate feature bits that are scattered in the kernel's cpufeatures word
 * into KVM feature words that align with hardware's definitions.
 */
static __always_inline u32 __feature_translate(int x86_feature)
{
#define KVM_X86_TRANSLATE_FEATURE(f)	\
	case X86_FEATURE_##f: return KVM_X86_FEATURE_##f

	switch (x86_feature) {
	KVM_X86_TRANSLATE_FEATURE(SGX1);
	KVM_X86_TRANSLATE_FEATURE(SGX2);
	KVM_X86_TRANSLATE_FEATURE(SGX_EDECCSSA);
	KVM_X86_TRANSLATE_FEATURE(CONSTANT_TSC);
	KVM_X86_TRANSLATE_FEATURE(PERFMON_V2);
	KVM_X86_TRANSLATE_FEATURE(RRSBA_CTRL);
	KVM_X86_TRANSLATE_FEATURE(BHI_CTRL);
	KVM_X86_TRANSLATE_FEATURE(TSA_SQ_NO);
	KVM_X86_TRANSLATE_FEATURE(TSA_L1_NO);
	default:
		return x86_feature;
	}
}

static __always_inline u32 __feature_leaf(int x86_feature)
{
	u32 x86_leaf = __feature_translate(x86_feature) / 32;

	reverse_cpuid_check(x86_leaf);
	return x86_leaf;
}

/*
 * Retrieve the bit mask from an X86_FEATURE_* definition.  Features contain
 * the hardware defined bit number (stored in bits 4:0) and a software defined
 * "word" (stored in bits 31:5).  The word is used to index into arrays of
 * bit masks that hold the per-cpu feature capabilities, e.g. this_cpu_has().
 */
static __always_inline u32 __feature_bit(int x86_feature)
{
	x86_feature = __feature_translate(x86_feature);

	reverse_cpuid_check(x86_feature / 32);
	return 1 << (x86_feature & 31);
}

#define feature_bit(name)  __feature_bit(X86_FEATURE_##name)

static __always_inline struct cpuid_reg x86_feature_cpuid(unsigned int x86_feature)
{
	unsigned int x86_leaf = __feature_leaf(x86_feature);

	return reverse_cpuid[x86_leaf];
}

static __always_inline u32 *__cpuid_entry_get_reg(struct kvm_cpuid_entry2 *entry,
						  u32 reg)
{
	switch (reg) {
	case CPUID_EAX:
		return &entry->eax;
	case CPUID_EBX:
		return &entry->ebx;
	case CPUID_ECX:
		return &entry->ecx;
	case CPUID_EDX:
		return &entry->edx;
	default:
		BUILD_BUG();
		return NULL;
	}
}

static __always_inline u32 *cpuid_entry_get_reg(struct kvm_cpuid_entry2 *entry,
						unsigned int x86_feature)
{
	const struct cpuid_reg cpuid = x86_feature_cpuid(x86_feature);

	return __cpuid_entry_get_reg(entry, cpuid.reg);
}

static __always_inline u32 cpuid_entry_get(struct kvm_cpuid_entry2 *entry,
					   unsigned int x86_feature)
{
	u32 *reg = cpuid_entry_get_reg(entry, x86_feature);

	return *reg & __feature_bit(x86_feature);
}

static __always_inline bool cpuid_entry_has(struct kvm_cpuid_entry2 *entry,
					    unsigned int x86_feature)
{
	return cpuid_entry_get(entry, x86_feature);
}

static __always_inline void cpuid_entry_clear(struct kvm_cpuid_entry2 *entry,
					      unsigned int x86_feature)
{
	u32 *reg = cpuid_entry_get_reg(entry, x86_feature);

	*reg &= ~__feature_bit(x86_feature);
}

static __always_inline void cpuid_entry_set(struct kvm_cpuid_entry2 *entry,
					    unsigned int x86_feature)
{
	u32 *reg = cpuid_entry_get_reg(entry, x86_feature);

	*reg |= __feature_bit(x86_feature);
}

static __always_inline void cpuid_entry_change(struct kvm_cpuid_entry2 *entry,
					       unsigned int x86_feature,
					       bool set)
{
	u32 *reg = cpuid_entry_get_reg(entry, x86_feature);

	/*
	 * Open coded instead of using cpuid_entry_{clear,set}() to coerce the
	 * compiler into using CMOV instead of Jcc when possible.
	 */
	if (set)
		*reg |= __feature_bit(x86_feature);
	else
		*reg &= ~__feature_bit(x86_feature);
}

#endif /* ARCH_X86_KVM_REVERSE_CPUID_H */

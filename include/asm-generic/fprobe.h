/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * linux/include/asm-generic/fprobe.h
 */
#ifndef __ASM_GENERIC_FPROBE_H__
#define __ASM_GENERIC_FPROBE_H__

#include <linux/bits.h>

#define ARCH_HAVE_ENCODE_FPROBE_HEADER

#define FPROBE_HEADER_MSB_SIZE_SHIFT (BITS_PER_LONG - FPROBE_DATA_SIZE_BITS)
#define FPROBE_HEADER_MSB_MASK					\
	GENMASK(FPROBE_HEADER_MSB_SIZE_SHIFT - 1, 0)
#define FPROBE_HEADER_MSB_PATTERN				\
	GENMASK(BITS_PER_LONG - 1, FPROBE_HEADER_MSB_SIZE_SHIFT)

#define arch_fprobe_header_encodable(fp)			\
	(((unsigned long)(fp) & ~FPROBE_HEADER_MSB_MASK) ==	\
	 FPROBE_HEADER_MSB_PATTERN)

#define arch_encode_fprobe_header(fp, size) 			\
	((unsigned long)(fp) & FPROBE_HEADER_MSB_MASK) | 	\
	((unsigned long)(size) << FPROBE_HEADER_MSB_SIZE_SHIFT)

#define arch_decode_fprobe_header_size(val) 			\
	((unsigned long)(val) >> FPROBE_HEADER_MSB_SIZE_SHIFT)

#define arch_decode_fprobe_header_fp(val) 					\
	(struct fprobe *)(((unsigned long)(val) & FPROBE_HEADER_MSB_MASK) |	\
			  FPROBE_HEADER_MSB_PATTERN)

#endif /* __ASM_GENERIC_FPROBE_H__ */
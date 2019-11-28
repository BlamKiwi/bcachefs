/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_ACCELERATION_H
#define _BCACHEFS_ACCELERATION_H

#include <linux/types.h>

/**
 * Dispatch handlers for underlying storage algorithms to enable ISA-L/Kernel abstraction. 
 */

u64 accel_crc64_be(u64 crc, const void* p, size_t len);
u64 accel_crc32c(u32 crc, const void* p, size_t len);

#endif /* _BCACHEFS_ACCELERATION_H */

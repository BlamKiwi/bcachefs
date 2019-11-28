#include "acceleration.h"

#include <linux/crc64.h>
#include <linux/crc32c.h>
#include <asm/fpu/api.h>
#include "isa-l/include/crc.h"
#include "isa-l/include/crc64.h"

u64 accel_crc64_be(u64 crc, const void* p, size_t len) {
	#ifdef CONFIG_BCACHEFS_ISAL_BACKEND
	// Some implementations may call SIMD code
	kernel_fpu_begin();
	return crc64_ecma_norm(crc, (const unsigned char*)p, len);
	kernel_fpu_end();
	#else
	return crc64(crc, p, len);
	#endif
}

u64 accel_crc32c(u32 crc, const void* p, size_t len) {
	#ifdef CONFIG_BCACHEFS_ISAL_BACKEND
	// Some implementations may call SIMD code
	kernel_fpu_begin();
	return crc32_iscsi((unsigned char *)p, len, crc);
	kernel_fpu_end();
	#else
	return crc32c(crc, p, len);
	#endif
}

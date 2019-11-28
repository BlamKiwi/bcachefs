#include "accel.h"

#include <linux/crc64.h>
#include <linux/crc32c.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/ktime.h>
#include <asm/fpu/api.h>
#include "isa-l/include/crc.h"
#include "isa-l/include/crc64.h"

static u64 kernel_crc64(u64 crc, const void* p, size_t len) {
	return crc64_be(crc, p, len);
}

static u64 isal_crc64(u64 crc, const void* p, size_t len) {
	kernel_fpu_begin();
	return crc64_ecma_norm(crc, (const unsigned char*)p, len);
	kernel_fpu_end();
}

static u32 kernel_crc32c(u32 crc, const void* p, size_t len) {
	return crc32c(crc, p, len);
}

static u32 isal_crc32c(u32 crc, const void* p, size_t len) {
	kernel_fpu_begin();
	return crc32_iscsi((unsigned char *)p, len, crc);
	kernel_fpu_end();
}

u64 accel_crc64(u64 crc, const void* p, size_t len) {
	#ifdef CONFIG_BCACHEFS_ISAL_BACKEND
	return isal_crc64(crc, p, len);
	#else
	return kernel_crc64(crc, p, len);
	#endif
}

u64 accel_crc32c(u32 crc, const void* p, size_t len) {
	#ifdef CONFIG_BCACHEFS_ISAL_BACKEND
	return isal_crc32c(crc, p, len);
	#else
	return kernel_crc32c(crc, p, len);
	#endif
}

static const int CRC_BENCH_SIZE = 1024 * 1024 * 1024; // 1MB
static const int WARMUP_ITER = 3;
static const int BENCH_ITER = 11;
static const int NS_IN_S = 1000000000;

static void bench_crc32c(u32(*f)(u32, const void*, size_t), const char* name) {
	char* buf = vmalloc(CRC_BENCH_SIZE);
	int i;
	u64 quotient;
	u64 remainder;
	u64 sum = 0;
	
	for(i = 0; i < CRC_BENCH_SIZE; i++) {
		buf[i] = (char)i;
	}

	for(i = 0; i < WARMUP_ITER; i++) {
		f(~0, buf, CRC_BENCH_SIZE);
	}

	for(i = 0; i < BENCH_ITER; i++) {
		u64 begin;
		u64 end;

		begin = ktime_get_ns();
		f(~0, buf, CRC_BENCH_SIZE);
		end = ktime_get_ns();

		sum += end - begin;
	}

	sum /= 11;
	quotient = sum / NS_IN_S;
	remainder = sum % NS_IN_S;

	printk("%s: %llu.%llu MiB/s. Average Time: %llu", name, quotient, remainder, sum);

	vfree(buf);
}

static void bench_crc64(u64(*f)(u64, const void*, size_t), const char* name) {
	char* buf = vmalloc(CRC_BENCH_SIZE);
	int i;
	u64 quotient;
	u64 remainder;
	u64 sum = 0;
	
	for(i = 0; i < CRC_BENCH_SIZE; i++) {
		buf[i] = (char)i;
	}

	for(i = 0; i < WARMUP_ITER; i++) {
		f(~0, buf, CRC_BENCH_SIZE);
	}

	for(i = 0; i < BENCH_ITER; i++) {
		u64 begin;
		u64 end;

		begin = ktime_get_ns();
		f(~0, buf, CRC_BENCH_SIZE);
		end = ktime_get_ns();

		sum += end - begin;
	}

	sum /= 11;
	sum *= sum * NS_IN_S;
	quotient = sum / NS_IN_S;
	remainder = sum % NS_IN_S;

	printk("%s: %llu.%llu MiB/s. Average Time: %llu", name, quotient, remainder, sum);

	vfree(buf);
}


int accel_benchmark(const char* prim) {
	int crc64 = 0;
	int crc32c = 0;
	int ret = -EINVAL;

	if(strcmp(prim, "all") == 0) {
		crc32c = 1;
		crc64 = 1;
		ret = 0;
	} else if (strcmp(prim, "crc32c") == 0) {
		crc32c = 1;
		ret = 0;
	} else if (strcmp(prim, "crc64") == 0) {
		crc64 = 1;
		ret = 0;
	}

	if(crc32c) {
		bench_crc32c(&kernel_crc32c, "kernel crc32c");
		#ifdef CONFIG_BCACHEFS_ISAL_BACKEND
		bench_crc32c(&isal_crc32c, "isal crc32c");
		#endif
	}

	if(crc64) {
		bench_crc64(&kernel_crc64, "kernel crc64");
		#ifdef CONFIG_BCACHEFS_ISAL_BACKEND
		bench_crc64(&isal_crc64, "isal crc64");
		#endif
	}

	return ret;
}

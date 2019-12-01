#include "accel.h"

#include <linux/crc64.h>
#include <linux/crc32c.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/ktime.h>
#include <asm/fpu/api.h>

#ifdef CONFIG_BCACHEFS_ISAL_BACKEND
#include "isal/include/crc.h"
#include "isal/include/crc64.h"
#endif

static u64 kernel_crc64(u64 crc, const void* p, size_t len) {
	return crc64_be(crc, p, len);
}

static u64 isal_crc64(u64 crc, const void* p, size_t len) { 
	u64 state = ~crc;

	kernel_fpu_begin();
	state =  crc64_ecma_norm(state, (const unsigned char*)p, len);
	kernel_fpu_end();

	return ~state;
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
	return kernel_crc64(crc, p, len);

	#ifdef CONFIG_BCACHEFS_ISAL_BACKEND
	return isal_crc64(crc, p, len);
	#else
	return kernel_crc64(crc, p, len);
	#endif
}

u64 accel_crc32c(u32 crc, const void* p, size_t len) {
	return kernel_crc32c(crc, p, len);

	#ifdef CONFIG_BCACHEFS_ISAL_BACKEND
	return isal_crc32c(crc, p, len);
	#else
	return kernel_crc32c(crc, p, len);
	#endif
}

static const int MB = 1024 * 1024;
static const int LARGE_BLOCK = 2 * MB; // Filesystem large IOs (Media etc)
static const int SMALL_BLOCK = 4096; // Filesystem small IOs (Databases etc)
static const int CACHE_THRASH = 512 * MB; // Larger than Epyc ROME L3 Cache
static const int WARMUP_ITER = 3;
static const int BENCH_ITER = 5;

static void bench_crc32c(u32(*f)(u32, const void*, size_t), size_t bench_size, const char* name) {
	char* buf = vmalloc(bench_size);
	int i;
	u64 sum = 0;

	for(i = 0; i < bench_size; i++) {
		buf[i] = (char)i;
	}

	for(i = 0; i < WARMUP_ITER; i++) {
		f(~0, buf, bench_size);
	}

	for(i = 0; i < BENCH_ITER; i++) {
		u64 begin;
		u64 end;
		u64 diff;

		begin = ktime_get_ns();
		f(~0, buf, bench_size);
		end = ktime_get_ns();

		diff = end - begin;
		sum += diff;
	}

	sum /= BENCH_ITER;
	printk("%s: %llu ns\n", name, sum);

	vfree(buf);
}

static void bench_crc64(u64(*f)(u64, const void*, size_t), size_t bench_size, const char* name) {
	char* buf = vmalloc(bench_size);
	int i;
	u64 sum = 0;

	for(i = 0; i < bench_size; i++) {
		buf[i] = (char)i;
	}

	for(i = 0; i < WARMUP_ITER; i++) {
		f(~0, buf, bench_size);
	}

	for(i = 0; i < BENCH_ITER; i++) {
		u64 begin;
		u64 end;
		u64 diff;

		begin = ktime_get_ns();
		f(~0, buf, bench_size);
		end = ktime_get_ns();

		diff = end - begin;
		sum += diff;
	}

	sum /= BENCH_ITER;
	printk("%s: %llu ns\n", name, sum);

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
		bench_crc32c(&kernel_crc32c, CACHE_THRASH, "KERNEL CRC32C 512MB");
		bench_crc32c(&kernel_crc32c, LARGE_BLOCK, "KERNEL CRC32C 2MB");
		bench_crc32c(&kernel_crc32c, SMALL_BLOCK, "KERNEL CRC32C 4KB");

		#ifdef CONFIG_BCACHEFS_ISAL_BACKEND
		bench_crc32c(&isal_crc32c, CACHE_THRASH, "ISAL CRC32C 512MB");
		bench_crc32c(&isal_crc32c, LARGE_BLOCK, "ISAL CRC32C 2MB");
		bench_crc32c(&isal_crc32c, SMALL_BLOCK, "ISAL CRC32C 4KB");
		#endif
	}

	if(crc64) {
		bench_crc64(&kernel_crc64, CACHE_THRASH, "KERNEL CRC64 512MB");
		bench_crc64(&kernel_crc64, LARGE_BLOCK, "KERNEL CRC64 2MB");
		bench_crc64(&kernel_crc64, SMALL_BLOCK, "KERNEL CRC64 4KB");

		#ifdef CONFIG_BCACHEFS_ISAL_BACKEND
		bench_crc64(&isal_crc64, CACHE_THRASH, "ISAL CRC64 512MB");
		bench_crc64(&isal_crc64, LARGE_BLOCK, "ISAL CRC64 2MB");
		bench_crc64(&isal_crc64, SMALL_BLOCK, "ISAL CRC64 4KB");
		
		#endif
	}

	return ret;
}

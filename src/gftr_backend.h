#ifndef GFTR_BACKEND_H
#define GFTR_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Signed int4 keys map to [0, 2^32); SQL NULL sorts last. */
#define GFTR_NULL_KEY (UINT64_C(1) << 32)
#define GFTR_ERROR_SIZE 512

typedef struct GftrValue
{
	int32_t value;
	uint32_t valid;
} GftrValue;

typedef struct GftrColumnStats
{
	int64_t count;
	int64_t sum;
	int32_t min;
	int32_t max;
} GftrColumnStats;

typedef struct GftrInput
{
	int32_t nrows;
	int32_t ncols;
	const uint64_t *keys;
	GftrValue **columns;
} GftrInput;

typedef struct GftrResult
{
	int32_t ngroups;
	int32_t ncols;
	uint64_t *keys;
	int32_t *counts;
	GftrColumnStats **columns;
	double sort_ms;
	double reduce_ms;
	double transfer_ms;
	double total_ms;
	char device_name[128];
} GftrResult;

int gftr_result_alloc(GftrResult *result, int32_t capacity, int32_t ncols);
void gftr_result_free(GftrResult *result);
int gftr_cpu_compute(const GftrInput *input, GftrResult *result,
					 char error[GFTR_ERROR_SIZE]);
int gftr_cuda_compute(const GftrInput *input, int use_gftr, int device,
					  size_t max_device_bytes, GftrResult *result,
					  char error[GFTR_ERROR_SIZE]);

#ifdef __cplusplus
}
#endif
#endif

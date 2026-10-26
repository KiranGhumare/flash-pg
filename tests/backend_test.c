#include "gftr_backend.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
	uint64_t keys[] = {GFTR_NULL_KEY, 0, 4, 0, 4, GFTR_NULL_KEY, UINT32_MAX};
	GftrValue a[] = {{8,1}, {INT_MAX,1}, {7,0}, {INT_MIN,1}, {-9,0}, {4,1}, {-3,1}};
	GftrValue b[] = {{0,0}, {4,1}, {6,1}, {5,1}, {7,1}, {0,0}, {2,1}};
	GftrValue *cols[] = {a, b};
	GftrInput input = {7, 2, keys, cols};
	GftrResult result = {0};
	char error[GFTR_ERROR_SIZE] = {0};
	assert(gftr_cpu_compute(&input, &result, error) == 0);
	assert(result.ngroups == 4);
	assert(result.keys[0] == 0 && result.keys[3] == GFTR_NULL_KEY);
	assert(result.counts[0] == 2 && result.counts[3] == 2);
	assert(result.columns[0][0].sum == -1);
	assert(result.columns[0][0].min == INT_MIN);
	assert(result.columns[0][0].max == INT_MAX);
	assert(result.columns[0][1].count == 0);
	assert(result.columns[0][3].sum == 12);
	assert(result.columns[1][1].sum == 13);
	assert(result.columns[1][3].count == 0);
	gftr_result_free(&result);
	input.nrows = 0;
	assert(gftr_cpu_compute(&input, &result, error) == 0);
	assert(result.ngroups == 0);
	gftr_result_free(&result);
	input.nrows = 7;
	input.ncols = 0;
	assert(gftr_cpu_compute(&input, &result, error) == 0);
	assert(result.ngroups == 4 && result.counts[0] == 2);
	gftr_result_free(&result);
	puts("CPU backend edge cases passed");
	return 0;
}

/* A portable correctness oracle. This is not a GPU performance baseline. */
#include "gftr_backend.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct KeyIndex
{
	uint64_t key;
	int32_t index;
} KeyIndex;

static double
now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}

void
gftr_result_free(GftrResult *result)
{
	int c;
	if (result->columns)
		for (c = 0; c < result->ncols; c++)
			free(result->columns[c]);
	free(result->columns);
	free(result->keys);
	free(result->counts);
	memset(result, 0, sizeof(*result));
}

int
gftr_result_alloc(GftrResult *result, int32_t capacity, int32_t ncols)
{
	int c;
	memset(result, 0, sizeof(*result));
	result->ncols = ncols;
	if (capacity == 0)
		return 0;
	result->keys = malloc((size_t) capacity * sizeof(uint64_t));
	result->counts = calloc((size_t) capacity, sizeof(int32_t));
	result->columns = calloc((size_t) (ncols ? ncols : 1), sizeof(void *));
	if (!result->keys || !result->counts || !result->columns)
		goto fail;
	for (c = 0; c < ncols; c++)
	{
		result->columns[c] = calloc((size_t) capacity, sizeof(GftrColumnStats));
		if (!result->columns[c])
			goto fail;
	}
	return 0;
fail:
	gftr_result_free(result);
	return -1;
}

static int
compare_keys(const void *a, const void *b)
{
	const KeyIndex *x = a;
	const KeyIndex *y = b;
	return (x->key > y->key) - (x->key < y->key);
}

int
gftr_cpu_compute(const GftrInput *input, GftrResult *result,
				 char error[GFTR_ERROR_SIZE])
{
	KeyIndex *order;
	int32_t i;
	int32_t group = -1;
	int c;
	double start = now_ms();
	double sorted;
	memset(result, 0, sizeof(*result));
	if (input->nrows < 0 || input->ncols < 0)
	{
		snprintf(error, GFTR_ERROR_SIZE, "invalid input dimensions");
		return -1;
	}
	if (gftr_result_alloc(result, input->nrows, input->ncols))
		goto oom;
	snprintf(result->device_name, sizeof(result->device_name), "CPU reference");
	if (input->nrows == 0)
	{
		result->total_ms = now_ms() - start;
		return 0;
	}
	order = malloc((size_t) input->nrows * sizeof(KeyIndex));
	if (!order)
		goto oom;
	for (i = 0; i < input->nrows; i++)
	{
		order[i].key = input->keys[i];
		order[i].index = i;
	}
	qsort(order, (size_t) input->nrows, sizeof(KeyIndex), compare_keys);
	sorted = now_ms();
	for (i = 0; i < input->nrows; i++)
	{
		if (group < 0 || result->keys[group] != order[i].key)
		{
			group++;
			result->keys[group] = order[i].key;
			for (c = 0; c < input->ncols; c++)
			{
				result->columns[c][group].min = INT_MAX;
				result->columns[c][group].max = INT_MIN;
			}
		}
		result->counts[group]++;
		for (c = 0; c < input->ncols; c++)
		{
			GftrValue v = input->columns[c][order[i].index];
			GftrColumnStats *s = &result->columns[c][group];
			if (v.valid)
			{
				s->count++;
				s->sum += v.value;
				if (v.value < s->min)
					s->min = v.value;
				if (v.value > s->max)
					s->max = v.value;
			}
		}
	}
	free(order);
	result->ngroups = group + 1;
	result->sort_ms = sorted - start;
	result->reduce_ms = now_ms() - sorted;
	result->total_ms = now_ms() - start;
	return 0;
oom:
	snprintf(error, GFTR_ERROR_SIZE, "out of memory in CPU reference backend");
	gftr_result_free(result);
	return -1;
}

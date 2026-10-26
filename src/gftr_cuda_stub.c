#include "gftr_backend.h"
#include <stdio.h>

int
gftr_cuda_compute(const GftrInput *input, int use_gftr, int device,
				  size_t max_device_bytes, GftrResult *result,
				  char error[GFTR_ERROR_SIZE])
{
	(void) input;
	(void) use_gftr;
	(void) device;
	(void) max_device_bytes;
	(void) result;
	snprintf(error, GFTR_ERROR_SIZE,
			 "CUDA backend was not built; rebuild with USE_CUDA=1, "
			 "or explicitly SET pg_gftr.backend = 'cpu'");
	return -1;
}

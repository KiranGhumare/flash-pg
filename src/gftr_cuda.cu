/*
 * CUB stable radix sort + segmented aggregation. GFTR sorts actual payloads
 * with the original keys for every payload column. GFUR sorts row IDs once
 * and reads payloads through that map. No PostgreSQL longjmp crosses C++.
 */
#include "gftr_backend.h"

#include <cub/cub.cuh>
#include <cuda_runtime.h>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(cudaError_t status, const char *operation)
{
    if (status != cudaSuccess)
        throw std::runtime_error(std::string(operation) + ": " +
                                 cudaGetErrorString(status));
}

struct Stream {
    cudaStream_t stream = nullptr;
    Stream() { check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "create stream"); }
    ~Stream() { if (stream) { cudaStreamSynchronize(stream); cudaStreamDestroy(stream); } }
};

struct Events {
    cudaEvent_t begin = nullptr, end = nullptr;
    Events()
    {
        check(cudaEventCreate(&begin), "create event");
        cudaError_t status = cudaEventCreate(&end);
        if (status != cudaSuccess) {
            cudaEventDestroy(begin);
            check(status, "create event");
        }
    }
    ~Events() { cudaEventDestroy(begin); cudaEventDestroy(end); }
    template<class F> double measure(cudaStream_t stream, F fn)
    {
        check(cudaEventRecord(begin, stream), "record event");
        fn();
        check(cudaEventRecord(end, stream), "record event");
        check(cudaEventSynchronize(end), "synchronize event");
        float ms = 0;
        check(cudaEventElapsedTime(&ms, begin, end), "elapsed time");
        return ms;
    }
};

struct Arena {
    std::vector<void *> pointers;
    size_t used = 0, limit;
    explicit Arena(size_t max_bytes) : limit(max_bytes) {}
    ~Arena() { for (auto p : pointers) cudaFree(p); }
    template<class T> T *allocate(size_t count)
    {
        if (count > SIZE_MAX / sizeof(T))
            throw std::runtime_error("device allocation size overflow");
        size_t bytes = count * sizeof(T);
        if (bytes > limit - used)
            throw std::runtime_error("device memory budget exceeded; increase pg_gftr.max_device_mb or use PostgreSQL's native aggregation");
        void *p = nullptr;
        check(cudaMalloc(&p, bytes), "allocate device memory");
        try { pointers.push_back(p); }
        catch (...) { cudaFree(p); throw; }
        used += bytes;
        return static_cast<T *>(p);
    }
};

__global__ void make_ids(int32_t *ids, int n)
{
    for (int64_t i = (int64_t) blockIdx.x * blockDim.x + threadIdx.x;
         i < n; i += (int64_t) blockDim.x * gridDim.x)
        ids[i] = static_cast<int32_t>(i);
}

struct StatsAdd {
    __host__ __device__ GftrColumnStats operator()(GftrColumnStats a,
                                                 GftrColumnStats b) const
    {
        a.count += b.count;
        a.sum += b.sum;
        a.min = a.min < b.min ? a.min : b.min;
        a.max = a.max > b.max ? a.max : b.max;
        return a;
    }
};

template<bool Indirect>
__global__ void reduce_groups(const GftrValue *values, const int32_t *ids,
                              const int32_t *offsets, const int32_t *counts,
                              int ngroups, GftrColumnStats *output)
{
    using Reduce = cub::BlockReduce<GftrColumnStats, 256>;
    __shared__ typename Reduce::TempStorage storage;
    for (int64_t group = blockIdx.x; group < ngroups; group += gridDim.x) {
        GftrColumnStats s{0, 0, INT_MAX, INT_MIN};
        int64_t end = (int64_t) offsets[group] + counts[group];
        for (int64_t i = (int64_t) offsets[group] + threadIdx.x;
             i < end; i += blockDim.x) {
            GftrValue v = values[Indirect ? ids[i] : i];
            if (v.valid) {
                s.count++;
                s.sum += v.value;
                s.min = s.min < v.value ? s.min : v.value;
                s.max = s.max > v.value ? s.max : v.value;
            }
        }
        auto total = Reduce(storage).Reduce(s, StatsAdd{});
        if (threadIdx.x == 0)
            output[group] = total;
        __syncthreads();
    }
}

} // namespace

extern "C" int
gftr_cuda_compute(const GftrInput *input, int use_gftr, int device,
                  size_t max_device_bytes, GftrResult *result,
                  char error[GFTR_ERROR_SIZE])
{
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    *result = GftrResult{};
    try {
        if (input->nrows < 0 || input->ncols < 0)
            throw std::runtime_error("invalid input dimensions");
        check(cudaSetDevice(device), "select CUDA device");
        cudaDeviceProp properties;
        check(cudaGetDeviceProperties(&properties, device), "get device properties");
        if (input->nrows == 0) {
            std::snprintf(result->device_name, sizeof(result->device_name), "%.*s",
                          (int) sizeof(result->device_name) - 1, properties.name);
            result->ncols = input->ncols;
            result->total_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            return 0;
        }
        const int n = input->nrows;
        size_t free_bytes, total_bytes;
        check(cudaMemGetInfo(&free_bytes, &total_bytes), "get available device memory");
        Arena arena(std::min(max_device_bytes, free_bytes - free_bytes / 10));
        /* Stream is destroyed first, so outstanding work completes before free. */
        Stream owner;
        auto stream = owner.stream;
        Events timer;
        double sort_ms = 0, reduce_ms = 0, transfer_ms = 0;
        auto keys = arena.allocate<uint64_t>(n);
        auto sorted_keys = arena.allocate<uint64_t>(n);
        auto unique_keys = arena.allocate<uint64_t>(n);
        auto counts = arena.allocate<int32_t>(n);
        auto offsets = arena.allocate<int32_t>(n);
        auto group_count = arena.allocate<int32_t>(1);
        GftrValue *values = nullptr, *sorted_values = nullptr;
        int32_t *ids = nullptr, *sorted_ids = nullptr;
        GftrColumnStats *stats = nullptr;
        if (input->ncols) {
            values = arena.allocate<GftrValue>(n);
            stats = arena.allocate<GftrColumnStats>(n);
            if (use_gftr)
                sorted_values = arena.allocate<GftrValue>(n);
            else {
                ids = arena.allocate<int32_t>(n);
                sorted_ids = arena.allocate<int32_t>(n);
            }
        }

        size_t sort_bytes = 0, encode_bytes = 0, scan_bytes = 0;
        if (!input->ncols)
            check(cub::DeviceRadixSort::SortKeys(nullptr, sort_bytes, keys,
                  sorted_keys, n, 0, 33, stream), "size radix sort");
        else if (use_gftr)
            check(cub::DeviceRadixSort::SortPairs(nullptr, sort_bytes, keys,
                  sorted_keys, values, sorted_values, n, 0, 33, stream), "size GFTR radix sort");
        else
            check(cub::DeviceRadixSort::SortPairs(nullptr, sort_bytes, keys,
                  sorted_keys, ids, sorted_ids, n, 0, 33, stream), "size GFUR radix sort");
        check(cub::DeviceRunLengthEncode::Encode(nullptr, encode_bytes,
              sorted_keys, unique_keys, counts, group_count, n, stream), "size run length encoding");
        check(cub::DeviceScan::ExclusiveSum(nullptr, scan_bytes, counts,
              offsets, n, stream), "size prefix scan");
        size_t temp_bytes = std::max({sort_bytes, encode_bytes, scan_bytes});
        auto temp = arena.allocate<unsigned char>(std::max(temp_bytes, size_t(1)));

        transfer_ms += timer.measure(stream, [&] {
            check(cudaMemcpyAsync(keys, input->keys, (size_t) n * sizeof(uint64_t),
                  cudaMemcpyHostToDevice, stream), "transfer keys");
            if (input->ncols && use_gftr)
                check(cudaMemcpyAsync(values, input->columns[0], (size_t) n * sizeof(GftrValue),
                      cudaMemcpyHostToDevice, stream), "transfer first payload");
        });
        sort_ms += timer.measure(stream, [&] {
            if (!input->ncols)
                check(cub::DeviceRadixSort::SortKeys(temp, sort_bytes, keys,
                      sorted_keys, n, 0, 33, stream), "sort keys");
            else if (use_gftr)
                check(cub::DeviceRadixSort::SortPairs(temp, sort_bytes, keys,
                      sorted_keys, values, sorted_values, n, 0, 33, stream), "sort key/payload pairs");
            else {
                make_ids<<<std::min(65535, (int) (((int64_t) n + 255) / 256)), 256, 0, stream>>>(ids, n);
                check(cudaGetLastError(), "initialize tuple IDs");
                check(cub::DeviceRadixSort::SortPairs(temp, sort_bytes, keys,
                      sorted_keys, ids, sorted_ids, n, 0, 33, stream), "sort key/ID pairs");
            }
        });
        reduce_ms += timer.measure(stream, [&] {
            check(cub::DeviceRunLengthEncode::Encode(temp, encode_bytes, sorted_keys,
                  unique_keys, counts, group_count, n, stream), "identify groups");
        });
        int ngroups = 0;
        transfer_ms += timer.measure(stream, [&] {
            check(cudaMemcpyAsync(&ngroups, group_count, sizeof(int),
                  cudaMemcpyDeviceToHost, stream), "retrieve group count");
        });
        if (ngroups < 1 || ngroups > n)
            throw std::runtime_error("invalid group count returned by GPU");
        reduce_ms += timer.measure(stream, [&] {
            check(cub::DeviceScan::ExclusiveSum(temp, scan_bytes, counts,
                  offsets, ngroups, stream), "compute group offsets");
        });
        if (gftr_result_alloc(result, ngroups, input->ncols))
            throw std::runtime_error("out of host memory for aggregate results");
        result->ngroups = ngroups;
        transfer_ms += timer.measure(stream, [&] {
            check(cudaMemcpyAsync(result->keys, unique_keys, (size_t) ngroups * sizeof(uint64_t),
                  cudaMemcpyDeviceToHost, stream), "retrieve group keys");
            check(cudaMemcpyAsync(result->counts, counts, (size_t) ngroups * sizeof(int32_t),
                  cudaMemcpyDeviceToHost, stream), "retrieve group sizes");
        });
        for (int c = 0; c < input->ncols; c++) {
            if (!use_gftr || c > 0)
                transfer_ms += timer.measure(stream, [&] {
                    check(cudaMemcpyAsync(values, input->columns[c], (size_t) n * sizeof(GftrValue),
                          cudaMemcpyHostToDevice, stream), "transfer payload");
                });
            if (use_gftr && c > 0)
                sort_ms += timer.measure(stream, [&] {
                    /* Always sort ORIGINAL keys and this column's actual values. */
                    check(cub::DeviceRadixSort::SortPairs(temp, sort_bytes, keys,
                          sorted_keys, values, sorted_values, n, 0, 33, stream), "transform payload");
                });
            reduce_ms += timer.measure(stream, [&] {
                int blocks = std::min(ngroups, 65535);
                if (use_gftr)
                    reduce_groups<false><<<blocks, 256, 0, stream>>>(sorted_values, nullptr,
                        offsets, counts, ngroups, stats);
                else
                    reduce_groups<true><<<blocks, 256, 0, stream>>>(values, sorted_ids,
                        offsets, counts, ngroups, stats);
                check(cudaGetLastError(), "aggregate groups");
            });
            transfer_ms += timer.measure(stream, [&] {
                check(cudaMemcpyAsync(result->columns[c], stats,
                      (size_t) ngroups * sizeof(GftrColumnStats),
                      cudaMemcpyDeviceToHost, stream), "retrieve aggregates");
            });
        }
        result->sort_ms = sort_ms;
        result->reduce_ms = reduce_ms;
        result->transfer_ms = transfer_ms;
        std::snprintf(result->device_name, sizeof(result->device_name), "%.*s",
                      (int) sizeof(result->device_name) - 1, properties.name);
        /* Includes allocation and synchronization; phase sums need not equal this. */
    } catch (const std::exception &e) {
        gftr_result_free(result);
        std::snprintf(error, GFTR_ERROR_SIZE, "%s", e.what());
        return -1;
    } catch (...) {
        gftr_result_free(result);
        std::snprintf(error, GFTR_ERROR_SIZE, "unknown CUDA backend failure");
        return -1;
    }
    result->total_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return 0;
}

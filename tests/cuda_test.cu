#include "gftr_backend.h"
#include <cuda_runtime.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <vector>

static void require(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
}

static void compare(const GftrResult &a, const GftrResult &b)
{
    require(a.ngroups == b.ngroups, "group count mismatch");
    for (int g = 0; g < a.ngroups; g++) {
        require(a.keys[g] == b.keys[g], "group key mismatch");
        require(a.counts[g] == b.counts[g], "row count mismatch");
        for (int c = 0; c < a.ncols; c++) {
            const auto &x = a.columns[c][g];
            const auto &y = b.columns[c][g];
            require(x.count == y.count && x.sum == y.sum, "count/sum mismatch");
            require(x.min == y.min && x.max == y.max, "min/max mismatch");
        }
    }
}

int main()
{
    try {
        std::mt19937 rng(231200720);
        char error[GFTR_ERROR_SIZE] = {};
        for (int n : {0, 1, 31, 256, 1025, 100003}) {
            for (int groups : {1, 7, 4096, 100003}) {
                for (int nc : {0, 1, 3}) {
                    std::vector<uint64_t> keys(n);
                    std::vector<std::vector<GftrValue>> cols(nc, std::vector<GftrValue>(n));
                    std::vector<GftrValue *> pointers(nc);
                    for (int c = 0; c < nc; c++) pointers[c] = cols[c].data();
                    for (int i = 0; i < n; i++) {
                        keys[i] = i % 23 == 0 ? GFTR_NULL_KEY : rng() % groups;
                        if (i % 37 == 0) keys[i] = UINT32_MAX;
                        for (int c = 0; c < nc; c++)
                            cols[c][i] = {static_cast<int32_t>(rng()), (i + c) % 11 != 0};
                    }
                    GftrInput input{n, nc, keys.data(), pointers.data()};
                    GftrResult cpu{}, gpu{};
                    require(!gftr_cpu_compute(&input, &cpu, error), error);
                    for (int mode : {0, 1}) {
                        require(!gftr_cuda_compute(&input, mode, 0, size_t(4) << 30, &gpu, error), error);
                        compare(cpu, gpu);
                        gftr_result_free(&gpu);
                    }
                    gftr_result_free(&cpu);
                }
            }
        }
        uint64_t keys[] = {1, 1};
        GftrInput input{2, 0, keys, nullptr};
        GftrResult result{};
        require(gftr_cuda_compute(&input, 1, 0, 1, &result, error) != 0,
                "tiny device memory budget must fail");
        gftr_result_free(&result);
        std::puts("CUDA GFTR/GFUR comparison with CPU passed (72 fixtures, 144 GPU runs)");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "CUDA test failed: %s\n", e.what());
        return 1;
    }
}

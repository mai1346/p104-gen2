/*
 * bwtest2.c —— H2D/D2H 带宽测试（pageable vs pinned 对照）
 *
 * ★ 为什么必须做 pinned/pageable 对照（教训来自参考项目 bin/bw_test.py）：
 *   参考项目用 torch 的 pin_memory=True 测带宽，判据是
 *       Gen1 x4 ≈ 0.78 GB/s   Gen2 x4 ≈ 1.55 GB/s
 *   而我们之前的 bwtest.c 用普通 malloc（pageable）：
 *   从可换页内存做 H2D，驱动必须走 bounce buffer（staging buffer），
 *   典型上限就在 0.7–0.8 GB/s —— 恰好和 Gen1 的数字撞上。
 *   ⇒ 之前所有"Gen1 = 0.778"的读数，可能测的是 bounce buffer，不是 PCIe 链路！
 *   本工具同时测两种，一眼看出瓶颈在哪。
 *
 * 不依赖 CUDA toolkit：dlopen("libcuda.so.1") 手工取 driver API 入口。
 *
 * 编译：gcc -O2 -o bwtest2 bwtest2.c -ldl
 * 用法：./bwtest2 [MB] [迭代次数]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>

typedef int            CUresult;
typedef int            CUdevice;
typedef unsigned long long CUdeviceptr;
typedef void          *CUcontext;

#define CUDA_SUCCESS 0

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

#define LOAD(name) do { \
    *(void **)(&p_##name) = dlsym(h, #name); \
    if (!p_##name) { fprintf(stderr, "缺符号: %s\n", #name); return 2; } \
} while (0)

static CUresult (*p_cuInit)(unsigned int);
static CUresult (*p_cuDeviceGet)(CUdevice *, int);
static CUresult (*p_cuCtxCreate_v2)(CUcontext *, unsigned int, CUdevice);
static CUresult (*p_cuMemAlloc_v2)(CUdeviceptr *, size_t);
static CUresult (*p_cuMemcpyHtoD_v2)(CUdeviceptr, const void *, size_t);
static CUresult (*p_cuMemcpyDtoH_v2)(void *, CUdeviceptr, size_t);
static CUresult (*p_cuCtxSynchronize)(void);
static CUresult (*p_cuMemFree_v2)(CUdeviceptr);
static CUresult (*p_cuMemAllocHost_v2)(void **, size_t);
static CUresult (*p_cuMemFreeHost)(void *);

static void run(const char *tag, void *host, CUdeviceptr d, size_t bytes, int iter)
{
    p_cuMemcpyHtoD_v2(d, host, bytes);
    p_cuMemcpyDtoH_v2(host, d, bytes);
    p_cuCtxSynchronize();

    double *h2d = malloc(sizeof(double) * iter);
    double *d2h = malloc(sizeof(double) * iter);

    for (int i = 0; i < iter; i++) {
        double t0 = now();
        p_cuMemcpyHtoD_v2(d, host, bytes);
        p_cuCtxSynchronize();
        h2d[i] = (double)bytes / (now() - t0) / 1e9;
    }
    for (int i = 0; i < iter; i++) {
        double t0 = now();
        p_cuMemcpyDtoH_v2(host, d, bytes);
        p_cuCtxSynchronize();
        d2h[i] = (double)bytes / (now() - t0) / 1e9;
    }
    qsort(h2d, iter, sizeof(double), cmp_d);
    qsort(d2h, iter, sizeof(double), cmp_d);
    printf("  %-10s H2D med %.3f  max %.3f   D2H med %.3f  max %.3f  GB/s\n",
           tag, h2d[iter / 2], h2d[iter - 1], d2h[iter / 2], d2h[iter - 1]);
    free(h2d); free(d2h);
}

int main(int argc, char **argv)
{
    size_t mb   = (argc > 1) ? (size_t)atoi(argv[1]) : 256;
    int    iter = (argc > 2) ? atoi(argv[2]) : 7;      /* 参考项目用 7 次取中位数 */
    size_t bytes = mb << 20;

    void *h = dlopen("libcuda.so.1", RTLD_NOW);
    if (!h) h = dlopen("libcuda.so", RTLD_NOW);
    if (!h) { fprintf(stderr, "打不开 libcuda.so.1: %s\n", dlerror()); return 2; }

    LOAD(cuInit); LOAD(cuDeviceGet); LOAD(cuCtxCreate_v2);
    LOAD(cuMemAlloc_v2); LOAD(cuMemcpyHtoD_v2); LOAD(cuMemcpyDtoH_v2);
    LOAD(cuCtxSynchronize); LOAD(cuMemFree_v2);
    LOAD(cuMemAllocHost_v2); LOAD(cuMemFreeHost);

    CUresult r;
    if ((r = p_cuInit(0)) != CUDA_SUCCESS) { fprintf(stderr, "cuInit=%d\n", r); return 3; }

    CUdevice dev;
    if ((r = p_cuDeviceGet(&dev, 0)) != CUDA_SUCCESS) { fprintf(stderr, "cuDeviceGet=%d\n", r); return 3; }

    CUcontext ctx;
    if ((r = p_cuCtxCreate_v2(&ctx, 0, dev)) != CUDA_SUCCESS) {
        fprintf(stderr, "cuCtxCreate=%d\n", r); return 3;
    }

    CUdeviceptr d;
    if ((r = p_cuMemAlloc_v2(&d, bytes)) != CUDA_SUCCESS) {
        fprintf(stderr, "cuMemAlloc(%zu MB)=%d\n", mb, r); return 3;
    }

    printf("buffer=%zu MB  iter=%d\n", mb, iter);

    /* ---- 1. pageable（普通 malloc，会走 bounce buffer）---- */
    void *pageable = malloc(bytes);
    if (!pageable) { fprintf(stderr, "malloc 失败\n"); return 3; }
    memset(pageable, 0xA5, bytes);
    run("pageable", pageable, d, bytes, iter);

    /* ---- 2. pinned（锁页，直通 DMA）---- */
    void *pinned = NULL;
    if ((r = p_cuMemAllocHost_v2(&pinned, bytes)) != CUDA_SUCCESS) {
        fprintf(stderr, "  cuMemAllocHost(%zu MB)=%d —— 跳过 pinned\n", mb, r);
        pinned = NULL;
    } else {
        memset(pinned, 0x5A, bytes);
        run("pinned", pinned, d, bytes, iter);
    }

    /* ---- 判据 ---- */
    printf("\n判据（x4 宽度，参考项目值）:\n");
    printf("  Gen1 x4 ≈ 0.78 GB/s | Gen2 x4 ≈ 1.55 GB/s | Gen3 x4 ≈ 3.10 GB/s\n");

    if (pinned) p_cuMemFreeHost(pinned);
    free(pageable);
    p_cuMemFree_v2(d);
    dlclose(h);
    return 0;
}

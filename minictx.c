/*
 * minictx.c —— 只创建/销毁 CUDA 上下文，不做任何 DMA
 *
 * 用途（reports/195）：
 *   已证明 `_nv017281rm` 每次 cuCtxCreate 都执行一次，并在 sp==1 时
 *   通过 `_nv053912rm(CX=0x8c040, R8=SPEED)` 把 0x8C040 的 SPEED 写成
 *   SPEED=2（2.5GT/s）。补丁把该处改成 SPEED=1（5.0GT/s）。
 *
 *   想验证"补丁后链路能否自己升 Gen2"，就不能用 bwtest2 ——
 *   它建完上下文立刻做几十 MB 的真实 DMA，一旦链路在 RM 已初始化后升速，
 *   极易触发 Xid 79（GPU fallen off the bus）。
 *
 *   本程序只做 cuInit → cuCtxCreate → cuCtxDestroy，
 *   让那个 SPEED 写入发生（这是我们要观察的），但不搬运任何数据。
 *
 * 不依赖 CUDA toolkit：dlopen libcuda.so.1，手工取 driver API 入口。
 * 编译：gcc -O2 -o minictx minictx.c -ldl
 * 用法：./minictx [次数]
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;

int main(int argc, char **argv)
{
    int rounds = (argc > 1) ? atoi(argv[1]) : 1;
    if (rounds < 1) rounds = 1;

    void *h = dlopen("libcuda.so.1", RTLD_NOW);
    if (!h) h = dlopen("libcuda.so", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen libcuda 失败: %s\n", dlerror()); return 1; }

    CUresult (*cuInit)(unsigned) = dlsym(h, "cuInit");
    CUresult (*cuDeviceGet)(CUdevice *, int) = dlsym(h, "cuDeviceGet");
    CUresult (*cuCtxCreate)(CUcontext *, unsigned, CUdevice) = dlsym(h, "cuCtxCreate_v2");
    CUresult (*cuCtxDestroy)(CUcontext) = dlsym(h, "cuCtxDestroy_v2");
    const char *(*cuGetErrorString)(CUresult, const char **) = dlsym(h, "cuGetErrorString");

    if (!cuInit || !cuDeviceGet || !cuCtxCreate || !cuCtxDestroy) {
        fprintf(stderr, "缺少 driver API 入口\n");
        return 1;
    }
    /* cuCtxCreate_v2 不存在时退回无后缀名 */
    if (!cuCtxCreate) cuCtxCreate = dlsym(h, "cuCtxCreate");
    if (!cuCtxDestroy) cuCtxDestroy = dlsym(h, "cuCtxDestroy");

    CUresult r = cuInit(0);
    if (r) { fprintf(stderr, "cuInit 失败 %d\n", r); return 1; }

    CUdevice dev;
    r = cuDeviceGet(&dev, 0);
    if (r) { fprintf(stderr, "cuDeviceGet 失败 %d\n", r); return 1; }

    for (int i = 0; i < rounds; i++) {
        CUcontext ctx = NULL;
        r = cuCtxCreate(&ctx, 0, dev);   /* ★ 这里会触发 _nv017281rm */
        if (r) {
            const char *s = NULL;
            if (cuGetErrorString) cuGetErrorString(r, &s);
            fprintf(stderr, "第 %d 轮 cuCtxCreate 失败 %d (%s)\n", i + 1, r, s ? s : "?");
            return 2;
        }
        printf("第 %d 轮 ctx 创建成功 (%p)\n", i + 1, ctx);
        fflush(stdout);
        usleep(200000);
        r = cuCtxDestroy(ctx);
        if (r) { fprintf(stderr, "第 %d 轮 cuCtxDestroy 失败 %d\n", i + 1, r); return 3; }
        printf("第 %d 轮 ctx 已销毁\n", i + 1);
        fflush(stdout);
        usleep(200000);
    }
    printf("完成 %d 轮，无 DMA\n", rounds);
    return 0;
}

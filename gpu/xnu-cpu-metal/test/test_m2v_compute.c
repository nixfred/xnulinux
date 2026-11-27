// SPDX-License-Identifier: GPL-2.0-or-later
// The bridge by itself (libxnu_m2v: Metal's objects over metal2vulkan), no plugin and no Metal framework: the kernel of
// test/vecadd.metal, from its metallib, on the Vulkan device the arguments name (scripts/test-m2v.sh: lavapipe).
// The second of three layers that add the same two vectors: test_lavapipe_compute.c (the driver by itself), this one,
// and test_bundle.mm (the plugin). The lowest one that fails says where to look.
//
//   test_m2v_compute <Vulkan loader> <ICD JSON> <vecadd.metallib> [shader cache directory]
//   ("" for the loader or the ICD JSON: the system's)
//
// 1. The library lists its one kernel, and a name it does not have gives no function.
// 2. dispatchThreads over N threads, N not a multiple of the threadgroup's 64: every sum right, and the floats after
//    the N-th keep their value (the kernel has no bounds check: a grid rounded up to whole threadgroups writes there).
// 3. dispatchThreadgroups over whole threadgroups, with offsets into all three buffers.
// 4. Both again on a second device, opened after the first and everything made on it were released (with a cache
//    directory: over what the first left there).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xnu_m2v.h"

enum { N = 100000, GUARD = 64, GROUP = 64, COMPLETED = 4 };
#define SENTINEL (-12345.0f)

static int fails;
static char refused[512];      // the error of the one call that is meant to fail, not to be shown with a later failure

static void result(int ok, const char *what) {
    printf("%s: %s", ok ? "PASS" : "FAIL", what);
    if (!ok && xm2v_last_error()[0] && strcmp(xm2v_last_error(), refused)) printf(" (the bridge's last error: %s)", xm2v_last_error());
    printf("\n");
    fails += !ok;
}
static void *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *p = n > 0 ? malloc((size_t)n) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    *len = p ? (size_t)n : 0;
    return p;
}
// One command buffer with one compute pass: the three buffers at the given float offsets, then the dispatch.
static bool run(Xm2vQueue *q, Xm2vComputePipeline *pso, Xm2vBuffer *a, Xm2vBuffer *b, Xm2vBuffer *c, uint64_t first,
                bool threads, uint64_t count) {
    Xm2vCommandBuffer *cb = xm2v_command_buffer_new(q);
    Xm2vComputeEncoder *enc = cb ? xm2v_compute_encoder_new(cb) : NULL;
    bool ok = enc && xm2v_compute_encoder_set_pipeline(enc, pso)
        && xm2v_compute_encoder_set_buffer(enc, a, first * sizeof(float), 0)
        && xm2v_compute_encoder_set_buffer(enc, b, first * sizeof(float), 1)
        && xm2v_compute_encoder_set_buffer(enc, c, first * sizeof(float), 2)
        && (threads ? xm2v_compute_encoder_dispatch_threads(enc, count, 1, 1, GROUP, 1, 1)
                    : xm2v_compute_encoder_dispatch_threadgroups(enc, count, 1, 1, GROUP, 1, 1));
    ok = enc && xm2v_compute_encoder_end(enc) && ok;      // end frees the encoder, whatever came before
    ok = ok && xm2v_command_buffer_commit(cb) && xm2v_command_buffer_wait_until_completed(cb, 0)
        && xm2v_command_buffer_status(cb) == COMPLETED;
    if (cb) xm2v_command_buffer_release(cb);
    return ok;
}
// c[from, to) holds the sums and nothing else of c changed; says what it found otherwise.
static bool sums(const float *a, const float *b, const float *c, uint32_t from, uint32_t to) {
    uint32_t wrong = 0, first = 0, touched = 0, where = 0;
    for (uint32_t i = from; i < to; i++)
        if (c[i] != a[i] + b[i] && !wrong++) first = i;
    for (uint32_t i = 0; i < N + GUARD; i++)
        if ((i < from || i >= to) && c[i] != SENTINEL && !touched++) where = i;
    if (wrong) printf("      %u of %u sums wrong, the first at %u: %g + %g gave %g\n", wrong, to - from, first, a[first], b[first], c[first]);
    if (touched) printf("      %u floats outside [%u, %u) were written, the first at %u\n", touched, from, to, where);
    return !wrong && !touched;
}
static void device(int round, const char *loader, const char *icd, const char *cache, const void *ml, size_t ml_len) {
    char what[160], name[128] = "";
    Xm2vDevice *dev = xm2v_device_open(loader, icd, cache);
    if (!dev) { snprintf(what, sizeof what, "device %d opens", round); result(0, what); return; }
    xm2v_device_name(dev, name, sizeof name);
    printf("Device %d: %s\n", round, name);

    Xm2vLibrary *lib = xm2v_library_new_with_data(dev, ml, ml_len);
    char fname[64] = "";
    if (lib) xm2v_library_function_name_at(lib, 0, fname, sizeof fname);
    Xm2vFunction *none = lib ? xm2v_library_new_function(lib, "no_such_kernel") : NULL;
    if (lib && !none) snprintf(refused, sizeof refused, "%s", xm2v_last_error());
    Xm2vFunction *fn = lib ? xm2v_library_new_function(lib, "vecadd") : NULL;
    snprintf(what, sizeof what, "device %d: the library lists one function, vecadd, and has none called no_such_kernel", round);
    result(lib && xm2v_library_function_count(lib) == 1 && !strcmp(fname, "vecadd") && fn && !none, what);
    if (none) xm2v_function_release(none);
    Xm2vComputePipeline *pso = fn ? xm2v_compute_pipeline_new(dev, fn) : NULL;
    Xm2vQueue *q = xm2v_queue_new(dev);
    // a and b made from bytes, c made empty and filled through its contents: both ways a plugin makes buffers
    static float av[N + GUARD], bv[N + GUARD];
    for (uint32_t i = 0; i < N + GUARD; i++) { av[i] = (float)i * 0.5f; bv[i] = (float)(N - i) * 0.25f + 1000.0f; }   // every sum exact
    Xm2vBuffer *ab = xm2v_buffer_new(dev, sizeof av, 0, av), *bb = xm2v_buffer_new(dev, sizeof bv, 0, bv);
    Xm2vBuffer *cbuf = xm2v_buffer_new(dev, sizeof av, 0, NULL);
    float *c = cbuf ? xm2v_buffer_contents(cbuf) : NULL;
    if (!pso || !q || !ab || !bb || !c) {
        snprintf(what, sizeof what, "device %d: the compute pipeline, a queue and three buffers are made", round);
        result(0, what);
    } else {
        for (uint32_t i = 0; i < N + GUARD; i++) c[i] = SENTINEL;
        bool ran = run(q, pso, ab, bb, cbuf, 0, true, N);
        snprintf(what, sizeof what, "device %d: dispatchThreads of %d threads (%d threadgroups and %d more): %d sums, nothing after them",
                 round, N, N / GROUP, N % GROUP, N);
        result(ran && sums(av, bv, c, 0, N), what);

        for (uint32_t i = 0; i < N + GUARD; i++) c[i] = SENTINEL;
        ran = run(q, pso, ab, bb, cbuf, 1000, false, 10);      // 10 threadgroups from float 1000 of each buffer
        snprintf(what, sizeof what, "device %d: dispatchThreadgroups of 10 threadgroups at an offset of 1000 floats: %d sums there, nothing else",
                 round, 10 * GROUP);
        result(ran && sums(av, bv, c, 1000, 1000 + 10 * GROUP), what);
    }
    if (cbuf) xm2v_buffer_release(cbuf);
    if (bb) xm2v_buffer_release(bb);
    if (ab) xm2v_buffer_release(ab);
    if (q) xm2v_queue_release(q);
    if (pso) xm2v_compute_pipeline_release(pso);
    if (fn) xm2v_function_release(fn);
    if (lib) xm2v_library_release(lib);
    xm2v_device_release(dev);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "Usage: %s <Vulkan loader> <ICD JSON> <vecadd.metallib> [shader cache directory]\n", argv[0]); return 2; }
    const char *loader = argv[1][0] ? argv[1] : NULL, *icd = argv[2][0] ? argv[2] : NULL;
    const char *cache = argc > 4 && argv[4][0] ? argv[4] : NULL;
    size_t ml_len; void *ml = read_file(argv[3], &ml_len);
    if (!ml) { fprintf(stderr, "FAIL: cannot read %s\n", argv[3]); return 1; }
    device(1, loader, icd, cache, ml, ml_len);
    device(2, loader, icd, cache, ml, ml_len);
    free(ml);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}

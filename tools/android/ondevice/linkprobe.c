// tools/android/ondevice/linkprobe.c: a stand-in for liblasgame.so in the on-phone link test
// (docs/android-ondevice-build.md). Compiled and linked ON the phone exactly like the game code (the on-device
// compiler, cckit/link.rsp, DT_NEEDED libmain.so), then loaded by bionic's linker (-DLOADER: the test program, built
// with the NDK): it calls into libmain.so (plat_now_ns), libc (memcpy, strlen) and the compiler runtime (an outline
// atomic, as the generated code's __aarch64_cas8_acq_rel). Only the headers the game code has (cckit: no stdio.h).
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef LOADER
uint64_t plat_now_ns(void);  // (libmain.so's)
static uint64_t counter;

__attribute__((visibility("default"))) int linkprobe(char *out, size_t n) {
    uint64_t want = 0;
    __atomic_compare_exchange_n(&counter, &want, 42, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    char tmp[64] = "counter 42, now ", num[24];
    uint64_t t = plat_now_ns();
    int k = 0;
    do num[k++] = (char)('0' + t % 10), t /= 10; while (t);
    size_t at = strlen(tmp);
    while (k && at < sizeof tmp - 4) tmp[at++] = num[--k];
    memcpy(tmp + at, " ns", 4);
    memcpy(out, tmp, n < sizeof tmp ? n : sizeof tmp);
    out[n - 1] = 0;
    return counter == 42;
}
#else
#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char **argv) {
    void *h = dlopen(argc > 1 ? argv[1] : "./liblinkprobe.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    int (*f)(char *, size_t) = (int (*)(char *, size_t))dlsym(h, "linkprobe");
    if (!f) { printf("dlsym: %s\n", dlerror()); return 1; }
    char buf[64];
    int ok = f(buf, sizeof buf);
    printf("linkprobe: %s -> %s\n", buf, ok ? "ok" : "FAILED");
    return !ok;
}
#endif

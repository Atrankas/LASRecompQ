// tools/android/ondevice/gamebuild_test.c: runtime/gamebuild.c's Android branch run on the phone without the app
// (phone_gamebuild_test.sh): the app's own libmain.so loaded from <dir>/lib (beside liblaspython.so and libzig.so, as in
// the native library folder), gamebuild_run on a fake game folder (fake exefs files, fake_gen.py as the generator:
// LARECOMP_ONDEVICE_DIR), then the library it installed loaded through the app's gamelib_load. Built with the NDK.
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    void (*progress)(double fraction, const char *step);
    int jobs;
    bool (*cancelled)(void);
} GameBuildOpts;  // (runtime/gamebuild.h)

static void progress(double f, const char *step) {
    static int last = -1;
    if ((int)(f * 20) != last) last = (int)(f * 20), printf("  %3d%% %s\n", (int)(f * 100), step), fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: gamebuild_test <libmain.so> <game dir>\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    bool (*run)(const char *, const GameBuildOpts *, char *, size_t) =
        (bool (*)(const char *, const GameBuildOpts *, char *, size_t))dlsym(h, "gamebuild_run");
    bool (*avail)(void) = (bool (*)(void))dlsym(h, "gamebuild_available");
    if (!run || !avail) { printf("no gamebuild_run in libmain.so\n"); return 1; }
    printf("gamebuild_available: %d\n", avail());
    GameBuildOpts o = {progress, 4, NULL};
    char why[256] = "";
    bool ok = run(argv[2], &o, why, sizeof why);
    printf("gamebuild_run: %s %s\n", ok ? "ok" : "FAILED", why);
    if (!ok || argc < 4) return ok ? 0 : 1;
    // the installed library (argv[3]: where install_internal put it), loaded by bionic: its tables, as the app reads them
    void *g = dlopen(argv[3], RTLD_NOW | RTLD_LOCAL);
    if (!g) { printf("dlopen %s: %s\n", argv[3], dlerror()); return 1; }
    typedef struct { int abi; const char *id; const unsigned long long *ma, *sa; const void *mp, *sp;
                     const unsigned long long *mc, *sc; } Tables;  // (runtime/gamelib.h GameLibTables)
    const Tables *(*entry)(void) = (const Tables *(*)(void))dlsym(g, "lasgame_tables");
    if (!entry) { printf("no lasgame_tables\n"); return 1; }
    const Tables *t = entry();
    printf("%s: id %s, %llu + %llu functions\n", argv[3], t->id, *t->mc, *t->sc);
    return 0;
}

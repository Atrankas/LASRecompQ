// Builds the game code library on the player's machine (runtime/gamebuild.h).
#if defined(__ANDROID__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  // (sched_setaffinity, cpu_set_t)
#endif
#include "gamebuild.h"
#include "build_cache.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <signal.h>
#endif

#include "gamelib.h"
#include "paths.h"
#include "platform.h"
#include "rt.h"
#ifdef __ANDROID__
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include "platform/android.h"
#endif

// From the Makefile: the flags the game code is compiled and linked with (as `make` builds it in a source checkout)
#ifndef GAMELIB_CFLAGS
#define GAMELIB_CFLAGS "-O1 -fno-strict-aliasing -fvisibility=hidden"
#endif
#ifndef GAMELIB_SHARED
#define GAMELIB_SHARED "-shared"
#endif
#ifndef GAMELIB_BASE_ID
#define GAMELIB_BASE_ID "dev"
#endif
// The generator's options and the extra flags of its split-dispatch files (Android: tools/android/gamelib_id.sh
// --gen-args / --split-flags, the same a computer's tools/android/build_game.sh uses: the same code, the same id)
#ifndef GAMELIB_GEN_ARGS
#define GAMELIB_GEN_ARGS ""
#endif
#ifndef GAMELIB_SPLIT_FLAGS
#define GAMELIB_SPLIT_FLAGS ""
#endif
// The first line of a split-dispatch file (tools/gen.py SPLIT_MARK)
#define SPLIT_MARK "/* lasgen: split-dispatch */"
// and of a file of cold functions (tools/gen.py --hot COLD_MARK): compiled at -O0
#define COLD_MARK "/* lasgen: cold */"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

// ------------------------------------------------------------------ child processes
// Every one running, so that quitting mid-build ends them too (atexit) instead of leaving compilers behind.
#define MAX_PROCS 80
static pthread_mutex_t g_plock = PTHREAD_MUTEX_INITIALIZER;
static PlatProc g_procs[MAX_PROCS];
static int g_nprocs;
static bool g_quitting;

static void end_all(void) {
    pthread_mutex_lock(&g_plock);
    g_quitting = true;
    for (int i = 0; i < g_nprocs; i++) plat_proc_kill(g_procs[i]);
    g_nprocs = 0;
    pthread_mutex_unlock(&g_plock);
}

static PlatProc spawn(char **argv, const char *log) {
    pthread_mutex_lock(&g_plock);
    PlatProc p = g_quitting ? 0 : plat_spawn((const char *const *)argv, log, false);
    if (p && g_nprocs < MAX_PROCS) g_procs[g_nprocs++] = p;
    pthread_mutex_unlock(&g_plock);
    return p;
}

// plat_proc_wait without blocking, then forgotten once it has ended
static int finished(PlatProc p) {
    pthread_mutex_lock(&g_plock);
    int r = plat_proc_wait(p, false);
    if (r >= 0)
        for (int i = 0; i < g_nprocs; i++)
            if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; break; }
    pthread_mutex_unlock(&g_plock);
    return r;
}

static void stop(PlatProc p) {  // ends one that is still running, then forgotten
    pthread_mutex_lock(&g_plock);
    bool ours = false;
    for (int i = 0; i < g_nprocs; i++)
        if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; ours = true; break; }
    pthread_mutex_unlock(&g_plock);
    if (ours) plat_proc_kill(p);
}

static void sleep_ms(int ms) { plat_sleep_until_ns(plat_now_ns() + (uint64_t)ms * 1000000ull); }

// ------------------------------------------------------------------ argument lists
typedef struct { char **v; int n, cap; } Args;
static void add(Args *a, const char *s) {
    if (a->n + 2 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 32;
        a->v = (char **)realloc(a->v, sizeof *a->v * a->cap);
    }
    a->v[a->n++] = strdup(s);
    a->v[a->n] = NULL;
}
static void add_split(Args *a, const char *flags) {  // (space-separated, no quoting)
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", flags);
    for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) add(a, t);
}
static void args_free(Args *a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    memset(a, 0, sizeof *a);
}

// ------------------------------------------------------------------ the tools
typedef struct {
    char gen[1100], gen_script[1100];  // generator: lasgen, or (source checkout) the venv's python + tools/gen.py
    char cc[1100];                     // compiler: zig (as `zig cc`), or LARECOMP_CC / clang
    bool zig;
    char inc[1100], entry[1100], hle[1100];  // headers the game code includes, tools/gamelib_entry.c, HLE names
#ifdef _WIN32
    char implib[1100];  // the .exe's import library (the runtime functions the game code calls)
    char zig_rt[1100];  // the release's zig: its runtime built ahead of time (zig/rt, tools/zig_slim.sh), "" = none
#endif
#ifdef __ANDROID__
    // the app's native library folder (programs run only from there: liblaspython.so, libzig.so; and libmain.so, to
    // link against) and the NDK pieces unpacked from the APK's assets (files/ondevice/cckit: tools/android/ondevice/)
    char nlib[1100], kit[1100], ondev[1100];
#endif
} Tools;

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

#ifdef __ANDROID__
// The folder libmain.so was loaded from: the native library folder when the APK's libraries are extracted at install
// (android:extractNativeLibs: the on-phone build's packaging, android/app/build.gradle onDeviceBuild); from inside
// the APK ("base.apk!/lib/...") there are no programs to run.
static bool native_lib_dir(char *out, size_t n) {
    Dl_info di;
    if (!dladdr((void *)&native_lib_dir, &di) || !di.dli_fname || strchr(di.dli_fname, '!')) return false;
    snprintf(out, n, "%s", di.dli_fname);
    char *s = strrchr(out, '/');
    if (!s) return false;
    *s = 0;
    return true;
}

// Phones: the generator is liblaspython.so running tools/gen.py, the compiler zig's clang and ld.lld (libzig.so) with
// the NDK pieces (cckit/), all from the on-phone build's packaging (docs/android-ondevice-build.md). ondev: where
// LasActivity.buildTools unpacked the asset archive; NULL = not asked (only checks that the programs are there).
static bool find_tools_android(Tools *t, const char *ondev) {
    memset(t, 0, sizeof *t);
    if (!native_lib_dir(t->nlib, sizeof t->nlib)) return false;
    snprintf(t->gen, sizeof t->gen, "%s/liblaspython.so", t->nlib);
    snprintf(t->cc, sizeof t->cc, "%s/libzig.so", t->nlib);
    t->zig = true;
    if (!file_exists(t->gen) || !file_exists(t->cc)) return false;
    if (!ondev) return true;
    snprintf(t->ondev, sizeof t->ondev, "%s", ondev);
    snprintf(t->gen_script, sizeof t->gen_script, "%s/tools/gen.py", ondev);
    snprintf(t->kit, sizeof t->kit, "%s/cckit", ondev);
    snprintf(t->inc, sizeof t->inc, "%s/gamelib-src", t->kit);
    snprintf(t->entry, sizeof t->entry, "%s/gamelib_entry.c", t->inc);
    snprintf(t->hle, sizeof t->hle, "%s/hle_symbols.txt", t->inc);
    return file_exists(t->gen_script) && file_exists(t->entry) && file_exists(t->hle);
}
#endif

static bool find_tools(Tools *t) {
#ifdef __ANDROID__
    return find_tools_android(t, NULL);
#endif
    memset(t, 0, sizeof *t);
    const char *e = getenv("LARECOMP_GEN");
    if (e && *e) snprintf(t->gen, sizeof t->gen, "%s", e);
    else if (!rt_resource("lasgen" SEP "lasgen" PLAT_EXE_SUFFIX, NULL, t->gen, sizeof t->gen) &&
             !(rt_resource(NULL, "tools/" PLAT_VENV_PYTHON, t->gen, sizeof t->gen) &&
               rt_resource(NULL, "tools/gen.py", t->gen_script, sizeof t->gen_script)))
        return false;
    e = getenv("LARECOMP_CC");
    if (e && *e) snprintf(t->cc, sizeof t->cc, "%s", e);
    else if (rt_resource("zig" SEP "zig" PLAT_EXE_SUFFIX, NULL, t->cc, sizeof t->cc)) t->zig = true;
#ifndef _WIN32
    else if (rt_dev_mode() && file_exists("/usr/bin/clang")) snprintf(t->cc, sizeof t->cc, "/usr/bin/clang");
#endif
    else return false;
    if (!rt_resource("gamelib-src" SEP "cpu.h", "runtime/cpu.h", t->inc, sizeof t->inc) ||
        !rt_resource("gamelib-src" SEP "gamelib_entry.c", "tools/gamelib_entry.c", t->entry, sizeof t->entry) ||
        !rt_resource("gamelib-src" SEP "hle_symbols.txt", "runtime/hle/hle_symbols.txt", t->hle, sizeof t->hle))
        return false;
    char *s = strrchr(t->inc, '/'), *b = strrchr(t->inc, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
#ifdef _WIN32
    if (!rt_resource("lasrecomp.dll.a", "build/lasrecomp.dll.a", t->implib, sizeof t->implib)) return false;
    t->zig_rt[0] = 0;
    if (t->zig && rt_resource("zig" SEP "rt" SEP "link.rsp", NULL, t->zig_rt, sizeof t->zig_rt)) {
        char *s = strrchr(t->zig_rt, '/'), *b = strrchr(t->zig_rt, '\\');
        if (b > s) s = b;
        if (s) *s = 0;
        else t->zig_rt[0] = 0;
    }
#endif
    return true;
}

bool gamebuild_available(void) {
    Tools t;
    return find_tools(&t);
}

// zig cc on Linux: an explicit target, so it uses its own glibc headers and stubs instead of looking for the system's
// (SteamOS and other systems without development packages have none: "failed to find libc installation", #181)
#if defined(__ANDROID__)
#define ZIG_TARGET ""  // (compiler() below: zig's clang driver with the Android target and the kit's headers)
#elif defined(__linux__) && defined(__x86_64__)
#define ZIG_TARGET "x86_64-linux-gnu"
#elif defined(__linux__) && defined(__aarch64__)
#define ZIG_TARGET "aarch64-linux-gnu"
#else
#define ZIG_TARGET ""
#endif

static void compiler(Args *a, const Tools *t) {
    add(a, t->cc);
#ifdef __ANDROID__
    // zig's own clang driver (not `zig cc`: zig has no libc for Android and would build its compiler_rt from source),
    // the headers from the kit (bionic's few + this clang's own: tools/android/ondevice/build_cckit.sh)
    static const char *const sub[] = {"/clang/include", "/sysroot/usr/include", "/sysroot/usr/include/aarch64-linux-android"};
    add(a, "clang");
    add(a, "-target");
    add(a, "aarch64-linux-android30");
    add(a, "-nostdinc");
    for (int i = 0; i < 3; i++) {
        char inc[1200];
        snprintf(inc, sizeof inc, "%s%s", t->kit, sub[i]);
        add(a, "-isystem");
        add(a, inc);
    }
    return;
#endif
    if (t->zig) add(a, "cc");
    if (t->zig && ZIG_TARGET[0]) add(a, "-target"), add(a, ZIG_TARGET);
}

// ------------------------------------------------------------------ generator progress (its log)
typedef struct {
    const char *name;
    char log[1100], out[1100];
    PlatProc p;
    int exit;                // -1 while running
    int files;               // chunk files written so far (complete)
    unsigned long long ci, n;  // functions written / all
} Gen;

static void read_gen_log(Gen *g) {
    FILE *f = fopen(g->log, "rb");
    if (!f) return;
    static char buf[1 << 16];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0;
    fseek(f, from, SEEK_SET);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *last = NULL;
    for (const char *p = strstr(buf, ": progress "); p; p = strstr(p + 1, ": progress ")) last = p;
    unsigned long long ci, all;
    int files;
    if (last && sscanf(last, ": progress %llu/%llu files %d", &ci, &all, &files) == 3) {
        g->ci = ci, g->n = all;
        if (files > g->files) g->files = files;
    }
}

static void log_tail(const char *path, char *out, size_t n) {  // the last line of a log (an error message)
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0, SEEK_SET);
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[k] = 0;
    while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
    char *nl = strrchr(buf, '\n');
    snprintf(out, n, "%s", nl ? nl + 1 : buf);
}

// A helper that failed (plat_proc_wait's r != 0): its log's last line for our log, and how it ended when that says
// nothing or a signal ended it (the system killing it for memory writes nothing). A signal also gives the player why.
static bool g_killed;
static void helper_failed(const char *what, int r, const char *log, char *why, size_t why_size) {
    char last[300], how[120] = "";
    log_tail(log, last, sizeof last);
#ifndef _WIN32
    if (r > PLAT_PROC_SIGNALED) {
        int sig = r - PLAT_PROC_SIGNALED;
        if (sig == SIGKILL) {
            snprintf(how, sizeof how, "killed by signal %d (SIGKILL: likely out of memory)", sig);
            snprintf(why, why_size, "A build helper was killed by the system, likely out of memory.");
        } else {
            snprintf(how, sizeof how, "killed by signal %d (%s)", sig, strsignal(sig));
            snprintf(why, why_size, "A build helper crashed (signal %d).", sig);
        }
        g_killed = true;
    } else
#endif
    if (!last[0]) {
        snprintf(how, sizeof how, "exit status %d", r);
    }
    LOG("[gamebuild] %s failed: %s%s%s\n", what, how, how[0] && last[0] ? ": " : "", last);
}

bool gamebuild_killed(void) { return g_killed; }

// ------------------------------------------------------------------ compiling
typedef struct {
    char src[1100], obj[1100], tmp[1100], log[1100], stem[64], source_hash[65];
    unsigned long long bytes;
    bool split;  // a split-dispatch file (split_dispatch)
    bool cold;   // a file of functions the play profile didn't see run (compiled at -O0)
    double cost;  // its expected compile time, in bytes of ordinary C (the longest go first: next_unit)
} Unit;

// The generator's split-dispatch files (tools/gen.py --split-dispatch) and files of cold functions (--hot) say so on
// their first line
static bool split_dispatch(const char *src, bool *cold) {
    char head[64] = {0};
    *cold = false;
    FILE *f = fopen(src, "rb");
    if (!f) return false;
    size_t n = fread(head, 1, sizeof head - 1, f);
    fclose(f);
    head[n] = 0;
    *cold = !strncmp(head, COLD_MARK, strlen(COLD_MARK));
    return !strncmp(head, SPLIT_MARK, strlen(SPLIT_MARK));
}
// (a split-dispatch file takes ~8x the time per byte of the rest: one huge jump-table function; measured over the
// whole library with zig clang 22, 25.6 vs 3.1 CPU-seconds per MB on an M1)
#define SPLIT_COST 8.0
// (a file of cold functions at -O0 takes this share of an ordinary one's time per byte: zig clang 22 on a PC)
#define COLD_COST 0.25

typedef struct {
    Unit *u;
    int n, cap;
} Units;

static void add_unit(Units *us, const char *src, const char *work, const char *stem) {
    if (us->n == us->cap) {
        us->cap = us->cap ? us->cap * 2 : 64;
        us->u = (Unit *)realloc(us->u, sizeof *us->u * us->cap);
    }
    Unit *u = &us->u[us->n++];
    snprintf(u->src, sizeof u->src, "%s", src);
    snprintf(u->obj, sizeof u->obj, "%s" SEP "%s.o", work, stem);
    snprintf(u->tmp, sizeof u->tmp, "%s" SEP "%s.o.tmp", work, stem);
    snprintf(u->stem, sizeof u->stem, "%s", stem);
    snprintf(u->log, sizeof u->log, "%s" SEP "%s.log", work, stem);
    struct stat st;
    u->bytes = stat(src, &st) == 0 ? (unsigned long long)st.st_size : 0;
    u->split = split_dispatch(src, &u->cold) && GAMELIB_SPLIT_FLAGS[0];
    u->cost = (double)u->bytes * (u->split ? SPLIT_COST : u->cold ? COLD_COST : 1.0);
}

// The next file to compile: of those written so far (from..n), the longest (longest processing time first: no big
// file is left running alone at the end while the other cores wait; the generator writes them in address order, and
// a phone takes minutes over the biggest ones). Swapped into place `from`; the gamelib entry, unit 0, goes first.
static void next_unit(Units *us, int from) {
    if (from < 1) return;
    int best = from;
    for (int i = from + 1; i < us->n; i++)
        if (us->u[i].cost > us->u[best].cost) best = i;
    if (best != from) {
        Unit t = us->u[from];
        us->u[from] = us->u[best];
        us->u[best] = t;
    }
}

// The flags the game code was built with (GAMELIB_CFLAGS, part of its id). zig cc reads -march=<cpu> with its own CPU
// names (x86_64_v3) and takes "-<feature>" off the end, so x86-64-v3 would be CPU "x86": the same CPU as -mcpu there.
// cold: a file of cold functions, at -O0 instead
static void add_cflags(Args *a, const Tools *t, bool cold) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", GAMELIB_CFLAGS);
    for (char *f = strtok(buf, " "); f; f = strtok(NULL, " ")) {
        if (cold && f[0] == '-' && f[1] == 'O') f = (char *)"-O0";
#ifdef __ANDROID__
        if (!strcmp(f, "-Iruntime")) continue;  // (gamelib_id.sh's, for a computer's checkout: the kit's -I instead)
        add(a, f);
        continue;
#endif
        if (t->zig && !strncmp(f, "-march=", 7)) {
            char cpu[128];
            snprintf(cpu, sizeof cpu, "-mcpu=%s", f + 7);
            for (char *c = cpu + 6; *c; c++) if (*c == '-') *c = '_';
            add(a, cpu);
        } else {
            add(a, f);
        }
    }
}

static PlatProc start_compile(const Tools *t, const Unit *u) {
    Args a = {0};
    compiler(&a, t);
    add_cflags(&a, t, u->cold);
    if (u->split) add_split(&a, GAMELIB_SPLIT_FLAGS);
    char inc[1200];
    snprintf(inc, sizeof inc, "-I%s", t->inc);
    add(&a, inc);
    add(&a, "-DGAMELIB_ID=\"" GAMELIB_ID "\"");
    add(&a, "-c");
    add(&a, u->src);
    add(&a, "-o");
    add(&a, u->tmp);
    PlatProc p = spawn(a.v, u->log);
    args_free(&a);
    return p;
}

#ifdef __ANDROID__
// MemAvailable in MB (/proc/meminfo), -1 if unknown
static long android_mem_available_mb(void) {
    FILE *f = fopen("/proc/meminfo", "rb");
    if (!f) return -1;
    char line[128];
    long kb = -1;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb < 0 ? -1 : kb / 1024;
}

static bool copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    bool ok = in && out;
    static char buf[1 << 20];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    if (in) ok = ok && !ferror(in), fclose(in);
    if (out && fclose(out) != 0) ok = false;
    return ok;
}

// The library goes where runtime/gamelib.c looks on a phone: the app's internal storage (the game folder is on the
// shared storage, mounted without exec). ondev = <internal>/ondevice. The external copy is removed (100 MB).
static bool install_internal(const char *game_dir, const char *ondev, char *why, size_t why_size) {
    char internal[1100], from[1200], to[1200], tmp[1300];
    snprintf(internal, sizeof internal, "%s", ondev);
    char *s = strrchr(internal, '/');
    if (s) *s = 0;
    char a[PATH_MAX], b[PATH_MAX];
    if (realpath(game_dir, a) && realpath(internal, b) && !strcmp(a, b)) return true;  // (the same folder)
    static const char *const ext[2] = {"", ".id"};  // (the library first: a matching .id says it's complete)
    for (int i = 0; i < 2; i++) {
        snprintf(from, sizeof from, "%s/%s%s", game_dir, GAMELIB_NAME, ext[i]);
        snprintf(to, sizeof to, "%s/%s%s", internal, GAMELIB_NAME, ext[i]);
        snprintf(tmp, sizeof tmp, "%s.tmp", to);
        if (i == 0) {
            char id[1200];  // (an old pair: its id goes first, so a half-copied library never looks current)
            snprintf(id, sizeof id, "%s/%s.id", internal, GAMELIB_NAME);
            remove(id);
        }
        if (!copy_file(from, tmp) || rename(tmp, to) != 0) {
            remove(tmp);
            LOG("[gamebuild] installing %s: %s\n", to, strerror(errno));
            snprintf(why, why_size, "the game code couldn't be installed (storage full?)");
            return false;
        }
    }
    snprintf(from, sizeof from, "%s/%s", game_dir, GAMELIB_NAME);
    remove(from);
    LOG("[gamebuild] installed %s/%s\n", internal, GAMELIB_NAME);
    return true;
}
#endif

// One module's generator (tools/gen.py <name> <nso> <base> <out> [--hle] + GAMELIB_GEN_ARGS)
static bool start_gen(const Tools *t, Gen *g, const char *game_dir, const char *base, bool hle) {
    char nso[1100];
    snprintf(nso, sizeof nso, "%s" SEP "exefs" SEP "%s", game_dir, g->name);
    Args a = {0};
    add(&a, t->gen);
    if (t->gen_script[0]) add(&a, t->gen_script);
    add(&a, g->name);
    add(&a, nso);
    add(&a, base);
    add(&a, g->out);
    if (hle) { add(&a, "--hle"); add(&a, t->hle); }
    if (GAMELIB_GEN_ARGS[0]) add_split(&a, GAMELIB_GEN_ARGS);
    g->p = spawn(a.v, g->log);
    args_free(&a);
    return g->p != 0;
}

// ------------------------------------------------------------------ the build
bool gamebuild_run(const char *game_dir, const GameBuildOpts *o, char *why, size_t why_size) {
    uint64_t t0 = plat_now_ns();
    static bool registered;
    if (!registered) registered = true, atexit(end_all);
    g_killed = false;
    Tools t;
#ifdef __ANDROID__
    // the generator and the NDK pieces out of the APK's assets (once per app version: LasActivity.buildTools)
    if (o->progress) o->progress(0, "Preparing the build tools...");
    // (test: LARECOMP_ONDEVICE_DIR=<an unpacked ondevice.zip>, without the app's Java side -
    // tools/android/ondevice/phone_gamebuild_test.sh)
    const char *ondev = getenv("LARECOMP_ONDEVICE_DIR");
    if (!ondev || !*ondev) ondev = android_build_tools();
    if (!ondev || !find_tools_android(&t, ondev)) {
#else
    if (!find_tools(&t)) {
#endif
        snprintf(why, why_size, "the game code builder is missing from this copy of LAS Recomp");
        return false;
    }
#ifdef __ANDROID__
    int android_jobs = 0;
    {
        // A phone stays usable: the compilers (they inherit this thread's niceness and CPUs) at a lower priority, one
        // per core, the little ones too. A little core takes ~3x as long per file but at a fraction of a big one's
        // power, so it adds throughput for little heat (the build is heat-bound: a Snapdragon 865 with 4 jobs on its
        // big cores reached 42 C and throttled); the biggest files go first (next_unit), the little cores end up with
        // the small ones. (An affinity mask from an earlier build is undone: all cores.)
        setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
        long n = sysconf(_SC_NPROCESSORS_CONF);
        int ncpu = n < 1 ? 1 : n > 64 ? 64 : (int)n;
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int i = 0; i < ncpu; i++) CPU_SET(i, &set);
        sched_setaffinity(0, sizeof set, &set);
        android_jobs = ncpu;
        LOG("[gamebuild] nice 10, %d jobs on all cores\n", ncpu);
        // the generator's Python (tools/android/ondevice/pylauncher.c) and its modules' libraries (ctypes)
        char v[2400];
        snprintf(v, sizeof v, "%s/pyhome", ondev);
        setenv("LASPY_HOME", v, 1);
        snprintf(v, sizeof v, "%s/tools:%s/pyhome/lib/python3.14/site-packages", ondev, ondev);
        setenv("LASPY_PATH", v, 1);
        setenv("LIBCAPSTONE_PATH", t.nlib, 1);
        setenv("LIBUNICORN_PATH", t.nlib, 1);
        setenv("TMPDIR", ondev, 1);
    }
#endif
    char work[1100], tmp[1100];
    snprintf(work, sizeof work, "%s" SEP ".gamebuild", game_dir);
    snprintf(tmp, sizeof tmp, "%s" SEP "%s.tmp", work, GAMELIB_NAME);
    char input_paths[6][1100], spec[4096];
    snprintf(input_paths[0], sizeof input_paths[0], "%s" SEP "exefs" SEP "main", game_dir);
    snprintf(input_paths[1], sizeof input_paths[1], "%s" SEP "exefs" SEP "sdk", game_dir);
    const char *headers[] = {"cpu.h", "simd.h", "simd_fp.h", "gamelib.h"};
    for (int i = 0; i < 4; ++i) snprintf(input_paths[i+2], sizeof input_paths[i+2], "%s" SEP "%s", t.inc, headers[i]);
    // Generator changes should rebuild only chunks whose C changed, as in the existing incremental builder.
    // Compiler/header/flag changes invalidate every object; entry always rebuilds because it embeds GAMELIB_ID.
    const char *inputs[] = {input_paths[2], input_paths[3], input_paths[4], input_paths[5], t.cc};
    // The compiler's contents identify it, so relocating the app does not discard reusable objects.
    snprintf(spec, sizeof spec, "LASBUILD3\n%s\n%s\n%s\nzig=%d pointer=%d%s%s\n%s\n", GAMELIB_BASE_ID,
             GAMELIB_CFLAGS, GAMELIB_SHARED, t.zig, (int)sizeof(void *),  // (+ the Linux zig target: objects from
             t.zig && ZIG_TARGET[0] ? " target=" : "", t.zig ? ZIG_TARGET : "",  // before it are rebuilt once)
             GAMELIB_SPLIT_FLAGS);
    if (o->progress) o->progress(0, "Checking completed build files...");
    bool resuming = build_cache_resumable(game_dir);  // (a cancelled or interrupted build; read before open marks this one)
    BuildCache *cache = build_cache_open(game_dir, spec, inputs, 5, why, why_size);
    if (!cache) return false;
    // an update keeps the last good build's objects (open drops them when compiler, headers or flags changed)
    bool update = false;
    if (!resuming) {
        char rp[1200];
        struct stat st;
        snprintf(rp, sizeof rp, "%s" SEP "gamelib_entry.receipt", work);
        update = stat(rp, &st) == 0;
    }
    const char *gen_inputs[5] = {input_paths[0], input_paths[1], t.gen, t.hle};
    int ngen = 4;
    if (t.gen_script[0]) gen_inputs[ngen++] = t.gen_script;
    if (!build_cache_prepare_generation(cache, GAMELIB_ID, gen_inputs, ngen, why, why_size)) {
        build_cache_failed(cache);
        build_cache_close(cache);
        return false;
    }
    if (t.zig) {  // keep its caches with this installation; the explicit Clear action removes them too
        char zc[1200];
        snprintf(zc, sizeof zc, "%s" SEP "zig-cache", work);
#ifdef _WIN32
        _putenv_s("ZIG_GLOBAL_CACHE_DIR", zc);
        _putenv_s("ZIG_LOCAL_CACHE_DIR", zc);
#else
        setenv("ZIG_GLOBAL_CACHE_DIR", zc, 1);
        setenv("ZIG_LOCAL_CACHE_DIR", zc, 1);
#endif
    }
    LOG("[gamebuild] generator %s%s%s, compiler %s%s\n", t.gen, t.gen_script[0] ? " " : "", t.gen_script,
        t.cc, t.zig ? " cc" : "");

    // 1. the generator, both modules at once (phones: one after the other, sdk first - main alone peaks at ~1.7 GB);
    // their finished files are compiled while it goes on
    Gen gens[2] = {{.name = "main"}, {.name = "sdk"}};
    static const char *const base[2] = {"0x7000000000", "0x7004000000"};
#ifdef __ANDROID__
    const bool one_at_a_time = true;
#else
    const bool one_at_a_time = false;
#endif
    bool ok = true, gen_waiting = false;  // (main not started yet: it waits for sdk)
    for (int k = 0; k < 2 && ok; k++) {
        int i = 1 - k;  // (sdk first)
        Gen *g = &gens[i];
        g->exit = -1;
        snprintf(g->log, sizeof g->log, "%s" SEP "gen_%s.log", work, g->name);
        snprintf(g->out, sizeof g->out, "%s" SEP "%s", work, g->name);
        if (build_cache_generation(cache, g->name, &g->files)) {
            g->exit = 0;
            LOG("[gamebuild] reused generated %s: %d chunks\n", g->name, g->files);
            continue;
        }
        if (i == 0 && one_at_a_time && gens[1].exit != 0) { gen_waiting = true; continue; }
        if (!start_gen(&t, g, game_dir, base[i], i == 1)) { snprintf(why, why_size, "the game code builder didn't start"); ok = false; }
    }

    Units units = {0};
    int queued[2] = {0, 0};  // chunk files of each module put in units so far
    bool tables[2] = {false, false};
    int next = 0, jobs = o->jobs < 1 ? 1 : o->jobs > 64 ? 64 : o->jobs;
#ifdef __ANDROID__
    if (android_jobs > jobs) jobs = android_jobs;
    uint64_t last_start = 0;  // (memory: see below)
    long low_mem = -1;        // the lowest MemAvailable seen while starting compilers (the log, for tuning)
    int held = 0;             // compiler starts put off for memory
#endif
    {  // (testing / tuning on a device: LARECOMP_BUILD_JOBS in env.txt)
        const char *e = getenv("LARECOMP_BUILD_JOBS");
        if (e && atoi(e) > 0) jobs = atoi(e) > 64 ? 64 : atoi(e);
    }
    LOG("[gamebuild] %d compile jobs\n", jobs);
    struct { PlatProc p; int unit; } run[64];
    int nrun = 0;
    int reused = 0, compiled = 0;
    double done_cost = 0;  // (progress in compile time - Unit cost - rather than bytes: the slow files go first)
    double shown = 0;
    {
        add_unit(&units, t.entry, work, "gamelib_entry");
    }
    while (ok) {
        if (o->cancelled && o->cancelled()) {
            snprintf(why, why_size, "Compilation cancelled. Completed files are kept for Resume."); ok = false; break;
        }
        // generator progress, newly finished files
        bool gens_done = true;
        double gen_frac = 0;
        if (gen_waiting && gens[1].exit == 0) {  // (sdk done: main's turn)
            gen_waiting = false;
            if (!start_gen(&t, &gens[0], game_dir, base[0], false)) {
                snprintf(why, why_size, "the game code builder didn't start");
                ok = false;
                break;
            }
        }
        for (int i = 0; i < 2; i++) {
            Gen *g = &gens[i];
            if (g->exit < 0 && g->p) {  // (not a waiting one: no process yet)
                read_gen_log(g);
                int r = finished(g->p);
                if (r >= 0) {
                    g->exit = r;
                    read_gen_log(g);
                    if (r != 0) {
                        char what[64];
                        snprintf(what, sizeof what, "generator (%s)", g->name);
                        snprintf(why, why_size, "the game code couldn't be prepared");
                        helper_failed(what, r, g->log, why, why_size);
                        ok = false;
                    } else if (!build_cache_save_generation(cache, g->name, g->files, why, why_size)) {
                        ok = false;
                    } else {
                        LOG("[gamebuild] generated %s: %d chunks, at %.0fs\n", g->name, g->files,
                            (double)(plat_now_ns() - t0) / 1e9);
                    }
                }
            }
            if (g->exit != 0) gens_done = false;
            for (; queued[i] < g->files; queued[i]++) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_%03d", g->name, queued[i]);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem);
            }
            if (g->exit == 0 && !tables[i]) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_table", g->name);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem);
                tables[i] = true;
            }
            // (weighted by the share of the C each module makes: main ~78%, sdk ~22%)
            double w = i == 0 ? 0.78 : 0.22;
            gen_frac += g->exit == 0 ? w : g->n ? w * (double)g->ci / (double)g->n : 0;
        }
        if (!ok) break;
        double written_cost = 0;
        for (int k = 0; k < units.n; k++) written_cost += units.u[k].cost;

        // compiles
        while (nrun < jobs && next < units.n) {
#ifdef __ANDROID__
            // Memory: the generator of main alone takes ~1.7 GB and the biggest compiles 300-500 MB each (they go
            // first), on phones with 6 GB. Below 1.5 GB available a new compiler starts at most once a second (its
            // memory shows in MemAvailable by then), below 700 MB not at all while two still run - the build slows
            // down instead of the system killing it.
            if (nrun >= 2) {
                long avail = android_mem_available_mb();
                uint64_t now = plat_now_ns();
                if (avail >= 0 && (low_mem < 0 || avail < low_mem)) low_mem = avail;
                if (avail >= 0 && (avail < 700 || (avail < 1500 && now - last_start < 1000000000ull))) { ++held; break; }
            }
#endif
            next_unit(&units, next);
            Unit *u = &units.u[next];
            int cached = build_cache_object(cache, u->stem, u->src, u->source_hash);
            if (cached < 0) { snprintf(why, why_size, "A generated source file is missing or unreadable."); ok = false; break; }
            if (cached && next != 0) { done_cost += u->cost; ++reused; ++next; continue; }
            PlatProc p = start_compile(&t, &units.u[next]);
            if (!p) { snprintf(why, why_size, "the compiler didn't start"); ok = false; break; }
            run[nrun].p = p, run[nrun].unit = next++;
            nrun++;
#ifdef __ANDROID__
            last_start = plat_now_ns();
#endif
        }
        for (int k = 0; k < nrun; k++) {
            int r = finished(run[k].p);
            if (r < 0) continue;
            Unit *u = &units.u[run[k].unit];
            if (r != 0) {
                char what[1200];
                snprintf(what, sizeof what, "compiling %s", u->src);
                snprintf(why, why_size, "the game code didn't compile");
                helper_failed(what, r, u->log, why, why_size);
                ok = false;
            } else if (!build_cache_save_object(cache, u->stem, u->src, u->source_hash, why, why_size)) {
                ok = false;
            } else {
                ++compiled;
            }
            done_cost += u->cost;
            run[k--] = run[--nrun];
        }
        if (!ok) break;
        if (gens_done && next == units.n && nrun == 0) {
            LOG("[gamebuild] compiled at %.0fs\n", (double)(plat_now_ns() - t0) / 1e9);
#ifdef __ANDROID__
            LOG("[gamebuild] lowest MemAvailable %ld MB, %d compiler starts put off for memory\n", low_mem, held);
#endif
            break;
        }

        // progress: the generator's share (its functions written), the compiler's (the expected compile time of the
        // files done - Unit cost - of an estimate of all of it while the generator still writes)
        double est_total = gens_done ? written_cost : gen_frac > 0.02 ? written_cost / gen_frac : 0;
        double comp_frac = est_total > 0 ? done_cost / est_total : 0;
        if (comp_frac > 1) comp_frac = 1;
        // (an update that keeps the last build's pieces compiles little: generating the C is most of its time)
        double f = update ? 0.85 * gen_frac + 0.1 * comp_frac : 0.2 * gen_frac + 0.75 * comp_frac;
        double secs = (double)(plat_now_ns() - t0) / 1e9;
        if (gen_frac == 0) f = 0.05 * (secs / (secs + 20));  // (the generator's first pass: no count yet)
        if (f > shown) shown = f;
        if (o->progress) o->progress(shown, gen_frac < 1 && !gens_done ? "Reading the game code..." : resuming ? "Resuming compilation..." : "Recompiling...");
        sleep_ms(50);
    }
    for (int k = 0; k < nrun; k++) stop(run[k].p);  // (a failure: the rest is of no use - the failure shows at once)
    for (int i = 0; i < 2; i++)
        if (gens[i].p && gens[i].exit < 0) stop(gens[i].p);

    // 2. link
    if (ok) {
        if (o->progress) o->progress(shown > 0.95 ? shown : 0.95, "Finishing...");
        char rsp[1100], log[1100];
        snprintf(rsp, sizeof rsp, "%s" SEP "link.rsp", work);
        snprintf(log, sizeof log, "%s" SEP "link.log", work);
        FILE *f = fopen(rsp, "wb");
        if (f) {
            for (int k = 0; k < units.n; k++) {  // (forward slashes: the response file's backslashes are escapes)
                fputc('"', f);
                for (const char *c = units.u[k].obj; *c; c++) fputc(*c == '\\' ? '/' : *c, f);
                fputs("\"\n", f);
            }
            fclose(f);
        }
        Args a = {0};
        char at[1200];
        snprintf(at, sizeof at, "@%s", rsp);
#ifdef _WIN32
        if (t.zig_rt[0]) {  // the linker itself, with the runtime built ahead of time (its flags and libraries: link.rsp)
            char opt[1200];
            add(&a, t.cc);
            add(&a, "lld-link");
            snprintf(opt, sizeof opt, "@%s" SEP "link.rsp", t.zig_rt);
            add(&a, opt);
            snprintf(opt, sizeof opt, "-LIBPATH:%s", t.zig_rt);
            add(&a, opt);
            snprintf(opt, sizeof opt, "-OUT:%s", tmp);
            add(&a, opt);
            add(&a, at);
            add(&a, t.implib);
        } else
#endif
#ifdef __ANDROID__
        {  // zig's ld.lld as NDK clang runs it (cckit/link.rsp: tools/android/ondevice/build_cckit.sh), against the
           // app's own libmain.so (the runtime functions the game code calls: it is loaded first)
            char opt[1200];
            static const char *const sys = "/sysroot/usr/lib/aarch64-linux-android/30";
            add(&a, t.cc);
            add(&a, "ld.lld");
            snprintf(opt, sizeof opt, "@%s/link.rsp", t.kit);
            add(&a, opt);
            add(&a, "-o");
            add(&a, tmp);
            snprintf(opt, sizeof opt, "%s%s/crtbegin_so.o", t.kit, sys);
            add(&a, opt);
            add(&a, at);
            snprintf(opt, sizeof opt, "-L%s", t.nlib);
            add(&a, opt);
            add(&a, "-lmain");
            snprintf(opt, sizeof opt, "-L%s%s", t.kit, sys);
            add(&a, opt);
            snprintf(opt, sizeof opt, "%s/clang/lib/libclang_rt.builtins-aarch64-android.a", t.kit);
            add(&a, opt);
            add(&a, "-ldl");
            add(&a, "-lc");
            add(&a, opt);
            snprintf(opt, sizeof opt, "%s%s/crtend_so.o", t.kit, sys);
            add(&a, opt);
        }
        if (0)
#endif
        {
            compiler(&a, &t);
            add_split(&a, GAMELIB_SHARED);
            add(&a, "-o");
            add(&a, tmp);
            add(&a, at);
#ifdef _WIN32
            add(&a, t.implib);
#endif
        }
        PlatProc p = f ? spawn(a.v, log) : 0;
        args_free(&a);
        int r = p ? -1 : 255;
        while (p && (r = finished(p)) < 0) {
            if (o->cancelled && o->cancelled()) { stop(p); r = 130; break; }
            sleep_ms(50);
        }
        if (r == 130) {
            LOG("[gamebuild] linking cancelled\n");
            snprintf(why, why_size, "Compilation cancelled. Completed files are kept for Resume.");
            ok = false;
        } else if (!p) {
            LOG("[gamebuild] linking failed: the linker didn't start\n");
            snprintf(why, why_size, "the game code didn't link");
            ok = false;
        } else if (r != 0) {
            snprintf(why, why_size, "the game code didn't link");
            helper_failed("linking", r, log, why, why_size);
            ok = false;
        }
    }
    if (ok) ok = build_cache_publish(cache, GAMELIB_NAME, GAMELIB_ID, why, why_size);
#ifdef __ANDROID__
    if (ok) ok = install_internal(game_dir, t.ondev, why, why_size);
#endif
    bool cancelled = !ok && o->cancelled && o->cancelled();
    if (!ok && !cancelled) build_cache_failed(cache);
    free(units.u);
    build_cache_close(cache);
    LOG("[gamebuild] reused %d objects, compiled %d objects\n", reused, compiled);
    LOG("[gamebuild] %s in %.0fs\n", ok ? "done" : cancelled ? "cancelled" : "failed", (double)(plat_now_ns() - t0) / 1e9);
    if (ok && o->progress) o->progress(1, "Finishing...");
    return ok;
}

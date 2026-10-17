// Android host layer (runtime/platform.h), arm64. The Linux layer's approach (one read/write guest window, sigaction
// crash handler), with what Android does differently: the app is forked from the zygote (no re-exec, no fresh address
// space: the guest window sits GUEST_SHIFT lower, runtime/cpu.h), there is no terminal (stderr goes to logcat and to
// lasrecomp.log in the data folder), backtraces come from the unwinder, and the "executable" is libmain.so.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

#include "../platform.h"

void rt_crash_explain(char *out, size_t n, const char *log_path);  // runtime/crash_explain.c

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// ---- guest address space
// As on Linux: everything readable/writable from the start, untouched pages cost nothing (MAP_NORESERVE).
bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (p != (void *)(uintptr_t)lo) { munmap(p, hi - lo); return false; }  // (kernels before 4.17: just a hint)
    return true;
}

bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                -1, 0) == (void *)(uintptr_t)lo;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {  // (no write tracking)
    (void)addr; (void)size; (void)pages; (void)max; (void)page;
    return -1;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    struct timespec ts = {.tv_sec = (time_t)(t / 1000000000u), .tv_nsec = (long)(t % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

// Apps can't have SCHED_FIFO; the most urgent nice value an app may set (Android's display / audio threads use it).
void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    setpriority(PRIO_PROCESS, (id_t)gettid(), -10);
}

// ---- log
// stdout / stderr into a pipe; a thread hands each line to logcat (tag LASRecomp) and appends it to lasrecomp.log in
// the data folder (the previous run's log is kept as lasrecomp.prev.log: after a crash, the app is started again).
static char g_log_path[1024];
static FILE *g_log_file;

static void *log_pump(void *arg) {
    plat_thread_name("log");
    int fd = (int)(intptr_t)arg;
    char buf[4096], line[4096];
    size_t n_line = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        if (g_log_file) { fwrite(buf, 1, (size_t)n, g_log_file); fflush(g_log_file); }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || n_line == sizeof line - 1) {
                line[n_line] = 0;
                __android_log_write(ANDROID_LOG_INFO, "LASRecomp", line);
                n_line = 0;
                if (buf[i] == '\n') continue;
            }
            line[n_line++] = buf[i];
        }
    }
    return NULL;
}

static void log_start(void) {
    const char *dir = SDL_GetAndroidExternalStoragePath();
    if (!dir) dir = SDL_GetAndroidInternalStoragePath();
    if (dir) {
        char prev[1100];
        snprintf(g_log_path, sizeof g_log_path, "%s/lasrecomp.log", dir);
        snprintf(prev, sizeof prev, "%s/lasrecomp.prev.log", dir);
        rename(g_log_path, prev);
        g_log_file = fopen(g_log_path, "w");
    }
    int p[2];
    if (pipe(p) != 0) return;
    dup2(p[1], STDERR_FILENO);
    dup2(p[1], STDOUT_FILENO);
    close(p[1]);
    pthread_t t;
    pthread_create(&t, NULL, log_pump, (void *)(intptr_t)p[0]);
    pthread_detach(t);
}

void plat_init(void) { log_start(); }

// The "executable": libmain.so (SDL's convention), possibly inside the APK (".../base.apk!/lib/arm64-v8a/libmain.so":
// libraries stored uncompressed are loaded from the APK in place).
bool plat_exe_path(char *buf, size_t size) {
    Dl_info di = {0};
    if (!dladdr((void *)plat_exe_path, &di) || !di.dli_fname) return false;
    snprintf(buf, size, "%s", di.dli_fname);
    return true;
}

void plat_exec(char *const argv[]) { (void)argv; }  // (an app can't replace its process; callers handle the return)

// ---- diagnostics
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

void plat_fatal_notice(void) {
    fflush(stderr);
    char text[1600];
    rt_crash_explain(text, sizeof text, g_log_path[0] ? g_log_path : "logcat (tag LASRecomp)");
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "LAS Recomp", text, NULL);
}

bool plat_module_name(uint64_t pc, char *out, size_t n) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_fname) return false;
    snprintf(out, n, "%s", di.dli_fname);
    return true;
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

typedef struct { void **pcs; int n, max; } Frames;
static _Unwind_Reason_Code unwind_cb(struct _Unwind_Context *ctx, void *arg) {
    Frames *f = arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && f->n < f->max) f->pcs[f->n++] = (void *)pc;
    return f->n < f->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}

void plat_print_backtrace(void) {
    void *bt[64];
    Frames fr = {bt, 0, 64};
    _Unwind_Backtrace(unwind_cb, &fr);
    Dl_info self = {0};
    dladdr((void *)plat_print_backtrace, &self);
    for (int i = 0; i < fr.n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        Dl_info di = {0};
        if (!dladdr(bt[i], &di)) di.dli_fname = NULL;
        bool ours = di.dli_fbase && di.dli_fbase == self.dli_fbase;
        const char *s = plat_symbol(pc, &off);
        if (!s && ours) s = rt_host_fn_name(pc, &off);
        const char *mod = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        uint64_t moff = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx (%s+%llx)\n", i, (unsigned long long)pc, s, (unsigned long long)off,
                       mod, (unsigned long long)moff);
        else fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, mod, (unsigned long long)moff);
    }
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die)
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
    uint64_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);  // (ends in abort(): the system's tombstone follows)
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes (none on Android: the seed generator and the game code build run elsewhere)
#include <ftw.h>

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    (void)argv; (void)log_path; (void)append_log;
    return 0;
}
void plat_proc_kill(PlatProc p) { (void)p; }
int plat_proc_wait(PlatProc p, bool block) { (void)p; (void)block; return 255; }

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }

void plat_terminate(void) { _exit(0); }

void plat_thread_name(const char *name) { pthread_setname_np(pthread_self(), name); }

// Phones have one or two fast cores (the S20 FE's Snapdragon 865: cpu7 at 2.84 GHz, cpu4-6 at 2.42, cpu0-3 small) and
// the game runs on one thread: that one gets the fastest core to itself, the render thread the other big ones, the
// game's other threads anything but the fastest. Cores by their top frequency. LARECOMP_AFFINITY=0: leave it to Android.
static cpu_set_t g_core_sets[3];
static bool g_core_on;

static void core_scan(void) {
    long freq[CPU_SETSIZE] = {0}, best = 0, second = 0;
    int n = 0, top = -1, ntop = 0, nsecond = 0;
    for (; n < CPU_SETSIZE; n++) {
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", n);
        FILE *f = fopen(path, "r");
        if (!f) break;
        if (fscanf(f, "%ld", &freq[n]) != 1) freq[n] = 0;
        fclose(f);
        if (freq[n] > best) best = freq[n], top = n;
    }
    for (int i = 0; i < n; i++) if (freq[i] < best && freq[i] > second) second = freq[i];
    for (int i = 0; i < n; i++) ntop += freq[i] == best, nsecond += freq[i] == second;
    const char *e = getenv("LARECOMP_AFFINITY");
    if ((e && !atoi(e)) || ntop != 1 || !nsecond) return;  // (no single fastest core: nothing to do)
    for (int r = 0; r < 3; r++) CPU_ZERO(&g_core_sets[r]);
    for (int i = 0; i < n; i++) {
        if (i == top) CPU_SET(i, &g_core_sets[PLAT_CORE_MAIN]);
        if (freq[i] == second) CPU_SET(i, &g_core_sets[PLAT_CORE_RENDER]);
        if (i != top) CPU_SET(i, &g_core_sets[PLAT_CORE_OTHER]);
    }
    g_core_on = true;
    fprintf(stderr, "[cpu] game thread on cpu%d (%ld MHz), render thread on the %d cores at %ld MHz\n", top, best / 1000,
            nsecond, second / 1000);
}

// Performance hints (APerformanceHint, Android 13+, looked up at run time: the app starts on Android 11): one session
// for the game's main thread and the render thread, told each frame how long the game thread worked against the
// frame's target. That is what gets a phone's fastest core out of isolation and the clocks up (core_ctl parks the
// prime core while nothing it counts as heavy runs; a 70% thread on a big core isn't).
static pid_t g_hint_tids[2];
static void *g_hint_session;
static void *(*p_get_manager)(void);
static void *(*p_create_session)(void *, const int32_t *, size_t, int64_t);
static int (*p_update_target)(void *, int64_t);
static int (*p_report)(void *, int64_t);
static int (*p_set_threads)(void *, const pid_t *, size_t);

static void hint_load(void) {
    void *lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return;
    p_get_manager = (void *(*)(void))dlsym(lib, "APerformanceHint_getManager");
    p_create_session = (void *(*)(void *, const int32_t *, size_t, int64_t))dlsym(lib, "APerformanceHint_createSession");
    p_update_target = (int (*)(void *, int64_t))dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
    p_report = (int (*)(void *, int64_t))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    p_set_threads = (int (*)(void *, const pid_t *, size_t))dlsym(lib, "APerformanceHint_setThreads");
}

void plat_thread_core(int role) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, core_scan);
    if (role == PLAT_CORE_MAIN || role == PLAT_CORE_RENDER) {  // (the hint session's threads)
        pid_t tid = gettid(), *slot = &g_hint_tids[role == PLAT_CORE_RENDER];
        if (*slot != tid) {
            *slot = tid;
            if (g_hint_session && p_set_threads) {
                pid_t t[2];
                size_t n = 0;
                for (int i = 0; i < 2; i++) if (g_hint_tids[i]) t[n++] = g_hint_tids[i];
                p_set_threads(g_hint_session, t, n);
            }
        }
    }
    if (!g_core_on || role < 0 || role > 2) return;
    // the fastest core alone; while it's parked (isolated: EINVAL) the big ones
    if (sched_setaffinity(0, sizeof g_core_sets[role], &g_core_sets[role]) == 0) return;
    cpu_set_t big = g_core_sets[PLAT_CORE_RENDER];
    CPU_OR(&big, &big, &g_core_sets[PLAT_CORE_MAIN]);
    if (role != PLAT_CORE_MAIN || sched_setaffinity(0, sizeof big, &big) != 0)
        fprintf(stderr, "[cpu] affinity: %s\n", strerror(errno));
}

void plat_frame_work(uint64_t busy_ns, uint64_t target_ns) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    static int64_t target;
    static bool failed;
    const char *e = getenv("LARECOMP_PERF_HINT");
    if (failed || (e && !atoi(e))) return;
    pthread_once(&once, hint_load);
    if (!g_hint_session) {
        if (!p_get_manager || !p_create_session || !p_report || !g_hint_tids[0]) { failed = true; return; }
        void *m = p_get_manager();
        int32_t t[2];
        size_t n = 0;
        for (int i = 0; i < 2; i++) if (g_hint_tids[i]) t[n++] = g_hint_tids[i];
        g_hint_session = m ? p_create_session(m, t, n, (int64_t)target_ns) : NULL;
        if (!g_hint_session) {
            failed = true;
            fprintf(stderr, "[cpu] no performance hint session\n");
            return;
        }
        target = (int64_t)target_ns;
        fprintf(stderr, "[cpu] performance hint session: %zu threads, target %.1f ms\n", n, target_ns / 1e6);
    }
    if ((int64_t)target_ns != target && p_update_target) p_update_target(g_hint_session, target = (int64_t)target_ns);
    if (busy_ns) p_report(g_hint_session, (int64_t)busy_ns);
}

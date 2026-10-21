// Data root and shipped resources (runtime/paths.h).
#include <SDL3/SDL.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "gamelib.h"
#include "paths.h"
#include "platform.h"

static char g_root[1024], g_start[1024], g_exe_dir[1024];

#ifdef _WIN32
#define SEP "\\"
static bool is_link(const char *p) {  // symlink or junction
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
// The final path, junctions and symlinks followed (GetFinalPathNameByHandle); _fullpath (lexical, as before) only
// where the file system can't say
static bool resolve(const char *p, char *out, size_t n) {
    wchar_t w[1100], f[1100];
    if (MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 1100) > 0) {
        HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, NULL);  // (BACKUP_SEMANTICS: folders too)
        if (h != INVALID_HANDLE_VALUE) {
            DWORD k = GetFinalPathNameByHandleW(h, f, 1100, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            CloseHandle(h);
            if (k && k < 1100) {
                const wchar_t *s = f;
                if (!wcsncmp(f, L"\\\\?\\UNC\\", 8)) s = f + 6, f[6] = L'\\';  // \\?\UNC\srv\share -> \\srv\share
                else if (!wcsncmp(f, L"\\\\?\\", 4)) s = f + 4;                    // \\?\C:\... -> C:\...
                return WideCharToMultiByte(CP_UTF8, 0, s, -1, out, (int)n, NULL, NULL) > 0;
            }
        }
    }
    return _fullpath(out, p, n) != NULL;
}
#define path_eq(a, b) (_stricmp(a, b) == 0)
#define path_prefix(a, b, n) (_strnicmp(a, b, n) == 0)
#else
#define SEP "/"
static bool is_link(const char *p) {
    struct stat st;
    return lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
}
static bool resolve(const char *p, char *out, size_t n) {
    char buf[PATH_MAX];
    if (!realpath(p, buf)) return false;
    snprintf(out, n, "%s", buf);
    return true;
}
#define path_eq(a, b) (strcmp(a, b) == 0)
#define path_prefix(a, b, n) (strncmp(a, b, n) == 0)
#endif

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/' || *c == '\\') { char s = *c; *c = 0; mkdir(p, 0755); *c = s; }
    mkdir(p, 0755);
}

static bool writable(const char *dir) {
    char t[1100];
    snprintf(t, sizeof t, "%s" SEP ".write_test", dir);
    FILE *f = fopen(t, "wb");
    if (!f) return false;
    fclose(f);
    remove(t);
    return true;
}

static void fail(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "LAS Recomp", msg, NULL);
    exit(1);
}

static bool g_dev;  // the data root is the starting folder because it has extracted/ (a source checkout)
bool rt_dev_mode(void) { return g_dev; }

#ifdef __ANDROID__
// The files the app ships are APK assets (android/app/src/main/assets), which only SDL's file streams can read: copied
// once into <internal storage>/res, which then stands in for the executable's folder (rt_resource).
static void android_resources(void) {
    static const char *const names[] = {"Baloo2-SemiBold.ttf", "DejaVuSans.ttf", "icon.png"};
    const char *internal = SDL_GetAndroidInternalStoragePath();
    if (!internal) return;
    snprintf(g_exe_dir, sizeof g_exe_dir, "%s/res", internal);
    mkdirs(g_exe_dir);
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        SDL_IOStream *in = SDL_IOFromFile(names[i], "rb");
        if (!in) continue;
        Sint64 size = SDL_GetIOSize(in);
        char out[1200];
        snprintf(out, sizeof out, "%s/%s", g_exe_dir, names[i]);
        struct stat st;
        if (size > 0 && !(stat(out, &st) == 0 && st.st_size == size)) {
            void *data = SDL_LoadFile_IO(in, NULL, false);
            FILE *f = data ? fopen(out, "wb") : NULL;
            if (f) { fwrite(data, 1, (size_t)size, f); fclose(f); }
            SDL_free(data);
        }
        SDL_CloseIO(in);
    }
}
#endif

void rt_paths_init(void) {
    if (!getcwd(g_start, sizeof g_start)) g_start[0] = 0;
#ifdef __ANDROID__
    android_resources();
#else
    if (plat_exe_path(g_exe_dir, sizeof g_exe_dir)) {
        char *s = strrchr(g_exe_dir, '/'), *b = strrchr(g_exe_dir, '\\');
        if (b > s) s = b;
        if (s) *s = 0;
    }
#endif
    char root[1024];
    const char *env = getenv("LARECOMP_DATA_ROOT");
    if (env && *env) snprintf(root, sizeof root, "%s", env);
    else if (is_dir("extracted")) snprintf(root, sizeof root, "%s", g_start), g_dev = true;  // dev / portable: as before
    else {
#ifdef _WIN32
        snprintf(root, sizeof root, "%s", g_exe_dir);
#elif defined(__ANDROID__)
        // Android/data/<package>/files: the app's own without any permission, and reachable from a computer over USB
        // (game files, seeds, saves, the log)
        const char *ext = SDL_GetAndroidExternalStoragePath();
        snprintf(root, sizeof root, "%s", ext ? ext : SDL_GetAndroidInternalStoragePath());
#elif defined(__linux__)
        // (the AppImage is read-only): $XDG_DATA_HOME/lasrecomp, ~/.local/share/lasrecomp by default
        const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
        if (xdg && *xdg == '/') snprintf(root, sizeof root, "%s/lasrecomp", xdg);
        else snprintf(root, sizeof root, "%s/.local/share/lasrecomp", home && *home ? home : "/tmp");
#else
        const char *home = getenv("HOME");
        snprintf(root, sizeof root, "%s/Library/Application Support/" RT_BUNDLE_ID, home && *home ? home : "/tmp");
#endif
    }
    mkdirs(root);
    char msg[1400];
    if (!is_dir(root) || !writable(root) || chdir(root) != 0) {
#ifdef _WIN32
        snprintf(msg, sizeof msg, "LAS Recomp can't write to its folder:\n%s\n\nIt keeps game files, saves and seeds "
                 "next to the program. Move the \"LAS Recomp\" folder somewhere you can write to (e.g. Documents or "
                 "Desktop, not Program Files) and start it again.", root);
#else
        snprintf(msg, sizeof msg, "LAS Recomp can't write to its data folder:\n%s", root);
#endif
        fail(msg);
    }
    if (!getcwd(g_root, sizeof g_root)) snprintf(g_root, sizeof g_root, "%s", root);
    fprintf(stderr, "[paths] data: %s\n", g_root);
}

const char *rt_data_root(void) { return g_root; }

bool rt_resource(const char *name, const char *dev_rel, char *out, size_t n) {
    char c[4][1100] = {{0}};
    if (name) {
        snprintf(c[0], sizeof c[0], "%s" SEP "%s", g_exe_dir, name);
        snprintf(c[1], sizeof c[1], "%s" SEP ".." SEP "Resources" SEP "%s", g_exe_dir, name);  // .app/Contents/Resources
    }
    if (dev_rel) {
        snprintf(c[2], sizeof c[2], "%s" SEP "%s", g_start, dev_rel);
        snprintf(c[3], sizeof c[3], "%s" SEP ".." SEP "%s", g_exe_dir, dev_rel);  // build/lasrecomp -> repo root
    }
    for (int i = 0; i < 4; i++)
        if (c[i][0] && exists(c[i])) return (size_t)snprintf(out, n, "%s", c[i]) < n;  // (as is: a venv python must stay a link)
    return false;
}

// ---- game files
// What the setup (runtime/ui/launcher.mm) creates in extracted/, plus what older versions' extraction and manual
// extractions left there (never read at run time).
static const char *const GAME_FILES[] = {"exefs", "romfs", "romfs.bin", GAMELIB_NAME, GAMELIB_NAME ".id", ".gamebuild",
                                         ".zigcache", ".extract", "extract.log", "xci_secure", "nso_raw", "update_pfs",
                                         ".DS_Store"};

// The other data folder the game files may be in (Android: the memory card's; rt_paths_set_game_root)
static char g_game_root[1024];
void rt_paths_set_game_root(const char *root) { snprintf(g_game_root, sizeof g_game_root, "%s", root ? root : ""); }

// <data root>/extracted with links resolved (false: no data root, or it can't be resolved)
static bool game_dir_want(char *want, size_t n) {
    char root[1100];
    if (!g_root[0] || !resolve(g_root, root, sizeof root)) return false;
    return (size_t)snprintf(want, n, "%s" SEP "extracted", root) < n;
}

// game_dir must be <data root>/extracted (or <the game files' data folder>/extracted, rt_paths_set_game_root), a
// real folder (a dev worktree's symlinked extracted/ is refused)
static bool game_dir_ok(const char *game_dir) {
    char want[1100], got[1100];
    if (is_link(game_dir)) {
        fprintf(stderr, "[paths] %s is a link (symlink / junction): its game files are not ours to delete\n", game_dir);
        return false;
    }
    if (!game_dir_want(want, sizeof want) || !resolve(game_dir, got, sizeof got)) return false;
    if (path_eq(want, got)) return true;
    char alt[1100], alt_r[1100];
    if (g_game_root[0] && (size_t)snprintf(alt, sizeof alt, "%s" SEP "extracted", g_game_root) < sizeof alt &&
        path_eq(resolve(alt, alt_r, sizeof alt_r) ? alt_r : alt, got))
        return true;
    fprintf(stderr, "[paths] %s resolves to %s, not the data folder's %s\n", game_dir, got, want);
    return false;
}

bool rt_remove_game_file(const char *game_dir, const char *name) {
    bool known = false;
    for (size_t i = 0; i < sizeof GAME_FILES / sizeof *GAME_FILES; i++) known |= !strcmp(name, GAME_FILES[i]);
    if (!known || !game_dir_ok(game_dir)) {
        fprintf(stderr, "[paths] refusing to delete %s/%s (not app-created game files in the data root)\n", game_dir, name);
        return false;
    }
    char p[1100];
    snprintf(p, sizeof p, "%s" SEP "%s", game_dir, name);
    if (!exists(p) && !is_link(p)) return true;
    if (is_link(p)) {
        fprintf(stderr, "[paths] refusing to delete %s (a link)\n", p);
        return false;
    }
    // the entry itself must resolve inside <data root>/extracted too (belt and braces: neither it nor game_dir is a link)
    char want[1100], got[1100];
    size_t wl = game_dir_want(want, sizeof want) ? strlen(want) : 0;
    if (!wl || !resolve(p, got, sizeof got) || strlen(got) <= wl || !path_prefix(got, want, wl) ||
        (got[wl] != '/' && got[wl] != '\\')) {
        fprintf(stderr, "[paths] refusing to delete %s (resolves outside %s)\n", p, wl ? want : "the data folder");
        return false;
    }
    return is_dir(p) ? plat_remove_tree(p) : remove(p) == 0;  // (plat_remove_tree doesn't follow links inside)
}

bool rt_remove_game_files(const char *game_dir) {
    if (!game_dir_ok(game_dir)) {
        fprintf(stderr, "[paths] refusing to delete game files in %s (not <data root>/extracted)\n", game_dir);
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < sizeof GAME_FILES / sizeof *GAME_FILES; i++) ok &= rt_remove_game_file(game_dir, GAME_FILES[i]);
    rmdir(game_dir);  // only if empty: anything else in there stays
    return ok;
}

static unsigned long long tree_size(const char *p) {
    struct stat st;
    if (is_link(p) || stat(p, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (unsigned long long)st.st_size;
    unsigned long long n = 0;
    DIR *d = opendir(p);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[2048];
            snprintf(c, sizeof c, "%s" SEP "%s", p, e->d_name);
            n += tree_size(c);
        }
        closedir(d);
    }
    return n;
}

unsigned long long rt_game_files_size(const char *game_dir) {
    unsigned long long n = 0;
    char p[1100];
    for (size_t i = 0; i < sizeof GAME_FILES / sizeof *GAME_FILES; i++) {
        snprintf(p, sizeof p, "%s" SEP "%s", game_dir, GAME_FILES[i]);
        n += tree_size(p);
    }
    return n;
}

// Window / Dock icon: icon.png shipped next to the exe or in the .app (none for a plain source build).
void rt_window_icon(SDL_Window *w) {
    char p[1100];
    if (!rt_resource("icon.png", NULL, p, sizeof p)) return;
    SDL_Surface *s = SDL_LoadPNG(p);
    if (s) SDL_SetWindowIcon(w, s), SDL_DestroySurface(s);
}

// LARECOMP_DISPLAY=<number from 0, or part of its name, e.g. Built-in>: the window opens centred on that display
// (test runs on a laptop's own screen while the external one stays free).
void rt_window_display(SDL_Window *w) {
    const char *e = getenv("LARECOMP_DISPLAY");
    if (!e || !*e) return;
    int n = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&n);
    SDL_DisplayID pick = 0;
    char *end;
    long i = strtol(e, &end, 10);
    if (!*end && i >= 0 && i < n) pick = ids[i];
    for (int k = 0; !pick && k < n; k++) {
        const char *name = SDL_GetDisplayName(ids[k]);
        if (name && strstr(name, e)) pick = ids[k];
    }
    SDL_free(ids);
    if (pick) SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED_DISPLAY(pick), SDL_WINDOWPOS_CENTERED_DISPLAY(pick));
}

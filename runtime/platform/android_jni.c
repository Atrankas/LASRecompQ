// The Java side of the Android app (android/app/src/main/java/com/lasrecomp/android/LasActivity.java): calls into it
// through SDL's JNI environment and activity, and its native methods.
#include <SDL3/SDL.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>

#include "android.h"

static void (*g_pick_cb)(const char *name);
static void (*g_import_progress)(uint64_t done, uint64_t total);

// A static method of the activity's class (LasActivity: GetObjectClass works on any thread, FindClass of an app class
// only on Java's own)
static jmethodID activity_method(JNIEnv **env, jclass *cls, const char *name, const char *sig) {
    *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!*env || !act) return NULL;
    *cls = (**env)->GetObjectClass(*env, act);
    (**env)->DeleteLocalRef(*env, act);
    jmethodID m = (**env)->GetStaticMethodID(*env, *cls, name, sig);
    if (!m) { (**env)->ExceptionClear(*env); fprintf(stderr, "[android] no method %s\n", name); }
    return m;
}

void android_pick_game_folder(void (*cb)(const char *name)) {
    g_pick_cb = cb;
    JNIEnv *env;
    jclass cls;
    jmethodID m = activity_method(&env, &cls, "pickGameFolder", "()V");
    if (!m) { if (cb) cb(NULL); return; }
    (*env)->CallStaticVoidMethod(env, cls, m);
    (*env)->DeleteLocalRef(env, cls);
}

// The game as a file: the picker for its .xci / .nsp, then for its prod.keys (LasActivity.pickGamePackage); the
// file's name comes back through cb as for the folder
void android_pick_game_package(void (*cb)(const char *name)) {
    g_pick_cb = cb;
    JNIEnv *env;
    jclass cls;
    jmethodID m = activity_method(&env, &cls, "pickGamePackage", "()V");
    if (!m) { if (cb) cb(NULL); return; }
    (*env)->CallStaticVoidMethod(env, cls, m);
    (*env)->DeleteLocalRef(env, cls);
}

void android_pick_game_keys(void (*cb)(const char *name)) {
    g_pick_cb = cb;
    JNIEnv *env;
    jclass cls;
    jmethodID m = activity_method(&env, &cls, "pickPackageKeys", "()V");
    if (!m) { if (cb) cb(NULL); return; }
    (*env)->CallStaticVoidMethod(env, cls, m);
    (*env)->DeleteLocalRef(env, cls);
}

static const char *call_string(const char *name, char *buf, size_t cap);
const char *android_game_keys_name(void) {
    static char name[256];
    return call_string("gameKeysName", name, sizeof name);
}

const char *android_saved_game_folder(void) {
    static char name[512];
    JNIEnv *env;
    jclass cls;
    jmethodID m = activity_method(&env, &cls, "savedGameFolder", "()Ljava/lang/String;");
    if (!m) return NULL;
    jstring r = (jstring)(*env)->CallStaticObjectMethod(env, cls, m);
    (*env)->DeleteLocalRef(env, cls);
    if (!r) return NULL;
    const char *s = (*env)->GetStringUTFChars(env, r, NULL);
    snprintf(name, sizeof name, "%s", s ? s : "");
    if (s) (*env)->ReleaseStringUTFChars(env, r, s);
    (*env)->DeleteLocalRef(env, r);
    return name[0] ? name : NULL;
}

const char *android_import_game_files(const char *dest, const char *build_id, void (*progress)(uint64_t, uint64_t)) {
    static char result[512];
    g_import_progress = progress;
    JNIEnv *env;
    jclass cls;
    jmethodID m = activity_method(&env, &cls, "importGameFiles",
                                  "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (!m) return "copy:no activity";
    jstring jdest = (*env)->NewStringUTF(env, dest);
    jstring jid = (*env)->NewStringUTF(env, build_id ? build_id : "");
    jstring r = (jstring)(*env)->CallStaticObjectMethod(env, cls, m, jdest, jid);
    (*env)->DeleteLocalRef(env, jid);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = NULL; snprintf(result, sizeof result, "copy:exception"); }
    else if (!r) result[0] = 0;
    else {
        const char *s = (*env)->GetStringUTFChars(env, r, NULL);
        snprintf(result, sizeof result, "%s", s ? s : "copy:?");
        if (s) (*env)->ReleaseStringUTFChars(env, r, s);
        (*env)->DeleteLocalRef(env, r);
    }
    (*env)->DeleteLocalRef(env, jdest);
    (*env)->DeleteLocalRef(env, cls);
    g_import_progress = NULL;
    return result[0] ? result : NULL;
}

// ---- custom GPU driver (LasActivity's gpuDriver* methods, GpuDrivers.java)
static void (*g_driver_cb)(const char *error);

// A static String method without arguments, copied into buf; NULL when it returned null
static const char *call_string(const char *name, char *buf, size_t cap) {
    JNIEnv *env;
    jclass cls;
    jmethodID m = activity_method(&env, &cls, name, "()Ljava/lang/String;");
    if (!m) return NULL;
    jstring r = (jstring)(*env)->CallStaticObjectMethod(env, cls, m);
    (*env)->DeleteLocalRef(env, cls);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return NULL; }
    if (!r) return NULL;
    const char *s = (*env)->GetStringUTFChars(env, r, NULL);
    snprintf(buf, cap, "%s", s ? s : "");
    if (s) (*env)->ReleaseStringUTFChars(env, r, s);
    (*env)->DeleteLocalRef(env, r);
    return buf;
}

static bool call_bool(const char *name) {
    JNIEnv *env;
    jclass cls;
    jmethodID m = activity_method(&env, &cls, name, "()Z");
    if (!m) return false;
    jboolean r = (*env)->CallStaticBooleanMethod(env, cls, m);
    (*env)->DeleteLocalRef(env, cls);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return false; }
    return r;
}

bool android_gpu_driver_supported(void) { return call_bool("gpuDriverSupported"); }
bool android_gpu_driver_selected(void) { return call_bool("gpuDriverSelected"); }

const char *android_gpu_driver_name(void) {
    static char name[256];
    return call_string("gpuDriverName", name, sizeof name);
}

const char *android_gpu_driver_failed(void) {
    static char name[256];
    return call_string("gpuDriverFailed", name, sizeof name);
}

void android_gpu_driver_select(bool use) {
    JNIEnv *env;
    jclass cls;
    jmethodID m = activity_method(&env, &cls, "selectGpuDriver", "(Z)V");
    if (!m) return;
    (*env)->CallStaticVoidMethod(env, cls, m, (jboolean)use);
    (*env)->DeleteLocalRef(env, cls);
}

void android_gpu_driver_install(void (*cb)(const char *error)) {
    g_driver_cb = cb;
    JNIEnv *env;
    jclass cls;
    jmethodID m = activity_method(&env, &cls, "installGpuDriver", "()V");
    if (!m) { if (cb) cb("no activity"); return; }
    (*env)->CallStaticVoidMethod(env, cls, m);
    (*env)->DeleteLocalRef(env, cls);
}

bool android_gpu_driver_config(AndroidGpuDriver *out) {
    static char cfg[2600];
    memset(out, 0, sizeof *out);
    if (!call_string("gpuDriverConfig", cfg, sizeof cfg) || !cfg[0]) return false;
    // one per line: driver folder, library, hook folder, temporary folder, probe file (GpuDrivers.config)
    char *fields[5] = {out->dir, out->lib, out->hook_dir, out->tmp_dir, out->probe};
    size_t caps[5] = {sizeof out->dir, sizeof out->lib, sizeof out->hook_dir, sizeof out->tmp_dir, sizeof out->probe};
    char *p = cfg;
    for (int i = 0; i < 5; i++) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        snprintf(fields[i], caps[i], "%s", p);
        if (!nl) return i == 4;
        p = nl + 1;
    }
    return true;
}

// ---- building the game code on the phone (LasActivity.buildTools / BuildService)
const char *android_build_tools(void) {
    static char dir[1024];
    return call_string("buildTools", dir, sizeof dir);
}

void android_build_service(bool on, const char *title) {
    JNIEnv *env;
    jclass cls;
    jmethodID m = activity_method(&env, &cls, "buildService", "(ZLjava/lang/String;)V");
    if (!m) return;
    jstring t = (*env)->NewStringUTF(env, title ? title : "");
    (*env)->CallStaticVoidMethod(env, cls, m, (jboolean)on, t);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, t);
    (*env)->DeleteLocalRef(env, cls);
}

void android_build_progress(int percent, const char *text) {
    JNIEnv *env;
    jclass cls;
    jmethodID m = activity_method(&env, &cls, "buildProgress", "(ILjava/lang/String;)V");
    if (!m) return;
    jstring t = (*env)->NewStringUTF(env, text ? text : "");
    (*env)->CallStaticVoidMethod(env, cls, m, (jint)percent, t);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, t);
    (*env)->DeleteLocalRef(env, cls);
}

JNIEXPORT void JNICALL Java_com_lasrecomp_android_LasActivity_nativeFolderPicked(JNIEnv *env, jclass cls, jstring name) {
    (void)cls;
    const char *s = name ? (*env)->GetStringUTFChars(env, name, NULL) : NULL;
    if (g_pick_cb) g_pick_cb(s);
    if (s) (*env)->ReleaseStringUTFChars(env, name, s);
}

JNIEXPORT void JNICALL Java_com_lasrecomp_android_LasActivity_nativeImportProgress(JNIEnv *env, jclass cls, jlong done,
                                                                                  jlong total) {
    (void)env, (void)cls;
    if (g_import_progress) g_import_progress((uint64_t)done, (uint64_t)total);
}

JNIEXPORT void JNICALL Java_com_lasrecomp_android_LasActivity_nativeGpuDriverInstalled(JNIEnv *env, jclass cls, jstring error) {
    (void)cls;
    const char *s = error ? (*env)->GetStringUTFChars(env, error, NULL) : NULL;
    if (g_driver_cb) g_driver_cb(s);
    if (s) (*env)->ReleaseStringUTFChars(env, error, s);
}

// ---- a fan translation (LasActivity.importTranslation)
static void (*g_translation_cb)(const char *error);

void android_import_translation(const char *dest, void (*cb)(const char *error)) {
    g_translation_cb = cb;
    JNIEnv *env;
    jclass cls;
    jmethodID m = activity_method(&env, &cls, "importTranslation", "(Ljava/lang/String;)V");
    if (!m) { if (cb) cb("nofolder"); return; }
    jstring jdest = (*env)->NewStringUTF(env, dest);
    (*env)->CallStaticVoidMethod(env, cls, m, jdest);
    (*env)->DeleteLocalRef(env, jdest);
    (*env)->DeleteLocalRef(env, cls);
}

JNIEXPORT void JNICALL Java_com_lasrecomp_android_LasActivity_nativeTranslationImported(JNIEnv *env, jclass cls, jstring error) {
    (void)cls;
    const char *s = error ? (*env)->GetStringUTFChars(env, error, NULL) : NULL;
    if (g_translation_cb) g_translation_cb(s);
    if (s) (*env)->ReleaseStringUTFChars(env, error, s);
}

// A graphics driver error that stops the app (plat_fatal_notice): LasActivity's dialog - install a driver, go back to
// the system's, or close - shown and waited for; the app restarts from there or closes. False: no dialog (no activity).
bool android_driver_error_dialog(const char *text) {
    JNIEnv *env;
    jclass cls;
    jmethodID m = activity_method(&env, &cls, "driverErrorDialog", "(Ljava/lang/String;)Z");
    if (!m) return false;
    jstring jt = (*env)->NewStringUTF(env, text);
    jboolean r = (*env)->CallStaticBooleanMethod(env, cls, m, jt);
    (*env)->DeleteLocalRef(env, jt);
    (*env)->DeleteLocalRef(env, cls);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return false; }
    return r;
}

// Where the game files go (LasActivity: the memory card's data folder, or internal storage)
const char *android_sd_card_path(void) {
    static char p[1024];
    return call_string("sdCardPath", p, sizeof p);
}

const char *android_game_storage_root(void) {
    static char p[1024];
    return call_string("gameStorageRoot", p, sizeof p);
}

void android_set_game_storage(bool sd) {
    JNIEnv *env;
    jclass cls;
    jmethodID m = activity_method(&env, &cls, "setGameStorage", "(Z)V");
    if (!m) return;
    (*env)->CallStaticVoidMethod(env, cls, m, (jboolean)sd);
    (*env)->DeleteLocalRef(env, cls);
}

// Saves to / from a Switch emulator's folder (LasActivity.transferSaves); cb gets "" (cancelled), "ok:<n>", "nosaves"
// or an error, on another thread
static void (*g_saves_cb)(const char *result);
void android_transfer_saves(bool importing, const char *save_dir, void (*cb)(const char *result)) {
    g_saves_cb = cb;
    JNIEnv *env;
    jclass cls;
    jmethodID m = activity_method(&env, &cls, "transferSaves", "(ZLjava/lang/String;)V");
    if (!m) { if (cb) cb("no activity"); return; }
    jstring d = (*env)->NewStringUTF(env, save_dir);
    (*env)->CallStaticVoidMethod(env, cls, m, (jboolean)importing, d);
    (*env)->DeleteLocalRef(env, d);
    (*env)->DeleteLocalRef(env, cls);
}

JNIEXPORT void JNICALL Java_com_lasrecomp_android_LasActivity_nativeSavesDone(JNIEnv *env, jclass cls, jstring result) {
    (void)cls;
    const char *s = result ? (*env)->GetStringUTFChars(env, result, NULL) : NULL;
    if (g_saves_cb) g_saves_cb(s ? s : "");
    if (s) (*env)->ReleaseStringUTFChars(env, result, s);
}

// The player's Switch fonts copied into fonts_dir (LasActivity.importFonts); cb(ok, skipped) on another thread
static void (*g_fonts_cb)(int ok, int skipped);
void android_import_fonts(const char *fonts_dir, const char *known, void (*cb)(int ok, int skipped)) {
    g_fonts_cb = cb;
    JNIEnv *env;
    jclass cls;
    jmethodID m = activity_method(&env, &cls, "importFonts", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (!m) { if (cb) cb(0, 0); return; }
    jstring d = (*env)->NewStringUTF(env, fonts_dir), k = (*env)->NewStringUTF(env, known);
    (*env)->CallStaticVoidMethod(env, cls, m, d, k);
    (*env)->DeleteLocalRef(env, d);
    (*env)->DeleteLocalRef(env, k);
    (*env)->DeleteLocalRef(env, cls);
}

JNIEXPORT void JNICALL Java_com_lasrecomp_android_LasActivity_nativeFontsImported(JNIEnv *env, jclass cls, jint ok, jint skipped) {
    (void)env, (void)cls;
    if (g_fonts_cb) g_fonts_cb(ok, skipped);
}

package com.lasrecomp.android;

import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.util.Log;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

// SDL's activity running libmain.so (the runtime: runtime/main.c is its SDL_main), plus what SDL doesn't do on
// Android: choosing the folder with the player's own game files (the system's folder picker hands out a tree of
// documents, not a path) and copying exefs/ and romfs/ out of it into the app's data folder (runtime/ui/launcher.mm) -
// or taking them out of the player's .xci / .nsp with their prod.keys (NxPackage).
public class LasActivity extends SDLActivity {
    private static final String TAG = "LASRecomp";
    private static final int PICK_FOLDER = 4100;
    private static final int PICK_DRIVER = 4101;
    private static final int PICK_TRANSLATION = 4102;
    private static final int PICK_KEYS = 4103;
    private static Uri sTree;  // the folder picked last (read permission kept across restarts)

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        String saved = getPreferences(MODE_PRIVATE).getString("tree", null);
        if (saved != null) sTree = Uri.parse(saved);
        immersive();
        installBundledTranslation();
        new Thread(() -> applyTranslationPatch(this), "translation patch").start();
    }

    // A personal build can carry the Brazilian Portuguese fan translation (assets/translations/pt-BR, build_game.sh
    // --apk): copied once into the data folder, where the Language row finds it (runtime/settings.c). Choosing the
    // language stays up to the player.
    private void installBundledTranslation() {
        final File base = getExternalFilesDir(null);
        if (base == null) return;
        final File marker = new File(base, "translations/pt-BR/.bundled");
        try {
            String[] top = getAssets().list("translations/pt-BR");
            if (top == null || top.length == 0 || marker.exists()) return;
        } catch (Exception e) { return; }
        new Thread(() -> {
            try {
                copyAssetTree("translations/pt-BR", new File(base, "translations/pt-BR"));
                //noinspection ResultOfMethodCallIgnored
                marker.createNewFile();
                Log.i(TAG, "bundled translation installed");
            } catch (Exception e) {
                Log.e(TAG, "bundled translation", e);
            }
        }).start();
    }

    // The public APK's fan translation (assets/translation/pt-BR.lasp, tools/translation_patch.py): a patch against the
    // player's own game files - copy ranges of their file, add the translated bytes - applied once those files are set
    // up (at start, and after a setup), into translations/pt-BR/ where the Language setting offers it. Each file is
    // made only from an original whose SHA-256 is the one the patch was made against (v1.0.0); the others are left out.
    private static final Object sPatchLock = new Object();
    static void applyTranslationPatch(android.content.Context ctx) {
        synchronized (sPatchLock) {
            File base = ctx.getExternalFilesDir(null);
            if (base == null) return;
            File romfs = new File(base, "extracted/romfs"), out = new File(base, "translations/pt-BR");
            File marker = new File(out, ".patched");
            if (!new File(romfs, "region_common").isDirectory() || marker.exists()) return;
            byte[] data;
            try (InputStream raw = ctx.getAssets().open("translation/pt-BR.lasp");
                 java.util.zip.InflaterInputStream in = new java.util.zip.InflaterInputStream(raw)) {
                java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; ) bo.write(buf, 0, n);
                data = bo.toByteArray();
            } catch (java.io.FileNotFoundException e) {
                return;  // (no patch in this APK)
            } catch (Exception e) {
                Log.e(TAG, "translation patch", e);
                return;
            }
            try {
                java.nio.ByteBuffer b = java.nio.ByteBuffer.wrap(data).order(java.nio.ByteOrder.LITTLE_ENDIAN);
                byte[] magic = new byte[5];
                b.get(magic);
                if (!new String(magic, java.nio.charset.StandardCharsets.US_ASCII).equals("LASP1")) throw new Exception("not a patch");
                int nf = b.getInt(), made = 0;
                for (int f = 0; f < nf; f++) {
                    byte[] pb = new byte[b.getShort() & 0xffff];
                    b.get(pb);
                    String rel = new String(pb, java.nio.charset.StandardCharsets.UTF_8);
                    byte[] want = new byte[32];
                    b.get(want);
                    int size = b.getInt(), nops = b.getInt();
                    File src = new File(base, "extracted/" + rel);
                    byte[] orig = src.isFile() ? java.nio.file.Files.readAllBytes(src.toPath()) : null;
                    boolean ok = orig != null && java.util.Arrays.equals(want, java.security.MessageDigest.getInstance("SHA-256").digest(orig))
                                 && !rel.contains("..");
                    byte[] res = new byte[size];
                    int at = 0;
                    for (int k = 0; k < nops; k++) {
                        int kind = b.get();
                        if (kind == 1) {
                            int o = b.getInt(), n = b.getInt();
                            if (ok && o >= 0 && n >= 0 && o + n <= orig.length && at + n <= size) System.arraycopy(orig, o, res, at, n);
                            else ok = false;
                            at += n;
                        } else {
                            int n = b.getInt();
                            if (ok && at + n <= size) b.get(res, at, n); else { ok = false; b.position(b.position() + n); }
                            at += n;
                        }
                    }
                    if (!ok || at != size) { Log.w(TAG, "translation patch: " + rel + " left out (not the v1.0.0 file)"); continue; }
                    File dst = new File(out, rel);
                    File dir = dst.getParentFile();
                    if (dir != null && !dir.isDirectory() && !dir.mkdirs()) throw new Exception("mkdir " + dir);
                    try (OutputStream os = new FileOutputStream(dst)) { os.write(res); }
                    made++;
                }
                //noinspection ResultOfMethodCallIgnored
                marker.createNewFile();
                Log.i(TAG, "translation patch: " + made + " of " + nf + " files made");
            } catch (Exception e) {
                Log.e(TAG, "translation patch", e);
            }
        }
    }

    private void copyAssetTree(String path, File dest) throws Exception {
        String[] kids = getAssets().list(path);
        if (kids == null || kids.length == 0) {  // a file
            File dir = dest.getParentFile();
            if (dir != null && !dir.isDirectory() && !dir.mkdirs()) throw new Exception("mkdir " + dir);
            byte[] buf = new byte[1 << 16];
            try (InputStream in = getAssets().open(path); OutputStream os = new FileOutputStream(dest)) {
                int n;
                while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
            }
            return;
        }
        for (String k : kids) copyAssetTree(path + "/" + k, new File(dest, k));
    }

    // SDL sets its fullscreen through the old system UI flags (Android 15+ ignores them, and they bring the bars back
    // over the game on some systems) and owns the window's insets listener: while in front, the bars are checked
    // every 3 s and hidden again (after a swipe that showed them too)
    private final Runnable mHideBars = new Runnable() {
        @Override public void run() {
            // (unconditionally: the window's own idea of the bars can be out of step with what the system shows)
            immersive();
            getWindow().getDecorView().postDelayed(this, 3000);
        }
    };

    @Override
    protected void onResume() {
        super.onResume();
        View d = getWindow().getDecorView();
        d.removeCallbacks(mHideBars);
        d.postDelayed(mHideBars, 1000);
        RootBoost.start(this);
    }

    @Override
    protected void onPause() {
        RootBoost.stop(this);
        getWindow().getDecorView().removeCallbacks(mHideBars);
        super.onPause();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) immersive();
    }

    // The whole screen for the game: status and navigation bars hidden, a swipe from the edge shows them for a moment
    // (Android 15+ ignores the theme's fullscreen: apps draw edge to edge)
    private void immersive() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c == null) return;
        c.hide(WindowInsets.Type.systemBars());
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    // ---- called from native code (runtime/platform/android_jni.c)

    private static boolean sAskedNotify;

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == 4104) pickGameFolder();  // (the notification question answered: on to the picker)
    }

    // Opens the system's folder picker; the choice comes back through nativeFolderPicked.
    public static void pickGameFolder() {
        final LasActivity a = (LasActivity) mSingleton;
        if (a == null) return;
        a.runOnUiThread(() -> {
            // (Android 13+: the setup's progress notification needs a permission - asked here, before the copy and the
            // build start, the picker opening once it is answered)
            if (android.os.Build.VERSION.SDK_INT >= 33 && !sAskedNotify &&
                a.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != android.content.pm.PackageManager.PERMISSION_GRANTED) {
                sAskedNotify = true;
                a.requestPermissions(new String[] {android.Manifest.permission.POST_NOTIFICATIONS}, 4104);
                return;
            }
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            try {
                a.startActivityForResult(i, PICK_FOLDER);
            } catch (Exception e) {
                Log.e(TAG, "folder picker", e);
                nativeFolderPicked(null);
            }
        });
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request == PICK_DRIVER) { driverPicked(result == RESULT_OK && data != null ? data.getData() : null); return; }
        if (request == PICK_KEYS) { keysPicked(result == RESULT_OK && data != null ? data.getData() : null); return; }
        if (request == PICK_TRANSLATION) { translationPicked(result == RESULT_OK && data != null ? data.getData() : null); return; }
        if (request != PICK_FOLDER) { super.onActivityResult(request, result, data); return; }
        Uri tree = result == RESULT_OK && data != null ? data.getData() : null;
        if (tree != null) {
            try {
                getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (Exception e) {
                Log.w(TAG, "no persistable permission", e);
            }
            sTree = tree;
            getPreferences(MODE_PRIVATE).edit().putString("tree", tree.toString()).apply();
        }
        nativeFolderPicked(tree != null ? describe(tree) : null);
    }

    // The folder picked in an earlier session (its permission kept), or null
    public static String savedGameFolder() {
        return sTree != null ? describe(sTree) : null;
    }

    private static String describe(Uri tree) {
        String id = DocumentsContract.getTreeDocumentId(tree);  // e.g. "primary:Games/LA"
        int colon = id.indexOf(':');
        return colon >= 0 ? id.substring(colon + 1) : id;
    }

    // Copies the picked folder's exefs/ (the folder with main and main.npdm) and romfs/ (the folder with region_common)
    // into dest/exefs and dest/romfs, reporting progress through nativeImportProgress. Runs on the calling (native)
    // thread. Returns null when done, else what went wrong: "nofolder", "notfound", "unusable" (game packages such
    // as .nsp / .xci instead of their files), "space:<bytes needed>" or "copy:<details>".
    public static String importGameFiles(String dest, String buildId) {
        LasActivity a = (LasActivity) mSingleton;
        if (a == null || sTree == null) return "nofolder";
        ContentResolver cr = a.getContentResolver();
        a.runOnUiThread(() -> a.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
        try {
            Doc root = new Doc(DocumentsContract.getTreeDocumentId(sTree), describe(sTree), true, 0);
            Doc[] found = new Doc[5];  // exefs, romfs, a game package (.xci / .nsp), prod.keys, title.keys
            boolean[] unusable = new boolean[1];
            find(cr, root, 0, found, unusable);
            if ((found[0] == null || found[1] == null) && found[2] != null) return importPackage(a, cr, found, dest, buildId);
            if (found[0] == null || found[1] == null) return unusable[0] ? "unusable" : "notfound";
            // the version first (exefs/main's build id at 0x40: runtime/ui/launcher.mm BUILD_ID), before ~6 GB of copy
            if (buildId != null && !buildId.isEmpty()) {
                String have = null;
                for (Doc k : children(cr, found[0])) {
                    if (k.dir || !k.name.equals("main")) continue;
                    Uri u = DocumentsContract.buildDocumentUriUsingTree(sTree, k.id);
                    try (InputStream in = cr.openInputStream(u)) {
                        byte[] h = new byte[0x48];
                        int n = 0, r;
                        while (in != null && n < h.length && (r = in.read(h, n, h.length - n)) > 0) n += r;
                        if (n == h.length) {
                            StringBuilder sb = new StringBuilder();
                            for (int i = 0x40; i < 0x48; i++) sb.append(String.format(Locale.ROOT, "%02x", h[i] & 0xff));
                            have = sb.toString();
                        }
                    }
                }
                if (!buildId.equalsIgnoreCase(have)) {
                    Log.w(TAG, "exefs/main build id " + have + ", wanted " + buildId);
                    return "version";
                }
            }
            List<Doc[]> files = new ArrayList<>();  // {file, its folder} pairs, relative names in Doc.rel
            long total = list(cr, found[0], "exefs", files, false) + list(cr, found[1], "romfs", files, true);
            long need = 0;  // (what an earlier, interrupted import already copied isn't copied again)
            for (Doc[] f : files) if (new File(dest, f[0].rel).length() != f[0].size) need += f[0].size;
            StatFs fs = new StatFs(new File(dest).getParentFile() != null ? new File(dest).getParent() : dest);
            if (fs.getAvailableBytes() < need + (512L << 20))
                return "space:" + (need + (512L << 20) - fs.getAvailableBytes());
            byte[] buf = new byte[1 << 20];
            long done = 0, last = 0;
            for (Doc[] f : files) {
                File out = new File(dest, f[0].rel);
                File dir = out.getParentFile();
                if (dir != null && !dir.isDirectory() && !dir.mkdirs()) return "copy:mkdir " + dir;
                if (out.length() == f[0].size && f[0].size > 0) {  // (copied by an earlier, interrupted import)
                    done += f[0].size;
                    continue;
                }
                Uri u = DocumentsContract.buildDocumentUriUsingTree(sTree, f[0].id);
                try (InputStream in = cr.openInputStream(u); OutputStream os = new FileOutputStream(out)) {
                    if (in == null) return "copy:" + f[0].rel;
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        os.write(buf, 0, n);
                        done += n;
                        if (done - last > (16 << 20)) { last = done; nativeImportProgress(done, total); }
                    }
                }
            }
            nativeImportProgress(total, total);
            applyTranslationPatch(a);  // (the files it applies to are there now)
            return null;
        } catch (Exception e) {
            Log.e(TAG, "import", e);
            return "copy:" + e.getMessage();
        } finally {
            a.runOnUiThread(() -> a.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
        }
    }

    // The game's files out of the player's .xci / .nsp (NxPackage) with their prod.keys: the one in the same folder, or
    // one they choose in the file picker (asked here, waited for). Results as importGameFiles, plus "nokeys" (no keys
    // chosen), "badkeys", "keys:<name>", "notgame", "updateonly" and "corrupt:<what>". A title.keys beside them is read
    // too (an eShop package without its ticket).
    private static String importPackage(LasActivity a, ContentResolver cr, Doc[] found, String dest, String buildId) {
        Doc pkg = found[2];
        java.util.Map<String, byte[]> keys;
        Uri ku = found[3] != null ? DocumentsContract.buildDocumentUriUsingTree(sTree, found[3].id) : pickKeys(a);
        if (ku == null) return "nokeys";
        try (InputStream in = cr.openInputStream(ku)) {
            if (in == null) return "nokeys";
            keys = NxPackage.parseKeys(in);
        } catch (Exception e) {
            Log.e(TAG, "keys", e);
            return "nokeys";
        }
        if (!keys.containsKey("header_key")) return "badkeys";
        if (found[4] != null)
            try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(sTree, found[4].id))) {
                if (in != null) for (java.util.Map.Entry<String, byte[]> e : NxPackage.parseKeys(in).entrySet()) keys.putIfAbsent(e.getKey(), e.getValue());
            } catch (Exception e) {
                Log.w(TAG, "title.keys", e);
            }
        Uri u = DocumentsContract.buildDocumentUriUsingTree(sTree, pkg.id);
        try (android.os.ParcelFileDescriptor fd = cr.openFileDescriptor(u, "r")) {
            if (fd == null) return "copy:" + pkg.name;
            try (java.io.FileInputStream fin = new java.io.FileInputStream(fd.getFileDescriptor());
                 java.nio.channels.FileChannel ch = fin.getChannel()) {
                long t0 = System.nanoTime();
                String r = NxPackage.extract(ch, keys, new File(dest), buildId, LasActivity::nativeImportProgress);
                Log.i(TAG, "package " + pkg.name + ": " + (r == null ? "extracted" : r) + " in " + (System.nanoTime() - t0) / 1000000 + " ms");
                if (r == null) applyTranslationPatch(a);
                return r;
            }
        } catch (Exception e) {
            Log.e(TAG, "package", e);
            return "copy:" + e.getMessage();
        }
    }

    // The system's file picker for prod.keys, from the import's (native) thread: waits for the choice
    private static final Object sKeysLock = new Object();
    private static Uri sKeysPicked;
    private static boolean sKeysDone;
    private static Uri pickKeys(LasActivity a) {
        synchronized (sKeysLock) { sKeysDone = false; sKeysPicked = null; }
        a.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            try {
                a.startActivityForResult(i, PICK_KEYS);
            } catch (Exception e) {
                Log.e(TAG, "keys picker", e);
                a.keysPicked(null);
            }
        });
        synchronized (sKeysLock) {
            while (!sKeysDone) {
                try { sKeysLock.wait(); } catch (InterruptedException e) { return null; }
            }
            return sKeysPicked;
        }
    }
    private void keysPicked(Uri uri) {
        synchronized (sKeysLock) { sKeysPicked = uri; sKeysDone = true; sKeysLock.notifyAll(); }
    }

    // ---- building the game code on the phone (runtime/gamebuild.c; docs/android-ondevice-build.md)

    // The asset archive (assets/ondevice.zip: the generator, its Python's standard library and modules, the NDK
    // pieces the compiler needs; tools/android/ondevice/make_assets.sh) unpacked into files/ondevice, once per app
    // version. Returns that folder, or null (an APK built without it, or the unpacking failed). Blocking.
    public static synchronized String buildTools() {
        LasActivity a = (LasActivity) mSingleton;
        if (a == null) return null;
        File dir = new File(a.getFilesDir(), "ondevice");
        File stamp = new File(dir, ".unpacked");
        String want;
        try {
            android.content.pm.PackageInfo pi = a.getPackageManager().getPackageInfo(a.getPackageName(), 0);
            want = pi.getLongVersionCode() + " " + pi.lastUpdateTime;
        } catch (Exception e) {
            want = "?";
        }
        try {
            if (stamp.isFile() && want.equals(new String(java.nio.file.Files.readAllBytes(stamp.toPath())).trim()))
                return dir.getAbsolutePath();
        } catch (Exception ignored) {
        }
        try (InputStream raw = a.getAssets().open("ondevice.zip");
             java.util.zip.ZipInputStream zin = new java.util.zip.ZipInputStream(new java.io.BufferedInputStream(raw, 1 << 16))) {
            deleteTree(dir);
            if (!dir.mkdirs()) return null;
            String root = dir.getCanonicalPath() + File.separator;
            byte[] buf = new byte[1 << 16];
            for (java.util.zip.ZipEntry e; (e = zin.getNextEntry()) != null; ) {
                File out = new File(dir, e.getName());
                if (!out.getCanonicalPath().startsWith(root)) continue;  // (no ../ out of the folder)
                if (e.isDirectory()) { out.mkdirs(); continue; }
                File p = out.getParentFile();
                if (p != null) p.mkdirs();
                try (OutputStream os = new FileOutputStream(out)) {
                    for (int n; (n = zin.read(buf)) > 0; ) os.write(buf, 0, n);
                }
            }
            try (OutputStream os = new FileOutputStream(stamp)) { os.write(want.getBytes()); }
            return dir.getAbsolutePath();
        } catch (java.io.FileNotFoundException e) {
            return null;  // (no assets/ondevice.zip: a build without -PonDeviceBuild)
        } catch (Exception e) {
            Log.e(TAG, "build tools", e);
            return null;
        }
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    // The build's foreground service (BuildService): on while it runs, off at its end
    public static void buildService(boolean on, String title) {
        final LasActivity a = (LasActivity) mSingleton;
        if (a == null) return;
        if (!on) { BuildService.stop(a); return; }
        a.runOnUiThread(() -> {
            // (Android 13+: its progress notification needs the permission - asked once; the build runs without it)
            if (android.os.Build.VERSION.SDK_INT >= 33 &&
                a.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != android.content.pm.PackageManager.PERMISSION_GRANTED)
                a.requestPermissions(new String[] {android.Manifest.permission.POST_NOTIFICATIONS}, 4103);
            BuildService.start(a, title);
        });
    }

    public static void buildProgress(int percent, String text) {
        BuildService.progress(percent, text);
    }

    // ---- a fan translation (the menu's Language row, runtime/ui/menu.mm): its RomFS folder copied into the app

    private static String sTranslationDest;

    // Opens the folder picker for the translation as extracted (any folder above its romfs/); the copy into dest runs
    // on a thread of its own and its result comes back through nativeTranslationImported (null = done).
    public static void importTranslation(String dest) {
        final LasActivity a = (LasActivity) mSingleton;
        if (a == null) { nativeTranslationImported("nofolder"); return; }
        sTranslationDest = dest;
        a.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            try {
                a.startActivityForResult(i, PICK_TRANSLATION);
            } catch (Exception e) {
                Log.e(TAG, "translation picker", e);
                nativeTranslationImported("nofolder");
            }
        });
    }

    private void translationPicked(Uri tree) {
        if (tree == null) { nativeTranslationImported("nofolder"); return; }
        ContentResolver cr = getContentResolver();
        new Thread(() -> {
            try {
                // the romfs/ folder: the one holding regionUS/ (an Atmosphère copy first when the release has several)
                List<Doc> found = new ArrayList<>();
                findRomfs(cr, tree, new Doc(DocumentsContract.getTreeDocumentId(tree), "", true, 0), "", 0, found);
                if (found.isEmpty()) { nativeTranslationImported("notfound"); return; }
                Doc romfs = found.get(0);
                for (Doc d : found) if (d.rel.toLowerCase(Locale.ROOT).contains("atmosphere")) { romfs = d; break; }
                List<Doc> files = new ArrayList<>();
                listTree(cr, tree, romfs, "", files);
                byte[] buf = new byte[1 << 16];
                for (Doc f : files) {
                    File out = new File(sTranslationDest, f.rel);
                    File dir = out.getParentFile();
                    if (dir != null && !dir.isDirectory() && !dir.mkdirs()) { nativeTranslationImported("copy:mkdir " + dir); return; }
                    Uri u = DocumentsContract.buildDocumentUriUsingTree(tree, f.id);
                    try (InputStream in = cr.openInputStream(u); OutputStream os = new FileOutputStream(out)) {
                        if (in == null) { nativeTranslationImported("copy:" + f.rel); return; }
                        int n;
                        while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
                    }
                }
                nativeTranslationImported(null);
            } catch (Exception e) {
                Log.e(TAG, "translation import", e);
                nativeTranslationImported("copy:" + e.getMessage());
            }
        }).start();
    }

    private static List<Doc> childrenOf(ContentResolver cr, Uri tree, Doc d) {
        List<Doc> out = new ArrayList<>();
        Uri u = DocumentsContract.buildChildDocumentsUriUsingTree(tree, d.id);
        String[] cols = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
                         Document.COLUMN_SIZE};
        try (Cursor c = cr.query(u, cols, null, null, null)) {
            while (c != null && c.moveToNext()) {
                boolean dir = Document.MIME_TYPE_DIR.equals(c.getString(2));
                out.add(new Doc(c.getString(0), c.getString(1), dir, c.isNull(3) ? 0 : c.getLong(3)));
            }
        }
        return out;
    }

    private static void findRomfs(ContentResolver cr, Uri tree, Doc d, String rel, int depth, List<Doc> found) {
        if (depth > 6) return;
        List<Doc> kids = childrenOf(cr, tree, d);
        for (Doc k : kids)
            if (k.dir && k.name.equals("regionUS")) { d.rel = rel; found.add(d); return; }
        for (Doc k : kids)
            if (k.dir && !k.name.startsWith(".")) findRomfs(cr, tree, k, rel + "/" + k.name, depth + 1, found);
    }

    private static void listTree(ContentResolver cr, Uri tree, Doc d, String rel, List<Doc> out) {
        for (Doc k : childrenOf(cr, tree, d)) {
            if (k.name.startsWith(".")) continue;
            k.rel = rel.isEmpty() ? k.name : rel + "/" + k.name;
            if (k.dir) listTree(cr, tree, k, k.rel, out);
            else out.add(k);
        }
    }

    // ---- a custom GPU driver (GpuDrivers; the start screen's Video settings, runtime/ui/menu.mm)
    public static boolean gpuDriverSupported() { return GpuDrivers.supported(); }

    // The installed driver's name (and version), or null
    public static String gpuDriverName() {
        LasActivity a = (LasActivity) mSingleton;
        GpuDrivers.Driver d = a != null ? GpuDrivers.find(a) : null;
        return d == null ? null : d.version.isEmpty() ? d.name : d.name + " " + d.version;
    }

    public static boolean gpuDriverSelected() {
        LasActivity a = (LasActivity) mSingleton;
        return a != null && GpuDrivers.selected(a);
    }

    public static void selectGpuDriver(boolean use) {
        LasActivity a = (LasActivity) mSingleton;
        if (a != null) GpuDrivers.select(a, use);
    }

    public static String gpuDriverConfig() {
        LasActivity a = (LasActivity) mSingleton;
        return a != null ? GpuDrivers.config(a) : "";
    }

    public static String gpuDriverFailed() {
        LasActivity a = (LasActivity) mSingleton;
        return a != null ? GpuDrivers.failed(a) : null;
    }

    // Opens the system's file picker for a driver .zip; installed (and selected) on a thread of its own, the result
    // comes back through nativeGpuDriverInstalled.
    public static void installGpuDriver() {
        final LasActivity a = (LasActivity) mSingleton;
        if (a == null) return;
        a.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");  // (file managers label .zip files differently: the mime types below are a preference)
            i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed",
                                                               "application/octet-stream"});
            try {
                a.startActivityForResult(i, PICK_DRIVER);
            } catch (Exception e) {
                Log.e(TAG, "driver picker", e);
                nativeGpuDriverInstalled("no file picker");
            }
        });
    }

    private void driverPicked(Uri uri) {
        if (uri == null) { nativeGpuDriverInstalled(""); return; }  // (cancelled)
        new Thread(() -> {
            String r;
            try {
                GpuDrivers.install(this, uri);
                r = null;
            } catch (Exception e) {
                Log.e(TAG, "driver install", e);
                r = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            nativeGpuDriverInstalled(r);
        }, "gpu driver install").start();
    }

    private static final class Doc {
        final String id, name;
        final boolean dir;
        final long size;
        String rel;
        Doc(String id, String name, boolean dir, long size) { this.id = id; this.name = name; this.dir = dir; this.size = size; }
    }

    private static List<Doc> children(ContentResolver cr, Doc d) {
        List<Doc> out = new ArrayList<>();
        Uri u = DocumentsContract.buildChildDocumentsUriUsingTree(sTree, d.id);
        String[] cols = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
                         Document.COLUMN_SIZE};
        try (Cursor c = cr.query(u, cols, null, null, null)) {
            while (c != null && c.moveToNext()) {
                boolean dir = Document.MIME_TYPE_DIR.equals(c.getString(2));
                out.add(new Doc(c.getString(0), c.getString(1), dir, c.isNull(3) ? 0 : c.getLong(3)));
            }
        }
        return out;
    }

    // as runtime/ui/launcher.mm find_game: in the folder or a few levels below it (dump tools nest them)
    // (found: exefs, romfs, the largest .xci / .nsp - the game rather than an update of it, prod.keys - or keys.txt,
    // title.keys)
    private static void find(ContentResolver cr, Doc d, int depth, Doc[] found, boolean[] unusable) {
        if (depth > 4 || (found[0] != null && found[1] != null)) return;
        List<Doc> kids = children(cr, d);
        boolean main = false, npdm = false, region = false;
        for (Doc k : kids) {
            String low = k.name.toLowerCase(Locale.ROOT);
            if (!k.dir && k.name.equals("main")) main = true;
            if (!k.dir && k.name.equals("main.npdm")) npdm = true;
            if (k.dir && k.name.equals("region_common")) region = true;
            if (!k.dir && (low.endsWith(".nca") || low.endsWith(".nsz") || low.endsWith(".xcz"))) unusable[0] = true;
            if (!k.dir && (low.endsWith(".nsp") || low.endsWith(".xci")) && (found[2] == null || k.size > found[2].size)) found[2] = k;
            if (!k.dir && (low.equals("prod.keys") || (low.equals("keys.txt") && found[3] == null))) found[3] = k;
            if (!k.dir && low.equals("title.keys")) found[4] = k;
        }
        if (found[0] == null && main && npdm) found[0] = d;
        if (found[1] == null && region) found[1] = d;
        for (Doc k : kids)
            if (k.dir && !k.name.equals("region_common") && !k.name.startsWith(".")) find(cr, k, depth + 1, found, unusable);
    }

    private static long list(ContentResolver cr, Doc d, String rel, List<Doc[]> out, boolean deep) {
        long total = 0;
        for (Doc k : children(cr, d)) {
            if (k.name.startsWith(".")) continue;
            k.rel = rel + "/" + k.name;
            if (k.dir) { if (deep) total += list(cr, k, k.rel, out, true); continue; }
            out.add(new Doc[] {k, d});
            total += k.size;
        }
        return total;
    }

    // ---- native (runtime/platform/android_jni.c)
    public static native void nativeFolderPicked(String name);
    public static native void nativeImportProgress(long done, long total);
    public static native void nativeGpuDriverInstalled(String error);
    public static native void nativeTranslationImported(String error);
}

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
    private static final int PICK_DRIVER_RECOVER = 4105;
    private static final int PICK_PACKAGE = 4106;
    private static final int PICK_PACKAGE_KEYS = 4107;
    private static final int PICK_SAVES_IMPORT = 4109;
    private static final int PICK_SAVES_EXPORT = 4110;
    private static final int PICK_FONTS = 4111;
    private static Uri sTree;  // the folder picked last (read permission kept across restarts)
    // or the game chosen as a file: its .xci / .nsp, then its prod.keys (read permissions kept across restarts); used
    // instead of the folder while sUsePackage
    private static Uri sPkg, sKeys;
    private static String sPkgName, sKeysName;
    private static boolean sUsePackage;

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        android.content.SharedPreferences pr = getPreferences(MODE_PRIVATE);
        String saved = pr.getString("tree", null);
        if (saved != null) sTree = Uri.parse(saved);
        if (pr.getString("pkg", null) != null) sPkg = Uri.parse(pr.getString("pkg", null));
        if (pr.getString("keys", null) != null) sKeys = Uri.parse(pr.getString("keys", null));
        sPkgName = pr.getString("pkg_name", null);
        sKeysName = pr.getString("keys_name", null);
        sUsePackage = pr.getBoolean("use_pkg", false) && sPkg != null;
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
            File game = gameDir(ctx);
            File romfs = new File(game, "romfs"), out = new File(base, "translations/pt-BR");
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
                    File src = new File(game, rel);
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

    // ---- where the game files go (the start screen's Storage row): the app's folder on internal storage, or its folder
    // on a memory card (Android/data/<package>/files there - the app's own, no permission; removed with the app, as on
    // internal storage). The game code library itself always runs from internal storage (runtime/gamebuild.c).
    static File sdFilesDir(android.content.Context c) {
        File[] dirs = c.getExternalFilesDirs(null);
        for (int i = 1; dirs != null && i < dirs.length; i++)
            if (dirs[i] != null && android.os.Environment.MEDIA_MOUNTED.equals(android.os.Environment.getExternalStorageState(dirs[i])))
                return dirs[i];
        return null;
    }

    static boolean useSd(android.content.Context c) {
        return c.getSharedPreferences("storage", MODE_PRIVATE).getBoolean("sd", false) && sdFilesDir(c) != null;
    }

    // The game files' folder (extracted/ in the data folder chosen)
    static File gameDir(android.content.Context c) {
        File sd = useSd(c) ? sdFilesDir(c) : null;
        return new File(sd != null ? sd : c.getExternalFilesDir(null), "extracted");
    }

    // (native: runtime/main.c, the start screen) the memory card's data folder, or null when there's none
    public static String sdCardPath() {
        LasActivity a = (LasActivity) mSingleton;
        File d = a != null ? sdFilesDir(a) : null;
        return d != null ? d.getAbsolutePath() : null;
    }

    // the data folder the game files go in when it's the memory card's, else null (internal storage)
    public static String gameStorageRoot() {
        LasActivity a = (LasActivity) mSingleton;
        if (a == null || !useSd(a)) return null;
        return sdFilesDir(a).getAbsolutePath();
    }

    public static void setGameStorage(boolean sd) {
        LasActivity a = (LasActivity) mSingleton;
        if (a != null) a.getSharedPreferences("storage", MODE_PRIVATE).edit().putBoolean("sd", sd).commit();
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
        if (requestCode == 4108) pickGamePackage();
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
        if (request == PICK_FONTS) { fontsPicked(result == RESULT_OK ? data : null); return; }
        if (request == PICK_SAVES_IMPORT || request == PICK_SAVES_EXPORT) {
            savesPicked(request == PICK_SAVES_IMPORT, result == RESULT_OK && data != null ? data.getData() : null);
            return;
        }
        if (request == PICK_PACKAGE) { packagePicked(result == RESULT_OK && data != null ? data.getData() : null); return; }
        if (request == PICK_PACKAGE_KEYS) { packageKeysPicked(result == RESULT_OK && data != null ? data.getData() : null); return; }
        if (request == PICK_DRIVER_RECOVER) { recoverDriverPicked(result == RESULT_OK && data != null ? data.getData() : null); return; }
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
            sUsePackage = false;
            getPreferences(MODE_PRIVATE).edit().putString("tree", tree.toString()).putBoolean("use_pkg", false).apply();
        }
        nativeFolderPicked(tree != null ? describe(tree) : null);
    }

    // The folder picked in an earlier session (its permission kept) - or the game file chosen - or null
    public static String savedGameFolder() {
        if (sUsePackage) return sPkgName;
        return sTree != null ? describe(sTree) : null;
    }

    // The prod.keys chosen for the game file, or null
    // ("": the game file chosen, its keys not yet; null: a folder)
    public static String gameKeysName() { return sUsePackage ? (sKeysName != null ? sKeysName : "") : null; }

    // The system's file picker for the game itself (.xci / .nsp), then for its prod.keys (pickPackageKeys) when none
    // is chosen yet; the choice comes back through nativeFolderPicked (the file's name, null: cancelled).
    public static void pickGamePackage() {
        final LasActivity a = (LasActivity) mSingleton;
        if (a == null) return;
        a.runOnUiThread(() -> {
            if (android.os.Build.VERSION.SDK_INT >= 33 && !sAskedNotify &&
                a.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != android.content.pm.PackageManager.PERMISSION_GRANTED) {
                sAskedNotify = true;
                a.requestPermissions(new String[] {android.Manifest.permission.POST_NOTIFICATIONS}, 4108);
                return;
            }
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");  // (.xci / .nsp have no registered type: every file, the package recognised by its content)
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            try {
                a.startActivityForResult(i, PICK_PACKAGE);
            } catch (Exception e) {
                Log.e(TAG, "package picker", e);
                nativeFolderPicked(null);
            }
        });
    }

    // The file picker for prod.keys alone (the setup page's keys row); the result comes back as for pickGamePackage
    public static void pickPackageKeys() {
        final LasActivity a = (LasActivity) mSingleton;
        if (a == null) return;
        a.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            try {
                a.startActivityForResult(i, PICK_PACKAGE_KEYS);
            } catch (Exception e) {
                Log.e(TAG, "keys picker", e);
                nativeFolderPicked(savedGameFolder());
            }
        });
    }

    private String displayName(Uri u) {
        try (Cursor c = getContentResolver().query(u, new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getString(0);
        } catch (Exception e) {
            Log.w(TAG, "display name", e);
        }
        String l = u.getLastPathSegment();
        return l != null ? l.substring(l.lastIndexOf('/') + 1) : "?";
    }

    private void keepPermission(Uri u) {
        try {
            getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception e) {
            Log.w(TAG, "no persistable permission", e);
        }
    }

    private void packagePicked(Uri u) {
        if (u == null) { nativeFolderPicked(null); return; }
        keepPermission(u);
        sPkg = u;
        sPkgName = displayName(u);
        sUsePackage = true;
        getPreferences(MODE_PRIVATE).edit().putString("pkg", u.toString()).putString("pkg_name", sPkgName)
            .putBoolean("use_pkg", true).apply();
        String low = sPkgName.toLowerCase(Locale.ROOT);
        // (a prod.keys or a folder's file chosen by mistake as the game: the keys row takes it instead)
        if (low.endsWith(".keys") || low.endsWith(".txt")) { packageKeysPicked(u); return; }
        if (sKeys == null) pickPackageKeys();  // (straight on to the keys: the second step)
        else nativeFolderPicked(sPkgName);
    }

    private void packageKeysPicked(Uri u) {
        if (u != null) {
            keepPermission(u);
            sKeys = u;
            sKeysName = displayName(u);
            getPreferences(MODE_PRIVATE).edit().putString("keys", u.toString()).putString("keys_name", sKeysName).apply();
        }
        nativeFolderPicked(savedGameFolder());
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
        if (a == null || (sTree == null && !sUsePackage)) return "nofolder";
        ContentResolver cr = a.getContentResolver();
        a.runOnUiThread(() -> a.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
        try {
            if (sUsePackage) {
                String low = sPkgName != null ? sPkgName.toLowerCase(Locale.ROOT) : "";
                if (low.endsWith(".nsz") || low.endsWith(".xcz") || low.endsWith(".nca") || low.endsWith(".zip")
                    || low.endsWith(".rar") || low.endsWith(".7z")) return "unusable";
                return importPackage(a, cr, sPkg, sPkgName, sKeys, null, dest, buildId);
            }
            Doc root = new Doc(DocumentsContract.getTreeDocumentId(sTree), describe(sTree), true, 0);
            Doc[] found = new Doc[5];  // exefs, romfs, a game package (.xci / .nsp), prod.keys, title.keys
            boolean[] unusable = new boolean[1];
            find(cr, root, 0, found, unusable);
            if ((found[0] == null || found[1] == null) && found[2] != null)
                return importPackage(a, cr, DocumentsContract.buildDocumentUriUsingTree(sTree, found[2].id), found[2].name,
                                     found[3] != null ? DocumentsContract.buildDocumentUriUsingTree(sTree, found[3].id) : null,
                                     found[4] != null ? DocumentsContract.buildDocumentUriUsingTree(sTree, found[4].id) : null,
                                     dest, buildId);
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
    private static String importPackage(LasActivity a, ContentResolver cr, Uri u, String pkgName, Uri keysUri, Uri titleKeys,
                                        String dest, String buildId) {
        java.util.Map<String, byte[]> keys;
        Uri ku = keysUri != null ? keysUri : pickKeys(a);
        if (ku == null) return "nokeys";
        if (keysUri == null && sUsePackage) {  // (asked for here: kept for the next try)
            sKeys = ku;
            sKeysName = a.displayName(ku);
            a.getPreferences(MODE_PRIVATE).edit().putString("keys", ku.toString()).putString("keys_name", sKeysName).apply();
        }
        try (InputStream in = cr.openInputStream(ku)) {
            if (in == null) return "nokeys";
            keys = NxPackage.parseKeys(in);
        } catch (Exception e) {
            Log.e(TAG, "keys", e);
            return "nokeys";
        }
        if (!keys.containsKey("header_key")) return "badkeys";
        if (titleKeys != null)
            try (InputStream in = cr.openInputStream(titleKeys)) {
                if (in != null) for (java.util.Map.Entry<String, byte[]> e : NxPackage.parseKeys(in).entrySet()) keys.putIfAbsent(e.getKey(), e.getValue());
            } catch (Exception e) {
                Log.w(TAG, "title.keys", e);
            }
        try (android.os.ParcelFileDescriptor fd = cr.openFileDescriptor(u, "r")) {
            if (fd == null) return "copy:" + pkgName;
            try (java.io.FileInputStream fin = new java.io.FileInputStream(fd.getFileDescriptor());
                 java.nio.channels.FileChannel ch = fin.getChannel()) {
                long t0 = System.nanoTime();
                String r = NxPackage.extract(ch, keys, new File(dest), buildId, LasActivity::nativeImportProgress);
                Log.i(TAG, "package " + pkgName + ": " + (r == null ? "extracted" : r) + " in " + (System.nanoTime() - t0) / 1000000 + " ms");
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

    // ---- a graphics driver error that stops the app (runtime/platform/android.c plat_fatal_notice): the Vulkan driver is
    // chosen at the start screen already, so the settings' driver rows can't be reached when it fails - this dialog
    // installs a driver (or goes back to the system's) and starts the app again. Blocks the calling (native) thread;
    // returns once the player closed it.
    private static final Object sDlgLock = new Object();
    private static boolean sDlgDone;
    private static String sDlgText;

    public static boolean driverErrorDialog(String text) {
        final LasActivity a = (LasActivity) mSingleton;
        if (a == null) return false;
        synchronized (sDlgLock) { sDlgDone = false; sDlgText = text; }
        a.runOnUiThread(() -> a.showDriverError(null));
        synchronized (sDlgLock) {
            while (!sDlgDone) {
                try { sDlgLock.wait(); } catch (InterruptedException e) { break; }
            }
        }
        return true;
    }

    private void dialogDone() {
        synchronized (sDlgLock) { sDlgDone = true; sDlgLock.notifyAll(); }
    }

    private void showDriverError(String extra) {
        String body = sDlgText;
        int cut = body.indexOf("\n\n");  // (its first line is the headline: the dialog's title already)
        if (cut > 0) body = body.substring(cut + 2);
        String msg = body + (extra != null ? "\n\n" + extra : "");
        android.app.AlertDialog.Builder b = new android.app.AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(R.string.driver_error_title).setMessage(msg).setCancelable(false)
            .setPositiveButton(R.string.driver_install, (d, w) -> {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed",
                                                                   "application/octet-stream"});
                try {
                    startActivityForResult(i, PICK_DRIVER_RECOVER);
                } catch (Exception e) {
                    Log.e(TAG, "driver picker", e);
                    showDriverError(getString(R.string.driver_install_failed, String.valueOf(e.getMessage())));
                }
            })
            .setNegativeButton(R.string.driver_close, (d, w) -> dialogDone());
        if (GpuDrivers.selected(this))
            b.setNeutralButton(R.string.driver_system, (d, w) -> { GpuDrivers.select(this, false); restartApp(); });
        b.show();
    }

    private void recoverDriverPicked(Uri uri) {
        if (uri == null) { showDriverError(null); return; }
        new Thread(() -> {
            try {
                GpuDrivers.install(this, uri);
                runOnUiThread(() -> {
                    android.widget.Toast.makeText(this, R.string.driver_installed, android.widget.Toast.LENGTH_LONG).show();
                    restartApp();
                });
            } catch (Exception e) {
                Log.e(TAG, "driver install", e);
                String why = e.getMessage() != null ? e.getMessage() : e.toString();
                runOnUiThread(() -> showDriverError(getString(R.string.driver_install_failed, why)));
            }
        }, "gpu driver install").start();
    }

    // A fresh process (the Vulkan driver is loaded once per process): the app started again, this one ended
    private void restartApp() {
        Intent i = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (i != null) {
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(i);
        }
        Runtime.getRuntime().exit(0);
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

    // ---- saves to and from a Switch emulator (the start screen's Saves rows): the game's save files (savedata0.bin ...
    // the same images a Switch and its emulators keep) between save/save_0 and a folder the player picks - an
    // emulator's save folder for this game (yuzu / Eden / Citron / Sudachi: nand/user/save/0000000000000000/<user>/
    // 01006BB00C6F0000, Ryujinx: bis/user/save/<id>/0). Importing first keeps the saves there now in
    // save/backup-<date>. The result comes back through nativeSavesDone ("" cancelled, "ok:<files>", or an error).
    private static String sSaveDir;

    public static void transferSaves(boolean importing, String saveDir) {
        final LasActivity a = (LasActivity) mSingleton;
        if (a == null) { nativeSavesDone("no activity"); return; }
        sSaveDir = saveDir;
        a.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | (importing ? 0 : Intent.FLAG_GRANT_WRITE_URI_PERMISSION));
            try {
                a.startActivityForResult(i, importing ? PICK_SAVES_IMPORT : PICK_SAVES_EXPORT);
            } catch (Exception e) {
                Log.e(TAG, "saves picker", e);
                nativeSavesDone("no folder picker");
            }
        });
    }

    private static boolean saveFileName(String n) {
        String low = n.toLowerCase(Locale.ROOT);
        return !n.startsWith(".") && (low.endsWith(".bin") || low.endsWith(".dat")) && !n.contains("/") && !n.contains("..");
    }

    private void savesPicked(boolean importing, Uri tree) {
        if (tree == null) { nativeSavesDone(""); return; }
        ContentResolver cr = getContentResolver();
        new Thread(() -> {
            try {
                File dir = new File(sSaveDir);
                Doc root = new Doc(DocumentsContract.getTreeDocumentId(tree), "", true, 0);
                int n = 0;
                if (importing) {
                    // the save files: in the folder picked, or in the game's folder (01006BB00C6F0000) / an "0" inside it
                    List<Doc> files = new ArrayList<>();
                    Doc at = root;
                    for (int depth = 0; depth < 8 && at != null; depth++) {
                        files.clear();
                        Doc next = null;
                        for (Doc k : childrenOf(cr, tree, at)) {
                            if (!k.dir && saveFileName(k.name)) files.add(k);
                            else if (k.dir && (k.name.equalsIgnoreCase("01006BB00C6F0000") || k.name.equals("0") ||
                                               k.name.equals("save_0"))) next = k;
                        }
                        boolean has = false;
                        for (Doc f : files) has |= f.name.toLowerCase(Locale.ROOT).startsWith("savedata");
                        if (has) break;
                        at = next;
                    }
                    boolean any = false;
                    for (Doc f : files) any |= f.name.toLowerCase(Locale.ROOT).startsWith("savedata");
                    if (!any) { nativeSavesDone("nosaves"); return; }
                    File[] old = dir.listFiles();
                    if (old != null && old.length > 0) {  // (the saves there now, kept)
                        String stamp = new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new java.util.Date());
                        File bak = new File(dir.getParentFile(), "backup-" + stamp);
                        if (!bak.mkdirs()) throw new Exception("mkdir " + bak);
                        for (File o : old)
                            if (o.isFile()) java.nio.file.Files.copy(o.toPath(), new File(bak, o.getName()).toPath());
                        for (File o : old) if (o.isFile() && saveFileName(o.getName())) //noinspection ResultOfMethodCallIgnored
                            o.delete();
                    }
                    if (!dir.isDirectory() && !dir.mkdirs()) throw new Exception("mkdir " + dir);
                    byte[] buf = new byte[1 << 16];
                    for (Doc f : files) {
                        Uri u = DocumentsContract.buildDocumentUriUsingTree(tree, f.id);
                        try (InputStream in = cr.openInputStream(u); OutputStream os = new FileOutputStream(new File(dir, f.name))) {
                            if (in == null) throw new Exception(f.name);
                            for (int k; (k = in.read(buf)) > 0; ) os.write(buf, 0, k);
                        }
                        n++;
                    }
                } else {
                    File[] mine = dir.listFiles();
                    if (mine == null || mine.length == 0) { nativeSavesDone("nosaves"); return; }
                    Uri parent = DocumentsContract.buildDocumentUriUsingTree(tree, root.id);
                    java.util.Map<String, Doc> there = new java.util.HashMap<>();
                    for (Doc k : childrenOf(cr, tree, root)) there.put(k.name, k);
                    for (File f : mine) {
                        if (!f.isFile() || !saveFileName(f.getName())) continue;
                        Doc ex = there.get(f.getName());
                        Uri u = ex != null ? DocumentsContract.buildDocumentUriUsingTree(tree, ex.id)
                                           : DocumentsContract.createDocument(cr, parent, "application/octet-stream", f.getName());
                        if (u == null) throw new Exception("cannot create " + f.getName());
                        try (OutputStream os = cr.openOutputStream(u, "wt")) {
                            if (os == null) throw new Exception(f.getName());
                            java.nio.file.Files.copy(f.toPath(), os);
                        }
                        n++;
                    }
                }
                nativeSavesDone("ok:" + n);
            } catch (Exception e) {
                Log.e(TAG, "saves", e);
                nativeSavesDone(e.getMessage() != null ? e.getMessage() : e.toString());
            }
        }, "saves").start();
    }

    // ---- the player's own Switch fonts (the start screen's Game font): the system's file picker (any file: .bfttf has
    // no type of its own, and SDL's picker filtered them out and handed back content:// names fopen can't open), the
    // files whose names the game's fonts have copied into fontsDir under those names (lower case); another .ttf / .otf
    // becomes the text font. The counts come back through nativeFontsImported.
    private static String sFontsDir, sFontNames;

    public static void importFonts(String fontsDir, String knownNames) {
        final LasActivity a = (LasActivity) mSingleton;
        if (a == null) { nativeFontsImported(0, 0); return; }
        sFontsDir = fontsDir;
        sFontNames = knownNames;
        a.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            try {
                a.startActivityForResult(i, PICK_FONTS);
            } catch (Exception e) {
                Log.e(TAG, "fonts picker", e);
                nativeFontsImported(0, 0);
            }
        });
    }

    private void fontsPicked(Intent data) {
        List<Uri> uris = new ArrayList<>();
        if (data != null && data.getClipData() != null)
            for (int k = 0; k < data.getClipData().getItemCount(); k++) uris.add(data.getClipData().getItemAt(k).getUri());
        else if (data != null && data.getData() != null) uris.add(data.getData());
        if (uris.isEmpty()) return;  // (cancelled)
        new Thread(() -> {
            int ok = 0, skipped = 0;
            java.util.Set<String> known = new java.util.HashSet<>(java.util.Arrays.asList(sFontNames.split("\n")));
            File dir = new File(sFontsDir);
            //noinspection ResultOfMethodCallIgnored
            dir.mkdirs();
            for (Uri u : uris) {
                String name = displayName(u).toLowerCase(Locale.ROOT);
                if (!known.contains(name)) {
                    if (name.endsWith(".ttf") || name.endsWith(".otf")) name = "nintendo_udsg-r_std_003.ttf";
                    else { skipped++; continue; }
                }
                try (InputStream in = getContentResolver().openInputStream(u); OutputStream os = new FileOutputStream(new File(dir, name))) {
                    if (in == null) { skipped++; continue; }
                    byte[] buf = new byte[1 << 16];
                    for (int k; (k = in.read(buf)) > 0; ) os.write(buf, 0, k);
                    ok++;
                } catch (Exception e) {
                    Log.e(TAG, "font " + name, e);
                    skipped++;
                }
            }
            nativeFontsImported(ok, skipped);
        }, "fonts").start();
    }

    // ---- native (runtime/platform/android_jni.c)
    public static native void nativeFolderPicked(String name);
    public static native void nativeImportProgress(long done, long total);
    public static native void nativeGpuDriverInstalled(String error);
    public static native void nativeTranslationImported(String error);
    public static native void nativeSavesDone(String result);
    public static native void nativeFontsImported(int ok, int skipped);
}

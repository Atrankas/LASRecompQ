package com.lasrecomp.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.util.Log;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipInputStream;

// A Vulkan driver the player installed (Mesa Turnip for Adreno GPUs, or a Qualcomm build), used in place of the
// system's from the next start on: runtime/nvn/vulkan/device.c loads it through libadrenotools. A driver is the .zip
// other emulators use (meta.json with its libraryName, and the driver's .so files); one is kept, in the app's private
// storage (a driver can only be loaded from there, not from shared storage). Adapted from WindWakerHD-Android-Plus.
final class GpuDrivers {
    private static final String TAG = "LASRecomp";
    private GpuDrivers() {}

    static final class Driver {
        String name, version, library;
        File dir;
    }

    // Custom drivers only work on Adreno GPUs (the kernel's kgsl device)
    static boolean supported() { return new File("/dev/kgsl-3d0").exists(); }

    static File root(Context c) { return new File(c.getFilesDir(), "gpu_driver"); }

    private static File installed(Context c) { return new File(root(c), "current"); }

    private static SharedPreferences prefs(Context c) { return c.getSharedPreferences("gpu_driver", Context.MODE_PRIVATE); }

    // The installed driver, or null
    static Driver find(Context c) { return read(installed(c)); }

    static boolean selected(Context c) { return prefs(c).getBoolean("use", false) && find(c) != null; }

    static void select(Context c, boolean use) {
        prefs(c).edit().putBoolean("use", use).commit();
        //noinspection ResultOfMethodCallIgnored
        new File(installed(c), ".probe").delete();  // (a fresh try)
    }

    private static Driver read(File dir) {
        try {
            JSONObject m = new JSONObject(new String(Files.readAllBytes(new File(dir, "meta.json").toPath()), StandardCharsets.UTF_8));
            Driver d = new Driver();
            d.name = m.optString("name", dir.getName());
            d.version = m.optString("driverVersion", "");
            d.library = m.getString("libraryName");
            d.dir = dir;
            if (d.library.contains("/") || !new File(dir, d.library).isFile()) return null;
            return d;
        } catch (Exception e) {
            return null;
        }
    }

    // Installs the driver package at uri (replacing the one installed before) and selects it. Returns its name; throws
    // with what's wrong.
    static String install(Context c, Uri uri) throws IOException {
        File tmp = new File(root(c), ".install");
        deleteTree(tmp);
        if (!tmp.mkdirs()) throw new IOException("cannot create " + tmp);
        long total = 0;
        try (InputStream in = c.getContentResolver().openInputStream(uri)) {
            if (in == null) throw new IOException("no data");
            ZipInputStream zip = new ZipInputStream(in);
            byte[] buf = new byte[1 << 16];
            for (ZipEntry e; (e = zip.getNextEntry()) != null; ) {
                // only meta.json and libraries at the top level; nothing else is needed
                String n = e.getName();
                if (e.isDirectory() || n.contains("/") || n.contains("\\") || !(n.equals("meta.json") || n.endsWith(".so"))) continue;
                try (OutputStream out = new FileOutputStream(new File(tmp, n))) {
                    for (int k; (k = zip.read(buf)) > 0; ) {
                        total += k;
                        if (total > (512L << 20)) throw new IOException("too large");
                        out.write(buf, 0, k);
                    }
                }
            }
        } catch (IOException e) {
            deleteTree(tmp);
            throw e;
        }
        Driver d = read(tmp);
        if (d == null) {
            deleteTree(tmp);
            throw new IOException("not a driver package (no meta.json with its libraryName)");
        }
        File dest = installed(c);
        deleteTree(dest);
        if (!tmp.renameTo(dest)) {
            deleteTree(tmp);
            throw new IOException("cannot move " + tmp);
        }
        select(c, true);
        return d.version.isEmpty() ? d.name : d.name + " " + d.version;
    }

    // What device.c needs to load the selected driver ("" when the system's runs): its folder, its library's name, the
    // folder with libadrenotools' hooks and a temporary folder, one per line (each folder with a trailing /). A
    // marker file (.probe, removed by device.c once the driver drew its first frame) catches a driver that crashed
    // or hung at its last start: the system driver then runs, and failed() names it once.
    static String config(Context c) {
        if (!supported() || !selected(c)) return "";
        Driver d = find(c);
        File probe = new File(d.dir, ".probe");
        if (probe.exists()) {
            select(c, false);
            prefs(c).edit().putString("failed", d.name).commit();
            return "";
        }
        String hooks = prepareHooks(c);
        if (hooks == null) return "";
        try {
            //noinspection ResultOfMethodCallIgnored
            probe.createNewFile();
        } catch (IOException e) {
            Log.w(TAG, "cannot create " + probe, e);
        }
        return d.dir.getAbsolutePath() + "/\n" + d.library + "\n" + hooks + "\n" + c.getCacheDir().getAbsolutePath() + "/\n"
               + probe.getAbsolutePath();
    }

    // The driver that failed at its last start (see config), once; else null
    static String failed(Context c) {
        String n = prefs(c).getString("failed", null);
        if (n != null) prefs(c).edit().remove("failed").commit();
        return n;
    }

    // libadrenotools loads its hooks (libmain_hook.so, libhook_impl.so) by name from a folder. The app's native library
    // folder has them only when libraries are extracted at install; this app maps them straight from the APK
    // (useLegacyPackaging false), so they're copied out of it once per APK. Returns the folder (trailing /) or null.
    private static String prepareHooks(Context c) {
        String[] libs = {"libmain_hook.so", "libhook_impl.so"};
        File lib = new File(c.getApplicationInfo().nativeLibraryDir);
        if (new File(lib, libs[0]).isFile() && new File(lib, libs[1]).isFile()) return lib.getAbsolutePath() + "/";
        File dir = new File(root(c), ".hooks");
        String apk = c.getApplicationInfo().sourceDir;
        String stamp = apk + ":" + new File(apk).lastModified();
        File stampFile = new File(dir, "apk");
        try {
            if (stampFile.isFile() && stamp.equals(new String(Files.readAllBytes(stampFile.toPath()), StandardCharsets.UTF_8)))
                return dir.getAbsolutePath() + "/";
            deleteTree(dir);
            if (!dir.mkdirs()) throw new IOException("cannot create " + dir);
            try (ZipFile z = new ZipFile(apk)) {
                for (String n : libs) {
                    ZipEntry e = z.getEntry("lib/arm64-v8a/" + n);
                    if (e == null) throw new IOException(n + " not in the APK");
                    try (InputStream in = z.getInputStream(e); OutputStream out = new FileOutputStream(new File(dir, n))) {
                        byte[] buf = new byte[1 << 16];
                        for (int k; (k = in.read(buf)) > 0; ) out.write(buf, 0, k);
                    }
                }
            }
            Files.write(stampFile.toPath(), stamp.getBytes(StandardCharsets.UTF_8));
            return dir.getAbsolutePath() + "/";
        } catch (IOException e) {
            Log.e(TAG, "GPU driver hooks", e);
            deleteTree(dir);
            return null;
        }
    }

    static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}

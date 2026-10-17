package com.lasrecomp.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

// Performance mode for rooted phones, while the game is in front. The game's frame is a chain - its CPU work, then the
// render thread's commands, then the GPU - so no part looks fully busy, and the phone's governors keep the clocks low
// (an S20 FE: GPU at 441 of 587 MHz, big cores capped at 1.75 GHz by Samsung's power policy): the overworld ran at 20
// fps with room to spare everywhere. With root, a small shell script keeps the CPU caps lifted, the fastest core on
// and the GPU at its top clock (also past the skin-temperature cap while the battery is under 45 C), re-applying every second (the system resets them), and puts everything back when the
// game leaves the front (a marker file it watches). Needs root granted to the app (Magisk asks once). Off when the
// phone has no su, or in the settings ("root_boost" in the "perf" preferences).
final class RootBoost {
    private static final String TAG = "LASRecomp";
    private static final String SCRIPT =
        "M=\"$1\"\n" +
        "g=/sys/class/kgsl/kgsl-3d0\n" +
        "c7=/sys/devices/system/cpu/cpu7/core_ctl/min_cpus\n" +
        "gmin=$(cat $g/devfreq/min_freq 2>/dev/null); gpl=$(cat $g/min_pwrlevel 2>/dev/null); cmin=$(cat $c7 2>/dev/null)\n" +
        "while [ -f \"$M\" ]; do\n" +
        "  echo -1 > /sys/power/cpufreq_max_limit 2>/dev/null\n" +
        "  for p in /sys/devices/system/cpu/cpufreq/policy*; do cat $p/cpuinfo_max_freq > $p/scaling_max_freq 2>/dev/null; done\n" +
        "  echo 1 > $c7 2>/dev/null\n" +
        "  cat $g/devfreq/max_freq > $g/devfreq/min_freq 2>/dev/null\n" +
        "  echo 0 > $g/min_pwrlevel 2>/dev/null\n" +
        // the GPU's skin-temperature cap too (Samsung's thermal service lowers the top clock from ~42 C skin), while
        // the battery stays under 45 C; above that the system's thermal control is left alone
        "  bt=$(cat /sys/class/power_supply/battery/temp 2>/dev/null)\n" +
        "  [ \"${bt:-999}\" -lt 450 ] && echo 0 > $g/thermal_pwrlevel 2>/dev/null\n" +
        "  sleep 1\n" +
        "done\n" +
        "[ -n \"$gmin\" ] && echo $gmin > $g/devfreq/min_freq 2>/dev/null\n" +
        "[ -n \"$gpl\" ] && echo $gpl > $g/min_pwrlevel 2>/dev/null\n" +
        "[ -n \"$cmin\" ] && echo $cmin > $c7 2>/dev/null\n";

    private static Process sProc;

    static boolean enabled(Context c) {
        SharedPreferences p = c.getSharedPreferences("perf", Context.MODE_PRIVATE);
        return p.getBoolean("root_boost", true) && hasSu();
    }

    private static boolean hasSu() {
        for (String d : new String[] {"/system/bin", "/system/xbin", "/sbin", "/product/bin", "/debug_ramdisk", "/system_ext/bin"})
            if (new File(d, "su").exists()) return true;
        return false;
    }

    private static File marker(Context c) { return new File(c.getFilesDir(), "root_boost.on"); }

    // The game is in front: start the script (if it isn't running)
    static synchronized void start(Context c) {
        if (!enabled(c)) return;
        try {
            File m = marker(c);
            //noinspection ResultOfMethodCallIgnored
            m.createNewFile();
            if (sProc != null && sProc.isAlive()) return;
            File script = new File(c.getFilesDir(), "root_boost.sh");
            try (FileOutputStream o = new FileOutputStream(script)) { o.write(SCRIPT.getBytes(StandardCharsets.UTF_8)); }
            sProc = new ProcessBuilder("su", "-c", "sh " + script.getAbsolutePath() + " " + m.getAbsolutePath())
                        .redirectErrorStream(true).redirectOutput(new File(c.getFilesDir(), "root_boost.log")).start();
            Log.i(TAG, "[perf] root performance mode on");
        } catch (Exception e) {
            Log.w(TAG, "[perf] root performance mode unavailable: " + e);
        }
    }

    // The game left the front: the script puts the clocks back and ends (within a second)
    static synchronized void stop(Context c) {
        //noinspection ResultOfMethodCallIgnored
        marker(c).delete();
    }
}

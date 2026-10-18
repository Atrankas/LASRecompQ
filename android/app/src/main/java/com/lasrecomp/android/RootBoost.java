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
// and the GPU at its top clock (also past the skin-temperature cap while the battery is under 45 C), re-applying every
// second (the system resets them), and puts everything back when the game leaves the front (a marker file it watches)
// or its process ends (a crash, a kill). Needs root granted to the app (Magisk asks once). Off when the
// phone has no su, or in the settings ("root_boost" in the "perf" preferences).
// The game's Power setting (power_mode in larecomp.ini, read every second): 0 balanced - the GPU's floor at about
// three quarters of its top clock, the system's CPU limits and temperature limits respected (the phone ran hot within
// the hour with the GPU held at its top clock, and restarted under peak load with the CPU caps lifted); 1 maximum
// performance - as described above.
final class RootBoost {
    private static final String TAG = "LASRecomp";
    private static final String SCRIPT =
        "M=\"$1\"; PID=\"$2\"; INI=\"$3\"\n" +
        "g=/sys/class/kgsl/kgsl-3d0\n" +
        "c7=/sys/devices/system/cpu/cpu7/core_ctl/min_cpus\n" +
        // the GPU's floor back to its lowest clock afterwards (not the value found: another run's script may still
        // have held it up when this one started)
        "gmin=; for f in $(cat $g/devfreq/available_frequencies 2>/dev/null); do [ -z \"$gmin\" ] || [ $f -lt $gmin ] && gmin=$f; done\n" +
        "gpl=$(( $(cat $g/num_pwrlevels 2>/dev/null || echo 1) - 1 )); cmin=$(cat $c7 2>/dev/null)\n" +
        "tpl=$(cat $g/thermal_pwrlevel 2>/dev/null); lim=$(cat /sys/power/cpufreq_max_limit 2>/dev/null)\n" +
        "smax=\"\"; for p in /sys/devices/system/cpu/cpufreq/policy*; do smax=\"$smax $p:$(cat $p/scaling_max_freq 2>/dev/null)\"; done\n" +
        // balanced: the GPU's floor at the clock nearest three quarters of the top one
        "gtop=$(cat $g/devfreq/max_freq 2>/dev/null); gbal=$gtop\n" +
        "for f in $(cat $g/devfreq/available_frequencies 2>/dev/null); do [ $(( f / 1000 * 4 )) -ge $(( gtop / 1000 * 3 )) ] && [ $f -lt $gbal ] && gbal=$f; done\n" +
        // on while the game is in front (the marker) and alive (a crash or a kill skips onPause)
        "while [ -f \"$M\" ] && kill -0 \"$PID\" 2>/dev/null; do\n" +
        "  mode=$(grep -m1 '^power_mode' \"$INI\" 2>/dev/null | tr -dc 0-9)\n" +
        "  if [ \"$mode\" = 1 ]; then\n" +
        "    echo -1 > /sys/power/cpufreq_max_limit 2>/dev/null\n" +
        "    for p in /sys/devices/system/cpu/cpufreq/policy*; do cat $p/cpuinfo_max_freq > $p/scaling_max_freq 2>/dev/null; done\n" +
        "    echo 1 > $c7 2>/dev/null\n" +
        "    cat $g/devfreq/max_freq > $g/devfreq/min_freq 2>/dev/null\n" +
        "    echo 0 > $g/min_pwrlevel 2>/dev/null\n" +
        // the GPU's skin-temperature cap too (Samsung's thermal service lowers the top clock from ~42 C skin), while
        // the battery stays under 45 C; above that the system's thermal control is left alone
        "    bt=$(cat /sys/class/power_supply/battery/temp 2>/dev/null)\n" +
        "    [ \"${bt:-999}\" -lt 450 ] && echo 0 > $g/thermal_pwrlevel 2>/dev/null\n" +
        "  else\n" +
        // (balanced: the CPU's limits as the system sets them - lifted, the phone restarted under peak load on its
        // worn battery: a hard reset, nothing logged - and those changed by maximum performance put back)
        "    [ -n \"$lim\" ] && [ \"$(cat /sys/power/cpufreq_max_limit 2>/dev/null)\" = -1 ] && echo $lim > /sys/power/cpufreq_max_limit 2>/dev/null\n" +
        "    echo $gbal > $g/devfreq/min_freq 2>/dev/null\n" +
        "    [ -n \"$tpl\" ] && [ \"$(cat $g/thermal_pwrlevel 2>/dev/null)\" = 0 ] && [ \"$tpl\" != 0 ] && echo $tpl > $g/thermal_pwrlevel 2>/dev/null\n" +
        "  fi\n" +
        "  sleep 1\n" +
        "done\n" +
        // everything back as it was (the system adjusts the caps again from there)
        "[ -n \"$gmin\" ] && echo $gmin > $g/devfreq/min_freq 2>/dev/null\n" +
        "[ -n \"$gpl\" ] && echo $gpl > $g/min_pwrlevel 2>/dev/null\n" +
        "[ -n \"$tpl\" ] && echo $tpl > $g/thermal_pwrlevel 2>/dev/null\n" +
        "[ -n \"$cmin\" ] && echo $cmin > $c7 2>/dev/null\n" +
        "for e in $smax; do echo ${e#*:} > ${e%%:*}/scaling_max_freq 2>/dev/null; done\n" +
        "[ -n \"$lim\" ] && echo $lim > /sys/power/cpufreq_max_limit 2>/dev/null\n";

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
            File ext = c.getExternalFilesDir(null);
            String ini = ext != null ? new File(ext, "larecomp.ini").getAbsolutePath() : "";
            sProc = new ProcessBuilder("su", "-c", "sh " + script.getAbsolutePath() + " " + m.getAbsolutePath() + " "
                                        + android.os.Process.myPid() + " " + ini)
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

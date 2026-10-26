"""Native settings regression checks; no game files or player settings are used."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    exe = directory / ('settings-test.exe' if os.name == 'nt' else 'settings-test')
    # runtime/platform/xr_quest.c: settings.c hands the Quest's floating-panel values to it (its own module is a no-op
    # off Android, so this also keeps that branch compiling)
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                   ['-std=gnu11', '-Iruntime', 'runtime/settings.c', 'runtime/cosmetic_colour.c', 'runtime/platform/xr_quest.c',
                    'tools/test_settings.c', '-o', str(exe), '-lm'],
                   cwd=ROOT, check=True)
    cases = [(setting, {}, code) for setting, code in
             [('zh-CN', 'zh-Hans'), ('zh-TW', 'zh-Hant'), ('zh-Hans', 'zh-Hans'),
              ('zh-Hant', 'zh-Hant'), ('en-US', 'en-US'), ('ja', 'ja'), ('unknown', 'en-US')]]
    if sys.platform.startswith('linux'):
        for locale in ['zh_CN.UTF-8', 'zh_SG.UTF-8', 'zh-Hans', 'zh-Hans-HK', 'zh_Hans_CN', 'zh']:
            cases.append(('auto', {'LC_ALL': locale}, 'zh-Hans'))
        for locale in ['zh_TW.UTF-8', 'zh_HK.UTF-8', 'zh_MO.UTF-8', 'zh-HK', 'zh-MO',
                       'zh-Hant', 'zh-Hant-TW', 'zh_Hant_CN', 'zh_hant', 'zh_hk.UTF-8']:
            cases.append(('auto', {'LC_ALL': locale}, 'zh-Hant'))
        cases += [('auto', {'LC_MESSAGES': 'zh_HK.UTF-8', 'LANG': 'zh_CN.UTF-8'}, 'zh-Hant'),
                  ('auto', {'LANG': 'zh_MO.UTF-8'}, 'zh-Hant'),
                  ('auto', {'LC_ALL': 'zh_CN.UTF-8', 'LC_MESSAGES': 'zh_HK.UTF-8'}, 'zh-Hans'),
                  ('auto', {'LC_ALL': 'ja_JP.UTF-8'}, 'ja'), ('auto', {'LC_ALL': 'C'}, 'en-US')]
    count = 0
    for newline in ['\n', '\r\n']:
        for setting, locale, code in cases:
            for seed in ['Regression seed', '']:
                ini = directory / 'test.ini'
                lines = [f'language = {setting}', 'aspect = original', 'upscaler = bilinear',
                         'prompts = xbox', 'volume = 1.25', 'volume =', 'language =', f'rando_seed = {seed}']
                ini.write_bytes((newline.join(lines) + newline).encode())
                env = dict(os.environ, LARECOMP_SETTINGS=str(ini), LC_ALL='', LC_MESSAGES='', LANG='C')
                env.update(locale)
                subprocess.run([str(exe), code, seed], env=env, check=True, capture_output=True)
                saved = ini.read_text()
                expected = 'auto' if setting in ['auto', 'unknown'] else code
                assert f'language = {expected}\n' in saved
                assert f'rando_seed = {seed}\n' in saved
                count += 1
    # the upscaler's default follows the GPU until the player picks one (settings_resolve_defaults)
    for lines, mode in [(['upscaler = auto'], 'upscaler-auto'), (['volume = 1'], 'upscaler-auto'),
                        (['upscaler = fsr1'], 'upscaler-chosen'), (['upscaler = metalfx'], 'upscaler-chosen')]:
        ini = directory / 'test.ini'
        ini.write_text('\n'.join(lines) + '\n')
        subprocess.run([str(exe), mode], env=dict(os.environ, LARECOMP_SETTINGS=str(ini)), check=True, capture_output=True)
        if mode == 'upscaler-auto':
            assert 'upscaler = auto\n' in ini.read_text()
        count += 1
    # likewise the resolution: Match display until the player picks one (also on integrated GPUs); a saved one is kept
    for lines, mode, saved in [(['resolution = auto'], 'res-auto', 'auto'), (['volume = 1'], 'res-auto', 'auto'),
                               (['resolution = match'], 'res-chosen', 'match'), (['resolution = 1x'], 'res-chosen', '1x'),
                               (['resolution = 75%'], 'res-chosen', '75%'), (['resolution = dynamic'], 'res-chosen', 'dynamic')]:
        ini = directory / 'test.ini'
        ini.write_text('\n'.join(lines) + '\n')
        env = dict(os.environ, LARECOMP_SETTINGS=str(ini))
        env.pop('LARECOMP_INTEGRATED', None)
        subprocess.run([str(exe), mode], env=env, check=True, capture_output=True)
        assert f'resolution = {saved}\n' in ini.read_text(), (lines, ini.read_text())
        count += 1
    # Cosmetics colour rows (shared palette): older names map onto it, Custom is a hex value, unknown -> Default
    ini = directory / 'test.ini'
    ini.write_text('tunic = Yellow\nhearts = Pink\nhair = #12ab34\nshield = Sparkly\nskin = Random\n')
    subprocess.run([str(exe), 'colours'], env=dict(os.environ, LARECOMP_SETTINGS=str(ini)), check=True, capture_output=True)
    saved = ini.read_text()
    for line in ['tunic = Gold', 'hearts = Pink', 'hair = #12AB34', 'shield = Default', 'skin = Random']:
        assert line + '\n' in saved, (line, saved)
    count += 1
    # the Randomizer tab's New seed options: an older file without the key stays without it (the defaults)
    for lines, value in [(['rando_options = 50000-00004-20000-40G20-04000-0G11R', 'rando_seed = X'],
                          '50000-00004-20000-40G20-04000-0G11R'), (['rando_seed = X'], '')]:
        ini = directory / 'test.ini'
        ini.write_text('\n'.join(lines) + '\n')
        subprocess.run([str(exe), 'rando-options', value], env=dict(os.environ, LARECOMP_SETTINGS=str(ini)), check=True,
                       capture_output=True)
        saved = ini.read_text()
        assert (f'rando_options = {value}\n' in saved) if value else 'rando_options' not in saved, saved
        count += 1
    # Meta Quest: the floating panel (runtime/settings.c vr_panel / vr_panel_adjust, runtime/platform/xr_quest.c): a
    # saved size and place are kept, a file from before this (or a fresh install) gets the defaults
    for lines, want in [([], 'vr_panel = 3.20, 2.20, 0.00, -0.10'),
                        (['vr_panel = 5.00, 1.50, -0.50, 0.25'], 'vr_panel = 5.00, 1.50, -0.50, 0.25'),
                        (['vr_panel = 8, 3, 1, 2'], 'vr_panel = 8.00, 3.00, 1.00, 2.00'),
                        (['vr_panel = 8, 3, -0.5, 0.25', 'vr_panel_adjust = 1'], 'vr_panel = 8.00, 3.00, -0.50, 0.25'),
                        (['vr_panel = 30, 0.1, 9, -9'], 'vr_panel = 20.00, 0.40, 6.00, -6.00')]:  # out of range: clamped
        ini = directory / 'test.ini'
        ini.write_text('\n'.join(lines + ['rando_seed = X']) + '\n')
        subprocess.run([str(exe), 'panel'], env=dict(os.environ, LARECOMP_SETTINGS=str(ini)), check=True, capture_output=True)
        saved = ini.read_text()
        assert want + '\n' in saved, (lines, saved)
        assert 'vr_panel_adjust' not in saved, saved  # (session state: never written)
        count += 1
    print(f'{count} parser, locale, upscaler / resolution default, panel and roundtrip cases passed (LF/CRLF, empty/nonempty seeds)')

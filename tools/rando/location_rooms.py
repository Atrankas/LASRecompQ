#!/usr/bin/env python3
"""Map LAS item locations to the room/level that contains them -> tools/rando/location_rooms.json."""
import json, os, re, sys, types
HERE = os.path.dirname(os.path.abspath(__file__))
LAS = os.path.normpath(os.path.join(HERE, '..', '..', 'third_party', 'LAS-Randomizer'))
sys.modules['PySide6'] = types.ModuleType('PySide6'); sys.modules['PySide6.QtCore'] = types.ModuleType('PySide6.QtCore')
sys.argv = [os.path.join(LAS, 'randomizer.py')]; sys.path.insert(0, LAS); os.chdir(LAS)
import yaml
from RandomizerCore.Randomizers import data
logic = yaml.safe_load(open('RandomizerCore/Data/logic.yml').read().split('---', 1)[1])
rooms = {}
for tab in ('CHEST_ROOMS', 'SMALL_KEY_ROOMS', 'GOLDEN_LEAF_ROOMS', 'INSTRUMENT_ROOMS', 'HEART_ROOMS'):
    for k, v in getattr(data, tab).items():
        if isinstance(v, (list, tuple)): v = v[0]
        rooms[k] = v
sys.path.insert(0, HERE)
import underwater  # (the sunken seashells: no LAS table has their rooms)
for k, (_, r, _) in underwater.SPOTS.items():
    rooms.setdefault(k, r)
# rooms no LAS table has (dungeon room shuffle and mini-boss souls follow a check's room): owl statues (their
# DungeonOwlStatue.bfevfl examine_<dungeon><cell> entries, owls.py), the Color Dungeon's rupee room and the Fairy Queen,
# Master Stalfos' last fight (his parameter 3). Boss drops stay None: room_logic.py takes the room above the boss door.
L = {'D0': 'Lv10ClothesDungeon', 'D1': 'Lv01TailCave', 'D2': 'Lv02BottleGrotto', 'D3': 'Lv03KeyCavern',
     'D4': 'Lv04AnglersTunnel', 'D5': 'Lv05CatfishsMaw', 'D6': 'Lv06FaceShrine', 'D7': 'Lv07EagleTower',
     'D8': 'Lv08TurtleRock'}
EXTRA = {'D1-owl-statue-long-hallway': '04C', 'D1-owl-statue-spinies': '04B', 'D1-owl-statue-3-of-a-kind': '05F',
         'D2-owl-statue-first-switch': '07E', 'D2-owl-statue-push-puzzle': '06G', 'D2-owl-statue-past-hinox': '04C',
         'D3-owl-statue-basement-north': '03G', 'D3-owl-statue-arrow': '02D', 'D3-owl-statue-northwest': '01A',
         'D4-owl-statue': '05E', 'D5-owl-statue-triple-stalfos': '06F', 'D5-owl-statue-before-slime-eel': '03C',
         'D6-owl-statue-ledge': '04D', 'D6-owl-statue-southeast': '08G', 'D6-owl-statue-canal': '03G',
         'D7-owl-statue-ball': '02D', 'D7-owl-statue-kirbys': '05D', 'D7-owl-statue-hookshot-chest': '04B',
         'D8-owl-statue-above-smasher': '06H', 'D8-owl-statue-below-gibdos': '04G', 'D8-owl-statue-eye-statue': '04C',
         'D0-owl-statue-nine-switches': '06C', 'D0-owl-statue-first-switches': '07D',
         'D0-owl-statue-before-mini-boss': '05F', 'D0-fairy-1': '04C', 'D0-fairy-2': '04C',
         'D5-master-stalfos': '01B', **{f'D0-rupee-{i}': '08D' for i in range(1, 29)}}
for k, c in EXTRA.items():
    rooms.setdefault(k, f'{L[k[:2]]}_{c}')
out = {}
for k, v in logic.items():
    if v.get('type') != 'item': continue
    r = rooms.get(k)
    lvl = re.match(r'(.+)_\d\d[A-Z]', r).group(1) if r and re.match(r'(.+)_\d\d[A-Z]', r) else None
    out[k] = {'room': r, 'level': lvl, 'region': v.get('region'), 'subtype': v.get('subtype'),
              'cond': v.get('condition-basic')}
json.dump(out, open(os.path.join(HERE, 'location_rooms.json'), 'w'), indent=1)
unk = [k for k, v in out.items() if not v['room']]
print(len(out), 'item locations;', len(unk), 'without a room:')
for k in unk: print('  ', k, out[k]['region'], out[k]['subtype'])

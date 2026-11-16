#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Patch plate_recognition.py on the Pi to add plate-DB matching."""
import io

path = "/home/<user>/parking_gateway/plate_recognition.py"
src = io.open(path, encoding="utf-8").read()

# 1) add import after the first import line
if "import plate_matcher" not in src:
    old = "import os, re, sys, time\n"
    assert src.count(old) == 1, "import marker count=%d" % src.count(old)
    src = src.replace(old, old + "import plate_matcher\n", 1)

# 2) load db at top of main()
marker = '    args = [a for a in sys.argv[1:] if not a.startswith("-")]\n'
if "plate_matcher.load_db()" not in src:
    assert src.count(marker) == 1, "args marker count=%d" % src.count(marker)
    src = src.replace(marker, "    db = plate_matcher.load_db()\n" + marker, 1)

# 3) apply matching right after post_process
old_fixed = "                fixed = post_process(text)\n"
if "plate_matcher.fix_plate" not in src:
    assert src.count(old_fixed) == 1, "fixed marker count=%d" % src.count(old_fixed)
    add = (
        "                fixed = post_process(text)\n"
        "                full, score, hit = plate_matcher.fix_plate(fixed, db)\n"
        "                if hit:\n"
        '                    print("  -> plate_db match: %s (sim %.2f)" % (full, score))\n'
        "                    fixed = full\n"
    )
    src = src.replace(old_fixed, add, 1)

io.open(path, "w", encoding="utf-8").write(src)
print("PATCH_OK")
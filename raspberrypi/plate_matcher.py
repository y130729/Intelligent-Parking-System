#!/usr/bin/env python3
"""Plate DB fuzzy matcher.

Takes OCR text (which may contain mis-recognized chars) and matches it
against a local plate database, returning the best matching full plate
(e.g. "T23475" -> "T234J5"). This avoids outputting wrong plates caused
by single-char OCR errors.

Usage:
    from plate_matcher import load_db, fix_plate
    full, score, hit = fix_plate("T23475")
"""
import json
import os
import re

DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vehicles.json")
MIN_WIN = 5             # shortest sliding-window length to try
MATCH_THRESHOLD = 0.80  # similarity >= this -> accept DB match

# OCR confusion pairs: correct char -> chars it is often misread as
CONFUSE = {
    "0": "OQD", "O": "0QD", "Q": "O0D", "D": "0O",
    "1": "ITL7", "I": "1TL", "T": "7I1", "L": "I1",
    "7": "TZ1", "Z": "72", "2": "Z",
    "5": "S6", "S": "5", "6": "G5", "G": "6",
    "8": "B3", "B": "83", "3": "8B",
    "M": "N", "N": "M", "U": "V", "V": "U", "W": "VV",
    "J": "7T", "K": "4",
}
CONFUSE = {k: set(v) for k, v in CONFUSE.items()}


def normalize(s):
    return re.sub(r"[^A-Za-z0-9]", "", s or "").upper()


def char_sim(a, b):
    if a == b:
        return 1.0
    if a in CONFUSE and b in CONFUSE[a]:
        return 0.6
    if b in CONFUSE and a in CONFUSE[b]:
        return 0.6
    return 0.0


def wlev_sim(a, b):
    """Weighted Levenshtein similarity in [0, 1]."""
    n, m = len(a), len(b)
    if n == 0 or m == 0:
        return 0.0
    prev = list(range(m + 1))
    for i in range(1, n + 1):
        cur = [i] + [0] * m
        ai = a[i - 1]
        for j in range(1, m + 1):
            cur[j] = min(prev[j] + 1, cur[j - 1] + 1,
                         prev[j - 1] + (1.0 - char_sim(ai, b[j - 1])))
        prev = cur
    return 1.0 - prev[m] / max(n, m)


def _windows(s):
    s = normalize(s)
    seen = set()
    for ln in range(MIN_WIN, len(s) + 1):
        for i in range(0, len(s) - ln + 1):
            w = s[i:i + ln]
            if w not in seen:
                seen.add(w)
                yield w
    if not seen and s:
        yield s


def load_db(path=None):
    path = path or DB_PATH
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    if isinstance(data, dict):
        data = data.get("vehicles", data.get("plates", []))
    return data


def _prep(entries):
    db = []
    for e in entries:
        full = e.get("plate", "") if isinstance(e, dict) else str(e)
        std = normalize(full)
        if std:
            db.append((full, std))
    return db


def match_plate(text, entries):
    """Return (full_plate, score) or (None, 0.0)."""
    db = _prep(entries)
    best_full, best_score = None, 0.0
    for win in _windows(text):
        for full, std in db:
            if abs(len(win) - len(std)) > 3:
                continue
            sc = wlev_sim(win, std)
            if sc > best_score:
                best_full, best_score = full, sc
    if best_score >= MATCH_THRESHOLD:
        return best_full, best_score
    return None, best_score


def fix_plate(text, entries=None):
    """OCR text -> (full plate, score, matched). Returns normalized text if no match."""
    if entries is None:
        entries = load_db()
    full, score = match_plate(text, entries)
    if full:
        return full, score, True
    return normalize(text), score, False
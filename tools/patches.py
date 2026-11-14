"""Per-instruction replacements applied by gen.py (guest address -> function(gen, addr) returning C code).

Derived from LASRecomp (MIT, (c) 2026 Peiikai). The Link's Awakening patches were removed: their
addresses belong to that game's executable; so were the Pokemon Let's Go ones. Patches for this game go here later.
"""

PATCHES = {}

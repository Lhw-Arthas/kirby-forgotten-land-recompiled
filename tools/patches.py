"""The game's code patches, applied by gen.py to the main module (tools/game_addresses.py selects the version's
address table first). The game's own; none yet.

PATCHES: guest address -> function(gen, addr) returning the C that replaces the instruction there.
HOOKS: guest address -> (the instruction word it must hold, (jump targets...), what it is): the generated code calls
rt_hook(c, site) there first (runtime/hle/hooks.c).
"""

PATCHES = {}
HOOKS = {}

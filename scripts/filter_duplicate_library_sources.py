"""Ignore identical numbered dependency copies created by desktop sync software.

Only dependency sources are considered. Originals remain authoritative; a
numbered file with different contents fails loudly rather than hiding edits.
"""
import atexit
from pathlib import Path
import re

Import("env")

root = Path(env.subst("$PROJECT_LIBDEPS_DIR"))
skipped = 0


def filter_source(node):
    global skipped
    source = Path(node.srcnode().get_abspath())
    try:
        source.relative_to(root)
    except ValueError:
        return node
    normal = re.sub(r" [0-9]+(?=\.[^.]+$)", "", source.name)
    if normal == source.name:
        return node
    original = source.with_name(normal)
    if not original.is_file():
        return node
    if source.read_bytes() != original.read_bytes():
        raise RuntimeError(f"Conflicting dependency copies: {source.name} and {original.name}")
    skipped += 1
    return None


def summary():
    if skipped:
        print(f"[DependencySources] Skipped {skipped} identical numbered source copies")


env.AddBuildMiddleware(filter_source)
atexit.register(summary)

#!/usr/bin/env python3
"""Verify the real build filter never hides different or project-owned sources."""
from pathlib import Path
import runpy
import tempfile
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as directory:
    deps=Path(directory)/'libdeps';deps.mkdir()
    class Env:
        callback=None
        def subst(self,_):return str(deps)
        def AddBuildMiddleware(self,callback):self.callback=callback
    class Node:
        def __init__(self,path):self.path=path
        def srcnode(self):return self
        def get_abspath(self):return str(self.path)
    env=Env()
    runpy.run_path(str(root/'scripts/filter_duplicate_library_sources.py'),init_globals={'env':env,'Import':lambda _:None})
    original=deps/'example.cpp';original.write_text('original')
    duplicate=deps/'example 2.cpp';duplicate.write_text('original')
    assert env.callback(Node(duplicate)) is None
    different=deps/'example 3.cpp';different.write_text('changed')
    try:env.callback(Node(different));raise AssertionError('different copy was accepted')
    except RuntimeError:pass
    outside=Path(directory)/'project 2.cpp';outside.write_text('original');node=Node(outside)
    assert env.callback(node) is node
    orphan=deps/'orphan 2.cpp';orphan.write_text('original');node=Node(orphan)
    assert env.callback(node) is node
    node=Node(original);assert env.callback(node) is node
print('PASS: identical dependency copies skipped; different, orphan and project sources preserved')

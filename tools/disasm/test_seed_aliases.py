"""Seeds marked "alias" become alias entries, never function starts.

A seed is a function start, and a start ends the function before it. An entry
point a title reaches in the middle of a function (a vtable slot or switch arm
another build of the analysis split out) must not cut that function short: its
own switch arms past the cut become indirect tail jumps to addresses that start
no function. A seed entry marked "alias" goes to _pass_seed_aliases as an alias
body instead, as a tail-jump alias does, and the enclosing function keeps its
whole extent.
"""
import json
import os
import sys
import tempfile
import unittest
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.__main__ import _load_seed_functions  # noqa: E402
from tools.disasm.functions import FunctionDetector  # noqa: E402


class SeedAliasTest(unittest.TestCase):
    def test_loader_keeps_alias_seeds_apart(self):
        seeds = [
            {"start": "0x0019FFC0"},
            {"start": "0x001A0003", "alias": True, "note": "an entry inside 0x0019FFC0"},
        ]
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "seeds.json")
            with open(path, "w") as f:
                json.dump(seeds, f)
            aliases = set()
            addrs = _load_seed_functions(path, set(), aliases)
        self.assertEqual(addrs, [0x0019FFC0])
        self.assertEqual(aliases, {0x001A0003})

    def test_alias_seed_inside_a_body_is_an_alias_to_its_end(self):
        det = FunctionDetector.__new__(FunctionDetector)
        det._candidates = {}
        det._alias_entries = {}
        det.functions = {0x0019FFC0: SimpleNamespace(start=0x0019FFC0, end=0x001A0521)}
        det.alias_seeds = {0x001A0003, 0x001A0190, 0x001A0800}
        det._pass_seed_aliases()
        self.assertEqual(det._alias_entries, {0x001A0003: 0x001A0521, 0x001A0190: 0x001A0521})
        self.assertNotIn(0x001A0003, det._candidates)


if __name__ == "__main__":
    unittest.main()

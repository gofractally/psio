"""Every ``python`` block of python/README.md, run in order in one module."""

from __future__ import annotations

import os
import re
import sys
import types
import unittest

import support  # noqa: F401  (puts src/ on the path)

README = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "README.md")


class Readme(unittest.TestCase):
    def test_examples(self) -> None:
        with open(README, encoding="utf-8") as f:
            text = f.read()
        blocks = re.findall(r"^```python\n(.*?)^```", text, re.S | re.M)
        self.assertGreaterEqual(len(blocks), 25)
        module = types.ModuleType("psio_readme_examples")
        sys.modules[module.__name__] = module
        try:
            for i, block in enumerate(blocks):
                line = text[:text.index(block)].count("\n") + 1
                try:
                    exec(compile("\n" * (line - 1) + block, README, "exec"), module.__dict__)
                except Exception as e:
                    raise AssertionError(f"README.md block {i + 1} (line {line}): {type(e).__name__}: {e}") from e
        finally:
            del sys.modules[module.__name__]
        print(f"README.md: {len(blocks)} python blocks ran")


if __name__ == "__main__":
    unittest.main()

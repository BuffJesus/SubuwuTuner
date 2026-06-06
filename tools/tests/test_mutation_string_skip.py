# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Tests for tools/mutation_test.py's _is_in_string_literal skip
heuristic. Locks in the behavior across regular strings, raw strings,
and char literals so a future refactor of the harness's regex matching
doesn't silently re-introduce false-positive mutants inside literals.

Run with:
    python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

_THIS = Path(__file__).resolve()
_MUTATION_TEST = _THIS.parent.parent / "mutation_test.py"
_spec = importlib.util.spec_from_file_location("mutation_test", _MUTATION_TEST)
assert _spec is not None and _spec.loader is not None
mutation_test = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = mutation_test
_spec.loader.exec_module(mutation_test)


_is_in = mutation_test._is_in_string_literal


def col_of(text: str, needle: str) -> int:
    """Return the column of the first character of `needle` in `text`."""
    idx = text.find(needle)
    assert idx >= 0, f"{needle!r} not found in {text!r}"
    return idx


class RegularStringTests(unittest.TestCase):
    """Locks in the pre-existing single-line `"..."` behavior so the
    raw-string + char-literal refactor doesn't regress it."""

    def test_operator_inside_quoted_string_is_skipped(self):
        text = 'if (true) { puts("count != 0 expected"); }'
        # `!=` inside the literal.
        self.assertTrue(_is_in(text, col_of(text, "!=")))

    def test_operator_outside_string_is_not_skipped(self):
        text = 'if (a != b) puts("hello");'
        self.assertFalse(_is_in(text, col_of(text, "!=")))

    def test_escaped_quote_does_not_close_string(self):
        # `\"` inside the literal must not flip state out.
        text = 'puts("he said \\"x != y\\" loudly");'
        self.assertTrue(_is_in(text, col_of(text, "!=")))

    def test_position_at_opening_quote_is_outside(self):
        text = 'puts("hi");'
        self.assertFalse(_is_in(text, col_of(text, '"')))

    def test_position_at_closing_quote_is_inside(self):
        text = 'puts("hi");'
        # Closing `"` is at the position right after `i`.
        close_pos = text.find('"', text.find('"') + 1)
        self.assertTrue(_is_in(text, close_pos))


class CharLiteralTests(unittest.TestCase):
    """New behavior: skip operators inside `'...'` char literals."""

    def test_less_than_inside_char_literal_is_skipped(self):
        text = "if (c == '<') return 1;"
        # `<` inside char literal.
        lt_pos = text.index("'<'") + 1
        self.assertTrue(_is_in(text, lt_pos))

    def test_greater_than_inside_char_literal_is_skipped(self):
        text = "if (c == '>') return 1;"
        gt_pos = text.index("'>'") + 1
        self.assertTrue(_is_in(text, gt_pos))

    def test_operator_outside_char_literal_unaffected(self):
        text = "if (c == '>') return a < b;"
        # `<` between `a` and `b` should NOT be in literal.
        outside_lt = text.rfind("<")
        self.assertFalse(_is_in(text, outside_lt))

    def test_escaped_quote_in_char_literal(self):
        # `'\''` — escaped single quote inside char literal.
        text = "if (c == '\\'') puts(\"x != y\");"
        # The `!=` inside the puts(...) string should be skipped.
        ne_pos = text.index("!=")
        self.assertTrue(_is_in(text, ne_pos))


class RawStringTests(unittest.TestCase):
    """New behavior: skip operators inside `R"(...)"` raw strings."""

    def test_operator_inside_raw_string_is_skipped(self):
        text = 'auto s = R"(x != y && a < b)";'
        self.assertTrue(_is_in(text, col_of(text, "!=")))
        self.assertTrue(_is_in(text, col_of(text, "<")))

    def test_raw_string_with_delimiter(self):
        text = 'auto sql = R"sql(SELECT * WHERE x != 0)sql";'
        self.assertTrue(_is_in(text, col_of(text, "!=")))

    def test_raw_string_backslash_is_literal(self):
        # A backslash inside a raw string must not be treated as an
        # escape — the next `"` would otherwise be ignored.
        text = 'auto s = R"(path\\to\\file != "x")";'
        self.assertTrue(_is_in(text, col_of(text, "!=")))

    def test_position_after_raw_string_is_outside(self):
        text = 'auto s = R"(abc)"; if (x != y) return;'
        self.assertFalse(_is_in(text, col_of(text, "!=")))

    def test_someR_identifier_not_treated_as_raw_string_prefix(self):
        # `someR"abc"` — `someR` is an identifier ending in R, the
        # `"abc"` that follows is a regular string. The `!=` later
        # should resolve to its actual context.
        text = 'auto s = someR"abc"; if (x != y) return;'
        # The `"abc"` here is a regular string after `someR`. Inside
        # it has no `!=` to check. The later `!=` is outside any
        # literal.
        self.assertFalse(_is_in(text, col_of(text, "!=")))

    def test_uppercase_R_at_word_boundary_starts_raw_string(self):
        # Plain `R"..."` at start-of-token IS a raw string.
        text = 'auto s = R"(a != b)"; x = 1;'
        self.assertTrue(_is_in(text, col_of(text, "!=")))

    def test_raw_string_closer_position_treated_as_in_literal(self):
        # Mirrors the existing regular-string behavior where col-at-
        # closing-`"` is reported as in-literal.
        text = 'auto s = R"(abc)";'
        close_paren_pos = text.index(")")
        self.assertTrue(_is_in(text, close_paren_pos))


class MixedTests(unittest.TestCase):
    """Several literal kinds on the same line."""

    def test_char_then_string_then_operator(self):
        text = "if (c == '!' && puts(\"a != b\"), x > 0) return;"
        # `>` between `x` and `0` is outside both literals.
        gt_pos = text.rfind(">")
        self.assertFalse(_is_in(text, gt_pos))
        # `!=` inside the puts literal is inside.
        self.assertTrue(_is_in(text, col_of(text, "!=")))

    def test_raw_string_then_operator(self):
        text = 'auto s = R"json({"k":"v"})json"; if (n != 0) f();'
        # The `!=` after the raw string is outside.
        self.assertFalse(_is_in(text, col_of(text, "!=")))


if __name__ == "__main__":
    unittest.main()

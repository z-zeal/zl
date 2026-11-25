# LeetCode-style algorithm solutions in ZL

ZL does not currently provide a LeetCode submission adapter or parse the
platform's serialized test input. The regression fixture
`tests/zl/valid/language_hardening_tests/LeetCodeStyleHarness.zl` shows the
closest equivalent: a `Solution` class with public typed methods plus a separate
harness that constructs inputs and checks expected results.

The fixture exercises eight familiar patterns:

| Problem pattern | ZL features exercised |
| --- | --- |
| Two Sum | `List<int>`, `Map<int,int>`, indexed numeric-key lookup |
| Binary Search | loops, integer arithmetic, list access |
| Maximum Subarray | iterative dynamic programming |
| Valid Parentheses | string slicing and a typed stack (`List<string>`) |
| Merge Two Sorted Arrays | two pointers and list construction |
| Climbing Stairs | constant-space dynamic programming |
| Kth Largest Element | bounded min-heap |
| Last Stone Weight | max-heap and repeated extraction |

Run both compiler paths from the repository root after building `zl_language`:

```sh
build/zl_language tests/zl/valid/language_hardening_tests/LeetCodeStyleHarness.zl
ZL_COMPILER=ast build/zl_language tests/zl/valid/language_hardening_tests/LeetCodeStyleHarness.zl
```

This is an algorithm-language smoke test, not proof of full LeetCode
compatibility. A real submission adapter would still need to translate input
and output formats, and problems that depend on judge-provided linked-list or
tree node types need equivalent ZL types. ZL `int` is signed 64-bit, so
constraints outside that range need a different representation.

# Task 14 docs fix report

## Fix verification

Command:

```bash
python3 - <<'PY'
from pathlib import Path
p = Path('docs/superpowers/plans/2026-09-17-stage-authoring-results.md')
s = p.read_text()
assert '| Baseline failures | Baseline errors | Final failures | Final errors |' in s
assert 'preserves `tests`/`failures`/`errors` for both runs' in s
section = s[s.index('| Suite | Baseline tests |'):s.index('\n\n**Totals:**')]
rows = [line for line in section.splitlines() if line.startswith('| `')]
values = [[int(x.strip()) for x in line.split('|')[2:-1]] for line in rows]
assert len(rows) == 96
assert [sum(row[i] for row in values) for i in range(6)] == [513, 508, 1, 1, 2, 1]
assert next(row for line, row in zip(rows, values) if 'test_voxel_settings' in line)[3] == 1
assert all(row[3] == 0 for line, row in zip(rows, values) if 'test_voxel_settings' not in line)
print('gdUnit comparison validation: PASS (96 suites; baseline 513/1/1; final 508/2/1)')
PY
git diff --check
printf 'git diff --check: 0\n'
```

Output:

```text
gdUnit comparison validation: PASS (96 suites; baseline 513/1/1; final 508/2/1)
git diff --check: 0
```

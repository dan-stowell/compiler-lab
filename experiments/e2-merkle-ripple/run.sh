#!/bin/sh
# Reproduce E2 from scratch: fetch chibicc, compute per-commit ripple, summarize.
set -eu
cd "$(dirname "$0")"
sh ../common/get-chibicc.sh chibicc
python3 ripple.py chibicc 90d1f7f ripple.csv
python3 ripple.py chibicc 90d1f7f ripple-with-tests.csv --include-tests
python3 ripple.py chibicc 90d1f7f ripple-any-ident.csv --any-ident  # old call rule, footnote only
for r in 90d1f7f $(git -C chibicc rev-list --first-parent --reverse 90d1f7f | sed -n '100p;200p'); do
  python3 spotcheck.py chibicc "$r"
done | tee spotcheck.txt
python3 summarize.py ripple.csv ripple-with-tests.csv ripple-any-ident.csv | tee summary.txt

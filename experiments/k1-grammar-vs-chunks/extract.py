#!/usr/bin/env python3
"""Write every committed version of lvm.c (oldest first, following renames) from a
lua/lua git clone into a versions file: u32 count, then {u32 len, bytes} per version."""
import struct, subprocess, sys
repo, out = sys.argv[1], sys.argv[2]
log = subprocess.run(["git", "-C", repo, "log", "--follow", "--reverse", "--name-only",
                      "--format=@%H", "--", "lvm.c"], capture_output=True, text=True, check=True).stdout
pairs, h = [], None
for line in log.splitlines():
    if line.startswith("@"): h = line[1:]
    elif line.strip() and h: pairs.append((h, line.strip())); h = None
blobs = []
for h, p in pairs:
    r = subprocess.run(["git", "-C", repo, "show", f"{h}:{p}"], capture_output=True)
    if r.returncode == 0: blobs.append(r.stdout)
with open(out, "wb") as f:
    f.write(struct.pack("<I", len(blobs)))
    for b in blobs: f.write(struct.pack("<I", len(b))); f.write(b)
print(f"{len(blobs)} versions of lvm.c ({len(pairs)} commits), {sum(map(len, blobs))} bytes", file=sys.stderr)

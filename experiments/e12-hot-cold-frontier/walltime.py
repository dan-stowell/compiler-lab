"""Native wall time of a command: 1 warm-up run, then N timed runs; prints median and all runs.
usage: python3 walltime.py N CWD CMD..."""
import os, statistics, subprocess, sys, time

n, cwd, cmd = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
subprocess.run(cmd, cwd=cwd, check=True)
ts, us = [], []
for _ in range(n):
    r0 = os.times()
    t0 = time.perf_counter()
    subprocess.run(cmd, cwd=cwd, check=True)
    ts.append(time.perf_counter() - t0)
    r1 = os.times()
    us.append((r1.children_user - r0.children_user) + (r1.children_system - r0.children_system))
print(f"native_wall_median_s={statistics.median(ts):.6f} user+sys_median_s={statistics.median(us):.4f} "
      f"runs_s={','.join(f'{t:.4f}' for t in ts)}")

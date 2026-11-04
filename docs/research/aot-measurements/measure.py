import resource, subprocess, sys, time, os
# usage: measure.py <cflags...> -- <files...> ; compiles each serially, prints seconds and peak RSS
i = sys.argv.index('--'); flags = sys.argv[1:i]; files = sys.argv[i+1:]
tot = 0.0; peak = 0
for f in files:
    t = time.time()
    p = subprocess.Popen(['clang'] + flags + ['-c', '-o', f + '.o', f])
    _, status, ru = os.wait4(p.pid, 0)
    dt = time.time() - t; tot += dt; peak = max(peak, ru.ru_maxrss)
    print(f"{os.path.basename(f)}: {os.path.getsize(f)/1e6:.1f} MB C, {dt:.1f} s, {ru.ru_maxrss/1024:.0f} MB peak, rc {status}", flush=True)
print(f"total {tot:.1f} s, max peak {peak/1024:.0f} MB")

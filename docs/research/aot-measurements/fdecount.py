"""Counts functions listed in .eh_frame_hdr (the binary search table of FDEs) and their total size."""
import struct, sys, subprocess
def sections(path):
    out = subprocess.run(["readelf", "-SW", path], capture_output=True, text=True).stdout
    s = {}
    for l in out.splitlines():
        p = l.replace("[ ", "[").split()
        if len(p) > 6 and p[0].startswith("[") and p[1].startswith("."):
            s[p[1]] = (int(p[3], 16), int(p[4], 16), int(p[5], 16))
    return s
for path in sys.argv[1:]:
    s = sections(path)
    if ".eh_frame_hdr" not in s:
        print(path, "no .eh_frame_hdr"); continue
    addr, off, size = s[".eh_frame_hdr"]
    data = open(path, "rb").read()
    h = data[off:off + size]
    ver, ptr_enc, cnt_enc, tab_enc = h[0], h[1], h[2], h[3]
    # common encodings: ptr_enc 0x1b (pcrel sdata4), cnt_enc 0x03 (udata4), tab_enc 0x3b (datarel sdata4)
    pos = 4 + 4
    count = struct.unpack_from("<I", h, pos)[0]; pos += 4
    starts = [addr + struct.unpack_from("<i", h, pos + 8 * i)[0] for i in range(count)] if tab_enc == 0x3b else []
    starts.sort()
    print("%s fdes=%d enc=%#x/%#x/%#x first=%#x" % (path, count, ptr_enc, cnt_enc, tab_enc, starts[0] if starts else 0))

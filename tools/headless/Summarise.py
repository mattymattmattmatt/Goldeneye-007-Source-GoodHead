"""One line per test run: memory at trace time, eye size, clipping, frame timing,
and whether both eye images are fully drawn.

    python summarise.py m1-backbuffer m2-eyert-2688-sep ...
"""
import os, re, sys
import numpy as np
from PIL import Image

here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results")

def grab(pat, text, group=0, last=False):
    m = list(re.finditer(pat, text))
    if not m:
        return None
    return (m[-1] if last else m[0]).group(group)

for name in sys.argv[1:]:
    d = os.path.join(here, name)
    log = open(os.path.join(d, "log.txt"), encoding="utf-8", errors="replace").read()
    mem = grab(r"EYEDIAG memory now (\d+) of \d+ MB \(peak (\d+)\), largest free block (\d+) MB", log, 0)
    m = re.search(r"now (\d+) of \d+ MB \(peak (\d+)\), largest free block (\d+)", mem or "")
    eye = grab(r"Eye RT size (\d+x\d+)", log, 1) or "backbuffer"
    ends = re.findall(r"END pass: (\d+) draws, (\d+) of them clipped", log)
    clipped = sum(int(b) for a, b in ends[:4])
    phases = re.findall(r"PHASE eng ([\d.]+)/[\d.]+\s+upd ([\d.]+)/[\d.]+\s+pres [\d.]+/[\d.]+\s+wait ([\d.]+)/", log)
    ph = phases[-1] if phases else None
    fills = []
    for side in "LR":
        p = os.path.join(d, f"gesvr_eye_{side}.bmp")
        if not os.path.exists(p):
            fills.append("--")
            continue
        a = np.asarray(Image.open(p)).astype(int).sum(2)
        rows = np.where(a.max(1) > 12)[0]
        cols = np.where(a.max(0) > 12)[0]
        fh = (rows.max() + 1) / a.shape[0] if len(rows) else 0
        fw = (cols.max() + 1) / a.shape[1] if len(cols) else 0
        fills.append(f"{fw*100:.0f}x{fh*100:.0f}%")
    print(f"{name:24} eye {eye:>10}  mem {m.group(1) if m else '?':>4} MB peak {m.group(2) if m else '?':>4} hole {m.group(3) if m else '?':>4}"
          f"  clipped {clipped:3}  filled L {fills[0]:>8} R {fills[1]:>8}"
          + (f"  eng {ph[0]} upd {ph[1]} wait {ph[2]} ms" if ph else "  (no PHASE line)"))

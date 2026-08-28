import struct
from pathlib import Path


def load(path):
    d = Path(path).read_bytes()
    e = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, e + 6)[0]
    optsz = struct.unpack_from("<H", d, e + 20)[0]
    imagebase = struct.unpack_from("<I", d, e + 24 + 28)[0]
    sect = e + 24 + optsz
    secs = []
    for _ in range(nsec):
        name = d[sect : sect + 8].split(b"\x00")[0].decode("latin1")
        vsize, va, rawsz, raw = struct.unpack_from("<IIII", d, sect + 8)
        secs.append((name, va, vsize, raw, rawsz))
        sect += 40
    return d, secs, imagebase


def va_to_off(secs, va, ib):
    rva = va - ib if va >= ib else va
    for name, sva, vsize, raw, rawsz in secs:
        if sva <= rva < sva + max(vsize, rawsz):
            return raw + (rva - sva), name, rva
    return None, None, rva


def off_to_rva(secs, off):
    for name, sva, vsize, raw, rawsz in secs:
        if raw <= off < raw + rawsz:
            return sva + (off - raw)
    return None


def find_vtables(dll, type_name, nslots=16):
    d, secs, ib = load(dll)
    needle = type_name.encode("ascii")
    name_off = d.find(needle)
    print("file", dll)
    print("imagebase", hex(ib), "name_off", hex(name_off) if name_off >= 0 else None)
    if name_off < 0:
        return
    td_off = name_off - 8
    td_rva = off_to_rva(secs, td_off)
    td_va = ib + td_rva
    print("TypeDescriptor RVA", hex(td_rva), "VA", hex(td_va))
    pat = struct.pack("<I", td_va)
    cols = []
    start = 0
    while True:
        i = d.find(pat, start)
        if i < 0:
            break
        cols.append(i)
        start = i + 1
    print("refs", len(cols))
    seen = set()
    for col_off in cols:
        col_start = col_off - 12
        if col_start < 0:
            continue
        sig, off, cdoff, ptd = struct.unpack_from("<IIII", d, col_start)
        col_rva = off_to_rva(secs, col_start)
        if col_rva is None:
            continue
        col_va = ib + col_rva
        pcol = struct.pack("<I", col_va)
        j = 0
        while True:
            k = d.find(pcol, j)
            if k < 0:
                break
            vt_off = k + 4
            vt_rva = off_to_rva(secs, vt_off)
            j = k + 1
            if vt_rva is None or vt_rva in seen:
                continue
            seen.add(vt_rva)
            print("vtable RVA", hex(vt_rva), "COL offset field", off)
            for n in range(nslots):
                slot = struct.unpack_from("<I", d, vt_off + n * 4)[0]
                o, sn, r = va_to_off(secs, slot, ib)
                pro = d[o : o + 20] if o else b""
                hexs = " ".join(f"{b:02X}" for b in pro)
                # find ret
                ret = ""
                if o:
                    for t in range(o + 8, min(o + 0x800, len(d) - 3)):
                        if d[t : t + 3] == bytes([0x8B, 0xE5, 0x5D]) and d[t + 3] in (0xC2, 0xC3):
                            if d[t + 3] == 0xC3:
                                ret = "ret0"
                            else:
                                ret = f"ret{d[t+4]}"
                            break
                        if d[t] == 0xC2 and d[t - 1] in (0x5D, 0x5B, 0x5E, 0x5F):
                            ret = f"ret{d[t+1]}"
                            break
                        if d[t] == 0xC3 and d[t - 1] == 0x5D:
                            ret = "ret0"
                            break
                print(f"  [{n:2d}] rva {slot-ib:#08x} {ret:6s} {hexs}")


if __name__ == "__main__":
    client = r"G:\gesource\bin\client.dll"
    mat = r"G:\SteamLibrary\steamapps\common\Source SDK Base 2007\bin\materialsystem.dll"
    find_vtables(client, ".?AVCViewRender@@", 14)
    print()
    find_vtables(mat, ".?AVCMaterialSystem@@", 12)

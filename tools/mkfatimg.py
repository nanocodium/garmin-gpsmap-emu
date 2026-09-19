#!/usr/bin/env python3
"""Build a FAT32 "superfloppy" image from a directory tree, without mtools.

    python3 tools/mkfatimg.py out.img --size 256M --label GARMINSD \
            --add pkg=Garmin/resources

`--add HOSTPATH[=DESTDIR]` copies a file, or the *contents* of a directory
(like `mcopy -s dir/*`), to DESTDIR inside the image.  The image has no
partition table: the whole device is one FAT32 volume, which is what the
firmware expects from an SD card (docs/sd_card.md).

Written because MSYS2 has no mtools and formatting a loop mount needs root;
the output is a plain FAT32 volume, so `7z l`, mtools or any OS reads it back.
"""
import argparse
import os
import pathlib
import struct
import sys
import time

SECTOR = 512
RESERVED_SECTORS = 32
NUM_FATS = 2
ATTR_DIR = 0x10
ATTR_VOLUME = 0x08
ATTR_LFN = 0x0F
ENTRY = 32
BAD_83 = set('"*+,./:;<=>?[\\]|')
#: A FAT volume's type follows from its cluster count, so a "FAT32" volume
#: with fewer clusters than this is read as FAT16 and nothing on it is found.
FAT32_MIN_CLUSTERS = 65525
#: Upper bound on the cluster count, if a reader ever needs one.  Unlimited by
#: default: a card with 318534 clusters reads fine on this firmware, so the
#: 16-bit cluster theory that this once encoded was simply wrong.
MAX_CLUSTERS = 0


def parse_size(s):
    s = str(s).strip().upper()
    mult = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}
    if s and s[-1] in mult:
        return int(float(s[:-1]) * mult[s[-1]])
    return int(s, 0)


def fat_time(ts):
    t = time.localtime(ts)
    year = max(1980, min(2107, t.tm_year))
    date = ((year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
    tm = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    return tm, date


def lfn_checksum(short11):
    s = 0
    for c in short11:
        s = ((((s & 1) << 7) | (s >> 1)) + c) & 0xFF
    return s


class Node:
    def __init__(self, name):
        self.name = name
        self.cluster = 0


class File(Node):
    def __init__(self, name, src=None, data=None):
        super().__init__(name)
        self.src = src
        self.data = data
        self.size = len(data) if data is not None else os.path.getsize(src)
        self.mtime = os.path.getmtime(src) if src else time.time()

    def read(self, chunk=1 << 20):
        if self.data is not None:
            yield self.data
            return
        with open(self.src, "rb") as f:
            while True:
                b = f.read(chunk)
                if not b:
                    return
                yield b


class Dir(Node):
    def __init__(self, name, parent=None):
        super().__init__(name)
        self.parent = parent
        self.children = {}          # upper-case name -> Node
        self.order = []
        self.mtime = time.time()

    def subdir(self, name):
        key = name.upper()
        node = self.children.get(key)
        if node is None:
            node = Dir(name, self)
            self.children[key] = node
            self.order.append(node)
        elif not isinstance(node, Dir):
            raise SystemExit("%s already exists as a file" % name)
        return node

    def add(self, node):
        key = node.name.upper()
        if key in self.children:
            raise SystemExit("duplicate name %s" % node.name)
        self.children[key] = node
        self.order.append(node)
        return node

    def walk(self):
        yield self
        for n in self.order:
            if isinstance(n, Dir):
                yield from n.walk()


def short_name(name, taken):
    """8.3 name for `name`; returns (11 raw bytes, lfn_needed)."""
    base, dot, ext = name.rpartition(".")
    if not dot:
        base, ext = name, ""

    def clean(s):
        return "".join("_" if (c in BAD_83 or ord(c) < 0x20 or ord(c) > 0x7E
                               or c == " ") else c for c in s).upper()

    cb, ce = clean(base).replace(".", "_"), clean(ext)[:3]
    lossy = (cb != base or ce != ext or len(base) > 8 or len(ext) > 3)
    if not cb:
        cb = "_"
    stem = cb[:8]
    if lossy or (stem + "." + ce) in taken:
        n = 1
        while True:
            tail = "~%d" % n
            stem = cb[:8 - len(tail)] + tail
            if (stem + "." + ce) not in taken:
                break
            n += 1
    taken.add(stem + "." + ce)
    return (stem.ljust(8)[:8] + ce.ljust(3)[:3]).encode("ascii", "replace"), lossy


def dir_entries(d, label=None):
    """The 32-byte entries of one directory, first-cluster fields still blank."""
    out = []                       # list of [bytearray, node_or_marker]
    taken = set()

    def entry(raw11, attr, size, mtime, node=None):
        tm, date = fat_time(mtime)
        e = bytearray(ENTRY)
        e[0:11] = raw11
        e[11] = attr
        struct.pack_into("<HH", e, 14, tm, date)       # create time/date
        struct.pack_into("<H", e, 18, date)            # access date
        struct.pack_into("<HH", e, 22, tm, date)       # write time/date
        struct.pack_into("<I", e, 28, size)
        out.append((e, node))

    if label is not None:
        entry(label.upper().ljust(11)[:11].encode("ascii", "replace"),
              ATTR_VOLUME, 0, time.time())
    if d.parent is not None:
        entry(b".          ", ATTR_DIR, 0, d.mtime, ("self", d))
        entry(b"..         ", ATTR_DIR, 0, d.mtime, ("parent", d))
    for node in d.order:
        raw11, lossy = short_name(node.name, taken)
        if lossy:
            chk = lfn_checksum(raw11)
            # 13 UTF-16 characters per entry.  The NUL terminator is only
            # written when the name leaves room for it: a name that fills its
            # last entry exactly needs no terminator, and appending one adds a
            # whole extra entry.  Lenient readers cope; the GPSMAP firmware
            # counts ceil(len/13) entries and cannot find such a file, which
            # silently lost every bitmap whose name length is a multiple of 13
            # (206 of the 2939 in the resource package: the page header, the
            # top-bar slices, several home-screen icons).
            u = node.name.encode("utf-16-le")
            if len(u) % 26:
                u += b"\x00\x00"
                u += b"\xff" * ((-len(u)) % 26)
            parts = [u[i:i + 26] for i in range(0, len(u), 26)]
            for i in range(len(parts), 0, -1):         # stored last-first
                p = parts[i - 1]
                e = bytearray(ENTRY)
                e[0] = i | (0x40 if i == len(parts) else 0)
                e[1:11] = p[0:10]
                e[11] = ATTR_LFN
                e[13] = chk
                e[14:26] = p[10:22]
                e[28:32] = p[22:26]
                out.append((e, None))
        isdir = isinstance(node, Dir)
        entry(raw11, ATTR_DIR if isdir else 0, 0 if isdir else node.size,
              node.mtime, node)
    return out


class Fat32:
    def __init__(self, path, size, cluster_bytes=None, label="NO NAME",
                 max_clusters=MAX_CLUSTERS):
        self.path = path
        self.size = size
        self.total_sectors = size // SECTOR
        self.label = label
        self.spc = (cluster_bytes // SECTOR) if cluster_bytes else None
        if self.spc is None:
            # The cluster count must stay at or above FAT32_MIN_CLUSTERS:
            # a reader works out the FAT width from the cluster count, so a
            # volume with fewer clusters is read as FAT16 and nothing on it
            # can be found.  Among the sizes that satisfy that, take the
            # largest, which keeps the FAT itself small.
            for cb in (32768, 16384, 8192, 4096, 2048, 1024, 512):
                n = self._clusters(cb // SECTOR)
                if n >= FAT32_MIN_CLUSTERS and (not max_clusters or
                                                n <= max_clusters):
                    self.spc = cb // SECTOR
                    break
            else:
                # Nothing satisfies both bounds; recognising the volume as
                # FAT32 at all matters more than the 16-bit preference, so
                # take the smallest cluster that reaches the minimum.
                for cb in (512, 1024, 2048, 4096, 8192, 16384, 32768):
                    if self._clusters(cb // SECTOR) >= FAT32_MIN_CLUSTERS:
                        self.spc = cb // SECTOR
                        break
                else:
                    raise SystemExit(
                        "%d bytes is too small for FAT32: it cannot hold the "
                        "%d clusters a reader needs to recognise one"
                        % (size, FAT32_MIN_CLUSTERS))
        self.fat_sectors = self._fat_sectors(self.spc)
        self.clusters = self._clusters(self.spc)
        if self.clusters < FAT32_MIN_CLUSTERS:
            print("warning: %d clusters is below the FAT32 minimum (%d); a "
                  "reader will take this volume for FAT16 and find nothing"
                  % (self.clusters, FAT32_MIN_CLUSTERS), file=sys.stderr)
        elif max_clusters and self.clusters > max_clusters:
            print("warning: %d clusters exceeds --max-clusters %d"
                  % (self.clusters, max_clusters), file=sys.stderr)
        self.data_start = RESERVED_SECTORS + NUM_FATS * self.fat_sectors
        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF
        self.next_free = 2

    def _fat_sectors(self, spc):
        """FAT size in sectors: n clusters need ceil((n+2)*4/512) each."""
        n = (self.total_sectors - RESERVED_SECTORS) // spc
        while n > 0:
            fs = -(-(n + 2) * 4 // SECTOR)
            if RESERVED_SECTORS + NUM_FATS * fs + n * spc <= self.total_sectors:
                return fs
            n -= 1
        raise SystemExit("%d bytes is too small for a FAT32 volume" % self.size)

    def _clusters(self, spc):
        fs = self._fat_sectors(spc)
        return (self.total_sectors - RESERVED_SECTORS - NUM_FATS * fs) // spc

    @property
    def cluster_bytes(self):
        return self.spc * SECTOR

    def alloc(self, n):
        """Allocate a chain of n clusters; returns the first one."""
        if self.next_free + n > self.clusters + 2:
            need = (self.next_free + n) * self.cluster_bytes
            raise SystemExit("image too small: the tree needs about %.0f MiB, "
                             "pass a bigger --size" % (need / (1 << 20)))
        first = self.next_free
        for i in range(n):
            c = first + i
            self.fat[c] = 0x0FFFFFFF if i == n - 1 else c + 1
        self.next_free += n
        return first

    def cluster_offset(self, c):
        return (self.data_start + (c - 2) * self.spc) * SECTOR

    def boot_sector(self):
        b = bytearray(SECTOR)
        b[0:3] = b"\xeb\x58\x90"
        b[3:11] = b"MSWIN4.1"
        struct.pack_into("<HBHBHHBHHHII", b, 11,
                         SECTOR, self.spc, RESERVED_SECTORS, NUM_FATS,
                         0,          # root entries (0 on FAT32)
                         0,          # total sectors 16
                         0xF8,       # media
                         0,          # FAT size 16
                         63, 255, 0, self.total_sectors)
        struct.pack_into("<IHHIHH", b, 36,
                         self.fat_sectors, 0, 0, 2, 1, 6)
        b[64] = 0x80
        b[66] = 0x29
        struct.pack_into("<I", b, 67, 0x47415231)      # volume id
        b[71:82] = self.label.upper().ljust(11)[:11].encode("ascii", "replace")
        b[82:90] = b"FAT32   "
        b[510:512] = b"\x55\xaa"
        return bytes(b)

    def fsinfo_sector(self):
        b = bytearray(SECTOR)
        struct.pack_into("<I", b, 0, 0x41615252)
        struct.pack_into("<I", b, 484, 0x61417272)
        struct.pack_into("<II", b, 488,
                         self.clusters + 2 - self.next_free, self.next_free)
        struct.pack_into("<I", b, 508, 0xAA550000)
        return bytes(b)

    def write(self, root):
        """Lay the tree out and write the image."""
        # 1. directory contents (their size does not depend on cluster numbers)
        entries = {id(d): dir_entries(d, self.label if d is root else None)
                   for d in root.walk()}

        # 2. clusters, root first: on FAT32 the root is an ordinary chain and
        #    the BPB above says it starts at cluster 2.
        for d in root.walk():
            n = max(1, -(-len(entries[id(d)]) * ENTRY // self.cluster_bytes))
            d.cluster = self.alloc(n)
            for node in d.order:
                if isinstance(node, File):
                    node.cluster = (self.alloc(-(-node.size // self.cluster_bytes))
                                    if node.size else 0)

        # 3. fill in the first-cluster fields now that they are known
        for d in root.walk():
            for e, node in entries[id(d)]:
                if node is None:
                    continue
                if isinstance(node, tuple):            # "." / ".."
                    c = (node[1].cluster if node[0] == "self"
                         else (0 if node[1].parent is root
                               else node[1].parent.cluster))
                else:
                    c = node.cluster
                struct.pack_into("<H", e, 20, c >> 16)
                struct.pack_into("<H", e, 26, c & 0xFFFF)

        with open(self.path, "wb") as f:
            f.truncate(self.size)
            for d in root.walk():
                blob = b"".join(bytes(e) for e, _ in entries[id(d)])
                nclu = max(1, -(-len(blob) // self.cluster_bytes))
                blob += b"\x00" * (nclu * self.cluster_bytes - len(blob))
                f.seek(self.cluster_offset(d.cluster))
                f.write(blob)
                for node in d.order:
                    if not isinstance(node, File) or not node.size:
                        continue
                    f.seek(self.cluster_offset(node.cluster))
                    for chunk in node.read():
                        f.write(chunk)
            fat = bytearray()
            for v in self.fat:
                fat += struct.pack("<I", v)
            fat += b"\x00" * (self.fat_sectors * SECTOR - len(fat))
            boot, fsinfo = self.boot_sector(), self.fsinfo_sector()
            for base in (0, 6 * SECTOR):               # boot + backup boot
                f.seek(base)
                f.write(boot)
                f.write(fsinfo)
            for i in range(NUM_FATS):
                f.seek((RESERVED_SECTORS + i * self.fat_sectors) * SECTOR)
                f.write(fat)
            f.truncate(self.size)


def add_tree_into(d, host):
    files = total = 0
    for entry in sorted(host.iterdir(), key=lambda p: p.name.lower()):
        if entry.is_dir():
            n, sz = add_tree_into(d.subdir(entry.name), entry)
            files += n
            total += sz
        else:
            d.add(File(entry.name, src=str(entry)))
            files += 1
            total += entry.stat().st_size
    return files, total


def add_path(root, host, dest):
    """Add a file, or the contents of a directory, at `dest` in the image."""
    d = root
    for part in pathlib.PurePosixPath(dest).parts:
        if part not in ("/", "."):
            d = d.subdir(part)
    host = pathlib.Path(host)
    if not host.exists():
        raise SystemExit("no such file or directory: %s" % host)
    if host.is_file():
        d.add(File(host.name, src=str(host)))
        return 1, host.stat().st_size
    return add_tree_into(d, host)


def verify(path, root):
    """Re-read the image the way a strict FAT driver would.

    Deliberately pedantic where lenient readers are not: every long name must
    occupy exactly ceil(len / 13) entries, because the GPSMAP firmware counts
    them and silently fails to find a file with one too many.  7-Zip, Linux
    and Windows all accept the extra entry, so they cannot catch that.
    """
    with open(path, "rb") as f:
        boot = f.read(512)
        bps, spc, reserved, nfats = struct.unpack_from("<HBHB", boot, 11)
        fatsz = struct.unpack_from("<I", boot, 36)[0]
        data_start = reserved + nfats * fatsz

        def fat(n):
            f.seek(reserved * bps + n * 4)
            return struct.unpack("<I", f.read(4))[0] & 0x0FFFFFFF

        def entries(cluster):
            while cluster and cluster < 0x0FFFFFF8:
                f.seek((data_start + (cluster - 2) * spc) * bps)
                blob = f.read(spc * bps)
                for i in range(0, len(blob), ENTRY):
                    e = blob[i:i + ENTRY]
                    if not e or e[0] == 0:
                        return
                    yield e
                cluster = fat(cluster)

        problems = []
        checked = 0

        def walk(d, cluster, prefix):
            nonlocal checked
            seen, lfn = {}, []
            for e in entries(cluster):
                if e[11] == ATTR_LFN:
                    lfn.append(e)
                    continue
                if e[11] & ATTR_VOLUME and not e[11] & ATTR_DIR:
                    lfn = []
                    continue
                if e[0:1] in (b".", b"\xe5"):
                    lfn = []
                    continue
                name = ""
                if lfn:
                    parts = sorted(lfn, key=lambda x: x[0] & 0x3F)
                    raw = b"".join(p[1:11] + p[14:26] + p[28:32] for p in parts)
                    name = raw.decode("utf-16-le", "replace")
                    name = name.split("\x00")[0].replace("￿", "")
                    want = -(-len(name) // 13)
                    if len(parts) != want:
                        problems.append(
                            "%s%s: %d long-name entries, a strict reader "
                            "expects %d" % (prefix, name, len(parts), want))
                seen[name or e[0:11].decode("ascii", "replace")] = (e, name)
                checked += 1
                lfn = []
            for node in d.order:
                key = node.name
                if key not in seen:
                    problems.append("%s%s: not found by long name"
                                    % (prefix, key))
                    continue
                e, _ = seen[key]
                first = (struct.unpack_from("<H", e, 20)[0] << 16 |
                         struct.unpack_from("<H", e, 26)[0])
                if isinstance(node, Dir):
                    walk(node, first, prefix + key + "/")
                else:
                    size = struct.unpack_from("<I", e, 28)[0]
                    if size != node.size:
                        problems.append("%s%s: size %d on card, %d on disk"
                                        % (prefix, key, size, node.size))

        walk(root, struct.unpack_from("<I", boot, 44)[0], "")
    return checked, problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out")
    ap.add_argument("--size", default="256M",
                    help="volume size, e.g. 256M (QEMU wants a power of two "
                         "for SD cards); default 256M")
    ap.add_argument("--label", default="NO NAME")
    ap.add_argument("--cluster-bytes", type=parse_size, default=None,
                    help="force the cluster size instead of picking the "
                         "smallest one that stays within --max-clusters")
    ap.add_argument("--max-clusters", type=int, default=MAX_CLUSTERS,
                    help="cap the cluster count (0 = no cap, the default)")
    ap.add_argument("--add", action="append", default=[], metavar="HOST[=DEST]",
                    help="file, or directory contents, to place at DEST")
    ap.add_argument("--no-verify", action="store_true",
                    help="skip the strict read-back check")
    a = ap.parse_args()

    root = Dir("", None)
    files = total = 0
    for spec in a.add:
        host, _, dest = spec.partition("=")
        n, sz = add_path(root, host, dest)
        files += n
        total += sz
    img = Fat32(a.out, parse_size(a.size), a.cluster_bytes, a.label,
                a.max_clusters)
    img.write(root)
    if not a.no_verify:
        checked, problems = verify(a.out, root)
        if problems:
            for p in problems[:20]:
                print("  %s" % p, file=sys.stderr)
            sys.exit("%s: %d of %d entries would not be read back correctly"
                     % (a.out, len(problems), checked))
    print("%s: FAT32 %.0f MiB, %d-byte clusters (%d clusters), %d files, "
          "%.1f MiB of data" % (a.out, img.size / (1 << 20), img.cluster_bytes,
                                img.clusters, files, total / (1 << 20)))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Make and check PS2 memory card images.

  python scripts/mcformat.py card.ps2            make a formatted, empty 8 MB card (refuses to overwrite a file)
  python scripts/mcformat.py card.ps2 --force    ... and replace an existing file (it deletes what is on that card)
  python scripts/mcformat.py check card.ps2      verify the error-check bytes (ECC) of every page, and the card's layout

The file is the usual ".ps2" layout that consoles' tools, PCSX2 and this runner (`ps2run --memcard card.ps2`) all use: 16384
pages of 528 bytes, which is 512 bytes of data followed by 16 "spare" bytes (3 ECC bytes for each 128 bytes of data, then 4
bytes of 0), 8650752 bytes in all. A game formats a blank card itself when it is asked to save, so a formatted card is only
needed to start without that step, or to test how a game reacts to a card that is formatted but has no saves.

Where each rule comes from (public documentation and permissive sources only; no GPL code was read or used):
  [mymc]   mymc by Ross Ridge, public domain: ps2mc.py (superblock, indirect FAT, FAT, format), ps2mc_dir.py (directory
           entries, date format), ps2mc_ecc.py (the ECC). https://github.com/ps2dev/mymc
  [mcman]  mcman, the memory card module of ps2sdk (Academic Free License 2.0): iop/memorycard/mcman/src/main.c (McReadPage,
           McDataChecksum, mcman_correctdata, mcman_setdevinfos, mcman_checkBackupBlocks, McCreateDirentry),
           ps2mc_fio.c (mcman_format2), mcman-internal.h (MCDevInfo, flags). https://github.com/ps2dev/ps2sdk
Both sources agree on every number below. The card 8 MB layout (superblock at page 0, indirect FAT in cluster 8, FAT in clusters
9 to 40, root directory at cluster 41) is the one a console writes: [mymc] format() derives it from the page and block sizes.

Options for tests: --break-ecc PAGE spoils the first ECC byte of that page (repeat it for more pages), --date "YYYY-MM-DD
HH:MM:SS" sets the clock written into the directory entries (the console clock is Japan time, [mymc] time_to_tod), so two runs
give the same file.
"""

import argparse
import os
import struct
import sys
import time

# ---------------------------------------------------------------------------------------------------------------------
# Card geometry. [mymc] PS2MC_STANDARD_PAGE_SIZE, PS2MC_STANDARD_PAGES_PER_CARD, PS2MC_STANDARD_PAGES_PER_ERASE_BLOCK,
# PS2MC_CLUSTER_SIZE; [mcman] the card answers command 0x26 with the same three numbers (page 512, block 16 pages, 16384 pages).
PAGE_SIZE = 512
SPARE_SIZE = ((PAGE_SIZE + 127) // 128) * 4  # [mymc] _calculate_derived: 4 bytes per 128 bytes; [mcman] mcman_sparesize: page/32
RAW_PAGE = PAGE_SIZE + SPARE_SIZE
PAGES_PER_CARD = 16384
PAGES_PER_BLOCK = 16
CLUSTER_SIZE = 1024
PAGES_PER_CLUSTER = CLUSTER_SIZE // PAGE_SIZE
CLUSTERS_PER_CARD = PAGES_PER_CARD // PAGES_PER_CLUSTER
CLUSTERS_PER_BLOCK = PAGES_PER_BLOCK // PAGES_PER_CLUSTER
BLOCKS_PER_CARD = PAGES_PER_CARD // PAGES_PER_BLOCK
CARD_BYTES = PAGES_PER_CARD * RAW_PAGE

# [mymc] constants at the top of ps2mc.py; [mcman] SUPERBLOCK_MAGIC and SUPERBLOCK_VERSION in main.c
MAGIC = b"Sony PS2 Memory Card Format "
VERSION = b"1.2.0.0"
FAT_ALLOCATED_BIT = 0x80000000
FAT_CHAIN_END = 0xFFFFFFFF            # in use, last cluster of its chain
FAT_FREE = 0x7FFFFFFF                 # [mymc] PS2MC_FAT_CLUSTER_MASK; [mcman] mcman_format2: "marking free cluster" ~0x80000000
MAX_INDIRECT_FAT_CLUSTERS = 32
INDIRECT_FAT_OFFSET = 0x2000
ENTRIES_PER_CLUSTER = CLUSTER_SIZE // 4

# [mymc] ps2mc_dir.py
DIRENT_LENGTH = 512
DF_READ, DF_WRITE, DF_EXECUTE, DF_DIR, DF_0400, DF_HIDDEN, DF_EXISTS = 0x0001, 0x0002, 0x0004, 0x0020, 0x0400, 0x2000, 0x8000
MODE_DOT = DF_READ | DF_WRITE | DF_EXECUTE | DF_DIR | DF_0400 | DF_EXISTS   # 0x8427, [mcman] McCreateDirentry says so too
MODE_DOTDOT = DF_WRITE | DF_EXECUTE | DF_DIR | DF_0400 | DF_HIDDEN | DF_EXISTS  # 0xA426 for the root directory's ".."
SUPERBLOCK_FORMAT = "<28s12sHHHHLLLLLL8x128s128sbbxx"  # [mymc] pack_superblock; 340 bytes, [mcman] MCDevInfo up to cardtype/cardflags
SUPERBLOCK_BYTES = struct.calcsize(SUPERBLOCK_FORMAT)
CARD_TYPE_PS2 = 2      # [mymc] write_superblock; [mcman] "Memory card type. Must be 2"
CARD_FLAGS = 0x2B      # [mymc] write_superblock; the card reports the same byte for command 0x26 ([mcman] McGetCardSpec: flags = p[2])


# ---------------------------------------------------------------------------------------------------------------------
# ECC. [mymc] ps2mc_ecc.py _ecc_calculate and [mcman] McDataChecksum compute the same three bytes for each 128 bytes of data:
#   column parity starts at 0x77 and takes, for every data byte, the parity of the byte's bits under the masks
#   0x55 0x33 0x0F (bits 0 to 2) and 0xAA 0xCC 0xF0 (bits 4 to 6); line parity starts at 0x7F and, for every byte whose bits
#   have odd parity, takes the byte's index (~index for the first line byte, index for the second).
# The loop below is the plain form. The fast form further down gives the same answer, because the column parity of a whole
# chunk depends only on the XOR of all its bytes and the line parity only on which indexes hold odd-parity bytes; the
# self test compares both on random data and against the first entries of [mcman]'s xor table.
_PARITY = [bin(b).count("1") & 1 for b in range(256)]
_COLUMN_MASKS = (0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0)
_COLUMN = [sum(_PARITY[b & m] << i for i, m in enumerate(_COLUMN_MASKS)) for b in range(256)]


def ecc_plain(chunk):
    """The three ECC bytes of 128 bytes of data, byte by byte ([mymc] _ecc_calculate)."""
    column, line0, line1 = 0x77, 0x7F, 0x7F
    for i, b in enumerate(chunk):
        column ^= _COLUMN[b]
        if _PARITY[b]:
            line0 ^= ~i
            line1 ^= i
    return bytes((column, line0 & 0x7F, line1 & 0x7F))


_PARITY_TABLE = bytes(_PARITY)
_popcount = int.bit_count if hasattr(int, "bit_count") else (lambda n: bin(n).count("1"))  # int.bit_count needs Python 3.10
# for each bit j of the byte index: a 128-byte mask with 0x01 where the index has that bit set
_INDEX_MASKS = [int.from_bytes(bytes((i >> j) & 1 for i in range(128)), "little") for j in range(7)]


def ecc_chunk(chunk):
    """Same result as ecc_plain, without a loop over the bytes."""
    folded = int.from_bytes(chunk, "little")
    for shift in (512, 256, 128, 64, 32, 16, 8):
        folded ^= folded >> shift
    folded &= 0xFF  # XOR of all 128 bytes
    odd = int.from_bytes(chunk.translate(_PARITY_TABLE), "little")  # byte i is 1 where data byte i has odd parity
    line1 = 0x7F
    for j in range(7):
        if _popcount(odd & _INDEX_MASKS[j]) & 1:
            line1 ^= 1 << j
    total = _PARITY[folded]  # parity of the number of odd-parity bytes
    line0 = 0x7F
    for j in range(7):
        # ~i has bit j set where i does not, so the byte count with odd parity enters as well
        if (total ^ (_popcount(odd & _INDEX_MASKS[j]) & 1)):
            line0 ^= 1 << j
    return bytes((0x77 ^ _COLUMN[folded], line0, line1))


def ecc_page(data):
    """The 12 ECC bytes of one 512-byte page (4 chunks of 3 bytes) followed by the 4 spare bytes the console leaves 0."""
    return b"".join(ecc_chunk(data[i:i + 128]) for i in range(0, PAGE_SIZE, 128)) + bytes(SPARE_SIZE - 12)


def ecc_verdict(chunk, stored):
    """'ok', 'corrected' (one wrong bit in the data or in the ECC bytes, a card would repair it) or 'bad'.
    [mymc] _ecc_check and [mcman] mcman_correctdata make the same decision."""
    computed = ecc_chunk(chunk)
    if computed == stored:
        return "ok"
    d0, d1, d2 = computed[0] ^ stored[0], computed[1] ^ stored[1], computed[2] ^ stored[2]
    d3 = d1 ^ d2
    d4 = ((d0 & 0x70) >> 4) ^ (d0 & 0x07)
    if d3 == 0x7F and d4 == 0x07:
        return "corrected"
    if bin(d3).count("1") + bin(d4).count("1") == 1 or (not d0 and not d1 and not d2):
        return "corrected"
    return "bad"


# ---------------------------------------------------------------------------------------------------------------------
# Making a card
def tod(when):
    """A date as the 8 bytes of a directory entry: [mymc] ps2mc_dir.py _tod_fmt "<xBBBBBH" = zero, second, minute, hour, day,
    month, year. The console clock runs on Japan time (UTC+9)."""
    return struct.pack("<xBBBBBH", when.tm_sec, when.tm_min, when.tm_hour, when.tm_mday, when.tm_mon, when.tm_year)


def dirent(mode, length, cluster, parent_entry, created, name):
    """One 512-byte directory entry. [mymc] ps2mc_dir.py _dirent_fmt "<HHL8sLL8sL28x448s":
    mode, 0, length (entries for a directory), created, first cluster, entry number in the parent, modified, attribute, name."""
    return struct.pack("<HHL8sLL8sL28x448s", mode, 0, length, created, cluster, parent_entry, created, 0, name)


def layout():
    """The numbers [mymc] format() works out for a 16384-page card with 512-byte pages."""
    first_ifc = -(-INDIRECT_FAT_OFFSET // CLUSTER_SIZE)                        # indirect FAT starts at 0x2000 = cluster 8
    allocatable = CLUSTERS_PER_CARD - (first_ifc + 2)
    fat_clusters = -(-allocatable // ENTRIES_PER_CLUSTER)
    ifc_clusters = min(-(-fat_clusters // ENTRIES_PER_CLUSTER), MAX_INDIRECT_FAT_CLUSTERS)
    if ifc_clusters == MAX_INDIRECT_FAT_CLUSTERS:
        fat_clusters = ifc_clusters * ENTRIES_PER_CLUSTER
    fat_entries = fat_clusters * ENTRIES_PER_CLUSTER
    alloc_offset = first_ifc + ifc_clusters + fat_clusters                     # 41: the first cluster the files may use
    backup1, backup2 = BLOCKS_PER_CARD - 1, BLOCKS_PER_CARD - 2
    alloc_end = backup2 * CLUSTERS_PER_BLOCK - alloc_offset                    # 8135
    return dict(first_ifc=first_ifc, fat_clusters=fat_clusters, ifc_clusters=ifc_clusters, fat_entries=fat_entries,
                alloc_offset=alloc_offset, alloc_end=alloc_end, backup1=backup1, backup2=backup2)


def make_card(when, break_ecc=()):
    """Returns the bytes of a formatted, empty card, following [mymc] ps2mc.format() and [mcman] mcman_format2()."""
    lay = layout()
    data = bytearray(PAGES_PER_CARD * PAGE_SIZE)
    used = bytearray(PAGES_PER_CARD)  # 1 for the pages the format writes; every other page is left erased

    def put_cluster(cluster, content):
        start = cluster * CLUSTER_SIZE
        data[start:start + len(content)] = content
        for page in range(start // PAGE_SIZE, -(-(start + len(content)) // PAGE_SIZE)):
            used[page] = 1

    # indirect FAT (cluster 8): it lists the FAT clusters 9 to 40, the rest is 0xFFFFFFFF [mymc] format()
    ifc = [lay["first_ifc"] + lay["ifc_clusters"] + i for i in range(lay["fat_clusters"])]
    ifc += [0xFFFFFFFF] * (ENTRIES_PER_CLUSTER - len(ifc))
    put_cluster(lay["first_ifc"], struct.pack("<%dL" % ENTRIES_PER_CLUSTER, *ifc))

    # FAT: cluster 0 is the root directory (in use, end of chain), the clusters up to alloc_end are free, the ones after that
    # (the two backup blocks and the rounding of the table) are marked in use so nothing is put there [mymc] format()
    fat = [FAT_FREE] * lay["fat_entries"]
    fat[0] = FAT_CHAIN_END
    for i in range(lay["alloc_end"], lay["fat_entries"]):
        fat[i] = FAT_CHAIN_END
    for n in range(lay["fat_clusters"]):
        put_cluster(ifc[n], struct.pack("<%dL" % ENTRIES_PER_CLUSTER, *fat[n * ENTRIES_PER_CLUSTER:(n + 1) * ENTRIES_PER_CLUSTER]))

    # root directory: "." and ".." in the first allocatable cluster [mymc] format(), [mcman] McCreateDirentry
    created = tod(when)
    root = dirent(MODE_DOT, 2, 0, 0, created, b".") + dirent(MODE_DOTDOT, 0, 0, 0, created, b"..")
    put_cluster(lay["alloc_offset"], root)

    # superblock (page 0) [mymc] pack_superblock/write_superblock
    ifc_list = [lay["first_ifc"] + i for i in range(lay["ifc_clusters"])] + [0] * (MAX_INDIRECT_FAT_CLUSTERS - lay["ifc_clusters"])
    superblock = struct.pack(SUPERBLOCK_FORMAT, MAGIC, VERSION, PAGE_SIZE, PAGES_PER_CLUSTER, PAGES_PER_BLOCK, 0xFF00,
                             CLUSTERS_PER_CARD, lay["alloc_offset"], lay["alloc_end"], 0, lay["backup1"], lay["backup2"],
                             struct.pack("<32L", *ifc_list), struct.pack("<32L", *([0xFFFFFFFF] * 32)), CARD_TYPE_PS2, CARD_FLAGS)
    data[0:PAGE_SIZE] = superblock + bytes(PAGE_SIZE - len(superblock))
    used[0] = 1

    # raw pages: data + ECC for the pages the format wrote; all the others are erased, 0xFF in the data and in the spare bytes,
    # which is how a card looks after a console formats it: [mcman] mcman_format2 tests every block of the card with
    # mcman_writecluster, which ends by erasing the block and checking for 0xFF. The second backup block must be erased too
    # [mymc] write_superblock, [mcman] mcman_checkBackupBlocks reads its first two pages and expects 0xFFFFFFFF there.
    # (mymc fills the whole card with zero pages that have the ECC of zeros instead; a console accepts both.)
    erased_first = lay["backup2"] * PAGES_PER_BLOCK
    assert not any(used[erased_first:erased_first + PAGES_PER_BLOCK])
    out = bytearray()
    for page in range(PAGES_PER_CARD):
        if not used[page]:
            out += b"\xFF" * RAW_PAGE
        else:
            chunk = bytes(data[page * PAGE_SIZE:(page + 1) * PAGE_SIZE])
            out += chunk + ecc_page(chunk)
    for page in break_ecc:
        if not 0 <= page < PAGES_PER_CARD:
            raise SystemExit("--break-ecc: page %d is outside the card (0 to %d)" % (page, PAGES_PER_CARD - 1))
        if not used[page]:
            raise SystemExit("--break-ecc: page %d is erased (a card does not check the ECC of an erased page); pick a page the format writes: 0, 16 to 83" % page)
        out[page * RAW_PAGE + PAGE_SIZE] ^= 0x5A
    return bytes(out)


# ---------------------------------------------------------------------------------------------------------------------
# Checking a card
def read_u32s(buf):
    return struct.unpack("<%dL" % (len(buf) // 4), buf)


# The file system rules below come from [mymc] ps2mc.py (fat_chain, lookup_fat, dir_open, read_allocatable_cluster) and
# [mcman] main.c (mcman_getFATentry, mcman_readdirentry, mcman_getdirinfo), which agree:
#  - The allocation table (FAT) has one 32-bit entry per allocatable cluster; entry n belongs to cluster alloc_offset + n.
#    The superblock lists the "indirect FAT" clusters; they list the clusters of the FAT itself.
#  - Bit 31 set means "in use", the other 31 bits are the NEXT cluster of the same file or directory; 0xFFFFFFFF is the last
#    cluster of a chain; 0x7FFFFFFF (bit 31 clear) is a free cluster.
#  - Every directory, the root directory included, is a chain of clusters of 1024 bytes holding 512-byte entries, two per
#    cluster. The first entry of a directory ("." ) says how many entries it has. So a root directory with three entries
#    takes two clusters and its FAT entry 0 is 0x80000001 (in use, next cluster 1). That is valid and what a console writes
#    once there are two saves; only an empty or one-save root fits in one cluster, with FAT entry 0 = 0xFFFFFFFF.
#  - A file's data is a chain of ceil(length / 1024) clusters, starting at the entry's cluster field (also relative to
#    alloc_offset). Entries without the "exists" flag (0x8000) are deleted and own nothing.
def check_file_system(raw, page_data, alloc_offset, alloc_end, root_cluster, backup2, ifc_raw, ppc, ppb, clusters):
    """Checks the backup block, the FAT and every chain of every directory and file. Returns the number of problems."""
    problems = 0
    cluster_bytes = lambda c: b"".join(page_data(c * ppc + i) for i in range(ppc))
    entries_per_cluster = CLUSTER_SIZE // DIRENT_LENGTH

    def problem(text):
        nonlocal problems
        problems += 1
        print("  PROBLEM: " + text)

    # the second backup block must be erased [mcman] mcman_checkBackupBlocks
    if backup2 * ppb + 1 >= len(raw) // RAW_PAGE:
        problem("backup block %d is outside the card" % backup2)
    else:
        first = read_u32s(page_data(backup2 * ppb)[:4])[0]
        second = read_u32s(page_data(backup2 * ppb + 1)[:4])[0]
        if first != 0xFFFFFFFF or second != 0xFFFFFFFF:
            problem("backup block %d is not erased (a game would treat that as an interrupted write)" % backup2)

    # load the FAT through the indirect FAT [mymc] ps2mc.py __init__ (fat_matrix)
    per = ENTRIES_PER_CLUSTER
    fat_clusters = -(-alloc_end // per)
    ifc_needed = -(-fat_clusters // per)
    ifc_list = read_u32s(ifc_raw)
    if ifc_needed > len(ifc_list) or not 0 < alloc_end <= clusters:
        problem("the superblock's table sizes are not usable (end of allocatable clusters %d)" % alloc_end)
        return problems
    fat_cluster_list = []
    for n in range(ifc_needed):
        ifc_cluster = ifc_list[n]
        if not 0 < ifc_cluster < alloc_offset:
            problem("indirect FAT cluster %d (number %d) lies outside the table area" % (ifc_cluster, n))
            return problems
        fat_cluster_list += read_u32s(cluster_bytes(ifc_cluster))
    fat = []
    for n in range(fat_clusters):
        fc = fat_cluster_list[n]
        if not 0 < fc < alloc_offset:
            problem("FAT cluster number %d is %d, outside the table area (clusters 1 to %d)" % (n, fc, alloc_offset - 1))
            return problems
        fat += read_u32s(cluster_bytes(fc))

    owner = {}   # allocatable cluster -> what uses it

    def walk(start, what):
        """The clusters of one chain; every kind of damage is reported once and ends the walk."""
        chain, cur = [], start
        while True:
            if not 0 <= cur < alloc_end:
                problem("%s: the chain points at cluster %d, outside the card's %d allocatable clusters" % (what, cur, alloc_end))
                return chain
            if cur in chain:
                problem("%s: the chain loops back to cluster %d" % (what, cur))
                return chain
            entry = fat[cur]
            if not entry & FAT_ALLOCATED_BIT:
                problem("%s: the chain uses cluster %d, which the FAT marks as free" % (what, cur))
                return chain
            if cur in owner:
                problem("%s: cluster %d is also used by %s" % (what, cur, owner[cur]))
                return chain
            owner[cur] = what
            chain.append(cur)
            if entry == FAT_CHAIN_END:
                return chain
            cur = entry & ~FAT_ALLOCATED_BIT

    dirs = files = 0
    top = []
    pending = [(root_cluster, "/", None)]
    visited_dirs = set()
    while pending:
        start, path, expected = pending.pop()
        if start in visited_dirs:
            problem("directory %s is reached a second time (cluster %d)" % (path, start))
            continue
        visited_dirs.add(start)
        dirs += 1
        chain = walk(start, "directory " + path)
        if not chain:
            continue
        data = b"".join(cluster_bytes(alloc_offset + c) for c in chain)
        # the entry count is in the "." entry of the root directory; in any other directory the "." entry holds 0 and the
        # count is the length field of the entry that points at the directory, as [mymc] dir_open does (seen on the card Gran Turismo 4 wrote)
        count = struct.unpack("<L", data[4:8])[0] if expected is None else expected
        if not 2 <= count <= len(data) // DIRENT_LENGTH:
            problem("directory %s says it has %d entries, its %d cluster(s) hold at most %d" % (path, count, len(chain), len(data) // DIRENT_LENGTH))
            continue
        ents = []
        for i in range(count):
            mode, _, length, _, cl, _, _, _, name = struct.unpack("<HHL8sLL8sL28x448s", data[i * DIRENT_LENGTH:(i + 1) * DIRENT_LENGTH])
            ents.append((mode, length, cl, name.split(b"\0")[0].decode("latin-1")))
        if [e[3] for e in ents[:2]] != [".", ".."] or not all(e[0] & DF_EXISTS and e[0] & DF_DIR for e in ents[:2]):
            problem("directory %s: the first two entries are %r (modes 0x%04X, 0x%04X), not an existing '.' and '..'"
                    % (path, [e[3] for e in ents[:2]], ents[0][0], ents[1][0]))
            continue
        for mode, length, cl, name in ents[2:]:
            if not mode & DF_EXISTS:
                continue   # a deleted entry owns nothing
            if mode & DF_DIR:
                if path == "/":
                    top.append((name, length - 2))
                pending.append((cl, path + name + "/", length))
            else:
                files += 1
                need = -(-length // CLUSTER_SIZE)
                if need:
                    got = walk(cl, "file " + path + name)
                    if got and len(got) < need:
                        problem("file %s%s: %d bytes need %d cluster(s), the chain has %d" % (path, name, length, need, len(got)))
    root_len = sum(1 for o in owner.values() if o == "directory /")
    print("  file system: %d director%s, %d file%s, %d of %d clusters in use, root directory chain of %d cluster(s)"
          % (dirs, "y" if dirs == 1 else "ies", files, "" if files == 1 else "s", len(owner), alloc_end, root_len))
    if top:
        print("  top level folders: " + ", ".join("%s (%d files)" % t for t in sorted(top)))
    lost = [c for c in range(alloc_end) if fat[c] & FAT_ALLOCATED_BIT and c not in owner]
    if lost:
        # a leak, not damage (an interrupted write can leave one): a note, not a problem
        print("  note: %d cluster(s) are marked in use but belong to no file or folder (first: %s)" % (len(lost), ", ".join(map(str, lost[:5]))))
    return problems


def check_card(path):
    with open(path, "rb") as f:
        raw = f.read()
    print("%s: %d bytes" % (path, len(raw)))
    problems = 0
    if len(raw) % RAW_PAGE != 0:
        if len(raw) % PAGE_SIZE == 0:
            print("  the size is a multiple of 512, not of 528: this looks like an image without spare bytes (no ECC), cannot check")
        else:
            print("  the size is not a multiple of a page (528 bytes)")
        return 1
    pages = len(raw) // RAW_PAGE
    print("  %d pages of %d bytes%s" % (pages, RAW_PAGE, "" if pages == PAGES_PER_CARD else " (a standard 8 MB card has %d)" % PAGES_PER_CARD))

    ok = corrected = erased = 0
    bad = []
    half_erased = 0
    for page in range(pages):
        base = page * RAW_PAGE
        data = raw[base:base + PAGE_SIZE]
        spare = raw[base + PAGE_SIZE:base + RAW_PAGE]
        if spare[-1] == 0xFF:  # [mcman] McReadPage: a page whose last spare byte is 0xFF counts as erased and is not checked
            erased += 1
            if data.count(0xFF) != PAGE_SIZE:
                half_erased += 1
            continue
        verdicts = [ecc_verdict(data[i * 128:(i + 1) * 128], spare[i * 3:i * 3 + 3]) for i in range(4)]
        if "bad" in verdicts:
            bad.append((page, [i for i, v in enumerate(verdicts) if v == "bad"]))
        elif "corrected" in verdicts:
            corrected += 1
        else:
            ok += 1
    print("  ECC correct on %d pages, %d pages are erased (last spare byte 0xFF, not checked), %d have one wrong bit that a card "
          "would repair, %d are wrong" % (ok, erased, corrected, len(bad)))
    if half_erased:
        print("  note: %d erased-looking pages (spare bytes 0xFF) hold data that is not all 0xFF" % half_erased)
    for page, chunks in bad[:20]:
        print("  BAD ECC: page %d (0x%X), 128-byte parts %s" % (page, page, ", ".join(map(str, chunks))))
    if len(bad) > 20:
        print("  ... and %d more" % (len(bad) - 20))
    problems += len(bad)

    # layout check of a formatted card
    def page_data(n):
        return raw[n * RAW_PAGE:n * RAW_PAGE + PAGE_SIZE]

    sb = page_data(0)
    if not sb.startswith(MAGIC):
        print("  superblock: the card is NOT formatted (page 0 does not start with \"Sony PS2 Memory Card Format\")")
        print("  RESULT: %s" % ("not formatted; %d problem(s)" % problems if problems else "not formatted, nothing else to check (a game formats it when it saves)"))
        return 1 if problems else 0
    (magic, version, page_size, ppc, ppb, unused, clusters, alloc_offset, alloc_end, root_cluster, backup1, backup2, ifc_raw, bad_raw,
     card_type, card_flags) = struct.unpack(SUPERBLOCK_FORMAT, sb[:SUPERBLOCK_BYTES])
    print("  superblock: version %s, page %d, %d pages per cluster, %d per block, %d clusters, first allocatable cluster %d, end %d, "
          "backup blocks %d and %d, type %d, flags 0x%02X" % (version.rstrip(b"\0").decode("ascii", "replace"), page_size, ppc, ppb,
                                                              clusters, alloc_offset, alloc_end, backup1, backup2, card_type, card_flags))
    if page_size != PAGE_SIZE or ppc != PAGES_PER_CLUSTER or ppb != PAGES_PER_BLOCK or clusters * ppc != pages:
        print("  PROBLEM: the sizes in the superblock do not fit this file")
        problems += 1
    try:
        problems += check_file_system(raw, page_data, alloc_offset, alloc_end, root_cluster, backup2, ifc_raw, ppc, ppb, clusters)
    except (IndexError, struct.error) as error:
        print("  PROBLEM: the file system part cannot be read (%s)" % error)
        problems += 1
    print("  RESULT: %s" % ("fine" if not problems else "%d problem(s)" % problems))
    return 1 if problems else 0


def selftest():
    """The two ECC forms agree with each other, with the first entries of [mcman]'s xor table, and with a known value."""
    import random
    rng = random.Random(941)
    for _ in range(2000):
        chunk = bytes(rng.randrange(256) for _ in range(128))
        assert ecc_plain(chunk) == ecc_chunk(chunk), chunk.hex()
    for chunk in (bytes(128), b"\xFF" * 128, bytes(range(128)), bytes([1]) + bytes(127), bytes(127) + b"\x80"):
        assert ecc_plain(chunk) == ecc_chunk(chunk)
    # [mcman] mcman_xortable[0..7] = 00 87 96 11 A5 22 33 B4: bit 7 is the byte's parity, bits 0 to 6 its column parity mask
    for b, entry in enumerate((0x00, 0x87, 0x96, 0x11, 0xA5, 0x22, 0x33, 0xB4)):
        assert entry == (_PARITY[b] << 7 | _COLUMN[b]), b
    assert ecc_chunk(bytes(128)) == bytes((0x77, 0x7F, 0x7F)) == ecc_chunk(b"\xFF" * 128)  # zeros and 0xFF give the same ECC
    # one flipped bit in the data is found as 'corrected', two flipped bits as 'bad'
    chunk = bytearray(bytes(rng.randrange(256) for _ in range(128)))
    stored = ecc_chunk(bytes(chunk))
    assert ecc_verdict(bytes(chunk), stored) == "ok"
    chunk[37] ^= 0x10
    assert ecc_verdict(bytes(chunk), stored) == "corrected"
    chunk[90] ^= 0x01
    assert ecc_verdict(bytes(chunk), stored) == "bad"
    lay = layout()
    assert (lay["alloc_offset"], lay["alloc_end"], lay["first_ifc"], lay["fat_clusters"]) == (41, 8135, 8, 32), lay
    assert SUPERBLOCK_BYTES == 0x154
    print("self test passed")


def main(argv):
    parser = argparse.ArgumentParser(description="Make a formatted empty PS2 memory card image, or check the ECC of one.",
                                     usage="mcformat.py [check] card.ps2 [--force] [--break-ecc PAGE] [--date 'YYYY-MM-DD HH:MM:SS']")
    parser.add_argument("words", nargs="+", metavar="[check] card.ps2")
    parser.add_argument("--force", action="store_true", help="replace the file if it exists (this deletes the saves on it)")
    parser.add_argument("--break-ecc", type=int, action="append", default=[], metavar="PAGE", help="test aid: spoil the ECC of this page")
    parser.add_argument("--date", metavar="'YYYY-MM-DD HH:MM:SS'", help="clock value to write into the directory entries (default: now)")
    parser.add_argument("--selftest", action="store_true", help="run the built-in checks of the ECC code and exit")
    if "--selftest" in argv:
        selftest()
        return 0
    args = parser.parse_args(argv)
    words = args.words
    if words[0] == "check" and len(words) == 2:
        return check_card(words[1])
    if len(words) != 1:
        parser.error("give one card file name (or: check card.ps2)")
    path = words[0]
    if os.path.exists(path) and not args.force:
        print("%s exists already; it may hold saves. Use --force to replace it." % path, file=sys.stderr)
        return 2
    if args.date:
        when = time.strptime(args.date, "%Y-%m-%d %H:%M:%S")
    else:
        when = time.gmtime(time.time() + 9 * 3600)
    card = make_card(when, args.break_ecc)
    assert len(card) == CARD_BYTES
    temp = path + ".tmp"
    with open(temp, "wb") as f:
        f.write(card)
    os.replace(temp, path)
    print("wrote %s: %d bytes, formatted, empty%s" % (path, len(card), "; ECC spoiled on page(s) %s" % ", ".join(map(str, args.break_ecc)) if args.break_ecc else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

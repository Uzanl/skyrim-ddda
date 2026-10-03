"""Skyrim SE BSA (version 105) reader.

Bsa(path).read("meshes\\architecture\\farmhouse\\farmhouse04.nif") -> bytes (None if absent)

Layout: header (36 bytes: "BSA\\0", version, header size, archive flags, folder count,
file count, folder names length, file names length, file flags) -> folder records
(24 bytes: hash u64, file count u32, pad u32, offset u64) -> per folder: name (u8 length
incl. the 0, chars) + file records (16 bytes: hash u64, size u32, offset u32) -> file
names (0-terminated) -> data. Size bit 0x40000000 toggles the archive's default
compression; flag 0x100 puts the full path (u8 length + chars) before each file's data.
Compressed data = u32 original size + an LZ4 frame.
"""
import struct

import lz4.frame


class Bsa:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(36)
        assert h[:4] == b"BSA\0", path
        (self.version, _, self.flags, nfolders, nfiles, _, names_len, _) = struct.unpack_from("<8I", h, 4)
        assert self.version == 105, self.version
        folders = [struct.unpack("<QIIQ", self.f.read(24)) for _ in range(nfolders)]
        entries = []
        for _, count, _, _ in folders:
            n = self.f.read(1)[0]
            folder = self.f.read(n)[:-1].decode("latin1")
            for _ in range(count):
                _, size, off = struct.unpack("<QII", self.f.read(16))
                entries.append((folder, size, off))
        names = self.f.read(names_len).split(b"\0")
        self.files = {}
        for (folder, size, off), name in zip(entries, names):
            self.files[(folder + "\\" + name.decode("latin1")).lower()] = (size, off)

    def read(self, path):
        e = self.files.get(path.lower().replace("/", "\\"))
        if e is None:
            return None
        size, off = e
        compressed = bool(self.flags & 4) != bool(size & 0x40000000)
        size &= 0x3FFFFFFF
        self.f.seek(off)
        data = self.f.read(size)
        if self.flags & 0x100:
            n = data[0]
            data = data[1 + n:]
        if compressed:
            return lz4.frame.decompress(data[4:])
        return data


class Archives:
    """Several BSAs, later ones first (like the game's load order for the vanilla files)."""

    def __init__(self, paths):
        self.list = [Bsa(p) for p in paths]

    def read(self, path):
        for b in self.list:
            d = b.read(path)
            if d is not None:
                return d
        return None

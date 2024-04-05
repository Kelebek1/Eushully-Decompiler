import struct
from pathlib import Path

class FileWrapper:
    def __init__(self, filename):
        self.file = open(filename, "rb")

    def __del__(self):
        self.file.close()

    def seek(self, offset, mode=0):
        self.file.seek(offset, mode)

    def read(self, offset, size):
        self.seek(offset)
        return self.file.read(size)

with open("SYS5INI.BIN", "rb") as f:
    fd = f.read()

magic = fd[:0x8].decode("utf16")
title = fd[0x10:0x110].decode("utf16")

if magic != "S5IN":
    print(f"Bad magic {magic}")
    exit()

print(f"Extracting {title}")

pos = 0x220

open_archives = {}

# TODO: Find archive count, trial only has a single arc
arc_count = 1
for i in range(arc_count):
    arc_name = fd[pos:fd.find(b"\x00\x00\x00", pos) + 1].decode("utf16")
    pos += 0x200
    arc_entry_count = struct.unpack_from("<I", fd, pos)[0]
    pos += 0x4

    if arc_name not in open_archives:
        open_archives[arc_name] = {"path":Path(arc_name.split(".",1)[0]), "file":FileWrapper(arc_name)}

    print(f"Extracting {hex(arc_entry_count)} entries")
    for x in range(arc_entry_count):
        file_name_len = fd.find(b"\x00\x00", pos)
        file_name_len += file_name_len % 2
        file_name_len -= pos

        file_name = fd[pos:pos + file_name_len].decode("utf16")
        file_offset, file_length = struct.unpack_from("<II", fd, pos + 0x88)
        pos += 0x90

        out_path = open_archives[arc_name]["path"] / file_name
        if not open_archives[arc_name]["path"].exists():
            out_path.mkdir(parents=True, exist_ok=True)

        print(f"offset {hex(file_offset):08X} size {hex(file_length):08X} name {out_path}")

        with open(out_path, "wb") as f:
            f.write(open_archives[arc_name]["file"].read(file_offset, file_length))


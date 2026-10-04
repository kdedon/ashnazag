#!/usr/bin/env python3
"""Read regular files from bounded OFS/FFS floppy images."""
from dataclasses import dataclass
from pathlib import PurePosixPath
import struct


@dataclass(frozen=True)
class Entry:
    path: str
    data: bytes
    protection: int


class ADF:
    def __init__(self, data):
        if len(data) not in (901120, 1802240) or data[:3] != b'DOS' or data[3] not in (0, 1, 2, 3):
            raise ValueError('expected OFS/FFS DD or HD floppy')
        self.data = data
        self.blocks = len(data) // 512
        self.ffs = data[3] & 1
        self.claimed = {0, 1}

    def block(self, number, metadata=True):
        if not 2 <= number < self.blocks:
            raise ValueError('block outside floppy')
        data = self.data[number * 512:(number + 1) * 512]
        words = struct.unpack('>128I', data)
        if metadata and sum(words) & 0xffffffff:
            raise ValueError('invalid block checksum')
        return data, words

    def claim(self, number):
        if number in self.claimed:
            raise ValueError('cyclic or shared block')
        self.claimed.add(number)

    def contents(self, number, header):
        size = header[81]
        if size > len(self.data):
            raise ValueError('file exceeds floppy capacity')
        data = bytearray()
        current = header
        sequence = 1
        while True:
            count = current[2]
            if count > 72 or any(current[6:78 - count]):
                raise ValueError('invalid data pointer count')
            for pointer in reversed(current[78 - count:78]):
                self.claim(pointer)
                block, words = self.block(pointer, metadata=not self.ffs)
                if self.ffs:
                    data.extend(block)
                else:
                    if words[0] != 8 or words[1] != number or words[2] != sequence or words[3] > 488:
                        raise ValueError('invalid OFS data block')
                    data.extend(block[24:24 + words[3]])
                    sequence += 1
            extension = current[126]
            if not extension:
                break
            self.claim(extension)
            _, current = self.block(extension)
            if current[0] != 16 or current[1] != extension or current[127] != 0xfffffffd or current[125] != number:
                raise ValueError('invalid file extension')
        capacity = 512 if self.ffs else 488
        if len(data) < size or len(data) >= size + capacity:
            raise ValueError('file length differs from data blocks')
        return bytes(data[:size])

    def entries(self):
        root = self.blocks // 2
        self.claim(root)
        _, header = self.block(root)
        if header[0] != 2 or header[3] != 72 or header[127] != 1:
            raise ValueError('invalid root block')
        result = []

        def directory(parent, words, prefix, depth):
            if depth > 32:
                raise ValueError('directory nesting exceeds limit')
            names = set()
            for first in words[6:78]:
                number = first
                while number:
                    self.claim(number)
                    raw, child = self.block(number)
                    if child[0] != 2 or child[1] != number or child[125] != parent:
                        raise ValueError('invalid directory entry')
                    length = raw[432]
                    name = raw[433:433 + length].decode('latin-1')
                    if not 1 <= length <= 30 or name in ('.', '..') or any(c in name for c in '/:\\') or any(ord(c) < 32 for c in name):
                        raise ValueError('unsafe filename')
                    if name.lower() in names:
                        raise ValueError('duplicate case-insensitive filename')
                    names.add(name.lower())
                    path = str(PurePosixPath(prefix) / name)
                    if child[127] == 2:
                        directory(number, child, path, depth + 1)
                    elif child[127] == 0xfffffffd:
                        result.append(Entry(path, self.contents(number, child), child[80]))
                    else:
                        raise ValueError('unsupported link or entry type')
                    number = child[124]
        directory(root, header, '', 0)
        return result

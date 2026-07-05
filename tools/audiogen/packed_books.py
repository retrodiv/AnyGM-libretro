# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Encode the compact codebook wire format from licensed reference values."""
import struct


class Bits:
    def __init__(self):
        self.data = bytearray()
        self.position = 0

    def write(self, value, width):
        if not 0 <= value < 1 << width:
            raise ValueError("codebook field overflow")
        for bit in range(width):
            if self.position // 8 == len(self.data):
                self.data.append(0)
            self.data[self.position // 8] |= ((value >> bit) & 1) << (self.position % 8)
            self.position += 1

    def finish(self):
        # This wire format includes a zero byte even on exact byte alignment.
        if self.position % 8 == 0:
            self.data.append(0)
        return bytes(self.data)


def pack_book(book):
    dim, entries, lengths, lookup, minimum, delta, width, sequence, quant = book
    if len(lengths) != entries or not lengths or not 1 <= max(lengths) <= 32:
        raise ValueError("invalid reference codeword lengths")
    output = Bits()
    output.write(dim, 4)
    output.write(entries, 14)
    ordered = all(lengths) and lengths == sorted(lengths)
    output.write(int(ordered), 1)
    if ordered:
        length = lengths[0]
        output.write(length - 1, 5)
        at = 0
        while at < entries:
            end = at
            while end < entries and lengths[end] == length:
                end += 1
            output.write(end - at, (entries - at).bit_length())
            at, length = end, length + 1
    else:
        length_width = max(lengths).bit_length()
        sparse = 0 in lengths
        output.write(length_width, 3)
        output.write(int(sparse), 1)
        for length in lengths:
            if sparse:
                output.write(int(length != 0), 1)
            if length:
                output.write(length - 1, length_width)
    output.write(lookup, 1)
    if lookup:
        output.write(minimum, 32)
        output.write(delta, 32)
        output.write(width - 1, 4)
        output.write(sequence, 1)
        for value in quant:
            output.write(value, width)
    return output.finish()


def pack_library(books):
    output = bytearray()
    offsets = []
    for book in books:
        offsets.append(len(output))
        output.extend(pack_book(book))
    offsets.append(len(output))
    output.extend(struct.pack("<" + "I" * len(offsets), *offsets))
    return bytes(output)

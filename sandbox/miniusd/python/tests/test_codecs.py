import os
import random
import unittest

import _util  # noqa: F401
from miniusd import intcodec, lz4


class TestLZ4(unittest.TestCase):
    def test_roundtrip(self):
        rng = random.Random(1)
        for n in (0, 1, 5, 12, 13, 64, 1000, 70000):
            for data in (os.urandom(n), b"abcd" * n, bytes(n),
                         bytes(rng.choice(b"ab") for _ in range(n))):
                self.assertEqual(lz4.decompress(lz4.compress(data)), data)

    def test_compresses(self):
        self.assertLess(len(lz4.compress(b"xyz" * 1000)), 100)

    def test_overlap_match(self):
        # hand-made block: literal 'a', then match offset 1 length 9 -> 'a' * 10
        self.assertEqual(lz4.block_decompress(bytes([0x15]) + b"a" + bytes([1, 0]) + bytes([0x00])),
                         b"a" * 10)


class TestIntCodec(unittest.TestCase):
    def test_u32(self):
        vals = [0, 1, 2, 3, 100, 70000, 2**32 - 1, 5, 5, 5]
        self.assertEqual(intcodec.decode(intcodec.encode(vals), len(vals)), vals)

    def test_signed(self):
        rng = random.Random(2)
        vals = [rng.randint(-2**31, 2**31 - 1) for _ in range(500)] + [-2, -1, 0]
        blob = intcodec.compress_ints(vals)
        self.assertEqual(intcodec.read_compressed_ints(blob, 0, len(vals), signed=True)[0], vals)

    def test_64(self):
        rng = random.Random(3)
        vals = [rng.randint(0, 2**64 - 1) for _ in range(100)]
        blob = intcodec.compress_ints(vals, is64=True)
        self.assertEqual(intcodec.read_compressed_ints(blob, 0, len(vals), is64=True)[0], vals)

    def test_empty(self):
        self.assertEqual(intcodec.encode([]), b"")
        self.assertEqual(intcodec.read_compressed_ints(intcodec.compress_ints([]), 0, 0)[0], [])


if __name__ == "__main__":
    unittest.main()

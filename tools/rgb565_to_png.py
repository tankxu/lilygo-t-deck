#!/usr/bin/env python3
"""把 RGB565 原始数据转成 PNG,用来在电脑上预览壁纸。只依赖标准库。"""
import struct, zlib, sys, pathlib

W, H = 320, 240
src = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "main/assets/wallpaper.rgb565")
dst = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "/tmp/wallpaper.png")

data = src.read_bytes()
raw = bytearray()
for y in range(H):
    raw.append(0)                      # PNG 每行的滤波器字节
    for x in range(W):
        v, = struct.unpack_from('<H', data, (y * W + x) * 2)
        r = ((v >> 11) & 0x1F) * 255 // 31
        g = ((v >> 5)  & 0x3F) * 255 // 63
        b = (v         & 0x1F) * 255 // 31
        raw += bytes((r, g, b))

def chunk(tag, payload):
    return (struct.pack('>I', len(payload)) + tag + payload
            + struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF))

png = (b'\x89PNG\r\n\x1a\n'
       + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
       + chunk(b'IEND', b''))
dst.write_bytes(png)
print(dst)

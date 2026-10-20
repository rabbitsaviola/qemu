#!/usr/bin/env python3
"""Exercise EPIC source formats, MONO and scaling without firmware."""
import argparse
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('qemu')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='epic-') as directory:
        root = Path(directory)
        source = root / 'pixels.bin'
        # Two RGB565+A8 pixels: half-alpha red, opaque green.
        source.write_bytes(bytes.fromhex('00f880e007ff'))
        path = str(root / 'qtest.sock')
        proc = subprocess.Popen([
            args.qemu, '-M', 'sf32lb52x,flash-base=0x12020000',
            '-accel', 'qtest', '-display', 'none', '-serial', 'none',
            '-qtest', f'unix:{path},server=on,wait=off',
            '-device', f'loader,file={source},addr=0x12020000,force-raw=on',
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        connection = socket.socket(socket.AF_UNIX)
        connection.settimeout(5)
        try:
            deadline = time.monotonic() + 5
            while not Path(path).exists():
                if proc.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('QEMU did not start')
                time.sleep(.01)
            connection.connect(path)
            stream = connection.makefile('rwb', buffering=0)

            def command(text):
                stream.write((text + '\n').encode())
                response = stream.readline().decode().strip()
                assert response.startswith('OK'), (text, response)
                return response

            def reg(offset, value):
                command(f'writel {0x50007000 + offset:#x} {value:#x}')

            def draw(format_, source_addr, stride, alpha=255, flags=0):
                command('write 0x20008000 4 0xffffffff')
                reg(0x10, 0)        # CANVAS_TL
                reg(0x14, 1)        # two pixels
                reg(0x18, 1 << 25)  # preserve destination
                reg(0x0f8, 0)       # RGB565 output
                reg(0x0fc, 0x20008000)
                reg(0x100, 0)
                reg(0x54, 0)        # L0_TL
                reg(0x58, 1)
                reg(0x60, source_addr)
                reg(0x50, format_ | flags | (alpha << 5) |
                    (stride << 16) | (1 << 30))
                reg(0, 1)
                return command('read 0x20008000 4').split()[1].lower()

            assert draw(3, 0x62020000, 6) == '0xeffbe007'
            print('PASS: MPI2 NOR and 3-byte ARGB8565 pixel stepping')
            command('write 0x20004000 2 0x80ff')
            reg(0x64, 0xff0000)  # A8 foreground colour: red
            assert draw(4, 0x20004000, 2) == '0xeffb00f8'
            print('PASS: A8 coverage and FILL colour')
            assert draw(4, 0x20004000, 2, 128, 1 << 31) == '0xf7fdeffb'
            print('PASS: A8 coverage multiplied by global alpha')
            command('write 0x20004000 1 0xf8')
            assert draw(5, 0x20004000, 1) == '0xaefb00f8'
            print('PASS: A4 low-nibble-first coverage')
            command('write 0x20004000 1 0x0e')
            assert draw(7, 0x20004000, 1) == '0xaafa00f8'
            print('PASS: A2 low-bit-first coverage')
            command('write 0x20004000 4 0xf8ee0fee')
            command('write 0x20008000 8 0xffffffffffffffff')
            reg(0x14, 0x10001)
            reg(0x58, 0x10001)
            reg(0x50, 5 | (2 << 16) | (1 << 30))
            reg(0, 1)
            actual = command('read 0x20008000 8').split()[1]
            assert actual == '0xaefb00f800f8ffff', actual
            print('PASS: packed A4 rows respect byte stride and padding')
            reg(0x14, 1)
            reg(0x58, 1)
            assert draw(4, 0, 0, 128, 1 << 4) == '0xeffbeffb'
            print('PASS: zero-stride MONO fill with constant alpha')
            # Transparent output keeps straight colour and source coverage.
            command('write 0x20004000 2 0x80ff')
            reg(0x18, 1 << 24)
            reg(0x0f8, 3 << 1)  # ARGB8565 output (O_FORMAT at bit 1)
            reg(0x50, 4 | (2 << 16) | (1 << 30))
            reg(0x60, 0x20004000)
            reg(0, 1)
            actual = command('read 0x20008000 6').split()[1]
            assert actual == '0x00f88000f8ff', actual
            print('PASS: transparent canvas keeps straight colour and alpha')
            reg(0x18, 1 << 25)
            reg(0x0f8, 0)
            reg(0x50, 0)  # disable L0
            command('write 0x20004000 4 0x00f80000')
            reg(0x1c, (4 << 16) | (1 << 30))  # VL RGB565
            reg(0x20, 0)
            reg(0x24, 1)
            reg(0x28, 1 << 16)  # source extents: 2 by 1
            reg(0x30, 0x20004000)
            reg(0x3c, 0x8000)  # half a source pixel per output pixel
            reg(0x40, 0x10000)
            reg(0, 1)
            actual = command('read 0x20008000 4').split()[1]
            assert actual == '0x00f80078', actual
            print('PASS: VL half-pixel interpolation')
            # Measured RGB565 scaler sample: two rounded /255 stages differ
            # from a single four-tap /65536 sum.
            command('write 0x20004000 8 0x547cfcde137498ad')
            reg(0x14, 0)
            reg(0x24, 0)
            reg(0x28, 0x10001)
            reg(0x114, 0x37ff)
            reg(0x118, 0xcb7f)
            reg(0, 1)
            actual = command('read 0x20008000 2').split()[1]
            assert actual == '0x7584', actual
            print('PASS: measured two-axis RGB565 scaler rounding')
            command('write 0x20004000 8 0x1ce777a51374f26b')
            reg(0x114, 0x2340)
            reg(0x118, 0x5ba0)
            reg(0x48, (1 << 29) | (4096 << 3))  # forced identity rotation
            reg(0, 1)
            actual = command('read 0x20008000 2').split()[1]
            assert actual == '0xd9b5', actual
            print('PASS: measured 12-bit rotation interpolation')
            reg(0x114, 0x10000)  # a complete 2x2 footprint no longer fits
            command('write 0x20008000 2 0xffff')
            reg(0, 1)
            actual = command('read 0x20008000 2').split()[1]
            assert actual == '0xffff', actual
            print('PASS: rotation rejects source edge without a full footprint')
            # Reject the chroma-key pixel, preserving the black canvas.
            reg(0x1c, 0)
            reg(0x18, 0)
            reg(0x5c, 0x00ff00)
            command('write 0x20004000 4 0xe00700f8')
            assert draw(0, 0x20004000, 4, flags=1 << 15) == '0xffff00f8'
            print('PASS: exact RGB colour key preserves destination')
            command('write 0x20004000 4 0x00f800f8')
            command('write 0x20006000 2 0x00ff')
            reg(0xc4, 0)  # mask TL
            reg(0xc8, 1)  # mask BR
            reg(0xcc, 0x20006000)
            reg(0xc0, (1 << 28) | (1 << 2) | (2 << 14))
            assert draw(0, 0x20004000, 4) == '0xffff00f8'
            print('PASS: independent A8 mask clips L0 coverage')
            command('write 0x20006000 1 0xf0')
            reg(0xc0, (1 << 28) | (1 << 2) | (1 << 14) | 1)
            assert draw(0, 0x20004000, 4) == '0xffff00f8'
            print('PASS: independent A4 mask uses low nibble first')
            reg(0xc0, (1 << 28) | (1 << 2) | (1 << 14) | 3)
            assert draw(0, 0x20004000, 4, 0, 1 << 4) == '0xffff00f8'
            print('PASS: mask overwrite replaces layer alpha')
        finally:
            connection.close()
            proc.terminate()
            proc.wait(timeout=5)


if __name__ == '__main__':
    main()

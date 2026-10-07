#!/usr/bin/env python3
"""场景 3：**内存图层**的 RGB565 缩小（不走 co-engine，走 VL 那条）。

固件是 single_mode 的 `scale_memory_demo(multiple)`，源图是内存里的
`mask_2_data`（270x270 RGB565），按 `scale_x = scale_y = 1024 * multiple`
缩到 buffer0。`main()` 里只跑 2 和 3 倍。

串口那行 `scale_mem start--- N` 是在 `drv_epic_blend()` 返回之后打的，
buffer0 已经画好，冻在那一刻读回来就是这次的输出。

    cap_scalemem.py <workdir> [tag]

产物（都在 workdir）：
    mem_m{2,3}_buffer0.bin  冻住那一刻的 390x450 RGB565 帧
    mem_m{2,3}_regs.txt     同时刻的 PC/r0-r15 + EPIC 寄存器
    mask_2_data.bin         源图，从 guest 内存读
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common as c  # noqa: E402

MARKER = r"scale_mem start--- (\d+)"
WANTED = (2, 3)
TIMEOUT = 480
FREEZE_DELAY = 0.12


def main():
    if len(sys.argv) < 2:
        sys.exit("用法：cap_scalemem.py <workdir> [tag]")
    workdir = sys.argv[1]
    tag = sys.argv[2] if len(sys.argv) > 2 else "scalemem"
    os.makedirs(workdir, exist_ok=True)

    syms = c.resolve_symbols(c.FW, ["buffer0", "mask_2_data"])
    _, mask_size, _ = c.symbol_bytes(c.FW, "mask_2_data")
    side = c.math.isqrt(mask_size // 2)
    if side * side * 2 != mask_size:
        sys.exit("mask_2_data 不是正方形 RGB565：%d 字节" % mask_size)

    print("固件   %s" % c.FW)
    print("buffer0      = 0x%08x" % syms["buffer0"])
    print("mask_2_data  = 0x%08x (%dx%d)" % (syms["mask_2_data"], side, side))

    got = set()
    src_saved = False
    with c.QemuSession(tag, workdir) as ser:
        seen = 0
        deadline = time.time() + TIMEOUT
        while len(got) < len(WANTED) and time.time() < deadline:
            found = ser.wait_markers(MARKER, seen, timeout=deadline - time.time())
            if not found:
                break
            seen = len(found)
            multiple = int(found[-1])
            if multiple not in WANTED or multiple in got:
                continue
            # 那行在 blend 返回之后、wait_done 之前，稳一点再冻。
            time.sleep(FREEZE_DELAY)
            regs = ser.freeze()
            ratio_h = ser.reg32(c.EPIC + 0x03c)
            ratio_v = ser.reg32(c.EPIC + 0x040)
            print("marker %d pc=0x%x SCALE_RATIO_H=0x%x V=0x%x"
                  % (multiple, regs[15], ratio_h, ratio_v))
            data = ser.read_frame(syms["buffer0"])
            with open(os.path.join(workdir, "mem_m%d_buffer0.bin" % multiple),
                      "wb") as f:
                f.write(data)
            c.dump_state(ser.rsp,
                         os.path.join(workdir, "mem_m%d_regs.txt" % multiple),
                         regs)
            if not src_saved:
                src = ser.read(syms["mask_2_data"], mask_size)
                with open(os.path.join(workdir, "mask_2_data.bin"), "wb") as f:
                    f.write(src)
                print("mask_2_data first=%s 非零=%d"
                      % (src[:8].hex(), sum(1 for b in src if b)))
                src_saved = True
            nz = sum(1 for i in range(0, len(data), 2)
                     if data[i] or data[i + 1])
            print("  抓到 m%d：非黑像素=%d" % (multiple, nz))
            got.add(multiple)
            if len(got) < len(WANTED):
                ser.cont()
        if len(got) < len(WANTED):
            print("只抓到 m%s（超时 %ds）" % (sorted(got), TIMEOUT))
            return 1
        print("done got=%s" % sorted(got))
    return 0


if __name__ == "__main__":
    sys.exit(main())

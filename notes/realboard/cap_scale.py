#!/usr/bin/env python3
"""场景 2：EPIC co-engine 的**缩放**（EZIP 资产 + SCALE_RATIO）。

固件是 single_mode 的 `scale_down_demo(multiple, 205, 208)`，`main()` 里
依次跑 1/2/3 倍。每次 `drv_epic_blend()` 返回后打一行 `show lcd ...`，那行
就是"buffer0 已经画好了"的信号；第 n 行对应 `multiple = (n - 1) % 3 + 1`，
因为 `main()` 那个 `while(1)` 会一轮轮重来。

采样点一律拿冻住那一刻的寄存器反算（VL_TL_POS / VL_BR_POS / SCALE_RATIO_H/V
/ SCALE_INIT_CFG1/2），不拿 HAL 的中间量。

顺带把这份 EZIP 资产**当场**解出来：源图是固件里那份 `ezip_img_data`，
用宿主同一个 `ezip_linux` 走模型自己那条命令解（见 common.build_ezip_asset）。
这样参照系不是某个留在 /tmp 的旧目录，而是当前这份固件。

    cap_scale.py <workdir> [tag]

产物（都在 workdir）：
    scale_m{1,2,3}_buffer0.bin  冻住那一刻的 390x450 RGB565 帧
    scale_m{1,2,3}_regs.txt     同时刻的 PC/r0-r15 + EPIC 寄存器
    ezip_image.bin              当场解出来的资产（容器头 4 字节 + RGB565A）
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common as c  # noqa: E402

MARKER = "show lcd ..."
TIMEOUT = 480
FREEZE_DELAY = 0.25


def main():
    if len(sys.argv) < 2:
        sys.exit("用法：cap_scale.py <workdir> [tag]")
    workdir = sys.argv[1]
    tag = sys.argv[2] if len(sys.argv) > 2 else "scale"
    os.makedirs(workdir, exist_ok=True)

    buffer0 = c.resolve_symbols(c.FW, ["buffer0"])["buffer0"]
    print("固件   %s" % c.FW)
    print("buffer0 = 0x%08x" % buffer0)

    # 资产先解出来：QEMU 自己也要靠同一个工具解 co-engine 那条路，缺了它
    # 这个场景根本没得验。
    asset = os.path.join(workdir, c.EZIP_ASSET_NAME)
    try:
        w, h, fmt = c.build_ezip_asset(c.FW, asset)
    except Exception as e:
        print("解 EZIP 资产失败：%s" % e)
        return 1
    print("EZIP 资产 %dx%d fmt=%d -> %s" % (w, h, fmt, asset))

    got = set()
    with c.QemuSession(tag, workdir) as ser:
        seen = 0
        deadline = time.time() + TIMEOUT
        while len(got) < 3 and time.time() < deadline:
            found = ser.wait_markers(re.escape(MARKER), seen,
                                     timeout=deadline - time.time())
            if not found:
                break
            seen = len(found)
            multiple = seen % 3 or 3
            if multiple in got:
                continue
            # 那行是在 blend 返回之后打的，但 HAL 还没 wait_done，稳一点再冻。
            time.sleep(FREEZE_DELAY)
            regs = ser.freeze()
            ratio_h = ser.reg32(c.EPIC + 0x03c)
            print("show 第 %d 次 -> multiple=%d pc=0x%x SCALE_RATIO_H=0x%x"
                  % (seen, multiple, regs[15], ratio_h))
            data = ser.read_frame(buffer0)
            with open(os.path.join(workdir, "scale_m%d_buffer0.bin" % multiple),
                      "wb") as f:
                f.write(data)
            c.dump_state(ser.rsp,
                         os.path.join(workdir, "scale_m%d_regs.txt" % multiple),
                         regs)
            print("  抓到 m%d：非零=%d first=%s"
                  % (multiple, sum(1 for b in data if b), data[:8].hex()))
            got.add(multiple)
            if len(got) < 3:
                ser.cont()
        if len(got) < 3:
            print("只抓到 m%s（超时 %ds）" % (sorted(got), TIMEOUT))
            return 1
        print("done got=%s" % sorted(got))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""场景 1：VL 变换单元的**旋转**。冻住 vCPU，读回真机 buffer0。

固件是 single_mode 的 `rotate_and_mask_demo()`，动画每帧往串口打一行
`mask start--------------------------- N...`。那行**只当触发用**——角度一律
从冻住那一刻的 `VL_ROT` 寄存器取，marker 里的 N 和寄存器的度数不是一回事。

抓到的帧必须当场通过内容闸门才收下：按寄存器反算的映射、全分辨率逐像素
比，命中率过 GATE 才算"这一帧画完了"。否则动画中途冻住的半帧也会被当成
证据——这正是这套 harness 存在的理由。

    cap_rot.py <workdir> [tag]

产物（都在 workdir）：
    rot_deg<角度>_buffer0.bin   冻住那一刻的 390x450 RGB565 帧
    rot_deg<角度>_regs.txt      同时刻的 PC/r0-r15 + EPIC 寄存器
    mask_2_data.bin             源图，从 guest 内存读（在 PSRAM .data 里，
                                启动代码开板后才从 flash 拷过去）
    shot_rot_deg<角度>.ppm      screendump，留个肉眼可看的证据
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common as c  # noqa: E402

MARKER = r"mask start-+ (\d+)\.\.\."
# 动画从 90 起一帧帧加，太靠前的帧还没转出可辨的角度。
MIN_MARKER_ANGLE = 700
GATE = 0.97          # 全分辨率命中率门槛，和上一轮同一个数
TIMEOUT = 420
DELAYS = (0.10, 0.08, 0.08, 0.08, 0.08)


def main():
    if len(sys.argv) < 2:
        sys.exit("用法：cap_rot.py <workdir> [tag]")
    workdir = sys.argv[1]
    tag = sys.argv[2] if len(sys.argv) > 2 else "rot"
    os.makedirs(workdir, exist_ok=True)

    syms = c.resolve_symbols(c.FW, ["buffer0", "mask_2_data"])
    _, mask_size, _ = c.symbol_bytes(c.FW, "mask_2_data")
    side = c.math.isqrt(mask_size // 2)
    if side * side * 2 != mask_size:
        sys.exit("mask_2_data 不是正方形 RGB565：%d 字节" % mask_size)

    print("固件   %s" % c.FW)
    print("buffer0      = 0x%08x" % syms["buffer0"])
    print("mask_2_data  = 0x%08x (%dx%d)" % (syms["mask_2_data"], side, side))

    accepted = None
    src = None
    with c.QemuSession(tag, workdir) as ser:
        seen = 0
        last_tried = None
        deadline = time.time() + TIMEOUT
        while accepted is None and time.time() < deadline:
            found = ser.wait_markers(MARKER, seen, timeout=deadline - time.time())
            if not found:
                break
            seen = len(found)
            angle = int(found[-1])
            if angle < MIN_MARKER_ANGLE or angle == last_tried:
                continue
            last_tried = angle
            for delay in DELAYS:
                ser.cont()
                time.sleep(delay)
                regs = ser.freeze()
                if src is None:
                    src = ser.read(syms["mask_2_data"], mask_size)
                    with open(os.path.join(workdir, "mask_2_data.bin"), "wb") as f:
                        f.write(src)
                    print("mask_2_data (guest 已起来): first=%s 非零=%d"
                          % (src[:8].hex(), sum(1 for b in src if b)))
                # 角度从寄存器取：marker 里的 N 不是度数。
                theta = (ser.reg32(c.EPIC + 0x034) >> 2) & 0x1ff
                data = ser.read_frame(syms["buffer0"])
                ok, n = c.rot_score(data, src, side, side, theta)
                print("  try delay=%.2f pc=0x%x ROT_DEG=%d match=%d/%d = %.4f"
                      % (delay, regs[15], theta, ok, n, ok / n if n else 0))
                if n > 1000 and ok / n > GATE:
                    accepted = theta
                    break
            if accepted is None:
                ser.cont()

        if accepted is None:
            print("没抓到通过闸门的旋转帧（超时 %ds）" % TIMEOUT)
            return 1

        base = os.path.join(workdir, "rot_deg%d" % accepted)
        with open(base + "_buffer0.bin", "wb") as f:
            f.write(data)
        c.dump_state(ser.rsp, base + "_regs.txt", regs)
        ser.screendump(os.path.join(workdir, "shot_rot_deg%d.ppm" % accepted))
        print("ACCEPTED rotation deg=%d -> %s_buffer0.bin" % (accepted, base))
    return 0


if __name__ == "__main__":
    sys.exit(main())

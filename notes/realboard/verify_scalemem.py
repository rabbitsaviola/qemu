#!/usr/bin/env python3
"""比对 3：内存图层缩放。寄存器是唯一真相。

    sx = (SCALE_INIT_CFG1 + dx * SCALE_RATIO_H) >> 16      （sy 同理）

dx/dy 是画布像素相对 `VL_TL_POS` 的偏移；落点超出 `VL_EXTENTS`（源图允许读
的范围）就给黑，框外也给黑。源图是内存里的 `mask_2_data`，由 cap_scalemem.py
从 guest 内存读回来。

两个假设都跑满整幅 390x450：

  正分  上面的缩放映射（模型现在做的）
  负分  1:1 原样贴——从 TL 起把源图不缩放铺下去，变换单元的旧行为

    verify_scalemem.py <workdir>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common as c  # noqa: E402

POSITIVE_MIN = 0.97
CONTROL_MAX = 0.70        # 实测 0.3866 / 0.4873


def check_multiple(v, workdir, src, mw, mh, m):
    bpath = os.path.join(workdir, "mem_m%d_buffer0.bin" % m)
    rpath = os.path.join(workdir, "mem_m%d_regs.txt" % m)
    if not (os.path.exists(bpath) and os.path.exists(rpath)):
        v.bad("m%d 缺抓取（%s / %s）" % (m, bpath, rpath))
        return

    reg = c.parse_regs(rpath)
    tl, br = reg["VL_TL_POS"], reg["VL_BR_POS"]
    lx0, ly0 = tl & c.X_MASK, (tl & c.Y_MASK) >> 16
    lx1, ly1 = br & c.X_MASK, (br & c.Y_MASK) >> 16
    px = reg["VL_SCALE_RATIO_H"] & c.PITCH_MASK
    py = reg["VL_SCALE_RATIO_V"] & c.PITCH_MASK
    ix = reg["VL_SCALE_INIT_CFG1"] & c.PITCH_MASK
    iy = reg["VL_SCALE_INIT_CFG2"] & c.PITCH_MASK
    ext = reg["VL_EXTENTS"]
    max_col, max_line = (ext & c.COL_MASK) >> 16, ext & c.LINE_MASK
    buf = c.load_pixels(bpath)

    print("\n=== 内存图层缩放, multiple=%d ===" % m)
    print("  VL_CFG=0x%08x  VL_SRC=0x%08x  VL_ROT=0x%x  VL_MISC_CFG=0x%x"
          % (reg["VL_CFG"], reg["VL_SRC"], reg["VL_ROT"], reg["VL_MISC_CFG"]))
    print("  VL_TL_POS=(%d,%d) VL_BR_POS=(%d,%d) -> 框 %dx%d 位于 TL"
          % (lx0, ly0, lx1, ly1, lx1 - lx0 + 1, ly1 - ly0 + 1))
    print("  VL_SCALE_RATIO_H=0x%x (%.4f) V=0x%x (%.4f)"
          % (px, px / 65536.0, py, py / 65536.0))
    print("  VL_SCALE_INIT_CFG1=0x%x (%.4f) CFG2=0x%x (%.4f)"
          % (ix, ix / 65536.0, iy, iy / 65536.0))
    print("  VL_EXTENTS max_col=%d max_line=%d" % (max_col, max_line))
    print("  采样 x: dx=0 -> sx=%d ; dx=%d -> sx=%d"
          % (ix >> 16, lx1 - lx0, (ix + (lx1 - lx0) * px) >> 16))
    bb = c.bbox(buf)
    if bb:
        print("  buffer0 非黑包围盒=(%d,%d) %dx%d 个数=%d"
              % (bb[0], bb[1], bb[2], bb[3], bb[4]))
    else:
        print("  buffer0 全黑")

    # 正分走满整幅画布：框内按映射，框外必须是黑的。
    pos_ok = pos_n = 0
    bad = []
    for y in range(c.CANVAS_H):
        in_y = ly0 <= y <= ly1
        dy = y - ly0
        sy = (iy + dy * py) >> 16 if in_y else None
        for x in range(c.CANVAS_W):
            got = buf[y * c.CANVAS_W + x]
            pos_n += 1
            if in_y and lx0 <= x <= lx1:
                dx = x - lx0
                sx = (ix + dx * px) >> 16
                if 0 <= sx <= max_col and 0 <= sy <= max_line:
                    want = src[sy * mw + sx]
                else:
                    want = 0
            else:
                want = 0
            if got == want:
                pos_ok += 1
            elif len(bad) < 12:
                bad.append((x, y, want, got))

    # 框内的非黑像素——真正被这次作业写过的那些，单独数一份。
    box_nonblack_ok = box_nonblack_n = 0
    for y in range(ly0, ly1 + 1):
        dy = y - ly0
        sy = (iy + dy * py) >> 16
        for x in range(lx0, lx1 + 1):
            got = buf[y * c.CANVAS_W + x]
            if not got:
                continue
            dx = x - lx0
            sx = (ix + dx * px) >> 16
            want = (src[sy * mw + sx]
                    if 0 <= sx <= max_col and 0 <= sy <= max_line else 0)
            box_nonblack_n += 1
            if got == want:
                box_nonblack_ok += 1

    # 负对照：同一批框内像素，换成"源图从 TL 起 1:1 铺"。
    c1_ok = c1_n = 0
    for y in range(ly0, ly1 + 1):
        dy = y - ly0
        for x in range(lx0, lx1 + 1):
            dx = x - lx0
            if 0 <= dx < mw and 0 <= dy < mh:
                c1_n += 1
                if buf[y * c.CANVAS_W + x] == src[dy * mw + dx]:
                    c1_ok += 1

    pos = pos_ok / pos_n if pos_n else 0.0
    ctl = c1_ok / c1_n if c1_n else 0.0
    boxnb = box_nonblack_ok / box_nonblack_n if box_nonblack_n else 0.0
    print("  全帧 按寄存器映射: %d/%d = %.4f" % (pos_ok, pos_n, pos))
    print("  框内 非黑像素   : %d/%d = %.4f" % (box_nonblack_ok,
                                                box_nonblack_n, boxnb))
    print("  负对照 1:1 原样贴: %d/%d = %.4f" % (c1_ok, c1_n, ctl))
    for x, y, want, got in bad:
        print("      不一致 (x=%d,y=%d) 期=0x%04x 实=0x%04x" % (x, y, want, got))

    v.check(pos >= POSITIVE_MIN,
            "m%d 全帧正分 %.4f ≥ %.2f" % (m, pos, POSITIVE_MIN))
    v.check(boxnb >= POSITIVE_MIN,
            "m%d 框内非黑正分 %.4f ≥ %.2f" % (m, boxnb, POSITIVE_MIN))
    v.check(ctl <= CONTROL_MAX,
            "m%d 负对照 %.4f ≤ %.2f" % (m, ctl, CONTROL_MAX))


def main():
    if len(sys.argv) < 2:
        sys.exit("用法：verify_scalemem.py <workdir>")
    workdir = sys.argv[1]
    v = c.Verdict("内存图层缩放")

    src_path = os.path.join(workdir, "mask_2_data.bin")
    if not os.path.exists(src_path):
        v.bad("缺 %s，先跑 cap_scalemem.py" % src_path)
        v.finish()
    blob = open(src_path, "rb").read()
    mw = mh = c.math.isqrt(len(blob) // 2)
    src = c.load_pixels(src_path, mw * mh)
    print("源图 mask_2_data %dx%d，非黑=%d"
          % (mw, mh, sum(1 for p in src if p)))

    for m in (2, 3):
        check_multiple(v, workdir, src, mw, mh, m)

    v.finish()


if __name__ == "__main__":
    main()

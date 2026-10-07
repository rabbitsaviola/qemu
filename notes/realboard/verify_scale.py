#!/usr/bin/env python3
"""比对 2：co-engine 缩放。采样点全部按**冻住那一刻的寄存器**反算。

    sx = (SCALE_INIT_CFG1 + dx * SCALE_RATIO_H) >> 16      （sy 同理）

其中 dx/dy 是画布像素相对 `VL_TL_POS` 的偏移，落点超出 `VL_EXTENTS` 就给黑。
源图是**当场**用宿主 ezip_linux 从固件里那份 ezip_img_data 解出来的资产
（cap_scale.py 生成的 ezip_image.bin），不是某个留在 /tmp 的旧目录。

两个假设一起打分：

  正分  上面的缩放映射（模型现在做的）
  负分  1:1 原样贴——从 TL 起把源图不缩放铺下去，变换单元的旧行为。
        负对照必须显著低，否则这个测试区分不出缩放有没有做。

    verify_scale.py <workdir>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common as c  # noqa: E402

POSITIVE_MIN = 0.97
CONTROL_MAX = 0.20        # 实测 0.017 / 0.0185


def check_identity(v, workdir, asset):
    """multiple=1：co-engine 那条**不带变换**的路。

    这次作业的寄存器不能用——抓到的 VL_SCALE_RATIO_H/EXTENTS 是作业完之后
    的值（HAL 会清 ACTIVE 和 COENG_CFG），拿它反算只会得到一堆假的采样点。
    所以这里不比寄存器，只验"资产原样贴、逐像素相等"：m1 本来就是 1:1，
    "1:1 原样贴"在这一档就是正分，负对照不适用。
    """
    bpath = os.path.join(workdir, "scale_m1_buffer0.bin")
    if not os.path.exists(bpath):
        return
    iw, ih, src = asset
    buf = c.load_pixels(bpath)
    bb = c.bbox(buf)
    print("\n=== co-engine 缩放, multiple=1（1:1 那条，不按键寄存器判）===")
    if bb is None:
        v.bad("m1 buffer0 全黑")
        return
    bx, by, bw, bh, nz = bb
    print("  非黑包围盒=(%d,%d) %dx%d 个数=%d；资产 %dx%d"
          % (bx, by, bw, bh, nz, iw, ih))
    ok = n = 0
    for dy in range(min(bh, ih)):
        for dx in range(min(bw, iw)):
            n += 1
            if buf[(by + dy) * c.CANVAS_W + bx + dx] == src[dy * iw + dx]:
                ok += 1
    ratio = ok / n if n else 0.0
    print("  资产 1:1 原样贴: %d/%d = %.4f" % (ok, n, ratio))
    v.check(ratio >= POSITIVE_MIN,
            "m1 资产 1:1 贴 %.4f ≥ %.2f" % (ratio, POSITIVE_MIN))


def check_multiple(v, workdir, asset, m):
    bpath = os.path.join(workdir, "scale_m%d_buffer0.bin" % m)
    rpath = os.path.join(workdir, "scale_m%d_regs.txt" % m)
    if not (os.path.exists(bpath) and os.path.exists(rpath)):
        v.bad("m%d 缺抓取（%s / %s）" % (m, bpath, rpath))
        return

    iw, ih, src = asset
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

    bw, bh = lx1 - lx0 + 1, ly1 - ly0 + 1
    print("\n=== co-engine 缩放, multiple=%d ===" % m)
    print("  寄存器 VL_TL_POS=(%d,%d) VL_BR_POS=(%d,%d) -> 框 %dx%d"
          % (lx0, ly0, lx1, ly1, bw, bh))
    print("  SCALE_RATIO_H=0x%x (%.4f) V=0x%x (%.4f)"
          % (px, px / 65536.0, py, py / 65536.0))
    print("  SCALE_INIT_CFG1=0x%x (%.4f) CFG2=0x%x (%.4f)"
          % (ix, ix / 65536.0, iy, iy / 65536.0))
    print("  VL_EXTENTS max_col=%d max_line=%d；资产 %dx%d"
          % (max_col, max_line, iw, ih))
    bb = c.bbox(buf)
    if bb:
        print("  buffer0 非黑包围盒=(%d,%d) %dx%d 个数=%d"
              % (bb[0], bb[1], bb[2], bb[3], bb[4]))

    # 两个假设都跑满整幅画布：框外应当是黑的，也要算进去。
    pos_ok = pos_n = box_ok = box_n = ctl_ok = ctl_n = 0
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
                    want = src[sy * iw + sx]
                else:
                    want = 0
                box_n += 1
                if got == want:
                    box_ok += 1
                # 负对照：同一批像素，换成"源图从 TL 起 1:1 铺"。
                ctl_n += 1
                if 0 <= dx < iw and 0 <= dy < ih and got == src[dy * iw + dx]:
                    ctl_ok += 1
            else:
                want = 0
            if got == want:
                pos_ok += 1

    pos = pos_ok / pos_n if pos_n else 0.0
    box = box_ok / box_n if box_n else 0.0
    ctl = ctl_ok / ctl_n if ctl_n else 0.0
    print("  全帧 按寄存器映射: %d/%d = %.4f" % (pos_ok, pos_n, pos))
    print("  框内 按寄存器映射: %d/%d = %.4f" % (box_ok, box_n, box))
    print("  负对照 1:1 原样贴: %d/%d = %.4f" % (ctl_ok, ctl_n, ctl))

    v.check(box >= POSITIVE_MIN,
            "m%d 框内正分 %.4f ≥ %.2f" % (m, box, POSITIVE_MIN))
    v.check(pos >= POSITIVE_MIN,
            "m%d 全帧正分 %.4f ≥ %.2f" % (m, pos, POSITIVE_MIN))
    v.check(ctl <= CONTROL_MAX,
            "m%d 负对照 %.4f ≤ %.2f" % (m, ctl, CONTROL_MAX))


def main():
    if len(sys.argv) < 2:
        sys.exit("用法：verify_scale.py <workdir>")
    workdir = sys.argv[1]
    v = c.Verdict("co-engine 缩放")

    asset_path = os.path.join(workdir, c.EZIP_ASSET_NAME)
    if not os.path.exists(asset_path):
        v.bad("缺 %s，先跑 cap_scale.py" % asset_path)
        v.finish()
    asset = c.load_asset(asset_path)
    print("资产 %s：%dx%d（当场解出来的那版）"
          % (c.EZIP_ASSET_NAME, asset[0], asset[1]))

    for m in (2, 3):
        check_multiple(v, workdir, asset, m)
    check_identity(v, workdir, asset)

    v.finish()


if __name__ == "__main__":
    main()

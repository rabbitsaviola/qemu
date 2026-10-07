#!/usr/bin/env python3
"""比对 1：捕获的帧是不是源图按 VL_ROT 里的角度转出来的。

正分：按 common.rot_index 反算的映射，**全分辨率、逐像素**。
负分：同一帧对"1:1 原样贴"（theta=0，也就是变换单元的旧行为）打分，外加
      几个邻近角和不相关角做对照。负对照不显著低于正分，这个测试就是假的。

    verify_rot.py <workdir>
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common as c  # noqa: E402

POSITIVE_MIN = 0.97        # 正分下限
CONTROL_MAX = 0.80         # 负对照上限（实测 0.60 上下）
HIST_STRIDE = 2            # 上一轮那道闸门用的连采样，只为和旧数对得上


def main():
    if len(sys.argv) < 2:
        sys.exit("用法：verify_rot.py <workdir>")
    workdir = sys.argv[1]
    v = c.Verdict("旋转")

    src_path = os.path.join(workdir, "mask_2_data.bin")
    if not os.path.exists(src_path):
        v.bad("缺 %s，先跑 cap_rot.py" % src_path)
        v.finish()
    src = open(src_path, "rb").read()
    side = c.math.isqrt(len(src) // 2)
    print("源图 mask_2_data %dx%d，非零像素 %d"
          % (side, side, sum(1 for i in range(0, len(src), 2)
                             if src[i] or src[i + 1])))

    frames = sorted(glob.glob(os.path.join(workdir, "rot_deg*_buffer0.bin")))
    if not frames:
        v.bad("没有 rot_deg*_buffer0.bin，先跑 cap_rot.py")
        v.finish()

    for path in frames:
        theta = int(os.path.basename(path).split("deg")[1].split("_")[0])
        buf = open(path, "rb").read()
        print("\n=== %s  (VL_ROT 反算角度 = %d) ==="
              % (os.path.basename(path), theta))

        ok, n = c.rot_score(buf, src, side, side, theta)
        ratio = ok / n if n else 0.0
        print("  正分 按寄存器映射（全分辨率）: %d/%d = %.5f" % (ok, n, ratio))

        ok1, n1 = c.rot_score(buf, src, side, side, theta, tol=1)
        print("       容差 ±1 像素（只用来量差多远）: %d/%d = %.5f"
              % (ok1, n1, ok1 / n1 if n1 else 0))

        hist_ok, hist_n = c.rot_score(buf, src, side, side, theta,
                                      stride=HIST_STRIDE)
        print("       上一轮那道闸门的连采样 stride=%d: %d/%d = %.5f"
              % (HIST_STRIDE, hist_ok, hist_n,
                 hist_ok / hist_n if hist_n else 0))

        # 负对照：1:1 原样贴，外加邻近角和不相关角。
        ctrl = {}
        for t in sorted({0, 90, 180, theta - 9, theta + 9}):
            ck, cn = c.rot_score(buf, src, side, side, t)
            ctrl[t] = ck / cn if cn else 0.0
            print("  对照 theta=%-4d: %d/%d = %.5f" % (t, ck, cn, ctrl[t]))
        print("  负对照（1:1 原样贴，theta=0）: %.5f" % ctrl[0])

        v.check(ratio >= POSITIVE_MIN,
                "deg=%d 正分 %.5f ≥ %.2f" % (theta, ratio, POSITIVE_MIN))
        v.check(ctrl[0] <= CONTROL_MAX,
                "deg=%d 负对照 %.5f ≤ %.2f" % (theta, ctrl[0], CONTROL_MAX))
        best_ctrl = max(ctrl.values())
        v.check(ratio > best_ctrl,
                "deg=%d 正分 %.5f 高于所有对照的最大值 %.5f"
                % (theta, ratio, best_ctrl))
        if ratio < POSITIVE_MIN:
            print("  前几个不一致的像素 (x, y, src_x, src_y, 实, 期):")
            for row in c.rot_mismatches(buf, src, side, side, theta):
                print("    %s" % (row,))

    v.finish()


if __name__ == "__main__":
    main()

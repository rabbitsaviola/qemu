#!/usr/bin/env python3
"""用 qtest 验证触控那条链：I2C 控制器 → FT6146 → GPIO1 → NVIC。

被 notes/qtest-sifli.sh 的第 13 项调用。单独跑也可以：

    python3 notes/qtest-touch.py [构建目录]

**为什么要 Python**：这一项同时要两个协议。寄存器用 qtest 敲（和其余各项
一致，走 address_space_write/read），但"手指按下去"这件事只能从 QMP 的
qom-set 注入——qtest 没有 qom 命令（system/qtest.c 里没有）。两个协议要
交错：先配好 GPIO，再注入，再读 ISR，中间还得有屏障。bash 里做这个要
coproc 加第二个客户端，不如直接写出来。

**这一项钉的是什么**：真机上"触摸能用"= 控制器把 FT6146 的中断线拉低、
GPIO 锁存下降沿、固件读走状态后中断线松开、下一次触摸又是新的一条下降沿。
四条里任何一条断了，现象都是"驱动起来了但永远收不到触摸"，在真机固件上
看不出来是哪一环。这里逐环钉死。

SPDX-License-Identifier: GPL-2.0-or-later
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
    "~/build-sifli")
QEMU = os.path.join(BUILD, "qemu-system-arm")

# GPIO1 的 bank0（PA00-PA31）。这些是 HAL 的 set/clear 别名，读的是旁边
# 那个回读寄存器——见 include/hw/misc/sifli-gpio.h。
GPIO_BASE = 0x500a0000
GPIO_DIR = 0x00          # 只读：各脚当前电平
GPIO_DOECR = 0x18        # W1C: DOER，清掉就是把脚变输入
GPIO_IESR = 0x20         # W1S: IER
GPIO_IECR = 0x24         # W1C: IER
GPIO_ITSR = 0x2c         # W1S: ITR，边沿触发
GPIO_IPHCR = 0x3c        # W1C: IPHR，别要上升沿
GPIO_IPLSR = 0x44        # W1S: IPL，要下降沿
GPIO_IPLCR = 0x48        # W1C: IPL
GPIO_ISR = 0x4c          # W1C: 锁存的中断状态

TOUCH_PIN = 31
TOUCH_BIT = 1 << TOUCH_PIN

I2C1_BASE = 0x5009c000
I2C_CR = 0x00
I2C_TCR = 0x04
I2C_SR = 0x0c
I2C_DBR = 0x10
I2C2_BASE = 0x5009d000

TCR_TB = 1 << 0
TCR_START = 1 << 1
TCR_STOP = 1 << 2
TCR_NACK = 1 << 3

FT6146_ADDR = 0x38
# 驱动读的就是这么长：read_regs(0x01, 2 + 6 * MAX_POINT_NUM)，MAX_POINT_NUM=2。
# 第一个字节是 reg 0x01，第二个才是 TD_STATUS。
FT6146_READ_LEN = 14
FT6146_TD_STATUS = 1     # 在 point_data[] 里的下标，即寄存器 0x02

FAILED = 0
RESULTS = []


def check(what, want, got):
    global FAILED
    if got == want:
        RESULTS.append("  [PASS] %-44s = 0x%x" % (what, got))
    else:
        RESULTS.append("  [FAIL] %-44s 期望 0x%x，实际 0x%x"
                       % (what, want, got))
        FAILED += 1


class Qtest:
    """qtest stdio 那一头。每次写命令都会读回一行，天然是屏障。"""

    def __init__(self, proc):
        self.proc = proc

    def cmd(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        resp = self.proc.stdout.readline().strip()
        if not resp.startswith("OK"):
            # 空回多半是 QEMU 已经死了，把它的 stderr 带出来。
            raise RuntimeError("qtest %r -> %r%s"
                               % (line, resp, self._why()))
        return resp[2:].strip()

    def _why(self):
        if self.proc.poll() is None:
            return ""
        return "（QEMU 已退出，rc=%s）" % self.proc.returncode

    def writel(self, addr, val):
        self.cmd("writel 0x%x 0x%x" % (addr, val))

    def readl(self, addr):
        return int(self.cmd("readl 0x%x" % addr), 16)


class Qmp:
    """QMP 那一头，只用来注入触摸。"""

    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.f = self.sock.makefile("rwb")
        self._read()
        self.cmd("qmp_capabilities")

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("QMP 断了")
            msg = json.loads(line)
            if "event" not in msg:
                return msg

    def cmd(self, name, **args):
        self.f.write((json.dumps({"execute": name, "arguments": args}) + "\n")
                     .encode())
        self.f.flush()
        msg = self._read()
        if "error" in msg:
            raise RuntimeError("%s: %s" % (name, msg["error"]))
        return msg.get("return")

    def touch(self, **props):
        for prop, value in props.items():
            self.cmd("qom-set", path="/machine/touch",
                     property="touch-" + prop, value=value)


def ft6146_read(qt, reg, length):
    """照抄驱动 read_regs()：写寄存器指针，STOP，再重新 START 读。

    指针要跨这两次传输活下来（中间隔着 STOP），所以这一串同时也在验
    FT6146 的寄存器指针语义——写错了读回来全是状态寄存器那一个字节。
    """
    qt.writel(I2C1_BASE + I2C_DBR, (FT6146_ADDR << 1))
    qt.writel(I2C1_BASE + I2C_TCR, TCR_START)
    qt.writel(I2C1_BASE + I2C_DBR, reg)
    qt.writel(I2C1_BASE + I2C_TCR, TCR_TB)
    qt.writel(I2C1_BASE + I2C_TCR, TCR_STOP)

    qt.writel(I2C1_BASE + I2C_DBR, (FT6146_ADDR << 1) | 1)
    qt.writel(I2C1_BASE + I2C_TCR, TCR_START)

    out = []
    for i in range(length):
        last = (i == length - 1)
        # 最后一个字节 HAL 是 TB|STOP|NACK 一起写的，收发和收尾同一次写。
        qt.writel(I2C1_BASE + I2C_TCR,
                  TCR_TB | (TCR_STOP | TCR_NACK if last else 0))
        out.append(qt.readl(I2C1_BASE + I2C_DBR))
    return out


def main():
    if not os.access(QEMU, os.X_OK):
        print("  找不到 %s，先跑 notes/build-sifli.sh" % QEMU)
        return 1

    sockdir = tempfile.mkdtemp(prefix="qtest-touch-")
    sock = os.path.join(sockdir, "qmp.sock")
    errlog = os.path.join(sockdir, "stderr")

    # -S：开机就停住。这一项不跑固件，CPU 放着自己跑会从地址 0 取指令、
    # 然后 Lockup 到 HardFault 把 QEMU 整个 abort 掉（`-qtest` 不隐含停机，
    # 其余几项是因为命令喂完就关 stdin、抢在崩之前跑完）。停了之后 MMIO
    # 照样读写——qtest 走的是 address_space，不需要 CPU。
    with open(errlog, "w") as errf:
        proc = subprocess.Popen(
            [QEMU, "-M", "sf32lb52x", "-display", "none", "-serial", "none",
             "-S", "-qtest", "stdio",
             "-qmp", "unix:%s,server,nowait" % sock],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=errf, text=True)

    qmp = None
    try:
        # qmp.sock 要等 QEMU 建出来。
        for _ in range(200):
            if os.path.exists(sock):
                break
            time.sleep(0.05)

        qt = Qtest(proc)
        qmp = Qmp(sock)

        # --- PA31 配成下降沿中断。这一串是照抄固件在真机上写的那一串，
        #     顺序也一样（从 GPIO 的写 trace 里录的）。
        qt.writel(GPIO_BASE + GPIO_IECR, TOUCH_BIT)
        qt.writel(GPIO_BASE + GPIO_ISR, TOUCH_BIT)
        qt.writel(GPIO_BASE + GPIO_IPHCR, TOUCH_BIT)
        qt.writel(GPIO_BASE + GPIO_IPLCR, TOUCH_BIT)
        qt.writel(GPIO_BASE + GPIO_DOECR, TOUCH_BIT)
        qt.writel(GPIO_BASE + GPIO_ITSR, TOUCH_BIT)
        qt.writel(GPIO_BASE + GPIO_IPLSR, TOUCH_BIT)
        qt.writel(GPIO_BASE + GPIO_IESR, TOUCH_BIT)

        print()
        print("[17] 触控链：I2C 控制器 → FT6146 → GPIO1")
        print("  [17a] 复位后：中断线是空闲（高），没有待处理中断")

        check("PA31 没配好之前 ISR 是空的", 0,
              qt.readl(GPIO_BASE + GPIO_ISR))

        # 复位之后 FT6146 把线驱到高电平（reset exit 那一相）。这一读是
        # 在证明"脚是输入、电平为高"——要是 doer 那一位没清掉，下面那条
        # 下降沿永远锁不上，而现象只是"收不到触摸"。
        check("PA31 读回高电平（空闲）", TOUCH_BIT,
              qt.readl(GPIO_BASE + GPIO_DIR))

        data = ft6146_read(qt, 0x01, FT6146_READ_LEN)
        check("复位后 TD_STATUS 为 0", 0, data[FT6146_TD_STATUS])

        print("  [17b] 注入一次按下：下降沿被锁存")
        qmp.touch(x=120, y=200, down=True)
        check("ISR 锁存 PA31", TOUCH_BIT, qt.readl(GPIO_BASE + GPIO_ISR))

        print("  [17c] 固件清中断（W1C）；主机还没读，线还低着")
        qt.writel(GPIO_BASE + GPIO_ISR, TOUCH_BIT)
        check("写 1 之后 ISR 清零", 0, qt.readl(GPIO_BASE + GPIO_ISR))

        # 这一格钉的是"中断线一直低到主机把数据取走"。主机还没读，线就是
        # 低的，此时再报一次不构成新的下降沿——ISR 必须还是 0。要是在主机
        # 取走之前就把线松开，这里会多出一次中断，现象是触摸事件重复上报。
        qmp.touch(x=300)
        check("主机没读走之前再报，ISR 仍是 0", 0,
              qt.readl(GPIO_BASE + GPIO_ISR))

        print("  [17d] 主机读走状态：线松开，且读到的就是最新坐标")
        data = ft6146_read(qt, 0x01, FT6146_READ_LEN)
        check("TD_STATUS 报 1 个触点", 1, data[FT6146_TD_STATUS])
        check("X = 300（最新那一次）", 300, ((data[2] & 0x0f) << 8) | data[3])
        check("Y = 200", 200, ((data[4] & 0x0f) << 8) | data[5])
        check("event 位是 CTP_DOWN(0)", 0, data[2] >> 6)

        print("  [17e] 线松开之后，下一次触摸又是新的一条下降沿")
        qmp.touch(y=400)
        check("ISR 再次锁存 PA31", TOUCH_BIT, qt.readl(GPIO_BASE + GPIO_ISR))

        qt.writel(GPIO_BASE + GPIO_ISR, TOUCH_BIT)
        data = ft6146_read(qt, 0x01, FT6146_READ_LEN)
        check("X = 300", 300, ((data[2] & 0x0f) << 8) | data[3])
        check("Y = 400", 400, ((data[4] & 0x0f) << 8) | data[5])

        print("  [17f] 抬起：TD_STATUS 归零")
        qt.writel(GPIO_BASE + GPIO_ISR, TOUCH_BIT)
        qmp.touch(down=False)
        check("ISR 又锁存一次", TOUCH_BIT, qt.readl(GPIO_BASE + GPIO_ISR))
        data = ft6146_read(qt, 0x01, FT6146_READ_LEN)
        check("TD_STATUS 回到 0", 0, data[FT6146_TD_STATUS])

        print("  [17g] 没挂面板的 I2C2 上，地址没人应答")
        # 同一条总线上没有从机时 START 必须报 BED，不然固件会一直等 RF。
        qt.writel(I2C2_BASE + I2C_DBR, (FT6146_ADDR << 1))
        qt.writel(I2C2_BASE + I2C_TCR, TCR_START)
        check("I2C2 上无人应答，SR 置 BED(bit10)", 1 << 10,
              qt.readl(I2C2_BASE + I2C_SR) & (1 << 10))

        for line in RESULTS:
            print(line)

        if FAILED == 0:
            print("  触控链全部通过。")
            return 0
        print("  有 %d 项失败。" % FAILED)
        return 1
    finally:
        # qtest 没有 quit 命令，停机要从 QMP 说。关掉 stdin 也一样能收尾，
        # 但那要让 QEMU 自己发现 EOF，不如直接下令干净。
        try:
            if qmp is not None:
                qmp.cmd("quit")
        except Exception:
            pass
        try:
            proc.stdin.close()
        except Exception:
            pass
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
            proc.wait()
        if proc.returncode not in (0, None):
            with open(errlog) as f:
                tail = f.read().strip().splitlines()[-5:]
            print("  QEMU 退出码 %s，stderr 末尾：" % proc.returncode)
            for line in tail:
                print("    " + line)
        try:
            os.unlink(sock)
            os.unlink(errlog)
            os.rmdir(sockdir)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())

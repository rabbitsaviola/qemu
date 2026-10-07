#!/usr/bin/env python3
"""共用部件：gdbstub / monitor 客户端、EPIC 寄存器表、起机器的驱动。

三个场景（旋转、co-engine 缩放、内存图层缩放）验证的是**真实板子固件 +
HAL 自己算出来的寄存器**，这是 qtest 替代不了的：qtest 只能按人写的值敲
寄存器，这里跑的是固件真正下发的作业。方法论三代脚本一直没变，收进仓库
时也不改：

  * **寄存器是唯一真相**。期望的采样点一律拿冻住那一刻的 EPIC VL 寄存器
    反算，不拿 HAL 的中间量、不拿写死的常数。
  * **全分辨率、不抽样**。比对逐像素走满，没有 stride。
  * **必须有负对照**。同一帧对"按寄存器反算的映射"打正分，再对"1:1 原样
    贴"（变换单元的旧行为）打一次分；负对照不显著低于正分，测试就是假的。

路径一律走环境变量 + 默认值，缺东西时调用方打 [SKIP] 跳过，不算失败。
只用标准库。

环境变量（和 notes/qtest-sifli.sh、notes/verify-sifli.sh 对齐）：

  SIFLI_QEMU_BUILD    构建目录（默认 ~/build-sifli）
  SIFLI_SDK           SDK 根目录（默认 /mnt/e/code2/SiFli-SDK）
  SIFLI_ARM_NM        arm-none-eabi-nm（要从固件里取符号地址和大小）
  SIFLI_EZIP_TOOL     宿主的 ezip 解码器（默认 <SDK>/tools/png2ezip/ezip_linux）
  SIFLI_REALBOARD_FW  固件 ELF（默认 single_mode 的 a128r16 那份）
"""
import math
import os
import re
import socket
import struct
import subprocess
import sys
import time

# ------------------------------------------------------------------ 环境
def env_path(name, default):
    """取环境变量；空串按没设处理，~ 按 shell 的习惯展开。"""
    return os.path.expanduser(os.environ.get(name) or default)


BUILD = env_path("SIFLI_QEMU_BUILD", "~/build-sifli")
QEMU = os.path.join(BUILD, "qemu-system-arm")
SDK = env_path("SIFLI_SDK", "/mnt/e/code2/SiFli-SDK")
ARM_NM = os.environ.get("SIFLI_ARM_NM") or "arm-none-eabi-nm"
EZIP_TOOL = env_path(
    "SIFLI_EZIP_TOOL", os.path.join(SDK, "tools", "png2ezip", "ezip_linux"))
FW = env_path(
    "SIFLI_REALBOARD_FW",
    os.path.join(SDK, "example", "rt_device", "gpu", "single_mode", "project",
                 "build_sf32lb52-lcd_a128r16_hcpu", "main.elf"))

# 面板几何，来自 LCD_HOR_RES_MAX / LCD_VER_RES_MAX（a128r16 板）。
CANVAS_W, CANVAS_H = 390, 450
CANVAS_PIXELS = CANVAS_W * CANVAS_H
CANVAS_BYTES = CANVAS_PIXELS * 2

EPIC = 0x50007000

# 只看换帧/分区必需的这些；偏移的来历见 notes/peripherals.md §6。
EPIC_REGS = [
    (0x004, "STATUS"), (0x010, "CANVAS_TL"), (0x014, "CANVAS_BR"),
    (0x018, "CANVAS_BG"), (0x01c, "VL_CFG"), (0x020, "VL_TL_POS"),
    (0x024, "VL_BR_POS"), (0x028, "VL_EXTENTS"), (0x030, "VL_SRC"),
    (0x034, "VL_ROT"), (0x03c, "VL_SCALE_RATIO_H"),
    (0x040, "VL_SCALE_RATIO_V"), (0x048, "VL_MISC_CFG"),
    (0x050, "L0_CFG"), (0x054, "L0_TL_POS"), (0x058, "L0_BR_POS"),
    (0x060, "L0_SRC"), (0x0d0, "COENG_CFG"), (0x0f8, "AHB_CTRL"),
    (0x0fc, "AHB_MEM"), (0x100, "AHB_STRIDE"), (0x108, "VL_ROT_M_CFG1"),
    (0x10c, "VL_ROT_M_CFG2"), (0x110, "VL_ROT_M_CFG3"),
    (0x114, "VL_SCALE_INIT_CFG1"), (0x118, "VL_SCALE_INIT_CFG2"),
]

# 寄存器字段里的位宽；几个场景共用，放一处免得各写一遍。
X_MASK = 0x3ff
Y_MASK = 0x3ff << 16
COL_MASK = 0x3ff << 16
LINE_MASK = 0x3ff
PITCH_MASK = 0x3ffffff


# ------------------------------------------------------- gdbstub 客户端
class RSP:
    """够用就好的 GDB remote serial protocol 客户端。

    只实现读内存、读寄存器、继续、打断这几条；校验和按协议算，收到 `-`
    就重发。`cont()` 在已经在跑的时候必须闭嘴——再发一个 `c` 会把 stub
    搞糊涂，之后 Ctrl-C 就再也没人应答。
    """

    def __init__(self, host, port, timeout=120):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.settimeout(timeout)
        self.buf = b""
        self.last = None
        self.running = False

    def _read1(self):
        while not self.buf:
            d = self.s.recv(65536)
            if not d:
                raise EOFError("closed")
            self.buf += d
        c, self.buf = self.buf[:1], self.buf[1:]
        return c

    def send(self, pkt):
        self.last = pkt
        b = pkt.encode()
        self.s.sendall(b"$" + b + b"#" + ("%02x" % (sum(b) & 0xFF)).encode())

    def recv(self):
        while True:
            c = self._read1()
            if c == b"+":
                continue
            if c == b"-":
                self.send(self.last)
                continue
            if c == b"$":
                data = b""
                while True:
                    d = self._read1()
                    if d == b"#":
                        break
                    data += d
                self._read1()
                self._read1()
                self.s.sendall(b"+")
                return data.decode(errors="replace")

    def cmd(self, pkt):
        self.send(pkt)
        return self.recv()

    def cont(self):
        if self.running:
            return
        b = b"c"
        self.s.sendall(b"$" + b + b"#" + ("%02x" % (sum(b) & 0xFF)).encode())
        self.running = True

    def interrupt(self):
        self.s.sendall(b"\x03")
        self.running = False
        for _ in range(50):
            p = self.recv()
            if p[:1] in ("T", "S", "W", "X"):
                return p
        raise IOError("打断之后没等到 stop reply")

    def read_mem(self, addr, length, chunk=16384):
        """读内存。CPU 必须停着——调用方负责先 interrupt()。"""
        out = bytearray()
        done = 0
        while done < length:
            n = min(chunk, length - done)
            r = self.cmd("m%x,%x" % (addr + done, n))
            if r.startswith("E"):
                raise IOError("mem read fail @0x%x: %s" % (addr + done, r))
            out += bytes.fromhex(r)
            done += n
        return bytes(out)

    def regs(self):
        raw = bytes.fromhex(self.cmd("g"))
        return [int.from_bytes(raw[i * 4:i * 4 + 4], "little") for i in range(16)]

    def reg32(self, addr):
        return int.from_bytes(self.read_mem(addr, 4), "little")


# ------------------------------------------------------- monitor 客户端
class Mon:
    """QEMU monitor 客户端；只用来 screendump。"""

    def __init__(self, path, tries=100):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(tries):
            try:
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise IOError("连不上 monitor socket %s" % path)
        self.s.settimeout(3)

    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
        time.sleep(0.35)
        out = b""
        while True:
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                break
            if not d:
                break
            out += d
        return out.decode(errors="replace")


# ------------------------------------------------------------------ ELF
# 符号地址和大小从 nm 取，字节从 ELF 里按 program header 翻。
# **别写死地址**——重编一次就变；也别从 freeze 之后的 guest 内存里读只读资产，
# 那份直接从镜像里取更省事，而且和固件真正用的那份是同一份。
ELF_MAGIC = b"\x7fELF"


def elf_symbols(elf):
    """{名字: (地址, 大小)}；大小只在 nm 给了的时候才有（-S）。"""
    out = subprocess.check_output([ARM_NM, "-S", elf], stderr=subprocess.DEVNULL)
    syms = {}
    for line in out.decode(errors="replace").splitlines():
        p = line.split()
        try:
            if len(p) == 4:
                syms[p[3]] = (int(p[0], 16), int(p[1], 16))
            elif len(p) == 3:
                syms[p[2]] = (int(p[0], 16), 0)
        except ValueError:
            continue
    return syms


def _elf_segments(blob):
    """[(文件偏移, 虚拟地址, 载入地址, 文件长度)]，只取 PT_LOAD。"""
    if blob[:4] != ELF_MAGIC:
        raise ValueError("不是 ELF")
    if blob[5] != 1:
        raise ValueError("只认小端 ELF（EI_DATA=%d）" % blob[5])
    if blob[4] == 1:                      # ELF32
        phoff, = struct.unpack_from("<I", blob, 28)
        phentsize, phnum = struct.unpack_from("<HH", blob, 42)
        # program header 里各字段的字节偏移：p_offset / p_vaddr / p_paddr /
        # p_filesz（p_type 在 0，占 4 字节，两种 class 都一样）。
        off_at, va_at, pa_at, sz_at, word = 4, 8, 12, 16, "<I"
    elif blob[4] == 2:                    # ELF64
        phoff, = struct.unpack_from("<Q", blob, 32)
        phentsize, phnum = struct.unpack_from("<HH", blob, 54)
        off_at, va_at, pa_at, sz_at, word = 8, 16, 24, 32, "<Q"
    else:
        raise ValueError("不认识的 ELF class %d" % blob[4])

    segs = []
    for i in range(phnum):
        base = phoff + i * phentsize
        ptype, = struct.unpack_from("<I", blob, base)
        if ptype != 1:                    # PT_LOAD
            continue
        fields = [struct.unpack_from(word, blob, base + at)[0]
                  for at in (off_at, va_at, pa_at, sz_at)]
        segs.append(tuple(fields))
    return segs


def elf_read(elf, vaddr, size):
    """从 ELF 镜像里取 `vaddr` 处的 `size` 字节。

    先按虚拟地址匹配，再按载入地址匹配——有些段的 VMA 在 PSRAM、LMA 在
    flash（`.data` 那种），两种地址都指向同一份文件内容，固件启动时自己
    拷过去。
    """
    with open(elf, "rb") as f:
        blob = f.read()
    for off, va, pa, filesz in _elf_segments(blob):
        for base in (va, pa):
            if base <= vaddr < base + filesz:
                start = off + (vaddr - base)
                if start + size > off + filesz:
                    raise ValueError(
                        "0x%x 处要 %d 字节，超出段尾" % (vaddr, size))
                return blob[start:start + size]
    raise ValueError("ELF 里找不到地址 0x%x" % vaddr)


def symbol_bytes(elf, name):
    """(地址, 大小, 字节)。名字不在符号表里就抛 KeyError。"""
    syms = elf_symbols(elf)
    if name not in syms:
        raise KeyError("%s 里没有符号 %s" % (elf, name))
    addr, size = syms[name]
    if not size:
        raise KeyError("%s 没有大小，nm 得用 -S" % name)
    return addr, size, elf_read(elf, addr, size)


def resolve_symbols(elf, names):
    """一次 nm 解一批符号，返回 {名字: 地址}。"""
    syms = elf_symbols(elf)
    missing = [n for n in names if n not in syms]
    if missing:
        raise KeyError("%s 里没有符号 %s" % (elf, ", ".join(missing)))
    return {n: syms[n][0] for n in names}


# ------------------------------------------------------- 转储 / 解析
def dump_state(rsp, path, regs):
    """冻住那一刻的现场：PC、r0-r15、EPIC 那几张表。"""
    lines = ["pc=0x%x\n" % regs[15]]
    lines += ["r%-2d = 0x%08x\n" % (i, regs[i]) for i in range(16)]
    for off, name in EPIC_REGS:
        lines.append("%-22s (0x%03x) 0x%08x\n"
                     % (name, off, rsp.reg32(EPIC + off)))
    with open(path, "w") as f:
        f.write("".join(lines))


def parse_regs(path):
    """读回 dump_state 写的那份 txt。"""
    reg = {}
    for line in open(path):
        p = line.split()
        if len(p) >= 3 and p[1].startswith("(0x"):
            reg[p[0]] = int(p[2], 16)
    return reg


def load_pixels(path, count=CANVAS_PIXELS):
    """390x450 RGB565 dump -> 像素值列表（小端）。"""
    b = open(path, "rb").read()
    if len(b) < count * 2:
        raise ValueError("%s 太短：%d 字节" % (path, len(b)))
    return [b[i * 2] | (b[i * 2 + 1] << 8) for i in range(count)]


def rgb565_to_channels(v):
    return (v >> 11) & 0x1f, (v >> 5) & 0x3f, v & 0x1f


# ------------------------------------------------------ 旋转单元的映射
# rotate_and_mask_demo() 把 270x270 的 mask 图层摆在画布 TL(60,90)、
# pivot 取图心 (135,135)（main.c 里写死的摆位），所以画布上的旋转中心是
# 两者之和。角度本身不写死——一律从 VL_ROT 寄存器取。
ROT_CENTER = (195.0, 225.0)
ROT_PIVOT = (135.0, 135.0)


def rot_index(x, y, theta_deg, center=ROT_CENTER, pivot=ROT_PIVOT):
    """画布像素 -> 未旋转的源坐标。

    正向摆放是 canvas = C + R(theta) * (u - P)，反解就是
    u = R(-theta) * (pos - C) + P。和模型无关，纯几何。
    """
    th = math.radians(theta_deg)
    c, s = math.cos(th), math.sin(th)
    dx, dy = x - center[0], y - center[1]
    return (math.floor(c * dx + s * dy + pivot[0]),
            math.floor(-s * dx + c * dy + pivot[1]))


def rot_score(buf, src, sw, sh, theta_deg, stride=1, tol=0):
    """在整幅画布上按上面的映射逐像素比，返回 (命中, 落在源范围内的总数)。

    `stride=1` 是全分辨率。`tol` 是容差搜索半径，只用来量"差多少个像素"，
    判据不用它。
    """
    ok = n = 0
    for y in range(0, CANVAS_H, stride):
        for x in range(0, CANVAS_W, stride):
            iu, ju = rot_index(x, y, theta_deg)
            if not (0 <= iu < sw and 0 <= ju < sh):
                continue
            n += 1
            got = buf[(y * CANVAS_W + x) * 2:(y * CANVAS_W + x) * 2 + 2]
            if got == src[(ju * sw + iu) * 2:(ju * sw + iu) * 2 + 2]:
                ok += 1
            elif tol:
                for dj in range(-tol, tol + 1):
                    for di in range(-tol, tol + 1):
                        a, b = iu + di, ju + dj
                        if 0 <= a < sw and 0 <= b < sh:
                            o = (b * sw + a) * 2
                            if got == src[o:o + 2]:
                                ok += 1
                                break
                    else:
                        continue
                    break
    return ok, n


def rot_mismatches(buf, src, sw, sh, theta_deg, limit=6):
    """全分辨率下前几个不一致的像素，作为失败时的证据。"""
    out = []
    for y in range(CANVAS_H):
        for x in range(CANVAS_W):
            iu, ju = rot_index(x, y, theta_deg)
            if not (0 <= iu < sw and 0 <= ju < sh):
                continue
            bo = (y * CANVAS_W + x) * 2
            got = buf[bo:bo + 2]
            want = src[(ju * sw + iu) * 2:(ju * sw + iu) * 2 + 2]
            if got != want:
                out.append((x, y, iu, ju, got.hex(), want.hex()))
                if len(out) >= limit:
                    return out
    return out


# ------------------------------------------------------------ EZIP 资产
LVGL_HEADER_SIZE = 4
EZIP_ASSET_NAME = "ezip_image.bin"


def lvgl_geometry(blob):
    """解出来的资产的容器头：`[31:21] height [20:10] width [9:0] format`。"""
    hdr = int.from_bytes(blob[:LVGL_HEADER_SIZE], "little")
    return (hdr >> 10) & 0x7ff, (hdr >> 21) & 0x7ff, hdr & 0x1f


def build_ezip_asset(elf, dest, tool=None):
    """当场解出固件里那份 EZIP 资产，写到 `dest`，返回 (宽, 高, 格式)。

    走的是**模型自己那条命令**（见 hw/display/sifli-ezip.c）：

        ezip -convert in/image.ezip -spt 1 -dpt 1 -binfile 1 \\
             -dec_off_no_header 0 -outdir out

    `-dpt 1` 是关键：只写 `-spt 1` 会得到 PNG，写成 `-dpt 0` 会得到 RGB888
    的另一种输出，像素和 co-engine 真正用的那份对不上（差一个取整）。
    输入输出必须分目录——工具拿输入的名字命名输出，同目录会把自己的输入
    覆盖掉。
    """
    import shutil
    import tempfile

    tool = tool or EZIP_TOOL
    if not os.path.isfile(tool) or not os.access(tool, os.X_OK):
        raise RuntimeError("EZIP 解码器不可用：%s" % tool)

    addr, size, blob = symbol_bytes(elf, "ezip_img_data")
    tmpdir = tempfile.mkdtemp(prefix="sifli-ezip-")
    try:
        in_dir = os.path.join(tmpdir, "in")
        out_dir = os.path.join(tmpdir, "out")
        os.mkdir(in_dir)
        os.mkdir(out_dir)
        with open(os.path.join(in_dir, "image.ezip"), "wb") as f:
            f.write(blob)
        argv = [os.path.abspath(tool), "-convert", "in/image.ezip",
                "-spt", "1", "-dpt", "1", "-binfile", "1",
                "-dec_off_no_header", "0", "-outdir", "out"]
        proc = subprocess.run(argv, cwd=tmpdir, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT)
        out_path = os.path.join(out_dir, "image.bin")
        if proc.returncode != 0 or not os.path.exists(out_path):
            raise RuntimeError(
                "%s 解码失败（rc=%d）：%s"
                % (tool, proc.returncode,
                   proc.stdout.decode(errors="replace")[-400:]))
        shutil.copyfile(out_path, dest)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    return lvgl_geometry(open(dest, "rb").read())


def load_asset(path):
    """解出来的资产 -> (宽, 高, RGB565 像素列表)。

    带 alpha 的源在 16 位工具下是 3 字节一像素：低两字节是 RGB565（小端），
    第三字节是 alpha。
    """
    blob = open(path, "rb").read()
    w, h, fmt = lvgl_geometry(blob)
    if not w or not h:
        raise ValueError("%s 的容器头解不出宽高" % path)
    payload = blob[LVGL_HEADER_SIZE:]
    if len(payload) < w * h * 3:
        raise ValueError("%s 的载荷不够 %dx%d" % (path, w, h))
    pixels = [payload[i * 3] | (payload[i * 3 + 1] << 8) for i in range(w * h)]
    return w, h, pixels


def bbox(pixels, width=CANVAS_W, height=CANVAS_H):
    """非黑像素的紧包围盒；(x, y, w, h, 个数)，全黑回 None。"""
    nz = [(i % width, i // width) for i, v in enumerate(pixels) if v]
    if not nz:
        return None
    xs = [a for a, _ in nz]
    ys = [b for _, b in nz]
    return (min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1,
            len(nz))


# ------------------------------------------------------------ 起机器
def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class QemuSession:
    """起一台 QEMU、接上 gdbstub 和 monitor，提供"盯串口 → 冻住 → 转储"。

    命令行和 notes/build-and-verify.md 里记的那条一致；`-S` 让 CPU 停在复位
    向量上，等 gdbstub 来接，这样不会漏掉开机头几行 marker。
    """

    def __init__(self, tag, workdir, fw=None, port=None, ezip_tool=None,
                 qemu=None):
        self.tag = tag
        self.workdir = workdir
        self.fw = fw or FW
        self.qemu = qemu or QEMU
        self.ezip_tool = ezip_tool or EZIP_TOOL
        self.port = port or free_port()
        self.serial = os.path.join(workdir, "serial-%s.log" % tag)
        self.monsock = os.path.join(workdir, "mon-%s.sock" % tag)
        self.qemu_log = os.path.join(workdir, "qemu-%s.log" % tag)
        self.guest_log = os.path.join(workdir, "guest-errors-%s.log" % tag)
        self.proc = None
        self.rsp = None
        self.mon = None

    def start(self):
        os.makedirs(self.workdir, exist_ok=True)
        for p in (self.monsock, self.serial):
            if os.path.exists(p):
                os.remove(p)
        cmd = [self.qemu,
               "-M", "sf32lb52x,ezip-tool=%s" % self.ezip_tool,
               "-device", "sifli-panel", "-display", "none",
               "-serial", "file:%s" % self.serial,
               "-monitor", "unix:%s,server,nowait" % self.monsock,
               "-gdb", "tcp::%d" % self.port, "-S",
               "-d", "guest_errors",
               "-D", self.guest_log,
               "-kernel", self.fw]
        log = open(self.qemu_log, "wb")
        self.proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
        print("qemu pid %d (gdb :%d, serial %s)" % (
            self.proc.pid, self.port, self.serial))

        last = None
        for _ in range(200):
            if self.proc.poll() is not None:
                raise RuntimeError("QEMU 起来就退了，看 %s" % self.qemu_log)
            try:
                self.rsp = RSP("127.0.0.1", self.port, timeout=120)
                break
            except OSError as e:
                last = e
                time.sleep(0.1)
        else:
            raise RuntimeError("连不上 gdbstub :%d (%s)" % (self.port, last))

        self.rsp.cmd("?")
        self.rsp.cmd("Hg0")
        self.rsp.cmd("Hc0")
        self.mon = Mon(self.monsock)
        self.rsp.cont()
        return self

    # -- 串口 -------------------------------------------------------
    def serial_text(self):
        try:
            with open(self.serial, "rb") as f:
                return f.read().decode(errors="replace")
        except FileNotFoundError:
            return ""

    def wait_markers(self, pattern, count, timeout, poll=0.05):
        """等到串口里 `pattern` 的匹配数**超过** `count`，返回全部匹配。

        超时返回 None（调用方自己决定是失败还是放过）。
        """
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("QEMU 中途退了，看 %s" % self.qemu_log)
            m = rx.findall(self.serial_text())
            if len(m) > count:
                return m
            time.sleep(poll)
        return None

    # -- 冻结 -------------------------------------------------------
    def freeze(self):
        """打断 vCPU 并取回通用寄存器。**读内存之前必须调它。**"""
        self.rsp.interrupt()
        return self.rsp.regs()

    def cont(self):
        self.rsp.cont()

    def read(self, addr, length):
        return self.rsp.read_mem(addr, length)

    def read_frame(self, addr, length=CANVAS_BYTES):
        return self.rsp.read_mem(addr, length)

    def reg32(self, addr):
        return self.rsp.reg32(addr)

    def screendump(self, path):
        return self.mon.cmd("screendump %s" % path)

    # -- 收尾 -------------------------------------------------------
    def close(self):
        try:
            if self.rsp:
                self.rsp.cmd("D")
        except Exception:
            pass
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                self.proc.kill()

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.close()
        return False


# ------------------------------------------------------- PASS/FAIL 口径
class Verdict:
    """和 notes/verify-sifli.sh 一个口径：`  [PASS] ...` / `  [FAIL] ...`，
    有任何一项失败就非零退出。"""

    def __init__(self, name):
        self.name = name
        self.failed = 0

    def ok(self, msg):
        print("  [PASS] %s" % msg)

    def bad(self, msg):
        print("  [FAIL] %s" % msg)
        self.failed += 1

    def check(self, cond, msg):
        (self.ok if cond else self.bad)(msg)
        return cond

    def finish(self):
        if self.failed:
            print("%s：有 %d 项失败。" % (self.name, self.failed))
            sys.exit(1)
        print("%s：全部通过。" % self.name)
        sys.exit(0)

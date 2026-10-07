# 真机固件验证（EPIC 三个场景）

跑**真实板子固件**，让 HAL 自己算寄存器，冻住 vCPU 把 buffer 从 guest 内存
读回来，再按寄存器反算采样点逐像素比。这不是 qtest 能替代的：qtest 只能按
人写的值敲寄存器，验不了"HAL 下发的寄存器对不对"。

以前这三个场景的脚本散在 `/tmp` 里（`cap3.py` / `cap_scale2.py` /
`cap_scalemem.py` 和三个 `verify_*.py`），重启就没了。这里把它们固化下来，
每轮 EPIC 改动之后跑一遍，回归不用靠记忆。

```
bash notes/realboard/run.sh
```

全部通过退出码 0，有失败为 1；缺 SDK / 固件 / ezip 工具的场景打 `[SKIP]`
跳过，不算失败（和 `notes/verify-sifli.sh`、`notes/qtest-sifli.sh` 一个口径）。

## 方法论

三代脚本一直没变，收进仓库时也不改：

1. **寄存器是唯一真相。** 期望的采样点一律拿冻住那一刻的 EPIC VL 寄存器反算
   （`VL_ROT`、`VL_SCALE_RATIO_H/V`、`VL_SCALE_INIT_CFG1/2`、`VL_TL_POS` 等），
   不拿 HAL 的中间量、不拿写死的常数。marker 行只当触发用——比如旋转那条
   `mask start--- N` 里的 N 和 `VL_ROT` 的度数不是一回事。
2. **全分辨率、不抽样。** 比对逐像素走满，没有 stride。旋转那条是 390×450
   全画布 72897 个落在源范围内的像素，缩放那两条是 175500 个。
3. **必须有负对照。** 同一帧对"按寄存器反算的映射"打正分，再对"1:1 原样贴"
   （变换单元的旧行为）打一次分。负对照不显著低于正分，这个测试就是假的。

顺带一条：**别写死地址**。符号地址和大小从固件 ELF 用 `nm -S` 取，重编一次
就变。

## 三个场景

三个场景跑的是同一份固件——`single_mode`，它的 `main()` 那个 `while(1)` 里
依次跑完混叠、渐变、co-engine 缩放（1/2/3 倍）、内存图层缩放（2/3 倍）、
旋转+遮罩、文字。每个场景单独起一次机器，互不影响。

| 场景 | 固件那条路 | marker | 源图 |
|---|---|---|---|
| 旋转 | `rotate_and_mask_demo()` | `mask start--- N...` | `mask_2_data`（guest 内存） |
| coeng缩放 | `scale_down_demo(m, 205, 208)` | `show lcd ...` 第 n 次 → `m = (n-1)%3+1` | 固件里的 `ezip_img_data` |
| 内存缩放 | `scale_memory_demo(m)` | `scale_mem start--- N` | `mask_2_data`（guest 内存） |

co-engine 那条的源图**当场**解：从 ELF 里取固件自己那份 `ezip_img_data`，
用宿主同一个 `ezip_linux` 走**模型自己那条命令**解（见
`common.build_ezip_asset` 的注释，`-dpt 1` 是关键）。参照系是当前这份固件，
不是某个留在 `/tmp` 的旧目录——旧目录里的那份和当场解的差一个取整，比出来
只有 0.19，很容易误判成模型坏了。

## 已知数字

跑通之后应当重现这些（`run.sh` 会把它们打出来）：

| 场景 | 正分 | 负对照 |
|---|---|---|
| 旋转 | `≈0.994`（`deg=189` 那次是 `72472/72897 = 0.99417`，`deg=144` 那次是 `72484/72897 = 0.99433`） | `≈0.600`（theta=0） |
| coeng m2 | `10712/10712 = 1.0000` | `183/10712 = 0.0171` |
| coeng m3 | `4761/4761 = 1.0000` | `88/4761 = 0.0185` |
| coeng m1（1:1 那条） | `42640/42640 = 1.0000` | 不适用 |
| 内存 m2 | 全帧 `175500/175500`，框内非黑 `7305/7305` | `7045/18225 = 0.3866` |
| 内存 m3 | 全帧 `175500/175500`，框内非黑 `3249/3249` | `3947/8100 = 0.4873` |

旋转那条另有一行 `stride=2: 18101/18224 = 0.99325`——那是上一轮 `cap3.py`
那道**连采样**闸门留下来的数，登在别处的 "ACCEPTED deg=189，18101/18224"
就是它。判据不看它（判据是全分辨率那行）。

旋转差的那 0.58% 是变换单元在图层边缘的 ±1 像素行为：容差 ±1 那行是
`72897/72897 = 1.0000`。别的角度对照 0.60–0.64，正分 0.994，区分度够。

缩放那两条同时打"全帧"和"框内/框内非黑"两个数，是因为**全帧那个数很钝**：
画布大部分是黑的，拿 m3 的帧去顶 m2 的寄存器，全帧还有 `0.9590`（离 `0.97`
的门槛很近），而框内非黑立刻掉到 `0.0311`。真正能抓住"模型出错了帧"的是框内
那个数，全帧那个只用来证明框外确实是黑的。

判据（写在各 `verify_*.py` 顶部，改了要说为什么）：

* 正分 ≥ `0.97`
* 负对照 ≤ 旋转 `0.80` / coeng `0.20` / 内存 `0.70`
* 旋转另加一条：正分要高于所有对照角的最大值

## 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `SIFLI_QEMU_BUILD` | `~/build-sifli` | 构建目录（和 `qtest-sifli.sh` 同名） |
| `SIFLI_SDK` | `/mnt/e/code2/SiFli-SDK` | SDK 根目录 |
| `SIFLI_ARM_NM` | `arm-none-eabi-nm` | 取符号地址和大小 |
| `SIFLI_REALBOARD_FW` | `$SIFLI_SDK/example/rt_device/gpu/single_mode/project/build_sf32lb52-lcd_a128r16_hcpu/main.elf` | 三个场景共用的固件 |
| `SIFLI_EZIP_TOOL` | `$SIFLI_SDK/tools/png2ezip/ezip_linux` | 宿主 ezip 解码器 |
| `SIFLI_REALBOARD_WORK` | `${TMPDIR:-/tmp}/sifli-realboard` | 工作目录（抓到的帧、寄存器、串口日志都在这） |
| `SIFLI_REALBOARD_TIMEOUT` | `900` | 单个场景超时秒数 |

## 文件

```
run.sh             入口：三个场景各跑一遍，打 PASS/FAIL 汇总
common.py          RSP（gdbstub）/ Mon（monitor）客户端、EPIC 寄存器表、
                   ELF 符号与取字节、旋转映射、EZIP 资产解码、QemuSession 驱动
cap_rot.py         场景 1 抓取（旋转）——驱动上的一份薄配置
cap_scale.py       场景 2 抓取（co-engine 缩放）
cap_scalemem.py    场景 3 抓取（内存图层缩放）
verify_rot.py      比对 1：按 VL_ROT 反算 vs 1:1 原样贴
verify_scale.py    比对 2：按 SCALE_RATIO/INIT 反算 vs 1:1 原样贴
verify_scalemem.py 比对 3：同上，源图换成内存里的 mask_2_data
```

抓取脚本产出（都在工作目录）：

* `rot_deg<角度>_buffer0.bin` + `_regs.txt` —— 冻住那一刻的 390×450 RGB565
  帧和 PC/r0-r15/EPIC 寄存器
* `scale_m{1,2,3}_*`、`mem_m{2,3}_*` —— 同上
* `mask_2_data.bin` —— 源图，从 guest 内存读（它在 PSRAM `.data` 里，开板
  之后启动代码才从 flash 拷过去，复位后立刻读是零）
* `ezip_image.bin` —— 当场解出来的 EZIP 资产（4 字节容器头 + RGB565A）
* `serial-<tag>.log`、`qemu-<tag>.log`、`guest-errors-<tag>.log` —— 排查用

## 加一个场景

在驱动上加一份薄配置即可：写个 `cap_xxx.py`，用 `common.QemuSession` 起机器、
`wait_markers()` 盯串口 marker、`freeze()` 之后 `read()` 转储、`reg32()` 读
寄存器；再配一个 `verify_xxx.py` 按寄存器反算、打分、负对照，然后加进
`run.sh` 的 `run_scenario` 调用。

抓取侧的"内容闸门"值得保留：旋转那条的 `GATE = 0.97` 是在**抓的时候就**
按全分辨率比一遍，只有画完的帧才收下；否则动画中途冻住的半帧会被当成证据。

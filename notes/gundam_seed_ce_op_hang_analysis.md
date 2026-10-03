# 高达SEED C.E. 汉化版：USB 模式 OP 开头停住 —— 定位到 USB 读路径

## 0. 已知事实

| # | 事实 | 来源 |
|---|---|---|
| 1 | 汉化版放 **HDD** 上 OP 正常 | 你实测 |
| 2 | USB 上某处起**彻底不再读盘**，画面停住 | 你实测 |
| 3 | 没有卡死，**按 Start 可以正常跳过** | 你实测 |
| 4 | 动画文件码率/大小几乎一样，汉化版只是重新打包 | 你指出 |
| 5 | **碎片数 = 1**，走 bd_defrag 的单碎片快速路径 | 你实测 |
| 6 | 去掉 `REM1_MIN_US` 的版本照样复现 | 你实测 |
| 7 | 两版每轮读盘耗时一样（~101 ms），汉化版轮周期 0.232 s vs 原版 0.636 s | 日志 |

**排除**：码率/文件大小、镜像损坏、碎片问题、`rem1`、带宽不足（带宽不足只会变慢，不会停）。

## 1. 关键推论：这不是“永久卡死”，而是“游戏自己不要数据了”

如果 USB 传输真的永久挂住，那么：`bdm_io_sema` 一直被占 → **之后任何一次读盘（包括按 Start 之后的加载）都会跟着挂住**。
你说按 Start 能正常跳过（游戏继续跑），说明 **USB 链路之后是活的** —— 所以：

> 停住的原因是：**某一笔读返回了错误（或返回了坏数据）→ 视频播放器放弃这段流 → 它不再发读请求**。
> “读盘停住”是结果，不是原因。

这也和 HDD 正常吻合（同一份数据、不同设备驱动，错误不出现）。

## 2. USB 读路径上有三种会“让游戏放弃”的失败签名（都已加了日志）

```
EE → cdvdfsv(8 扇区一段) → cdvdman_read → device-bdm.c → bd_defrag(单碎片快速路径)
   → usbmass_bd: scsi_read → USB BOT(CBW/数据/CSW) → usbd → OHCI
```

| 签名 | 日志（调试构建） | 含义 | 修法方向 |
|---|---|---|---|
| **A. 设备层读错误** | `bdm read failed: lsn=… rv=…`；通常前面还有 `usbmass_bd: ERROR: unable to read sector after N retries (sector=0x…)`（这行是他们 ps2sdk 分支里的 M_PRINTF，**默认就会打**） | SCSI 层重试 32 次/2 秒后放弃 → OPL 返回 `SCECdErREAD` → 游戏收到错误、放弃视频 | 有界重试 / 失败补零返回成功（`make USB_READ_RETRY=N` 就是测这个） |
| **B. 静默零填充** | `bdm read ZERO-FILL: lsn=… 超出碎片表范围` 或 `bdm read ZERO-TAIL: lsn=… 只读到 x/y 块`（**本机新增**） | 读失败但请求超出碎片表范围 → OPL **返回成功、内容是 0**；游戏拿到一堆 0，解码器停住 → 不再读盘 | 说明碎片表范围/文件长度和游戏要读的位置对不上 |
| **C. 真的卡住** | `bdm read HUNG: lsn=… sectors=… (N ms and counting)` 每秒刷（**本机新增**） | `usbmass_bd` 里所有 `WaitSema` **都没有超时**，URB 回调没来就永远等；`bdm_io_sema` 一直占着 | 只能在 ps2sdk（usbmass_bd）里加超时+复位（你们之前试过 5 秒超时，后来回退了） |

**一次运行就能分开**：调试构建跑到停住，看最后十行是上面哪一种；如果三种都没有、只是安静下来，那就是数据层问题（§4）。

## 3. 优先做这两个实验

1. **看最后几行日志**（最重要，直接定案）。
   顺便注意启动时 `usbmass_bd` 打的那行 `%08x%08x %u-byte logical blocks`：确认你的 U 盘报告的是 512 还是 4096 字节逻辑块
   （4096 的话 OPL 会走 `DeviceReadSectorsGeneric_2` 那条带 `mediaLsnCount` 截断 + 零填充的分支，完全是另一条路，需要单独看）。
2. **有界重试能不能救回来**：
   ```bash
   make DEBUG=1 IOPCORE_DEBUG=1 USB_READ_RETRY=8
   ```
   * 能过 OP ⇒ 是签名 A（偶发读错误被游戏当致命），修法就是重试（可再加“连续失败才报错”）；
   * 照样停住、且日志里出现 A 的报错 ⇒ 重试也失败，说明是那块数据/那个位置稳定读不出来（驱动器层面），换盘/重拷镜像一试；
   * 日志干净、只有 ZERO-FILL/ZERO-TAIL ⇒ 签名 B，问题在碎片表范围与游戏读取位置的对应关系上。

## 4. 如果三种签名都没有（数据层）

那么读全部成功、数据也正确，是**游戏自己不再要数据**，需要往“汉化版这段视频的数据/时间戳”方向查：

```bash
python3 pc/ps2_iso_entries.py 汉化版.iso --file MOV.AFS --lba 574094 --compare 原版.iso
```
* 打出该 AFS 条目的**真实时长、平均码率**（扫 MPEG-2 pack 头）、日志 LBA 落在条目内什么位置；
* 两版对比：码率/时长是否一致（你已确认“几乎一样”，这条是量化确认）；
* 用它算出**停住时游戏读到条目的百分之几**（如果每次都停在同一个百分比，说明是那段码流本身的问题；如果百分比每次不同，说明是 I/O 时序）。

## 5. 想请你确认的两点

1. **按 Start 跳过之后，游戏能不能继续正常读盘/加载？**（这一条决定签名 A/B 还是签名 C ——
   如果跳过之后一切正常，就基本排除 C；如果跳过之后的下一次加载也卡，那就是 C，得在 ps2sdk 的 USB 驱动里加超时。）
2. **停住时画面是最后一帧冻住还是黑屏？**（冻住更像 A/B：播放器主动放弃；黑屏更像 C。）

## 6. 本次改动清单

| 文件 | 改动 |
|---|---|
| `modules/iopcore/cdvdman/device-bdm.c` | 调试构建：读看门狗 `bdm read HUNG`；单笔读 >200 ms 打 `bdm read slow`；读失败打 `bdm read failed` / `bdm read attempt N failed`；**新增零填充分支的 `ZERO-FILL` / `ZERO-TAIL` 打印**（这条路径返回成功但内容是 0）；`USB_READ_RETRY=N` 有界重试 |
| `Makefile`、`modules/iopcore/cdvdman/Makefile` | `USB_READ_RETRY=N` 开关（默认 0） |
| `modules/iopcore/cdvdfsv/ncmd.c` | 调试打印每个 EE 请求的 `readee: start / done(+err)` |
| `src/bdmsupport.c` | 启动打印 `BDM image: N fragments in M contiguous runs, K sectors` |
| `pc/ps2_iso_entries.py` | ISO9660/AFS 结构 + MPEG-PS 码率扫描，`--lba` 命中定位、`--compare` 两镜像对比 |

已回退：`SMB_BIGWND`（上一轮误判 SMB 时加的）。
保留但与本题无关：`cdvdman.c` 的 SMB 扇区缓存修复、`device-smb.c` 的 SMB 速率打印。

> 本机无 ps2sdk，C 代码只做了静态检查（括号/声明顺序），未编译。

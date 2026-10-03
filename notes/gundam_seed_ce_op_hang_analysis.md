# 高达SEED C.E. 汉化版：USB 模式 OP 开头停住 —— 重新定位

## 0. 现在已知的事实（含你这次补充的三条）

| # | 事实 | 来源 |
|---|---|---|
| 1 | 汉化版放 **HDD** 上 OP 正常 | 你实测 |
| 2 | USB 上跑到某处后**彻底不再读盘**（不是读得慢），画面停住 | 你实测 |
| 3 | 停住时游戏**没死**，按 Start 能正常跳过 | 你实测 |
| 4 | 用**没有 `REM1_MIN_US`** 的旧版本，问题照旧 | 你实测 |
| 5 | 两版动画**码率/大小几乎一样**，汉化版只是重新打包过 | 你指出 |
| 6 | 两版每轮 4 笔读的耗时相同（~101 ms），差别在轮间等待（0.636 s vs 0.232 s） | 日志 |

**由 1 + 4 + 5 可以直接排除的**：

* ❌ “汉化版码率高、数据更胖”（你已指出，且 HDD 能播）
* ❌ `rem1` 余数补时（没有它的版本照样复现）
* ❌ 镜像/数据损坏（HDD 上同一份数据能正常播完）

**由此收敛到的机制**：USB 上不是“供给不足”，而是**读请求在某个点断了**。
带宽不足只会让读取**变慢、继续读**；而现在是**不读了**，说明游戏的读循环在等一个永远不来的结果：

* (A) 某笔读**永远不返回**（卡在 USB 设备层内部，OPL 这条路径没有超时）→
  `sync_flag` 一直是 1 → 游戏 `sceCdSync` 永远等 → 不再发新的读请求 → Start 还能跳过（游戏主循环没死）。**完全符合现象 2+3。**
* (B) 某笔读**返回了错误**（碎片/边界/短读）→ 播放器把错误当成“读失败/数据结束”，停止读取、等玩家操作（Start 跳过）。**同样符合现象 2+3。**

两种情况都在 USB 链路、都不影响 HDD（HDD 走的是另一条设备与碎片来源：PFS 区块表 vs FAT 簇表），和事实 1 一致。

> 为什么不是“带宽不够”：日志里汉化版确实比原版取数快 2.7 倍（每轮 384 KB / 0.232 s），
> 但那只会让它**越播越卡、读盘继续**；而你的现象是**读盘停住**。所以带宽是次要因素，主因是 (A)/(B)。

---

## 1. 怎么区分 (A) 和 (B)：一次调试运行就够

```bash
make DEBUG=1 IOPCORE_DEBUG=1 TTY_APPROACH=UDP        # 或者你惯用的 TTY 方式
```

跑汉化版到停住，看日志的最后几行（本次新加/已有的打印）：

```
readee: start lsn=574096 sectors=64          ← 新增：EE 请求进来
bdm read: ...                                ← 已有（cdvdman_read）
bdm read slow: lsn=... took 5000 ms          ← 新增：单笔读超过 200ms 会打
bdm read HUNG: lsn=... sectors=... (7000 ms and counting)   ← 新增：看门狗每秒打一次
bdm read failed: lsn=... sectors=... rv=...  ← 新增：设备层读失败/短读
bdm read attempt 2 failed: sector=... blocks=...            ← 新增：重试（见 §2）
readee: done lsn=... sectors=... nbytes=... err=0/1/5        ← 新增：请求收尾（err 非 0 = 设备报错）
```

| 日志表现 | 结论 | 下一步 |
|---|---|---|
| 最后一行是 `bdm read: ...`，随后 `bdm read HUNG: ...` 每秒刷 | **(A) 卡死在设备层**（usbd/usbmass_bd 内部，无超时） | §3 |
| 出现 `bdm read failed: ...` / `readee: done ... err=≠0` | **(B) 读返回错误，游戏放弃读盘** | §2 |
| 两者都没有、`readee: done err=0` 一堆后停住 | 读都成功了，是**游戏自己**不再要数据（另有原因） | 见 §4 |

另外启动时会打一行（新增，用于判断碎片表是否和镜像对得上）：

```
BDM image: N fragments in M contiguous runs, K sectors (first @S)
```

把 `K` 和镜像实际扇区数（`python3 pc/ps2_iso_entries.py xxx.iso` 输出的“镜像大小 xxx 扇区”）对比：
**不相等就说明给到 IOP 的碎片表和镜像不一致** → USB 上后段的读会越界/失败，而 HDD 用的是另一套碎片来源（PFS 区块表）不受影响 —— 这和事实 1 完全吻合，是最值得先看的一条。

---

## 2. 如果是 (B)：读错误被当成致命

已经做好了开关，先做这个实验（不用等改代码）：

```bash
make USB_READ_RETRY=3        # BDM 读失败时重试 3 次（每次间隔 50ms），默认 0 = 不重试
```

* **能过 OP** ⇒ 确认是“偶发读错误 → 游戏不再读”；那正式修法就是给 USB 加**有界重试**，或像 SMB 那样在失败时**补零返回成功**（SMB 分支已经这么做过容错，USB 这边目前是一失败就返回 `SCECdErREAD`）。
* **照样停住** ⇒ 排除 (B)，看 (A)。

---

## 3. 如果是 (A)：读卡在设备层

OPL 这条链路上没有超时：`device-bdm.c → bd_defrag_read_cached_indexed() → usbmass_bd.irx → usbd.irx → OHCI`。
USB 传输的等待在 `usbd` 里（等 URB 完成），**如果设备/驱动不回完成，就会一直等**，`bdm_io_sema` 一直被持有 → 后面所有读都排不上 → 现象就是“不再读盘”。

这一层**不在本仓库里**（`usbmass_bd`/`usbd` 来自 ps2sdk），所以：

1. 先用 §1 的日志确认是卡在哪一笔（`bdm read HUNG: lsn=X sectors=Y`）；
2. 拿到那个 LBA 后，用脚本看它落在哪：
   ```bash
   python3 pc/ps2_iso_entries.py 汉化版.iso --file MOV.AFS --lba X --compare 原版.iso
   ```
   —— 如果正好在 AFS 条目边界、镜像末尾、或碎片表的某段边界上，就能反推出触发条件；
3. 把 X 换到原版镜像里对应位置做对照（原版不卡），看差异是“绝对位置”还是“相对条目位置”；
4. 真要修，只能改 ps2sdk 的 `usbmass_bd`/`usbd`（加超时/复位/重试），或者换一根 U 盘/读卡器把触发点躲开。

---

## 4. 如果是“读都成功、游戏自己不要数据了”

那就不是 I/O 停，而是**游戏看到的数据不对**（比如它以为读到了有效数据、但校验/序列头不对）而进入等待。
这种情况要点：

* 用 §1 的日志确认停住前的读全部 `err=0`；
* 检查汉化版镜像的 **PVD 卷大小 vs 实际扇区数**（脚本会打；改版镜像常见这里不一致）；
* 用 `--lba` 看停住位置是否落在某条 AFS 条目之外（重新打包时条目表和实际数据错位）；
* 同一个 ISO 在 **SMB/MX4SIO** 上跑一次对照（SMB 够 1.7 MB/s），如果 SMB 也停 → 和载体无关，是数据/结构问题。

---

## 5. 本次改动清单

| 文件 | 改动 |
|---|---|
| `modules/iopcore/cdvdman/device-bdm.c` | 调试构建：读看门狗（`bdm read HUNG`，5 秒起每秒一次）、单笔读超 200 ms 打 `bdm read slow`、读失败打 `bdm read attempt N failed`；`bdm_read_blocks()` 支持有界重试（`USB_READ_RETRY`）；发布版不建线程、零开销 |
| `modules/iopcore/cdvdman/Makefile`、`Makefile` | 新增 `USB_READ_RETRY=N` 开关（默认 0），例：`make USB_READ_RETRY=3` |
| `modules/iopcore/cdvdfsv/ncmd.c` | 调试构建：每个 EE 请求打 `readee: start ...` / `readee: done ... err=N`，用于确认卡住时是哪一笔请求没结束 |
| `src/bdmsupport.c` | 启动打印镜像在 FAT 上的碎片情况（碎片数/连续段数/总扇区），用于核对碎片表是否和镜像一致 |
| `pc/ps2_iso_entries.py` | （上一轮已加）ISO9660/AFS 检查 + MPEG-PS 码率扫描；本轮无改动 |
| `notes/gundam_seed_ce_op_hang_analysis.md` | 本文 |

**已回退**：`SMB_BIGWND` 全部还原（上一轮误判 SMB 时加的）。
**保留但与本题无关**：`cdvdman.c` 的 SMB 扇区缓存修复、`device-smb.c` 的 SMB 速率打印（可随时回退）。

> 说明：本机没有 ps2sdk，改动的 C 代码只做了静态检查（括号/声明顺序），**没有真正编译过**；
> 请在 `C:\GitHub\Open-PS2-Loader` 编译确认。

---

## 6. 还需要你回答两个问题

1. 你说的“未加入 `REM1_MIN_US` 的旧版本”是**本 fork 的旧提交**，还是 **官方 OPL 1.2.0**？
   —— 如果是官方 OPL 也能复现，说明与本 fork 的 BDM 碎片/索引改造无关，方向直接转到 ps2sdk 的 USB 驱动；
   如果只有本 fork 复现，重点查 `bdmGetFragmentList` 分页 + `bd_defrag` 索引这条新代码路径。
2. 停住时画面是**最后一张画面冻住**还是**黑屏**？
   —— 冻住更像 (B)（播放器不再解码），黑屏更像 (A)（等数据等到花掉）。

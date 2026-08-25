# PS2 实机 VIF UNPACK 硬件路径与排查方案

## 结论摘要

- 真实 PS2 上不存在 `recVifUnpk`、HashBucket 或 UNPACK 主机代码缓存；这些都是 PCSX2 的动态重编译实现。
- `UNPACK` 由 VIF0/VIF1 固定功能硬件状态机解释，只写 VU data memory。VU micro memory 由 `MPG` 写入，微程序由 `MSCAL`、`MSCALF` 或 `MSCNT` 启动/继续。
- gsKit 在本仓库中通过 GIF DMA 绘制 OPL 菜单，没有实现 VIF UNPACK；游戏启动后菜单与 gsKit 也不参与逐帧渲染。应单独排除的是 OPL 的 GSM、IGR、PADEMU、Cheat、IGS 等驻留功能，而不是 gsKit。
- “极慢、图形错误、约 90% 触发而偶尔正常”更符合 VIF1/VU1 双缓冲、DMA cache 一致性或 GIF/VU/VIF 同步竞争，而不符合统一增加 CDVD 请求延迟就能解决的问题。

## 真实硬件数据路径

```text
EE 内存中的 DMA chain
  -> EE DMAC channel 0/1
  -> VIF0/VIF1 FIFO
  -> VIFcode 硬件解码
  -> UNPACK 写 VU0/VU1 data memory
  -> MPG 写 VU micro memory（若包中存在）
  -> MSCAL/MSCNT 启动 VU
  -> VU1 XGKICK
  -> GIF path 1
  -> GS
```

VIF1 的 `DIRECT/DIRECTHL` 还可走 `GIF path 2`，不经过 VU1 运算。

DMAC 完成、VIF 完成、VU 完成和 GIF/GS 完成是四个不同时间点：

- `D1_CHCR.STR=0` 只表示 DMA 通道已把数据推进接收侧，不表示 VIF、VU 或 GIF 已完成。
- `VIF1_STAT.FQC=0` 只表示 FIFO 空，不排除 VIF 正在等待 VU、GIF 或 IRQ。
- VIF 空闲不表示已经启动的 VU1 微程序结束。
- VU1 结束也不必然表示 XGKICK 送出的 GIF 数据已被 GS 完全消费。

因此，不能以“DMA 完成”作为复用 VU1 目标双缓冲区的依据。

## UNPACK 编码和状态

VIFcode 是 32 位：

```text
31       IRQ
30..24   CMD
23..16   NUM
15..0    IMM
```

UNPACK 的 `CMD` 范围是 `0x60-0x7f`：

```text
CMD bit 4    M：启用 STMASK
CMD bit 3:2  VN：S / V2 / V3 / V4
CMD bit 1:0  VL：32 / 16 / 8 / 5
```

`IMM` 中：bit 15 是 FLG/TOPS 相对寻址，bit 14 是 USN，bit 9..0 是目标 QW 地址。`NUM=0` 表示 256，不是零。

UNPACK 是有状态操作。结果还取决于先前设置的：

- `STCYCL.CL/WL`：决定输入消费、目标推进、跳过与填充；8 位的 0 表示 256。
- `STMOD`：普通、Offset 或 Difference/累加模式。
- `STMASK`、`STROW`、`STCOL`：决定每个目标分量使用输入、ROW、COL 或保留旧值。
- VIF1 `BASE/OFFSET/DBF/TOPS`：决定带 FLG 的 UNPACK 写入哪一侧双缓冲。

这些寄存器不会在每次 UNPACK 后自动复位。一个遗漏或损坏的状态指令会污染后续多个 UNPACK。

## PCSX2 黄字的准确含义

`recVifUnpk: Bucket ... has ... micro-programs` 来自 PCSX2 的 VIF UNPACK 动态重编译缓存。这里的 “micro-programs” 是 PCSX2 为宿主 CPU 生成的 UNPACK 小函数/代码块，不是上传到 VU micro memory 的 PS2 微程序。

HashBucket 的键由 PCSX2 内部 `nVifBlock` 的 `num` 与 `upkType` 构成。因此这些黄字只能说明某些 UNPACK 参数组合生成了多个宿主版本；不能推出实机在“创建缓存”，也不能把 bucket 值当成 VU 地址、微程序地址或光盘扇区。

黄字仍然可作为“游戏进入了大量执行这些 UNPACK 组合的场景”的时间标记，但不能作为根因证据。

## 与 gsKit 和 OPL 的边界

本仓库 gsKit 的常规队列经 `DMA_CHANNEL_GIF` 提交，`gsVU1.c` 也是空壳；没有游戏 VIF1 DMA 包生成器。OPL 使用 gsKit 绘制菜单，`ExecPS2` 交接游戏后菜单代码不会参与游戏每帧渲染。

仍需逐项排除的 OPL 驻留功能：

- GSM：挂钩 GS 模式和寄存器设置，不直接执行 UNPACK，但可能改变 GS/GIF 下游状态。
- IGR、PADEMU、Cheat：可能保留 EE hook 或中断路径。
- IGS：截图路径会直接控制 VIF1 FIFO、`MSKPATH3`、`FLUSHA`、`DIRECT`、`FDR` 和 `BUSDIR`；默认非 extra-features 构建未启用，但 extra-features 构建必须单独排除。
- OPL 的 IOP 侧虚拟 CDVD/存储模块：可能提供了错误或时序异常的上游数据，但这需要 CRC/内容比较证明，不能从 UNPACK 黄字推断。

## 优先级最高的可证伪假设

1. **VIF1/VU1 双缓冲竞争**：`BASE/OFFSET/DBF/TOPS` 错误或切换时机不对，VIF1 正在覆盖 VU1 当前读取区。
2. **VIF/VU/GIF 同步缺口**：程序以 DMA 完成代替 VU/GIF 完成，或缺少正确的 `FLUSH/FLUSHA/MSCALF`。
3. **EE D-cache 一致性错误**：提交 DMA 前未正确 writeback，VIF 读到旧数据或混合数据。
4. **包边界或状态损坏**：DMA QWC、UNPACK NUM、STCYCL 与实际 payload 不一致，导致后续数据被当作 VIFcode。
5. **下游硬件反压**：错误 GIFtag、XGKICK 或 path 仲裁使 GS/GIF 阻塞，继而 VU1、VIF1、DMAC channel 1 依次停住。
6. **VIF IRQ/force-break 残留**：VIF 停在 interrupt stall、stop 或 force-break 状态且未正确恢复。

典型反压链：

```text
GS/GIF path 阻塞
  -> VU1 XGKICK 无法推进
  -> VU1 微程序无法结束
  -> VIF1 停在 MSCAL/FLUSH 或等待 VU
  -> VIF1 FIFO 填满
  -> DMAC channel 1 无法继续
  -> 游戏等待，表现为极端卡顿
```

## 分阶段实机排查

### 1. 先建立干净对照

- 同一台主机、同一镜像、同一存储后端，每次冷启动。
- 使用标准 OPL 构建，关闭 GSM、IGR、PADEMU、Cheat、VMC、IGS/extra-features 和所有非必要兼容选项。
- 如果具备原盘或其他不经过 OPL 的启动方式，做同场景对照。若原盘也失败，可直接排除 OPL、gsKit 与 OPL 存储驱动；若只有 OPL 失败，再继续拆分 EE 驻留功能和 IOP 数据路径。
- 因故障率约 90%，每个变体至少做多次冷启动，记录成功/失败次数，不能用单次“碰巧正常”判定修复。

### 2. 只抓故障瞬间的寄存器快照

使用预分配的内存环形缓冲，避免在热路径打印。成功和失败都采集：

- DMAC channel 1：`CHCR/MADR/QWC/TADR`，以及 `D_STAT`。
- VIF1：`STAT/FBRST/ERR/MARK/CYCLE/MODE/NUM/MASK/CODE/BASE/OFST/TOPS/ITOP/TOP/ROW/COL`。
- VU1：执行状态与当前 PC。
- GIF：`STAT/CTRL/MODE` 及 path tag/count 状态。

快速判别：

- `CHCR.STR=1` 且 FQC 长时间满：VIF1 不再消费。
- DMA 已结束但 FQC 非零：VIF1 尚未处理完或正在等待。
- FQC 为零但 `VEW/VGW/VIS/VSS/VFS` 有效：分别定位到 VU、GIF、IRQ、stop 或 force-break。
- VU1 长时间忙且 PC 不前进：微程序循环或 XGKICK 等待。
- `CODE` 指向 UNPACK 且 `NUM` 长时间不减：UNPACK 的硬件推进受阻。

### 3. 再捕获并离线解码 VIF1 DMA 包

每条记录保存时间戳、DMA 序号、MADR/QWC/TADR、DMA tag、源数据 CRC、完整 payload，以及提交前后的 BASE/OFFSET/TOPS/DBF。

离线解码器必须维护 CYCLE、MODE、MASK、ROW、COL、TOPS 状态，并计算每条 UNPACK 的实际输入字节数、目标 QW 范围、是否回绕、是否覆盖 VU1 当前读取区，以及 payload 结束位置是否准确落到下一条 VIFcode。

比较一次成功和一次失败，寻找第一条不同的 DMA 包或硬件状态，而不是从最后的图形错误倒推。

### 4. 最后做分层同步实验

每个版本只增加一个同步点：

1. DMA 提交前强制 writeback。
2. 下一缓冲提交前等待 VIF FIFO 清空。
3. 再等待 VIF 不处于 processing/wait。
4. 再等待 VU1 完成。
5. 必要时等待 GIF path 清空。

哪个同步点首次让故障消失，就把竞争定位到对应层次。不要再用统一的 CDVD 延迟代替这些分层同步。

## 资料来源

- Sony Computer Entertainment Inc., *EE Core User's Manual*：DMAC、VIF、VU、GIF 章节。
- [PS2SDK `vif_codes.h`](https://github.com/ps2dev/ps2sdk/blob/master/common/include/vif_codes.h)
- [PS2SDK `vif_registers.h`](https://github.com/ps2dev/ps2sdk/blob/master/common/include/vif_registers.h)
- [PS2SDK `packet2_vif.h`](https://github.com/ps2dev/ps2sdk/blob/master/ee/packet2/include/packet2_vif.h)
- [PS2SDK VU1 sample](https://github.com/ps2dev/ps2sdk/tree/master/ee/draw/samples/vu1)
- [gsKit EE/GS source](https://github.com/ps2dev/gsKit/tree/master/ee/gs)
- [PCSX2 `Vif_Unpack.cpp`](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/Vif_Unpack.cpp)
- [PCSX2 `Vif_Codes.cpp`](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/Vif_Codes.cpp)
- [PCSX2 `Vif_Transfer.cpp`](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/Vif_Transfer.cpp)
- [PCSX2 `Vif_HashBucket.h`](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/Vif_HashBucket.h)


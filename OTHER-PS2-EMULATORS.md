# PC 平台其他 PS2 模拟器与 OPL SMB2 验证

## 结论

PC 平台存在其他 PS2 模拟器，但目前没有一款能够完整替代 PCSX2 验证 OPL 的 DEV9/SMB2 游戏启动链路。

| 模拟器 | ELF/Homebrew | 与 PCSX2 的关系 | DEV9 以太网 | 对当前测试的价值 |
|---|---:|---|---:|---|
| Play! | 支持 | 独立实现 | 官方资料未提供可用后端 | 可测试 OPL ELF，不能验证 SMB2 |
| Iris | 支持 | 独立实现 | 官方功能列表未提供 | 可作独立交叉测试，但兼容性和速度仍有限 |
| DobieStation | 实验性支持 | 独立实现 | 未提供 | 不适合当前链路 |
| hpsx64 | 实验性 | 独立实现 | 没有成熟后端 | 不适合当前链路 |
| RetroArch LRPS2 | 支持 ELF | PCSX2 的分支 | 未暴露完整 DEV9 网络能力 | 不是独立交叉验证 |
| ARMSX2 | 支持 | PCSX2 的 ARM64 分支 | 继承 PCSX2 架构 | 不能排除 PCSX2 共性问题 |

## 建议

1. 可先用 Play! 或 Iris 直接运行 OPL ELF，观察是否也在二次启动阶段失败。
2. 这个结果只能辅助判断 OPL 的 EE/IOP 切换兼容性，不能验证 SMB2，因为缺少完整 DEV9 以太网链路。
3. SMB2 的 PC 端验证仍应使用 PCSX2；若 OPL 二次启动本身不可靠，应制作独立 SMB2 测试 ELF，直接测试 SMAP、PS2IP、libsmb2 和连续读取。
4. 最终的 OPL 二次启动兼容性仍需回到 PS2 实机确认。

## 官方来源

- [Play! 官方仓库](https://github.com/jpd002/Play-)
- [Iris 官方仓库](https://github.com/allkern/iris)
- [DobieStation 官方仓库](https://github.com/PSI-Rockin/DobieStation)
- [hpsx64 官方项目](https://sourceforge.net/projects/hpsx64/)
- [LRPS2 官方文档](https://docs.libretro.com/library/lrps2/)
- [ARMSX2 官方仓库](https://github.com/ARMSX2/ARMSX2)

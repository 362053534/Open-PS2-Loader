from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
syshook = (root / "ee_core/src/syshook.c").read_text()
iopmgr = (root / "ee_core/src/iopmgr.c").read_text()
frontend = (root / "src/system.c").read_text()
makefile = (root / "ee_core/Makefile").read_text()

# 主机无法执行 EE 系统调用；此处仅检查生产入口的接线与时序，不冒充实机测试。
assert "config->EnableRnC3UyaPatch = RnC3_IsGameID(filename);" in frontend
load_elf = syshook.split("void sysLoadElf(", 1)[1].split("static void unpatchEELOADCopy", 1)[0]
assignment = "config->RnC3UyaMultiplayer = config->EnableRnC3UyaPatch && RnC3_IsMultiplayerElf(filename)"
assert load_elf.index(assignment) < load_elf.index("New_Reset_Iop(NULL, 0)")

reset = iopmgr.split("static void ResetIopSpecial(", 1)[1].split("int New_Reset_Iop(", 1)[0]
assert re.search(
    r"if\s*\(config->RnC3UyaMultiplayer\)\s*LoadOPLModule\(OPL_MODULE_ID_IOP_PATCH,\s*0,\s*0,\s*NULL\)",
    reset,
)
assert reset.count("OPL_MODULE_ID_IOP_PATCH") == 1
patch_load = reset.index("LoadOPLModule(OPL_MODULE_ID_IOP_PATCH")
for module in ("SMSTCPIP", "SMAP", "USBD", "USBMASSBD", "SMBINIT", "ILINK", "MX4SIOBD"):
    assert patch_load < reset.index(f"LoadOPLModule(OPL_MODULE_ID_{module}")
assert "rc_uya.o" in makefile

print("PASS: frontend validates game ID; ELF scope updates before reset and gates early IOP patch loading")

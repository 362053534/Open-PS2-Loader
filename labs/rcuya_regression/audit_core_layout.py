import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
REFERENCES = {
    "good": "d05b5bcdb128b63b621689b0a3f4514f70e8147f",
    "regressed": "0b88f04cde06a34ee93023fd9773ac60aa2309b8",
}


def symbol_address(map_path, symbol):
    pattern = re.compile(r"^\s*(0x[0-9a-fA-F]+)\s+" + re.escape(symbol) + r"(?:\s|$)", re.MULTILINE)
    match = pattern.search(map_path.read_text())
    if match is None:
        raise RuntimeError(f"Missing {symbol} in {map_path}")
    return int(match.group(1), 16)


def report(label, map_path, output):
    address = symbol_address(map_path, "RnC3_AlwaysAllocMem")
    jal = 0x0C000000 | ((address >> 2) & 0x03FFFFFF)
    config = symbol_address(map_path, "g_ee_core_config")
    end = symbol_address(map_path, "_end_bss")
    print(f"{label}: helper=0x{address:08x} JAL=0x{jal:08x} config=0x{config:08x} end_bss=0x{end:08x}")
    if output:
        with open(output, "a") as stream:
            stream.write(f"{label}_helper=0x{address:08x}\n{label}_jal=0x{jal:08x}\n")
    return address


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--igs", type=int, choices=(0, 1), required=True)
    parser.add_argument("--pademu", type=int, choices=(0, 1), required=True)
    args = parser.parse_args()
    output = os.environ.get("GITHUB_OUTPUT")
    parent = ROOT / "tmp/rcuya_layout"
    parent.mkdir(parents=True, exist_ok=True)

    # 从已知提交导出临时副本，不能切换工作分支或把旧文件覆盖到当前源码树。
    with tempfile.TemporaryDirectory(dir=parent) as temp:
        for label, revision in REFERENCES.items():
            target = Path(temp) / label
            target.mkdir()
            archive = subprocess.Popen(["git", "archive", revision], cwd=ROOT, stdout=subprocess.PIPE)
            unpack = subprocess.run(["tar", "-x", "-C", str(target)], stdin=archive.stdout)
            archive.stdout.close()
            if archive.wait() or unpack.returncode:
                raise RuntimeError(f"Cannot export {revision}")

            log = Path(temp) / f"{label}.log"
            with log.open("w") as stream:
                result = subprocess.run(
                    ["make", "-C", str(target / "ee_core"), f"IGS={args.igs}", f"PADEMU={args.pademu}"],
                    stdout=stream,
                    stderr=subprocess.STDOUT,
                )
            map_path = target / "ee_core/ee_core.map"
            # 旧版全功能组合已知越界，但链接 map 仍可用于比较补丁目标地址。
            known_overflow = label == "good" and args.igs == 1 and args.pademu == 1
            if result.returncode and not (known_overflow and "not within region `ram84'" in log.read_text()):
                print(log.read_text())
                raise RuntimeError(f"Cannot build {revision}")
            report(label, map_path, output)

    report("current", ROOT / "ee_core/ee_core.map", output)


if __name__ == "__main__":
    main()

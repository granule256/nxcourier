#!/usr/bin/env python3
"""门禁：错误文案（`fail` / `failf`）的长度检查。

## 为什么要它

`Reply::errDetail_` 只有 **256 字节**（`nxc_sdk.hpp`），草案里写超了会被 `snprintf` **静默截断** ——
而错误文案恰恰是"调用方（尤其是 AI）唯一能拿到的东西"：被截断就等于最后那句话消失。
2026-10-06 晚我在同一天里撞了两次（`title.launch` / `title.terminate` 的提示），
所以把它固化成一道门禁。

★ 中文一个字 **3 字节** ⇒ 大约 60~70 个汉字就到头了。

## 判据

* `> 250 字节` ⇒ **报错**（离上限只剩 5 字节，几乎肯定会截断）
* `> 220 字节` ⇒ **警告**（离上限太近，加一个参数就超）

占位符按"最坏情况"估：
`%s` 当作 32 字节（本平台最长的口令 `I-KNOW-THIS-WRITES-MEMORY` 是 25）·
`%.64s` 当作 64 · `%lld` 当作 20 · `%08X` 当作 10 · 其余整数格式当作 12。

## 跑法

    python3 device/platform/tests/check_error_msgs.py
退出码非 0 表示有超限项（可直接当 CI 门禁用）。
"""
import re
import sys
from pathlib import Path

LIMIT = 256
HARD = 250
WARN = 220

PLACEHOLDER_BYTES = {
    "s": 32, "64s": 64, "60s": 60, "40s": 40,
    "lld": 20, "llu": 20, "llX": 16, "X": 10, "08X": 10, "d": 12, "u": 12,
}

SRC = Path(__file__).resolve().parent.parent / "source"
CALL_RE = re.compile(r"\b(?:r|reply)\.fail[f]?\s*\(")


def scan_call(text: str, open_paren: int) -> str:
    """从 '(' 开始扫到配对的 ')'，把里面的双引号字符串按顺序拼起来。"""
    out, i, depth = [], open_paren, 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == '"':
            i += 1
            buf = []
            while i < n and text[i] != '"':
                if text[i] == "\\":          # 转义（\\ 或 \"）
                    buf.append(text[i + 1] if i + 1 < n else "")
                    i += 2
                    continue
                buf.append(text[i])
                i += 1
            out.append("".join(buf))
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                break
        i += 1
    return "".join(out)


def est_bytes(literal: str) -> int:
    """按最坏情况估算实际写进缓冲的字节数。"""
    total = 0
    i = 0
    while i < len(literal):
        if literal[i] == "%" and i + 1 < len(literal):
            m = re.match(r"%[-0-9.]*([a-zA-Z]+)", literal[i:])
            if m:
                spec = m.group(1)
                total += PLACEHOLDER_BYTES.get(spec, 12)
                i += m.end()
                continue
            if literal[i + 1] == "%":        # 字面的 %
                total += 1
                i += 2
                continue
        total += len(literal[i].encode("utf-8"))
        i += 1
    return total


def main() -> int:
    problems, warns, checked = [], [], 0
    files = sorted(SRC.glob("tools/tool_*.cpp")) + sorted(SRC.glob("core/*.cpp"))
    for path in files:
        text = path.read_text(encoding="utf-8")
        for m in CALL_RE.finditer(text):
            literal = scan_call(text, m.end() - 1)
            if not literal:
                continue
            checked += 1
            size = est_bytes(literal)
            line = text[: m.start()].count("\n") + 1
            where = f"{path.name}:{line}"
            if size > HARD:
                problems.append((size, where, literal))
            elif size > WARN:
                warns.append((size, where, literal))

    print(f"检查了 {checked} 条错误文案（上限 {LIMIT} 字节 / 报错线 {HARD} / 警告线 {WARN}）\n")
    for size, where, lit in sorted(problems, reverse=True):
        print(f"  x {size:>4}B  {where}\n      {lit[:120]}")
    for size, where, lit in sorted(warns, reverse=True):
        print(f"  ! {size:>4}B  {where}  （接近上限，注意）")
    if not problems and not warns:
        print("  全部在上限内 ✓")
    print()
    if problems:
        print(f"{len(problems)} 条超过上限，会被静默截断 ⇒ 必须改短或拆开")
        return 1
    if warns:
        print(f"{len(warns)} 条接近上限（不失败，但建议留意）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

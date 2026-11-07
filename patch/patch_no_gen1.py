#!/usr/bin/env python3
"""patch_no_gen1.py —— 阻止驱动在 cuInit 时把链路降回 Gen1

## 结论前置（reports/199 实测）

perf 四点断点，一次 `minictx`（1 个 CUDA 上下文）：

```
entry     0x127d60  1 次   DX=0x1                       ← mode = 1
or80000   0x127e65  1 次   AX 0x20505800                ← 清掉 SPEED[19:18]
wr_shared 0x127edd  1 次   R8=0x20585800                ← ★ SPEED=2 真的写进去了
bailret   0x1280c8  1 次
```

⇒ **SPEED=2 是软件写的，不是端点内部回写**。
  （reports/195/197 的"CPU 不可见"结论作废：那次断点是没生效。）

## 为什么改「范围检查边界」比 A/B/C 都干净

`_nv017105rm(dev, obj, mode)` 入口是一段白名单检查：

```asm
127d66: 8d 42 ff        lea  -0x1(%rdx),%eax     ; eax = mode - 1
127d81: 83 f8 02        cmp  $0x2,%eax
127d84: 0f 87 3e 03 00 00  ja 1280c8             ; (mode-1) > 2 → bail（即 mode ∉ {1,2,3}）
```

把它平移成 **`mode - 2` / `cmp $0x1`**：

```asm
127d66: 8d 42 fe        lea  -0x2(%rdx),%eax     ; eax = mode - 2
127d81: 83 f8 01        cmp  $0x1,%eax
127d84: 0f 87 ...       ja   1280c8              ; (mode-2) > 1 → bail
```

新的 bail 条件（无符号）：

| mode | mode-2 | > 1 ? | 结果 |
|---|---|---|---|
| 1 | `0xFFFFFFFF`（下溢） | 是 | **bail** ★ 唯一的行为变化 |
| 2 | 0 | 否 | 继续（Gen2） |
| 3 | 1 | 否 | 继续（Gen3） |
| ≥4 | ≥2 | 是 | bail（**与原来完全一致**） |

⇒ **唯一的行为差异就是 mode==1 从"执行"变成"bail"**，其余位、其余分支、其余 mode 全部不变。

### 为什么 bail 是安全的
- `1280c8` 是函数**唯一**的 return 点：`pop %rbx; mov %r12d,%eax; pop×4; add $0x40,%rbp; ret`
- `%r12d` 在 entry 就被 `xor %r12d,%r12d` 置 0，而 mode=1 路径里对 `%r12` 的改写
  （`lea 0x1e08(%r13),%r12`）在我们跳过的代码里
- ⇒ bail 返回 **0 = CUDA_SUCCESS**，驱动认为设置成功，不会报错、不会重试

## 与 A/B/C 的对比

| 方案 | 改什么 | 评价 |
|---|---|---|
| A 入口加 `cmp $1,%edx; je ret` | 需要 code cave | 该函数没有空隙可塞 5 字节；且要处理 CET/IBT |
| B 改 `je .mode1_case` 目标 | —— | **不存在这条指令**：mode==1 是**顺序落入**，不是跳转过来的 |
| C nop 掉 `or $0x80000` | 1 条指令 | 会落到 `movl $0x1,0xc(%rbp)`（改存储的 mode），语义不清 |
| **本方案** | **2 个字节** | 边界平移，白名单收窄，零附带影响 |

## 用法
  sudo python3 patch_no_gen1.py check
  sudo python3 patch_no_gen1.py apply
  sudo python3 patch_no_gen1.py revert

⚠ apply 后必须重载 nvidia 模块（或重启）才生效。
"""
import hashlib
import os
import shutil
import sys

KO = "/lib/modules/%s/updates/dkms/nvidia.ko" % os.uname().release
BAK = KO + ".orig_nogen1"

# ★ 唯一签名：从 entry 的 lea 到 ja 的完整 32 字节序列（全文件仅 1 处）
SIG = bytes.fromhex(
    "8d42ff"          # lea  -0x1(%rdx),%eax
    "4156"            # push %r14
    "4155"            # push %r13
    "4989fd"          # mov  %rdi,%r13
    "4154"            # push %r12
    "4531e4"          # xor  %r12d,%r12d
    "53"              # push %rbx
    "4883ed40"        # sub  $0x40,%rbp
    "48897510"        # mov  %rsi,0x10(%rbp)
    "89550c"          # mov  %edx,0xc(%rbp)
    "83f802"          # cmp  $0x2,%eax
    "0f87")           # ja   rel32

# 补丁后的形态（用于 check/重复 apply 判定）
SIG_NEW = (SIG[:2] + bytes([0xFE]) +        # lea -0x1 → lea -0x2
           SIG[3:0x1D] + bytes([0x01]) +    # cmp $0x2 → cmp $0x1
           SIG[0x1E:])

OFF_LEA = 2      # lea 的 imm8 位置（字节 ff → fe）
OFF_CMP = 0x1D   # cmp 的 imm8 位置（字节 02 → 01）

ORIG_LEA, NEW_LEA = 0xFF, 0xFE
ORIG_CMP, NEW_CMP = 0x02, 0x01


def md5(p):
    h = hashlib.md5()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def find_all(data, pat):
    out, i = [], 0
    while True:
        i = data.find(pat, i)
        if i < 0:
            return out
        out.append(i)
        i += 1


def main():
    act = sys.argv[1] if len(sys.argv) > 1 else "check"
    if os.geteuid() != 0:
        print("需要 root"); return 1
    if not os.path.exists(KO):
        print(f"找不到 {KO}"); return 1

    print(f"目标 {KO}")
    print(f"md5  {md5(KO)}")
    with open(KO, "rb") as f:
        data = f.read()
    print(f"大小 {len(data):,}\n")

    if act == "revert":
        if not os.path.exists(BAK):
            print(f"没有备份 {BAK}"); return 2
        shutil.copy2(BAK, KO)
        print(f"已还原，md5 {md5(KO)}")
        return 0

    # ---- 定位 ----
    orig_hits = find_all(data, SIG)
    new_hits = find_all(data, SIG_NEW)
    total = len(orig_hits) + len(new_hits)
    print(f"签名命中：原始形态 {len(orig_hits)} 处，已补丁形态 {len(new_hits)} 处")
    if total != 1:
        print("!! 签名不唯一或找不到，拒绝操作")
        for h in orig_hits[:10]:
            print(f"   原始 @ {h:#x}")
        for h in new_hits[:10]:
            print(f"   已补丁 @ {h:#x}")
        return 2

    already = len(new_hits) == 1
    base = new_hits[0] if already else orig_hits[0]
    print(f"  定位到 {base:#x}  （对应 .text 节内偏移 {base - 0x40:#x}）")

    cur_lea, cur_cmp = data[base + OFF_LEA], data[base + OFF_CMP]
    print(f"  lea imm8 @ {base + OFF_LEA:#x} = {cur_lea:#04x}   （原 {ORIG_LEA:#04x} / 补丁 {NEW_LEA:#04x}）")
    print(f"  cmp imm8 @ {base + OFF_CMP:#x} = {cur_cmp:#04x}   （原 {ORIG_CMP:#04x} / 补丁 {NEW_CMP:#04x}）")

    if act == "check":
        if already:
            print("\n[已打补丁] mode==1 会直接 bail，驱动不再把链路降回 Gen1")
            print("  ⚠ 需重载 nvidia 模块才生效")
        else:
            print("\n[未打补丁] 当前 mode==1 会写 SPEED=2 → 链路降回 2.5GT/s")
            print("  补丁后：mode ∈ {1} → bail；{2,3} 照旧；≥4 照旧")
        return 0

    if act == "apply":
        if already:
            print("\n已打过补丁"); return 0
        if cur_lea != ORIG_LEA or cur_cmp != ORIG_CMP:
            print(f"\n!! 字节异常（lea={cur_lea:#04x} cmp={cur_cmp:#04x}），拒绝打补丁")
            return 2
        if not os.path.exists(BAK):
            shutil.copy2(KO, BAK)
            print(f"\n已备份 → {BAK}")
        with open(KO, "r+b") as f:
            f.seek(base + OFF_LEA); f.write(bytes([NEW_LEA]))
            f.seek(base + OFF_CMP); f.write(bytes([NEW_CMP]))
        with open(KO, "rb") as f:
            nd = f.read()
        print(f"已写入: {nd[base:base+len(SIG)].hex(' ')}")
        print(f"新 md5: {md5(KO)}")
        print("\n下一步：rmmod nvidia_uvm nvidia_modeset nvidia_drm nvidia → modprobe nvidia")
        return 0

    print(f"未知动作 {act}")
    return 1


if __name__ == "__main__":
    sys.exit(main())

# P104-100 PCIe Gen2 解锁（Linux）

> NVIDIA P104-100 矿卡（GP104，`10de:1b87`）在 Linux 下的 PCIe Gen1 x4 (0.78 GB/s) → **Gen2 x4 (1.55 GB/s)** 解锁。
> 本机完整验证：Ubuntu 24.04 / kernel 6.11.0-1025-oem / NVRM 580.178.04 / CPU 直连 x16 槽 / Lenovo 台式机。
> 实测 pinned H2D **0.78 → 1.547 GB/s**，D2H **0.85 → 1.701 GB/s**，全程 Xid 0。

---

## 方案三件套（缺一不可）

| 组件 | 作用 | 载体 |
|---|---|---|
| **① Registry 键** `RmForceEnableGen2=1;RMPcieLinkSpeed=0x1` | 让**驱动自己**在 devinit 里（芯片复位后、PCIe 核心锁存前的时序窗口）解锁声明 → CAP2 锁存 0x06 且**跨复位保持** | `99-p104-gen2.conf` → modprobe.d |
| **② 2 字节补丁** | 拦截 `_nv017105rm(mode=1)`，cuInit 不再把链路降回 Gen1 | nvidia.ko（.ko/.zst/initramfs 三处同步）|
| **③ gen2kr.ko + 开机推送** | probe-return 窗口（唯一安全时机）写 SPEED + TLS + Retrain → 链路训练到 Gen2 | systemd 服务 `p104-gen2-push.service` |

**为什么必须三件套**（本机踩坑总结）：

- 只有 ③：CAP2 是 0x02，TLS 被钳到 Gen1，链路升不上去（WJNKAC 的原版方法在 CPU 直连槽上的失败点）
- ①+③：CAP2=0x06 但 cuInit 会把 SPEED 写回 Gen1 → 链路 40 秒内掉回
- ①+②：声明解锁但没有推送，链路永远停在 Gen1
- **只有 ①②③ 齐备**：声明解锁 + 推送 + 不降级 = Gen2 常驻

## 安装

```bash
sudo ./scripts/install.sh      # 一键：编译 + 补丁 + conf + 服务 + 立即激活
sudo reboot                    # 可选（install.sh 已做立即激活；重启验证开机自动化）
```

前提：`linux-headers-$(uname -r)`、`gcc`、`python3`、`zstd`、NVIDIA 驱动 580.178.04、
P104-100 在 `01:00.0`、Secure Boot 关闭（补丁模块无签名）、GPU 空闲无占用。

## 验证

```bash
./scripts/status.sh            # 一键状态：寄存器 + nvidia-smi + 组件 + 带宽
./bwtest2 512 15 | grep pinned # 带宽（必须 512MB×15，短测会踩恢复窗口）
```

预期：pinned H2D ~1.55 / D2H ~1.70 GB/s；`nvidia-smi` 报 `pcie.link.gen.current = 2`。

## 卸载

```bash
sudo ./scripts/uninstall.sh    # 还原原始 nvidia.ko.zst，删除全部组件
```

## 目录结构

```
99-p104-gen2.conf          registry 键（装到 /etc/modprobe.d/p104-gen2.conf）
module/gen2kr.c            内核模块源码（probe-return 推送 + 守卫）
module/Makefile
scripts/install.sh         一键安装
scripts/uninstall.sh       一键卸载
scripts/status.sh          状态一览
scripts/p104-gen2-push.sh  推送脚本（服务调用）
scripts/p104-gen2-push.service
patch/patch_no_gen1.py     2 字节补丁工具（check/apply/revert）
bwtest2 / minictx          带宽测试 / cuInit 探针（附源码）
```

## 本机实测纪律（重要，全部用掉卡换来的）

1. **probe 完成后的运行期写 `0x8C040.SPEED` = 掉卡**（BAR0 立即 ffffffff）。
   推送只能在 probe-return 窗口（gen2kr 的 kretprobe），绝不能做成事后脚本。
2. **`0x8841C.bit15`（覆盖武装位）跨芯片复位存活**：驱动 devinit 置位后，
   之后的每次复位 PCIe 核都会带着解锁重新锁存 CAP2=0x06 —— 这就是 registry 键方案的可持续性来源。
3. **反复 rmmod/modprobe 会进锁死态**（CAP2 恒 0x02 + TLS 恒 1，写什么都无效），
   只能整机重启清（报告 198 §7）。装好本方案后不需要反复 reload。
4. **`0x8C040` 的两个“死亡写入”**：
   - 运行期写 SPEED 位（E5）→ 掉卡；probe-return 窗口写 SPEED → ✅ 唯一安全时机；
   - **bit31（x16 声明门）在任何时机写都掉卡**（E9 运行期 / E10 probe-return 各一次实测）。
     ⇒ x16 声明在本机不可行，lane 4-15 补电容路线随之关闭。
5. `disable→enable`（PCI_COMMAND bit1）在驱动活动期触发 Xid 79（E5 100ms / E7 100ms）。
   2ms 窗口虽不触发 Xid 但也不产生锁存 —— 此路不通，勿再试。
6. **热重启能从掉卡态恢复，但保险起见优先断电冷启动**（Xid 154“GPU Reset Required”
   状态在热重启后仍有残留风险，冷启动彻底清零）。
5. `RMPcieLinkSpeed` 必须 `=1`（`=2` 会把 TLS 钳死）；`RmEnableGen2` 键在 580 驱动里不存在。
6. DKMS 重编 / 驱动升级会**静默覆盖补丁**（症状：带宽回 0.78）—— 重跑 `install.sh` 即可。
7. **int8_convrot 底模的 LoRA 配 GGUF 模型需要重映射键名**：这类 LoRA 的 MLP 键是融合
   的 `gate_up`（每块 2 键），GGUF 模型是拆开的 `gate_layer`/`proj` —— 直接挂会报 64 条
   "lora key not loaded"，LoRA 只剩部分效果。用 `scripts/lora_gguf_remap.py` 转换后即全量生效
   （拆分顺序 [gate; up] 依据 `comfy/ldm/qwen_image21/model.py` 源码）。

## 原理速记

P104 vBIOS/OTP 把 PCIe 声明限制在 Gen1（`LnkCap2=0x02`），硬件拒绝对未声明的速率训练。
`0x8841C.PRIV_MISC_1` 的 bit15(武装)/13/11(使能)/14/12(禁用) 是声明覆盖寄存器，
`LINK_CAP2 (0x880A4)` 是它的镜像。锁存有两个条件：覆盖位就位 + 在正确时机被 PCIe 核
读取 —— 后者只在驱动 devinit 内部成立（本机 CPU 直连槽），registry 键正是利用驱动
自己的 devinit 完成写入。Gen3 是 OTP 熔丝锁死，任何软件手段无解（详见上游项目边界文档）。

## 致谢与来源

- **WJNKAC/nvidia-p104-unlock-gen2-for-linux**：2 字节补丁、gen2kr、寄存器序列、大量实测纪律
- **amoghmunikote/cmpunlocker**（CMP 170HX）：`RmForceEnableGen2=1;RMPcieLinkSpeed=0x1`
  生产配置 —— 本方案的核心突破点（该键让驱动原生解锁声明，解决了 CPU 直连槽
  "锁存只发生在 probe 前、probe 又必然抹除"的死锁）
- **skbskhud/p104-unlock-v3**（Windows）：CPU 直连槽需要"复位后锁存"的现象记录与寄存器语义

## 免责

仅供研究自己拥有的硬件。所有写入均为易失寄存器/VBIOS 无关，断电即恢复。
不担保适用性；驱动更新/DKMS 重编会使补丁失效（重跑 install.sh）。

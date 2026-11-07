#!/bin/bash
# status.sh —— P104 Gen2 解锁状态一览（只读）
set -u
PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONF=/etc/modprobe.d/p104-gen2.conf

[ "$(id -u)" = "0" ] || { echo "需要 root"; exit 1; }

echo "=== 链路寄存器（硬件真相）==="
python3 -c "
import mmap, os, struct
p = int(open('/sys/bus/pci/devices/0000:01:00.0/resource').readline().split()[0], 16)
f = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(f, 0x1000000, mmap.MAP_SHARED, mmap.PROT_READ, offset=p)
cfg  = struct.unpack_from('<I', m, 0x8841C)[0]
cap2 = struct.unpack_from('<I', m, 0x880A4)[0]
lsta = struct.unpack_from('<I', m, 0x88088)[0]
lcfg = struct.unpack_from('<I', m, 0x8C040)[0]
m.close(); os.close(f)
spd = (lsta>>16)&0xF
print(f'  CAP2=0x{cap2:08x}（0x06=声明已解锁）')
print(f'  CurSpeed={spd}（2=Gen2）')
print(f'  SPEED={(lcfg>>18)&3}（1=5GT/s 请求）')
print(f'  判定: {\"✓ Gen2 运行中\" if spd==2 and cap2==6 else \"✗ 未在 Gen2\"}')"

echo
echo "=== lspci ==="
lspci -vv -s 01:00.0 2>/dev/null | grep -E 'LnkSta' | sed 's/^/  /'

echo
echo "=== nvidia-smi ==="
nvidia-smi --query-gpu=name,pcie.link.gen.current,pcie.link.width.current --format=csv,noheader 2>&1 | head -1 | sed 's/^/  /'

echo
echo "=== 组件状态 ==="
echo -n "  registry conf: "; grep -q RmForceEnableGen2 "$CONF" 2>/dev/null && echo "✓ 在位" || echo "✗ 缺失 ($CONF)"
echo -n "  nvidia 补丁:    "
KO=/lib/modules/$(uname -r)/updates/dkms/nvidia.ko
if [ -f "$KO" ]; then
    python3 -c "
import sys
data = open('$KO','rb').read()
sig = bytes.fromhex('8d42fe415641554989fd41544531e4534883ed404889751089550c83f8010f87')
print('✓' if data.find(sig)>=0 else '✗ 未打补丁（DKMS 可能覆盖了它，重跑 install.sh）')"
else
    echo "（.ko 不存在，检查 .zst: $(zstd -dc /lib/modules/$(uname -r)/updates/dkms/nvidia.ko.zst 2>/dev/null | python3 -c "
import sys
data = sys.stdin.buffer.read()
sig = bytes.fromhex('8d42fe415641554989fd41544531e4534883ed404889751089550c83f8010f87')
print('✓ 含补丁' if data.find(sig)>=0 else '✗ 未打补丁')") )"
fi
echo -n "  gen2kr 模块:    "; lsmod | grep -q '^gen2kr' && echo "✓ 已加载" || echo "未加载（服务运行时才有）"
echo -n "  推送服务:       "; systemctl is-enabled p104-gen2-push.service 2>/dev/null || echo "未启用"
echo
echo "=== 最近推送日志 ==="
tail -5 /var/log/p104-gen2-push.log 2>/dev/null | sed 's/^/  /'

echo
echo "=== 带宽速测（约 30 秒）==="
"$PKG_DIR/bwtest2" 512 15 2>&1 | grep pinned | sed 's/^/  /'

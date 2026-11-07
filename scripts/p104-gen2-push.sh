#!/bin/bash
# p104-gen2-push —— 推送 P104 链路到 Gen2
# 原理: insmod gen2kr（kretprobe 挂 nv_pci_probe）→ unbind/bind 触发 probe
#       → gen2kr 在 probe 返回瞬间写 SPEED + TLS + Retrain → 链路升 Gen2
# ★ 时序纪律: probe-return 是唯一安全的 SPEED 写入窗口；
#   probe 完成后的运行期写 SPEED 会掉卡（实测）。
# 由 p104-gen2-push.service 开机自动调用；也可手动 root 运行。
set -u

KVER=$(uname -r)
GEN2KR="$(dirname "$(readlink -f "$0")")/../module/gen2kr.ko"
[ -f "$GEN2KR" ] || GEN2KR=/lib/modules/$KVER/updates/gpu-unlock/gen2kr.ko
DEV=0000:01:00.0
LOG=/var/log/p104-gen2-push.log

exec >> "$LOG" 2>&1
echo "=== p104-gen2-push 开始: $(date) ==="

read_speed() {
    python3 -c "
import mmap, os, struct
p = int(open('/sys/bus/pci/devices/0000:01:00.0/resource').readline().split()[0], 16)
f = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(f, 0x1000000, mmap.MAP_SHARED, mmap.PROT_READ, offset=p)
lsta = struct.unpack_from('<I', m, 0x88088)[0]
m.close(); os.close(f)
print((lsta>>16)&0xF)" 2>/dev/null || echo 0
}

# 1. 等 nvidia 就绪（最多 30 秒）
for i in $(seq 1 30); do
    [ -e /proc/driver/nvidia/version ] && break
    sleep 1
done
if [ ! -e /proc/driver/nvidia/version ]; then
    echo "✗ nvidia 未就绪，放弃"; exit 1
fi
echo "nvidia 就绪 (第 ${i}s)"

# 2. 已是 Gen2 就直接退出（幂等）
if [ "$(read_speed)" = "2" ]; then
    echo "已是 Gen2，无需推送"; echo "=== 结束 ==="; exit 0
fi

# 3. 占用检查（unbind 卡死防护）
OCC=$(fuser /dev/nvidia* 2>/dev/null | tr -s ' ')
if [ -n "$OCC" ]; then
    echo "⚠ GPU 被占用 ($OCC)，跳过推送"; exit 1
fi

# 4. insmod gen2kr
if [ ! -f "$GEN2KR" ]; then echo "✗ 找不到 $GEN2KR"; exit 1; fi
if ! lsmod | grep -q '^gen2kr'; then
    insmod "$GEN2KR" && echo "gen2kr 已加载" || { echo "✗ insmod 失败"; exit 1; }
else
    echo "gen2kr 已在内存"
fi
sleep 1

# 5. unbind/bind 触发 probe
if [ -d "/sys/bus/pci/drivers/nvidia/$DEV" ]; then
    echo "$DEV" > /sys/bus/pci/drivers/nvidia/unbind 2>&1
    sleep 1
    echo "$DEV" > /sys/bus/pci/drivers/nvidia/bind 2>&1
    echo "probe 已触发"
else
    echo "⚠ 设备未绑定 nvidia，尝试仅 bind"
    echo "$DEV" > /sys/bus/pci/drivers/nvidia/bind 2>&1
fi

# 6. 等 Gen2（最多 15 秒）
GEN2=0
for i in $(seq 1 15); do
    sleep 1
    S=$(read_speed)
    echo "[${i}s] CurSpeed=$S"
    if [ "$S" = "2" ]; then GEN2=1; break; fi
done

if [ "$GEN2" = "1" ]; then
    echo "★ 成功: 链路已升到 Gen2 ($(date))"
else
    echo "✗ 未升到 Gen2 ($(date))；dmesg 最近 gen2kr 日志:"
    dmesg | grep gen2kr | tail -5
fi
echo "=== 结束 ==="
exit $([ "$GEN2" = "1" ] && echo 0 || echo 1)

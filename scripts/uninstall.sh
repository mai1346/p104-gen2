#!/bin/bash
# uninstall.sh —— 完全卸载，恢复原厂状态
set -u
KVER=$(uname -r)
KO_DIR=/lib/modules/$KVER/updates/dkms
CONF=/etc/modprobe.d/p104-gen2.conf

echo "=============================================="
echo " P104 Gen2 解锁卸载"
echo "=============================================="
[ "$(id -u)" = "0" ] || { echo "需要 root"; exit 1; }

echo
echo "--- 1. 停止服务并卸载模块 ---"
systemctl disable --now p104-gen2-push.service 2>/dev/null
rmmod gen2kr 2>/dev/null && echo "  gen2kr 已卸载" || echo "  gen2kr 未加载"

echo
echo "--- 2. 卸载驱动（以便恢复原始模块）---"
OCC=$(fuser /dev/nvidia* 2>/dev/null | tr -s ' ')
[ -n "$OCC" ] && { echo "✗ GPU 被占用 ($OCC)，先停掉再卸载"; exit 1; }
rmmod nvidia_uvm 2>/dev/null; rmmod nvidia_drm 2>/dev/null
rmmod nvidia_modeset 2>/dev/null; rmmod nvidia 2>/dev/null

echo
echo "--- 3. 恢复原始 nvidia.ko.zst ---"
if [ -f "$KO_DIR/nvidia.ko.zst.orig_backup" ]; then
    cp -av "$KO_DIR/nvidia.ko.zst.orig_backup" "$KO_DIR/nvidia.ko.zst"
    rm -fv "$KO_DIR/nvidia.ko.zst.orig_backup" "$KO_DIR/nvidia.ko" "$KO_DIR/nvidia.ko.orig_nogen1"
    ok "已还原原始 .zst，删除补丁版"
else
    echo "  ⚠ 无备份可还原（可能从未打补丁或已还原）"
fi

echo
echo "--- 4. 移除组件 ---"
rm -rfv /lib/modules/$KVER/updates/gpu-unlock 2>/dev/null | sed 's/^/  /'
rm -fv /etc/modprobe.d/p104-gen2.conf /etc/modprobe.d/gen2-regkey-test.conf | sed 's/^/  /'
rm -fv /usr/local/sbin/p104-gen2-push.sh | sed 's/^/  /'
rm -fv /etc/systemd/system/p104-gen2-push.service | sed 's/^/  /'
systemctl daemon-reload
rm -fv /etc/initramfs-tools/hooks/gen2early /etc/modprobe.d/gen2early.conf 2>/dev/null
sed -i '/^gen2early$/d' /etc/initramfs-tools/modules 2>/dev/null
depmod -a $KVER 2>/dev/null
update-initramfs -u -k $KVER 2>&1 | tail -1 | sed 's/^/  /'

echo
echo "--- 5. 重新加载原厂驱动 ---"
modprobe nvidia && modprobe nvidia_uvm
sleep 3
nvidia-smi --query-gpu=name --format=csv,noheader 2>&1 | head -1 | sed 's/^/  GPU: /'

echo
echo "=============================================="
echo " 卸载完成。重启后回到完全原厂状态（Gen1）。"
echo "=============================================="

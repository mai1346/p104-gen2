#!/bin/bash
# install.sh —— P104-100 PCIe Gen2 解锁一键安装
#
# 做什么:
#   1. 编译 gen2kr 内核模块（probe-return 推送）
#   2. 给 nvidia.ko 打 2 字节补丁（防 cuInit 降级），.ko/.zst/initramfs 三处同步
#   3. 安装 registry 键 conf（RmForceEnableGen2 → 驱动 devinit 自解锁声明）
#   4. 安装 gen2kr.ko + 推送脚本 + systemd 服务（开机自动推送）
#
# 用法: sudo ./install.sh
# 重启后自动生效。
set -u

PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KVER=$(uname -r)
KO_DIR=/lib/modules/$KVER/updates/dkms
CONF_SRC="$PKG_DIR/99-p104-gen2.conf"
CONF_DST=/etc/modprobe.d/p104-gen2.conf

fail() { echo "✗ $*"; exit 1; }
ok()   { echo "✓ $*"; }

echo "=============================================="
echo " P104-100 PCIe Gen2 解锁安装"
echo "=============================================="

[ "$(id -u)" = "0" ] || fail "需要 root，请用 sudo 运行"
[ "$(lspci -nn -s 01:00.0 2>/dev/null | grep -c 10de:1b87)" = "1" ] || fail "未找到 P104-100 (10de:1b87) 在 01:00.0"
ok "P104-100 @ 01:00.0"

# ---------- 占用检查 ----------
OCC=$(fuser /dev/nvidia* 2>/dev/null | tr -s ' ')
if [ -n "$OCC" ]; then
    echo "⚠ GPU 被占用（$OCC）。补丁文件操作不受影响，但稍后的驱动重载会失败。"
    echo "  建议先停掉占用程序再装。继续（文件操作模式）… "
fi

# ---------- 1. 编译 gen2kr ----------
echo
echo "--- 1. 编译 gen2kr 模块 ---"
BUILD_USER="${SUDO_USER:-$(id -un)}"
if [ ! -d "/lib/modules/$KVER/build" ]; then
    fail "缺少内核头: sudo apt install linux-headers-$KVER"
fi
( cd "$PKG_DIR/module" && make clean >/dev/null 2>&1; make -C /lib/modules/$KVER/build M="$PWD" modules ) \
    || fail "gen2kr 编译失败（详见上方输出）"
[ -f "$PKG_DIR/module/gen2kr.ko" ] || fail "gen2kr.ko 未生成"
ok "gen2kr.ko 编译成功"

# ---------- 2. 打补丁（三处同步）----------
echo
echo "--- 2. nvidia.ko 2 字节补丁（.ko/.zst/initramfs 同步）---"
if [ ! -f "$KO_DIR/nvidia.ko.zst.orig_backup" ]; then
    cp -av "$KO_DIR/nvidia.ko.zst" "$KO_DIR/nvidia.ko.zst.orig_backup"
    ok "已备份原始 .zst"
else
    echo "  备份已存在，跳过"
fi

zstd -dc "$KO_DIR/nvidia.ko.zst" > "$KO_DIR/nvidia.ko"
chmod 644 "$KO_DIR/nvidia.ko"; chown root:root "$KO_DIR/nvidia.ko"

PATCH_PY="$PKG_DIR/patch/patch_no_gen1.py"
[ -f "$PATCH_PY" ] || PATCH_PY="$PKG_DIR/patch_no_gen1.py"
python3 "$PATCH_PY" check | sed 's/^/  /'
python3 "$PATCH_PY" apply | sed 's/^/  /' || fail "补丁失败"

zstd -19 -f "$KO_DIR/nvidia.ko" -o "$KO_DIR/nvidia.ko.zst" >/dev/null 2>&1
chmod 644 "$KO_DIR/nvidia.ko.zst"; chown root:root "$KO_DIR/nvidia.ko.zst"

# 终检：两份都含补丁
python3 - "$KO_DIR/nvidia.ko" <<'PY' || exit 1
import sys
data = open(sys.argv[1],'rb').read()
sig = bytes.fromhex('8d42fe415641554989fd41544531e4534883ed404889751089550c83f8010f87')
sys.exit(0 if data.find(sig)>=0 else 1)
PY
ok "nvidia.ko 已含补丁"
zstd -dc "$KO_DIR/nvidia.ko.zst" | python3 -c "
import sys
data = sys.stdin.buffer.read()
sig = bytes.fromhex('8d42fe415641554989fd41544531e4534883ed404889751089550c83f8010f87')
sys.exit(0 if data.find(sig)>=0 else 1)" && ok "nvidia.ko.zst 已含补丁" || fail ".zst 补丁异常"
depmod -a $KVER 2>/dev/null
update-initramfs -u -k $KVER 2>&1 | tail -1 | sed 's/^/  /'
ok "initramfs 已重建"

# ---------- 3. registry 键 conf ----------
echo
echo "--- 3. 安装 registry 键 conf ---"
cp -av "$CONF_SRC" "$CONF_DST"
ok "已安装 $CONF_DST"

# ---------- 4. gen2kr 模块 + 推送脚本 + 服务 ----------
echo
echo "--- 4. 安装 gen2kr + 推送脚本 + 服务 ---"
mkdir -p /lib/modules/$KVER/updates/gpu-unlock
cp -av "$PKG_DIR/module/gen2kr.ko" /lib/modules/$KVER/updates/gpu-unlock/gen2kr.ko
depmod -a $KVER 2>/dev/null

cp -av "$PKG_DIR/scripts/p104-gen2-push.sh" /usr/local/sbin/p104-gen2-push.sh
chmod 755 /usr/local/sbin/p104-gen2-push.sh
cp -av "$PKG_DIR/scripts/p104-gen2-push.service" /etc/systemd/system/p104-gen2-push.service
systemctl daemon-reload
systemctl enable p104-gen2-push.service 2>&1 | sed 's/^/  /'
ok "服务已启用"

# ---------- 5. 立即激活（可选：不重启也能用）----------
echo
echo "--- 5. 立即激活（重载驱动 + 推送）---"
OCC=$(fuser /dev/nvidia* 2>/dev/null | tr -s ' ')
if [ -n "$OCC" ]; then
    echo "⚠ GPU 仍被占用，跳过立即激活。重启后自动生效。"
else
    rmmod nvidia_uvm 2>/dev/null; rmmod nvidia_drm 2>/dev/null
    rmmod nvidia_modeset 2>/dev/null; rmmod nvidia 2>/dev/null
    sleep 1
    modprobe nvidia && ok "nvidia 重载（registry 键生效）" || fail "modprobe 失败"
    modprobe nvidia_uvm 2>/dev/null
    sleep 3
    /usr/local/sbin/p104-gen2-push.sh
fi

echo
echo "=============================================="
echo " 安装完成！"
echo " 验证:"
echo "   nvidia-smi --query-gpu=name,pcie.link.gen.current --format=csv,noheader"
echo "   $(dirname "$0")/status.sh"
echo "=============================================="

// SPDX-License-Identifier: GPL-2.0
/*
 * gen2kr.c —— kretprobe 拦截 nvidia.ko 的 probe 末尾，把 PCIe 链路推到 Gen2
 *
 * 架构（本机验证，2026-10-31）：
 *   声明解锁（CAP2=0x06）由 RmForceEnableGen2=1 registry 键驱动完成
 *   （见 ../99-p104-gen2.conf），本模块只负责在 probe 返回瞬间：
 *     写 SPEED=1 → LTSSM=6 → 两端 TLS=5GT → 根端口 Retrain
 *   → 链路训练到 Gen2（CAP2 已锁存，TLS 不会被钳）
 *
 * ★ 本机（Lenovo, CPU 直连槽）实测纪律：
 *   - probe-return 窗口内写 0x8C040.SPEED 安全（多次实测成功）
 *   - probe 完成后的运行期写 SPEED = 掉卡（BAR0 立即 ffffffff）
 *   - 因此推送必须挂在 probe-return，不能做成事后手动脚本
 *
 * 原始来源：WJNKAC/nvidia-p104-unlock-gen2-for-linux（Debian 13 验证）
 * 本机适配：Ubuntu 24.04 / kernel 6.11.0-1025-oem / NVRM 580.178.04
 *
 * ★ 关键约束（WJNKAC 实测踩过）：
 *   kretprobe handler 运行在【原子上下文】，里面不能 msleep/ioremap 睡眠。
 *   handler 只 schedule_work，真正操作放 workqueue。
 *
 * 序列：
 *   ① 解锁声明：XP3G_OVR0/3 + VAL0/3 + 0x8841C + 0x88610
 *   ② 清 0x8C2C0 bit2 (DIS_G2)
 *   ③ 0x8C040 SPEED=1（★Pascal 编码 1=5GT/s；Ampere 是 2）—— 绝不发 COMMIT
 *   ④ 0x8872C = 6
 *   ⑤ msleep(50) → 两端 LNKCTL2 TLS=5GT → 根端口 Retrain
 *   ⑥ 轮询 LNKSTA 最多 20×100ms
 *   ⑦ 之后守 30×100ms，把 SPEED 按回 1（防驱动异步改回）
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/workqueue.h>
#include <linux/fs.h>
#include <linux/file.h>

#define NV_VENDOR   0x10de
#define NV_DEV_P104 0x1b87

#define OFF_XP3G_OVR0  0x8E110
#define OFF_XP3G_OVR3  0x8E11C
#define OFF_XP3G_VAL0  0x8E120
#define OFF_XP3G_VAL3  0x8E12C
#define OFF_CONFIG     0x8841C   /* PRIV_MISC_1 */
#define OFF_VSEC       0x88610   /* VSEC_HIERARCHY */
#define OFF_LNKCAP     0x88084   /* LINK_CAP 镜像（宽度声明）*/
#define OFF_CAP2       0x880A4   /* LINK_CAP2 镜像 */
#define OFF_LTSSM      0x8872C   /* XVE_OVR */
#define OFF_LINKCFG0   0x8C040   /* LINK_CONFIG_0 */
#define OFF_CYA        0x8C2C0   /* CYA_0, bit2 = DIS_G2 */
#define BAR0_LEN       0x90000

static int guard_rounds = 0;     /* 0 = 常驻；>0 = 只守这么多轮（每轮 100ms） */
module_param(guard_rounds, int, 0644);
MODULE_PARM_DESC(guard_rounds, "守卫轮数，0=常驻（默认）");

static struct kretprobe krp;
static bool triggered;
static void __iomem *early_bar;      /* init 时预映射，handler 里直接用（不睡眠） */

static struct delayed_work gen2_work;
static int guard_left;
static int retrain_cnt;
static bool bad_read_logged;
static bool pushing;

static int retrain_min_ms = 2000;
module_param(retrain_min_ms, int, 0644);
MODULE_PARM_DESC(retrain_min_ms, "两次重训尝试最小间隔 ms，默认 2000；0=不限（危险）");

static int stage_gap_ms = 150;
module_param(stage_gap_ms, int, 0644);
MODULE_PARM_DESC(stage_gap_ms, "stage0→stage1 间隔 ms，默认 150");

static int debounce_cnt = 3;
module_param(debounce_cnt, int, 0644);
MODULE_PARM_DESC(debounce_cnt, "重训防抖次数，默认 3");

static int guard_period_ms = 200;

static int allow_busy_retrain = 1;
module_param(allow_busy_retrain, int, 0644);
MODULE_PARM_DESC(allow_busy_retrain, "1=GPU忙时也重训(默认)；0=忙时跳过");

static int speed_settle_ms = 800;
module_param(speed_settle_ms, int, 0644);
MODULE_PARM_DESC(speed_settle_ms, "按回 SPEED 后等待多少 ms 才允许重训，默认 800");

static unsigned long last_retrain_jiffies;
static unsigned long last_attempt_jiffies;
static unsigned long stage_start_jiffies;
static unsigned long last_speed_fix_jiffies;
static int retrain_stage;
static int low_streak;
static int guard_left;
static int nvidia_ref_baseline;

static void push_fn(struct work_struct *w);
static DECLARE_WORK(push_work, push_fn);
static void guard_fn(struct work_struct *w);
static DECLARE_DELAYED_WORK(gen2_work, guard_fn);

/* ---------- 只读 nvidia.ko 引用计数（GPU 忙判定用） ---------- */
static int read_nvidia_refcnt(void)
{
    char buf[32];
    loff_t pos = 0;
    int v = -1;
    struct file *f = filp_open("/sys/module/nvidia/refcnt", O_RDONLY, 0);
    if (IS_ERR(f)) return -1;
    {
        ssize_t n = kernel_read(f, buf, sizeof(buf) - 1, &pos);
        if (n > 0) { buf[n] = 0; v = simple_strtol(buf, NULL, 10); }
    }
    filp_close(f, NULL);
    return v;
}

static bool gpu_busy(void)
{
    int rc = read_nvidia_refcnt();
    if (rc < 0) return true;               /* 读不到按忙处理 */
    if (rc > nvidia_ref_baseline) return true;
    return false;
}

/* ---------- 守卫（安全网；补丁在位时理论上零动作） ---------- */
static void guard_fn(struct work_struct *w)
{
    struct pci_dev *gpu;
    void __iomem *bar;
    u32 v;
    u16 sta = 0;

    if (guard_left <= 0) return;
    guard_left--;

    gpu = pci_get_device(NV_VENDOR, NV_DEV_P104, NULL);
    if (!gpu) goto out;

    bar = ioremap(pci_resource_start(gpu, 0), BAR0_LEN);
    if (!bar) { pci_dev_put(gpu); goto out; }

    v = readl(bar + OFF_LINKCFG0);
    if (v == 0xFFFFFFFFu) {
        iounmap(bar); pci_dev_put(gpu);
        schedule_delayed_work(&gen2_work, msecs_to_jiffies(guard_period_ms));
        return;
    }

    /* SPEED 被写回 2（Gen1）→ 按回 1 */
    if (((v >> 18) & 3u) != 1u) {
        writel((v & ~0x000C0000u) | (1u << 18), bar + OFF_LINKCFG0);
        last_speed_fix_jiffies = jiffies;
        retrain_stage = 0;
        low_streak = 0;
        iounmap(bar); pci_dev_put(gpu);
        schedule_delayed_work(&gen2_work, msecs_to_jiffies(guard_period_ms));
        return;
    }
    iounmap(bar);

    /* 链路速率检查 + 防抖重训 */
    if (!pcie_capability_read_word(gpu, PCI_EXP_LNKSTA, &sta)) {
        u8 spd = sta & PCI_EXP_LNKSTA_CLS;
        if (spd < PCI_EXP_LNKSTA_CLS_5_0GB) {
            low_streak++;
            if (low_streak >= debounce_cnt) {
                bool idle_ok = allow_busy_retrain || !gpu_busy();

                if (idle_ok &&
                    time_after(jiffies, last_speed_fix_jiffies + msecs_to_jiffies(speed_settle_ms)) &&
                    time_after(jiffies, last_attempt_jiffies + msecs_to_jiffies(retrain_min_ms))) {
                    struct pci_dev *up = pci_upstream_bridge(gpu);
                    u16 ctl2, ctl;
                    last_attempt_jiffies = jiffies;
                    retrain_cnt++;

                    if (retrain_stage == 0) {
                        bar = ioremap(pci_resource_start(gpu, 0), BAR0_LEN);
                        if (bar) {
                            writel(0x00000006, bar + OFF_LTSSM);
                            iounmap(bar);
                        }
                        retrain_stage = 1;
                    } else {
                        if (up) {
                            if (!pcie_capability_read_word(up, PCI_EXP_LNKCTL2, &ctl2)) {
                                pcie_capability_write_word(up, PCI_EXP_LNKCTL2,
                                    (ctl2 & ~PCI_EXP_LNKCTL2_TLS) | PCI_EXP_LNKCTL2_TLS_5_0GT);
                            }
                            if (!pcie_capability_read_word(up, PCI_EXP_LNKCTL, &ctl)) {
                                pcie_capability_write_word(up, PCI_EXP_LNKCTL,
                                                           ctl | PCI_EXP_LNKCTL_RL);
                            }
                        }
                        retrain_stage = 0;
                        low_streak = 0;
                    }
                }
            }
        } else {
            low_streak = 0;
        }
    }

    pci_dev_put(gpu);
out:
    if (guard_left > 0 || guard_rounds == 0)
        schedule_delayed_work(&gen2_work, msecs_to_jiffies(guard_period_ms));
}

/* ---------- 主推送 ---------- */
static void push_fn(struct work_struct *w)
{
    struct pci_dev *gpu;
    struct pci_dev *up;
    void __iomem *bar;
    u32 v;
    u16 ctl2, ctl, sta = 0;
    int i;

    pushing = false;
    gpu = pci_get_device(NV_VENDOR, NV_DEV_P104, NULL);
    if (!gpu) {
        pr_warn("gen2kr: 找不到 P104\n");
        return;
    }

    bar = ioremap(pci_resource_start(gpu, 0), BAR0_LEN);
    if (!bar) {
        pr_err("gen2kr: ioremap 失败\n");
        goto put;
    }

    pr_info("gen2kr: [push] 起始 cap2=%08x config=%08x lnkcfg0=%08x\n",
            readl(bar + OFF_CAP2), readl(bar + OFF_CONFIG),
            readl(bar + OFF_LINKCFG0));

    /* ① 解锁声明（幂等；RmForceEnableGen2 已让 CAP2=0x06 时这里是重复保险） */
    writel(0x00000001, bar + OFF_XP3G_OVR0);
    writel(0x00000004, bar + OFF_XP3G_OVR3);
    writel(0x00000000, bar + OFF_XP3G_VAL0);
    writel(0x00200000, bar + OFF_XP3G_VAL3);
    v = readl(bar + OFF_CONFIG);
    writel((v | (1u << 11) | (1u << 13) | (1u << 15)) & ~((1u << 12) | (1u << 14)),
           bar + OFF_CONFIG);
    v = readl(bar + OFF_VSEC);
    writel((v & ~(1u << 12)) | 1u, bar + OFF_VSEC);

    /* ② 清 DIS_G2 */
    v = readl(bar + OFF_CYA);
    writel(v & ~(1u << 2), bar + OFF_CYA);

    /* ③ SPEED = 1（Pascal），绝不发 COMMIT
     * ★ 严禁在此写 0x8C040.bit31（x16 宽度声明门）—— 本机实测（2026-10-31）:
     *   probe-return 窗口内写 bit31 同样立即掉卡（与运行期写无异）。
     *   x16 声明在本机任何时机都不可行；lane 4-15 硬件问题随之关闭。 */
    v = readl(bar + OFF_LINKCFG0);
    if (v == 0xFFFFFFFFu || (v & 1u) != 0) {
        pr_err("gen2kr: [push] LINKCFG0=%08x 不可信（ffffffff 或 COMMIT=1），跳过 SPEED 写入\n", v);
    } else {
        writel((v & ~0x000C0000u) | (1u << 18), bar + OFF_LINKCFG0);
    }

    /* ④ XVE_OVR */
    writel(0x00000006, bar + OFF_LTSSM);

    pr_info("gen2kr: [push] 写完 cap2=%08x lcap=%08x(声明宽=%u) config=%08x lnkcfg0=%08x cya=%08x\n",
            readl(bar + OFF_CAP2),
            readl(bar + OFF_LNKCAP), (readl(bar + OFF_LNKCAP) >> 4) & 0x3F,
            readl(bar + OFF_CONFIG),
            readl(bar + OFF_LINKCFG0), readl(bar + OFF_CYA));
    iounmap(bar);

    msleep(50);

    /* ⑤ 两端 TLS=5GT，根端口 Retrain */
    if (!pcie_capability_read_word(gpu, PCI_EXP_LNKCTL2, &ctl2)) {
        pcie_capability_write_word(gpu, PCI_EXP_LNKCTL2,
                                   (ctl2 & ~PCI_EXP_LNKCTL2_TLS) |
                                   PCI_EXP_LNKCTL2_TLS_5_0GT);
        pr_info("gen2kr: 端点 TLS2 %04x -> %04x\n", ctl2,
                (ctl2 & ~PCI_EXP_LNKCTL2_TLS) | PCI_EXP_LNKCTL2_TLS_5_0GT);
    }

    up = pci_upstream_bridge(gpu);
    if (up) {
        if (!pcie_capability_read_word(up, PCI_EXP_LNKCTL2, &ctl2)) {
            pcie_capability_write_word(up, PCI_EXP_LNKCTL2,
                                       (ctl2 & ~PCI_EXP_LNKCTL2_TLS) |
                                       PCI_EXP_LNKCTL2_TLS_5_0GT);
            pr_info("gen2kr: 根端口 TLS2 %04x -> %04x\n", ctl2,
                    (ctl2 & ~PCI_EXP_LNKCTL2_TLS) | PCI_EXP_LNKCTL2_TLS_5_0GT);
        }
        if (!pcie_capability_read_word(up, PCI_EXP_LNKCTL, &ctl)) {
            pcie_capability_write_word(up, PCI_EXP_LNKCTL, ctl | PCI_EXP_LNKCTL_RL);
            pr_info("gen2kr: 根端口 LNKCTL %04x -> %04x (Retrain)\n",
                    ctl, ctl | PCI_EXP_LNKCTL_RL);
        }
    }

    /* ⑥ 轮询 */
    for (i = 0; i < 20; i++) {
        msleep(100);
        if (pcie_capability_read_word(gpu, PCI_EXP_LNKSTA, &sta))
            continue;
        if ((sta & PCI_EXP_LNKSTA_CLS) >= PCI_EXP_LNKSTA_CLS_5_0GB) {
            pr_info("gen2kr: ★ SUCCESS Gen%u 宽度=%u LNKSTA=%04x（第 %d 次）\n",
                    sta & PCI_EXP_LNKSTA_CLS, (sta >> 4) & 0x3F, sta, i + 1);
            break;
        }
    }
    if (i == 20)
        pr_warn("gen2kr: 未升速，LNKSTA=%04x 训练宽度=%u\n", sta, (sta >> 4) & 0x3F);

    /* ⑦ 开始守卫 */
    last_retrain_jiffies = jiffies;
    last_attempt_jiffies = jiffies;
    stage_start_jiffies = jiffies;
    retrain_stage = 0;
    low_streak = 0;
    guard_left = (guard_rounds > 0) ? guard_rounds : 1;
    schedule_delayed_work(&gen2_work, msecs_to_jiffies(100));

put:
    pci_dev_put(gpu);
}

/* ---------- kretprobe handler：只调度 + 极速按回 SPEED ---------- */
static int ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    u32 lk;

    if (triggered)
        return 0;
    triggered = true;

    if (early_bar) {
        lk = readl(early_bar + OFF_LINKCFG0);
        if (lk != 0xFFFFFFFFu && (lk & 1u) == 0) {
            writel((lk & ~0x000C0000u) | (1u << 18), early_bar + OFF_LINKCFG0);
            writel(0x00000006, early_bar + OFF_LTSSM);
            pr_info("gen2kr: [fast] 直接写 SPEED=1，原 %08x → %08x\n",
                    lk, readl(early_bar + OFF_LINKCFG0));
        } else {
            pr_warn("gen2kr: [fast] LINKCFG0=%08x 不可信，跳过直接写（交给 workqueue）\n", lk);
        }
    }

    if (!pushing) {
        pushing = true;
        schedule_work(&push_work);
        pr_info("gen2kr: nv_pci_probe 已返回，已排队重训\n");
    }
    return 0;
}

static int __init gen2kr_init(void)
{
    int ret;

    memset(&krp, 0, sizeof(krp));
    krp.kp.symbol_name = "nv_pci_probe";
    krp.handler = ret_handler;
    krp.maxactive = 4;

    INIT_DELAYED_WORK(&gen2_work, guard_fn);

    {
        int rc = read_nvidia_refcnt();
        if (rc >= 0) {
            nvidia_ref_baseline = rc;
            pr_info("gen2kr: nvidia refcnt 基线 = %d\n", rc);
        }
    }

    {
        struct pci_dev *g = pci_get_device(NV_VENDOR, NV_DEV_P104, NULL);
        if (g) {
            early_bar = ioremap(pci_resource_start(g, 0), BAR0_LEN);
            pr_info("gen2kr: 预映射 BAR0 @ %llx -> %px\n",
                    (unsigned long long)pci_resource_start(g, 0), early_bar);
            pci_dev_put(g);
        }
    }

    ret = register_kretprobe(&krp);
    if (ret) {
        pr_err("gen2kr: register_kretprobe 失败 %d\n", ret);
        if (early_bar) { iounmap(early_bar); early_bar = NULL; }
        return ret;
    }
    pr_info("gen2kr: 已在 nv_pci_probe 上挂 kretprobe\n");
    return 0;
}

static void __exit gen2kr_exit(void)
{
    unregister_kretprobe(&krp);
    cancel_work_sync(&push_work);
    cancel_delayed_work_sync(&gen2_work);
    if (early_bar) { iounmap(early_bar); early_bar = NULL; }
    pr_info("gen2kr: 已卸载\n");
}

module_init(gen2kr_init);
module_exit(gen2kr_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Push NVIDIA P104-100 PCIe link to Gen2 after probe");

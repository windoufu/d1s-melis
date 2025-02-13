/*
 * drivers\media\platform\ise
 * Copyright (c) 2018 by Allwinnertech Co., Ltd.
 * http://www.allwinnertech.com
 *
 * Authors:Du Jianhao <dujianhao@allwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/time.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/mpp.h>

#include "eise.h"
#include "eise_api.h"

#define EISE_MODULE_VERS "V1.0"
#define EISE_MODULE_NAME "EISE"

#define EISE_CLK_HIGH_WATER  (700)
#define EISE_CLK_LOW_WATER   (300)

static unsigned int *eise_regs_init;
static unsigned int *eise_ccmu_base;
static unsigned int irq_flag;
static unsigned int eise_irq_id;
static unsigned int eise_en;

static unsigned long interrupt_times;
static unsigned long err_cnt;

struct clk *eise_moduleclk;
struct clk *eise_parent_pll_clk;
//static unsigned long eise_parent_clk_rate = 432000000;

static int clk_status;
static int frame_status;
static spinlock_t eise_spin_lock;

static struct dentry *eise_debugfs_root, *debugfsP;

#if defined(CONFIG_OF)
static struct of_device_id sunxi_eise_match[] = {
	{ .compatible = "allwinner,sunxi-eise",},
	{}
};
MODULE_DEVICE_TABLE(of, sunxi_eise_match);
#endif

static DECLARE_WAIT_QUEUE_HEAD(wait_eise);

static irqreturn_t eise_interrupt(unsigned long data)
{
	int irq_status;

	writel(0x00, eise_regs_init + (EISE_INTERRUPT_EN>>2));

	irq_status = readl(eise_regs_init + (EISE_INTERRUPT_STATUS>>2));

	if (0x00 != (irq_status & 0x07))
		irq_flag = 1;

	if (0x01 != (irq_status & 0x01))
		err_cnt++;

	interrupt_times++;
	wake_up(&wait_eise);

	return IRQ_HANDLED;
}

static int eisedev_open(struct inode *node, struct file *filp)
{
	return 0;
}

static int eisedev_close(struct inode *node, struct file *filp)
{
	return 0;
}

static int eise_debugfs_show_main(struct seq_file *m, void *v)
{
	int eise_clk;
	int eise_in_size, in_width, in_height;
	int eise_out_size, out_width, out_height;
	int eise_ocfg;
	int stride_y_out, stride_c_out;
	int plane_mode;

	seq_printf(m, "---------------EISE  MAIN  PARAM---------------\n");

	/* ISE MODULE CLK */
	eise_clk = clk_get_rate(eise_moduleclk);
	seq_printf(m, "ISE MODULE CLK:%dMHz\n", eise_clk/1000000);

	/* W_IN、H_IN、FMT_IN */
	eise_in_size = readl(eise_regs_init + (EISE_IN_SIZE>>2));
	in_width = 0x0000ffff & eise_in_size;
	in_height = eise_in_size >> 16;

	seq_printf(m, "W_IN:%d\n", in_width);
	seq_printf(m, "H_IN:%d\n", in_height);

	/* W_OUT、H_OUT、FMT_OUT */
	eise_out_size = readl(eise_regs_init + (EISE_OUT_SIZE>>2));
	out_width = 0x0000ffff & eise_out_size;
	out_height = eise_out_size >> 16;

	seq_printf(m, "W_OUT:%d\n", out_width);
	seq_printf(m, "H_OUT:%d\n", out_height);

	/* FLIP、MIRR */

	eise_ocfg = readl(eise_regs_init + (EISE_OCFG_REG>>2));
/*
	flip = 0X10 & eise_ocfg;
	if (flip)
		seq_printf(m, "FLIP:TRUE\n");
	else
		seq_printf(m, "FLIP:FALSE\n");

	mirror = 0X08 & eise_ocfg;
	if (mirror)
		seq_printf(m, "MIRR:TRUE\n");
	else
		seq_printf(m, "MIRR:FALSE\n");
*/
	stride_y_out = ((0X3ff00000 & eise_ocfg) >> 20) * 8;
	seq_printf(m, "STRIDE_Y_OUT:%d\n", stride_y_out);

	stride_c_out = ((0X3ff00 & eise_ocfg) >> 8) * 8;
	seq_printf(m, "STRIDE_C_OUT:%d\n", stride_c_out);

	plane_mode = 0X03 & eise_ocfg;
	switch (plane_mode) {
	case 0:
		seq_printf(m, "FMT_OUT:NV12\n");
		break;
	case 1:
		seq_printf(m, "FMT_OUT:NV21\n");
		break;

	default:
		seq_printf(m, "FMT_OUT:OUT FORMAT ERR!\n");
	}

	return 0;
}



static int eise_debugfs_show_status(struct seq_file *m, void *v)
{
	int eise_interrupt_status;
	int time_out;

	int eise_err_flag;
	int roi_err;

	seq_printf(m, "------------------ISE STATUS------------------\n");

	/* INTERRUPT_TIMES */
	seq_printf(m, "INTERRUPT_TIMES:%ld\n", interrupt_times);
	seq_printf(m, "ISE_ERR_CNT:%ld\n", err_cnt);

	/* TIMEOUT_ERR */
	eise_interrupt_status = readl(eise_regs_init + (EISE_INTERRUPT_STATUS>>2));
	time_out = 0x04 & eise_interrupt_status;
	if (time_out)
		seq_printf(m, "TIMEOUT_ERR:YES\n");
	else
		seq_printf(m, "TIMEOUT_ERR:NO\n");

	/* ROI_ERR */
	eise_err_flag = readl(eise_regs_init + (EISE_ERROR_FLAG>>2));
	roi_err = 0x08 & eise_err_flag;
	if (roi_err)
		seq_printf(m, "ROI_ERR:YES\n");
	else
		seq_printf(m, "ROI_ERR:NO\n");

	return 0;
}

static int eise_debugfs_show(struct seq_file *m, void *v)
{
	seq_printf(m, "\n[%s] Version:[%s Debug]!\n", EISE_MODULE_NAME, EISE_MODULE_VERS);

	eise_debugfs_show_main(m, v);

	eise_debugfs_show_status(m, v);

	return 0;
}

static int eise_debugfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, eise_debugfs_show, NULL);
}

static long eise_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct eise_register reg;
	unsigned int rData = 0x5a5a5a5a;
	long  ret = 0;

	if (unlikely(copy_from_user(&reg, (void __user *)arg, sizeof(struct eise_register))))
		return -EFAULT;
	else {
		switch (cmd) {
		case EISE_WRITE_REGISTER:
			writel(reg.value, eise_regs_init + (reg.addr>>2));
/*
			eise_debug("[EISE] write to %#010x, value %#010x\n",
				(unsigned int)(eise_regs_init+(reg.addr>>2)), reg.value);
			rData = readl(eise_regs_init + (reg.addr>>2));
			eise_debug("[EISE] read from %#010x, value %#010x\n",
				(unsigned int)(eise_regs_init+(reg.addr>>2)), rData);
*/
			return 0;
		case EISE_READ_REGISTER:
			rData = readl(eise_regs_init + (reg.addr>>2));
		/*	eise_debug("[EISE] read from %#010x, value %#010x\n",
				(unsigned int)(eise_regs_init+(reg.addr>>2)), rData);*/
			return rData;
		case ENABLE_EISE:
			if (clk_prepare_enable(eise_moduleclk))
				eise_err("[SYS] enable ise_moduleclk failed!\n");
			eise_en = 1;
			rData = readl(eise_ccmu_base + (PLL_ISE_CTRL_REG>>2));
			writel(0xb9002301 | rData,
			eise_ccmu_base + (PLL_ISE_CTRL_REG>>2));
			rData = readl(eise_ccmu_base + (EISE_CLK_REG>>2));
			writel(0x80000000 | rData,
			eise_ccmu_base + (EISE_CLK_REG>>2));
			rData = readl(eise_ccmu_base + (MBUS_CLK_GATING_REG>>2));
			writel(0x00802001 | rData,
			eise_ccmu_base + (MBUS_CLK_GATING_REG>>2));
			rData = readl(eise_ccmu_base + (EISE_BGR_REG>>2));
			writel(0x00010001 | rData,
			eise_ccmu_base + (EISE_BGR_REG>>2));
/*
			rData = readl(eise_ccmu_base+ (PLL_ISE_CTRL_REG>>2));
			eise_debug("[EISE] read from %#010x, value %#010x\n",
				(unsigned int)(eise_ccmu_base+ (PLL_ISE_CTRL_REG>>2)), rData);
			rData = readl(eise_ccmu_base+ (EISE_CLK_REG>>2));
			eise_debug("[EISE] read from %#010x, value %#010x\n",
				(unsigned int)(eise_ccmu_base+ (EISE_CLK_REG>>2)), rData);
			rData = readl(eise_ccmu_base+ (MBUS_CLK_GATING_REG>>2));
			eise_debug("[ISE] read from %#010x, value %#010x\n",
				(unsigned int)(eise_ccmu_base+ (MBUS_CLK_GATING_REG>>2)), rData);
			rData = readl(eise_ccmu_base+ (EISE_BGR_REG>>2));
			eise_debug("[ISE] read from %#010x, value %#010x\n",
				(unsigned int)(eise_ccmu_base+ (EISE_BGR_REG>>2)), rData);
*/
			break;
		case DISABLE_EISE:
			if ((NULL == eise_moduleclk) || IS_ERR(eise_moduleclk)) {
				eise_err("[SYS] ise_moduleclk is invalid!\n");
				return -EFAULT;
			} else {
				clk_disable_unprepare(eise_moduleclk);
				eise_en = 0;
				//eise_print("[SYS] disable ise_moduleclk!\n");
			}
			break;
		case WAIT_EISE_FINISH:
			wait_event_timeout(wait_eise, irq_flag,
								reg.value*HZ/30*3);
			if (0 == irq_flag)
				eise_print("[SYS] wait_event_timeout!\n");
			irq_flag = 0;
			frame_status = 1;
			break;
		case SET_EISE_FREQ:
		{
			unsigned int arg_rate = reg.value;

			if (arg_rate >= EISE_CLK_LOW_WATER &&
				arg_rate <= EISE_CLK_HIGH_WATER) {
#if defined(CONFIG_SOC_SUN8IW19P1)
				if (clk_set_rate(eise_moduleclk, arg_rate*1000000))
					eise_err("set clock failed\n");
#else
				unsigned long eise_parent_clk_rate = 432000000;
				if (!clk_set_rate(eise_parent_pll_clk, arg_rate*1000000)) {
					eise_parent_clk_rate = clk_get_rate(eise_parent_pll_clk);
					if (clk_set_rate(eise_moduleclk, eise_parent_clk_rate))
						eise_err("[SYS] set eise clock failed!\n");
				} else {
					eise_err("set pll clock failed!\n");
				}
#endif
			}
			ret = clk_get_rate(eise_moduleclk);
			eise_print("eise real_fre=%ld\n", ret);
			break;
		}
		default:
			eise_warn("[SYS] do not supprot this ioctrl now!\n");
			break;
		}

		return 0;
	}
}

int enable_eise_hw_clk(void)
{
	unsigned long flags;
	int res = -EFAULT;

	spin_lock_irqsave(&eise_spin_lock, flags);

	if (clk_status == 0) {
		res = 0;
		goto out;
	}
	clk_status = 0;

	if ((NULL == eise_moduleclk) || (IS_ERR(eise_moduleclk)))
		eise_err("eise_moduleclk is invalid!\n");
	else
		clk_prepare_enable(eise_moduleclk);

out:
	spin_unlock_irqrestore(&eise_spin_lock, flags);
	return 0;
}

int disable_eise_hw_clk(void)
{
	unsigned long flags;
	int res = -EFAULT;

	spin_lock_irqsave(&eise_spin_lock, flags);

	if (clk_status == 1) {
		res = 1;
		goto out;
	}
	clk_status = 1;

	if ((NULL == eise_moduleclk) || (IS_ERR(eise_moduleclk)))
		eise_err("eise_moduleclk is invalid!\n");
	else
		clk_disable_unprepare(eise_moduleclk);

out:
	spin_unlock_irqrestore(&eise_spin_lock, flags);
	return 0;
}

static int snd_sw_eise_suspend(struct platform_device *pdev, pm_message_t state)
{
	int ret = 0;

	if (0 == eise_en) {
		eise_print("[SYS] wait_event_timeout!\n");
		frame_status = 0;
	} else {
		ret = disable_eise_hw_clk();

		if (ret < 0) {
			eise_err("eise clk disable somewhere error!\n");
			return -EFAULT;
		}
	}
		printk("[eise]suspend: disable_eise_hw_clk finish!\n");

	return 0;
}

static int snd_sw_eise_resume(struct platform_device *pdev)
{
	int ret = 0;
	if (eise_en == 1) {
		ret = enable_eise_hw_clk();
		if (ret < 0) {
			eise_err("[eise] ise clk enable somewhere error!\n");
			return -EFAULT;
		}
		printk("[eise]resume enable_ise_hw_clk finish!\n");
	}
	printk("[eise] standby resume finish!\n");

	return 0;
}

static struct file_operations eise_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = eise_ioctl,
	.open           = eisedev_open,
	.release        = eisedev_close
};

static const struct file_operations eise_debugfs_fops = {
	.owner      = THIS_MODULE,
	.open       = eise_debugfs_open,
	.read       = seq_read,
	.llseek     = seq_lseek,
	.release    = single_release,
};

static struct miscdevice eise_dev = {
	.minor  = MISC_DYNAMIC_MINOR,
	.name   = DEVICE_NAME,
	.fops   = &eise_fops
};

#if defined(CONFIG_DEBUG_FS)
int sunxi_eise_debug_register_driver(void)
{
#if defined (CONFIG_SUNXI_MPP)
	eise_debugfs_root = debugfs_mpp_root;
#else
	eise_debugfs_root = debugfs_create_dir("mpp", NULL);
#endif
	if (eise_debugfs_root == NULL)
		return -ENOENT;

	debugfsP = debugfs_create_file("sunxi-eise", 0444, eise_debugfs_root,
								NULL, &eise_debugfs_fops);
	if (IS_ERR_OR_NULL(debugfsP)) {
		printk(KERN_INFO "Warning: fail to create debugfs node!\n");
		return -ENOMEM;
	}

	return 0;
}
#else
int sunxi_eise_debug_register_driver(void)
{
	return 0;
}
#endif

void sunxi_eise_debug_unregister_driver(void)
{
	debugfs_remove(debugfsP);
}

static int sunxi_eise_init(struct platform_device *pdev)
{
	struct device_node *node;
	int ret;
	//unsigned int val;

	ret = misc_register(&eise_dev);
	if (ret >= 0)
		eise_print("EISE device register success! %d\n", ret);
	else {
		eise_err("EISE device register failed! %d\n", ret);
		return -EINVAL;
	}

	node = of_find_compatible_node(NULL, NULL, "allwinner,sunxi-eise");
	eise_irq_id = irq_of_parse_and_map(node, 0);

	eise_print("irq_id = %d\n", eise_irq_id);

	ret = request_irq(eise_irq_id, (irq_handler_t)eise_interrupt, 0, "sunxi_eise", NULL);
	if (ret < 0) {
		eise_err("Request EISE Irq error! return %d\n", ret);
		return -EINVAL;
	} else
		eise_print("Request EISE Irq success! irq_id = %d, return %d\n", eise_irq_id, ret);

	eise_regs_init = (unsigned int *)of_iomap(node, 0);
	eise_ccmu_base = (unsigned int *)of_iomap(node, 1);

	eise_parent_pll_clk = of_clk_get(node, 0);
	if ((!eise_parent_pll_clk) || IS_ERR(eise_parent_pll_clk)) {
		eise_warn("[ise] try to get eise_parent_pll_clk failed!\n");
		return -EINVAL;
	}

	eise_moduleclk = of_clk_get(node, 1);
	if (!eise_moduleclk || IS_ERR(eise_moduleclk)) {
		eise_warn("[eise] get eise_moduleclk failed!\n");
		return -EINVAL;
	}
#if defined(CONFIG_SOC_SUN8IW19P1)
	if (clk_set_parent(eise_moduleclk,  eise_parent_pll_clk))
		eise_err("set eise clk and parent clk failed;\n");
#endif
	ret = sunxi_eise_debug_register_driver();
	if (ret) {
		eise_err("Sunxi eise debug register driver failed!\n");
		return -EINVAL;
	}

	return 0;
}

static void sunxi_eise_exit(void)
{
	sunxi_eise_debug_unregister_driver();

	if (NULL == eise_moduleclk || IS_ERR(eise_moduleclk)) {
		eise_warn("[eise] eise_moduleclk handle "
			"is invalid,just return!\n");
	} else {
		clk_put(eise_moduleclk);
		eise_moduleclk = NULL;
	}

	if (NULL == eise_parent_pll_clk || IS_ERR(eise_parent_pll_clk)) {
		eise_warn("[eise] eise_parent_pll_clk "
			"handle is invalid,just return!\n");
	} else {
		clk_put(eise_parent_pll_clk);
		eise_parent_pll_clk = NULL;
	}
	eise_en = 0;
	free_irq(eise_irq_id, NULL);
	iounmap(eise_regs_init);

	misc_deregister(&eise_dev);
	eise_print("EISE device has been removed!\n");
}

static int	sunxi_eise_remove(struct platform_device *pdev)
{
	sunxi_eise_exit();
	return 0;
}

static int	sunxi_eise_probe(struct platform_device *pdev)
{
	sunxi_eise_init(pdev);
	return 0;
}

static struct platform_driver sunxi_eise_driver = {
	.probe		= sunxi_eise_probe,
	.remove		= sunxi_eise_remove,
	.suspend	= snd_sw_eise_suspend,
	.resume		= snd_sw_eise_resume,

	.driver		= {
		.name	= "sunxi-eise",
		.owner	= THIS_MODULE,
		.of_match_table = sunxi_eise_match,
	},

};

static int __init eise_init(void)
{
	printk("sunxi eise version 1.0 \n");

	return platform_driver_register(&sunxi_eise_driver);
}

static void __exit eise_exit(void)
{
	platform_driver_unregister(&sunxi_eise_driver);
};

module_init(eise_init);
module_exit(eise_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("DuJianhao<dujianhao@allwinnertech.com>");
MODULE_DESCRIPTION("EISE device driver");
MODULE_VERSION("1.0");
MODULE_ALIAS("platform:EISE(AW1816)");

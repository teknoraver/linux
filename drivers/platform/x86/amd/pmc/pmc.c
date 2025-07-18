// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD SoC Power Management Controller Driver
 *
 * Copyright (c) 2020, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Shyam Sundar S K <Shyam-sundar.S-k@amd.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/serio.h>
#include <linux/suspend.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include <asm/amd_node.h>

#include "pmc.h"

static const struct amd_pmc_bit_map soc15_ip_blk_v2[] = {
	{"DISPLAY",     BIT(0)},
	{"CPU",         BIT(1)},
	{"GFX",         BIT(2)},
	{"VDD",         BIT(3)},
	{"VDD_CCX",     BIT(4)},
	{"ACP",         BIT(5)},
	{"VCN_0",       BIT(6)},
	{"VCN_1",       BIT(7)},
	{"ISP",         BIT(8)},
	{"NBIO",        BIT(9)},
	{"DF",          BIT(10)},
	{"USB3_0",      BIT(11)},
	{"USB3_1",      BIT(12)},
	{"LAPIC",       BIT(13)},
	{"USB3_2",      BIT(14)},
	{"USB4_RT0",	BIT(15)},
	{"USB4_RT1",	BIT(16)},
	{"USB4_0",      BIT(17)},
	{"USB4_1",      BIT(18)},
	{"MPM",         BIT(19)},
	{"JPEG_0",      BIT(20)},
	{"JPEG_1",      BIT(21)},
	{"IPU",         BIT(22)},
	{"UMSCH",       BIT(23)},
	{"VPE",         BIT(24)},
};

static const struct amd_pmc_bit_map soc15_ip_blk[] = {
	{"DISPLAY",	BIT(0)},
	{"CPU",		BIT(1)},
	{"GFX",		BIT(2)},
	{"VDD",		BIT(3)},
	{"ACP",		BIT(4)},
	{"VCN",		BIT(5)},
	{"ISP",		BIT(6)},
	{"NBIO",	BIT(7)},
	{"DF",		BIT(8)},
	{"USB3_0",	BIT(9)},
	{"USB3_1",	BIT(10)},
	{"LAPIC",	BIT(11)},
	{"USB3_2",	BIT(12)},
	{"USB3_3",	BIT(13)},
	{"USB3_4",	BIT(14)},
	{"USB4_0",	BIT(15)},
	{"USB4_1",	BIT(16)},
	{"MPM",		BIT(17)},
	{"JPEG",	BIT(18)},
	{"IPU",		BIT(19)},
	{"UMSCH",	BIT(20)},
	{"VPE",		BIT(21)},
};

static bool disable_workarounds;
module_param(disable_workarounds, bool, 0644);
MODULE_PARM_DESC(disable_workarounds, "Disable workarounds for platform bugs");

static struct amd_pmc_dev pmc;

static inline u32 amd_pmc_reg_read(struct amd_pmc_dev *dev, int reg_offset)
{
	return ioread32(dev->regbase + reg_offset);
}

static inline void amd_pmc_reg_write(struct amd_pmc_dev *dev, int reg_offset, u32 val)
{
	iowrite32(val, dev->regbase + reg_offset);
}

static void amd_pmc_get_ip_info(struct amd_pmc_dev *dev)
{
	switch (dev->cpu_id) {
	case AMD_CPU_ID_PCO:
	case AMD_CPU_ID_RN:
	case AMD_CPU_ID_YC:
	case AMD_CPU_ID_CB:
		dev->num_ips = 12;
		dev->ips_ptr = soc15_ip_blk;
		dev->smu_msg = 0x538;
		break;
	case AMD_CPU_ID_PS:
		dev->num_ips = 21;
		dev->ips_ptr = soc15_ip_blk;
		dev->smu_msg = 0x538;
		break;
	case PCI_DEVICE_ID_AMD_1AH_M20H_ROOT:
	case PCI_DEVICE_ID_AMD_1AH_M60H_ROOT:
		if (boot_cpu_data.x86_model == 0x70) {
			dev->num_ips = ARRAY_SIZE(soc15_ip_blk_v2);
			dev->ips_ptr = soc15_ip_blk_v2;
		} else {
			dev->num_ips = ARRAY_SIZE(soc15_ip_blk);
			dev->ips_ptr = soc15_ip_blk;
		}
		dev->smu_msg = 0x938;
		break;
	}
}

static int amd_pmc_setup_smu_logging(struct amd_pmc_dev *dev)
{
	if (dev->cpu_id == AMD_CPU_ID_PCO) {
		dev_warn_once(dev->dev, "SMU debugging info not supported on this platform\n");
		return -EINVAL;
	}

	/* Get Active devices list from SMU */
	if (!dev->active_ips)
		amd_pmc_send_cmd(dev, 0, &dev->active_ips, SMU_MSG_GET_SUP_CONSTRAINTS, true);

	/* Get dram address */
	if (!dev->smu_virt_addr) {
		u32 phys_addr_low, phys_addr_hi;
		u64 smu_phys_addr;

		amd_pmc_send_cmd(dev, 0, &phys_addr_low, SMU_MSG_LOG_GETDRAM_ADDR_LO, true);
		amd_pmc_send_cmd(dev, 0, &phys_addr_hi, SMU_MSG_LOG_GETDRAM_ADDR_HI, true);
		smu_phys_addr = ((u64)phys_addr_hi << 32 | phys_addr_low);

		dev->smu_virt_addr = devm_ioremap(dev->dev, smu_phys_addr,
						  sizeof(struct smu_metrics));
		if (!dev->smu_virt_addr)
			return -ENOMEM;
	}

	memset_io(dev->smu_virt_addr, 0, sizeof(struct smu_metrics));

	/* Start the logging */
	amd_pmc_send_cmd(dev, 0, NULL, SMU_MSG_LOG_RESET, false);
	amd_pmc_send_cmd(dev, 0, NULL, SMU_MSG_LOG_START, false);

	return 0;
}

static int get_metrics_table(struct amd_pmc_dev *pdev, struct smu_metrics *table)
{
	int rc;

	if (!pdev->smu_virt_addr) {
		rc = amd_pmc_setup_smu_logging(pdev);
		if (rc)
			return rc;
	}

	if (pdev->cpu_id == AMD_CPU_ID_PCO)
		return -ENODEV;
	memcpy_fromio(table, pdev->smu_virt_addr, sizeof(struct smu_metrics));
	return 0;
}

static void amd_pmc_validate_deepest(struct amd_pmc_dev *pdev)
{
	struct smu_metrics table;

	if (get_metrics_table(pdev, &table))
		return;

	if (!table.s0i3_last_entry_status)
		dev_warn(pdev->dev, "Last suspend didn't reach deepest state\n");
	pm_report_hw_sleep_time(table.s0i3_last_entry_status ?
				table.timein_s0i3_lastcapture : 0);
}

static int amd_pmc_get_smu_version(struct amd_pmc_dev *dev)
{
	int rc;
	u32 val;

	if (dev->cpu_id == AMD_CPU_ID_PCO)
		return -ENODEV;

	rc = amd_pmc_send_cmd(dev, 0, &val, SMU_MSG_GETSMUVERSION, true);
	if (rc)
		return rc;

	dev->smu_program = (val >> 24) & GENMASK(7, 0);
	dev->major = (val >> 16) & GENMASK(7, 0);
	dev->minor = (val >> 8) & GENMASK(7, 0);
	dev->rev = (val >> 0) & GENMASK(7, 0);

	dev_dbg(dev->dev, "SMU program %u version is %u.%u.%u\n",
		dev->smu_program, dev->major, dev->minor, dev->rev);

	return 0;
}

static ssize_t smu_fw_version_show(struct device *d, struct device_attribute *attr,
				   char *buf)
{
	struct amd_pmc_dev *dev = dev_get_drvdata(d);
	int rc;

	if (!dev->major) {
		rc = amd_pmc_get_smu_version(dev);
		if (rc)
			return rc;
	}
	return sysfs_emit(buf, "%u.%u.%u\n", dev->major, dev->minor, dev->rev);
}

static ssize_t smu_program_show(struct device *d, struct device_attribute *attr,
				   char *buf)
{
	struct amd_pmc_dev *dev = dev_get_drvdata(d);
	int rc;

	if (!dev->major) {
		rc = amd_pmc_get_smu_version(dev);
		if (rc)
			return rc;
	}
	return sysfs_emit(buf, "%u\n", dev->smu_program);
}

static DEVICE_ATTR_RO(smu_fw_version);
static DEVICE_ATTR_RO(smu_program);

static umode_t pmc_attr_is_visible(struct kobject *kobj, struct attribute *attr, int idx)
{
	struct device *dev = kobj_to_dev(kobj);
	struct amd_pmc_dev *pdev = dev_get_drvdata(dev);

	if (pdev->cpu_id == AMD_CPU_ID_PCO)
		return 0;
	return 0444;
}

static struct attribute *pmc_attrs[] = {
	&dev_attr_smu_fw_version.attr,
	&dev_attr_smu_program.attr,
	NULL,
};

static struct attribute_group pmc_attr_group = {
	.attrs = pmc_attrs,
	.is_visible = pmc_attr_is_visible,
};

static const struct attribute_group *pmc_groups[] = {
	&pmc_attr_group,
	NULL,
};

static int smu_fw_info_show(struct seq_file *s, void *unused)
{
	struct amd_pmc_dev *dev = s->private;
	struct smu_metrics table;
	int idx;

	if (get_metrics_table(dev, &table))
		return -EINVAL;

	seq_puts(s, "\n=== SMU Statistics ===\n");
	seq_printf(s, "Table Version: %d\n", table.table_version);
	seq_printf(s, "Hint Count: %d\n", table.hint_count);
	seq_printf(s, "Last S0i3 Status: %s\n", table.s0i3_last_entry_status ? "Success" :
		   "Unknown/Fail");
	seq_printf(s, "Time (in us) to S0i3: %lld\n", table.timeentering_s0i3_lastcapture);
	seq_printf(s, "Time (in us) in S0i3: %lld\n", table.timein_s0i3_lastcapture);
	seq_printf(s, "Time (in us) to resume from S0i3: %lld\n",
		   table.timeto_resume_to_os_lastcapture);

	seq_puts(s, "\n=== Active time (in us) ===\n");
	for (idx = 0 ; idx < dev->num_ips ; idx++) {
		if (dev->ips_ptr[idx].bit_mask & dev->active_ips)
			seq_printf(s, "%-8s : %lld\n", dev->ips_ptr[idx].name,
				   table.timecondition_notmet_lastcapture[idx]);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(smu_fw_info);

static int s0ix_stats_show(struct seq_file *s, void *unused)
{
	struct amd_pmc_dev *dev = s->private;
	u64 entry_time, exit_time, residency;

	/* Use FCH registers to get the S0ix stats */
	if (!dev->fch_virt_addr) {
		u32 base_addr_lo = FCH_BASE_PHY_ADDR_LOW;
		u32 base_addr_hi = FCH_BASE_PHY_ADDR_HIGH;
		u64 fch_phys_addr = ((u64)base_addr_hi << 32 | base_addr_lo);

		dev->fch_virt_addr = devm_ioremap(dev->dev, fch_phys_addr, FCH_SSC_MAPPING_SIZE);
		if (!dev->fch_virt_addr)
			return -ENOMEM;
	}

	entry_time = ioread32(dev->fch_virt_addr + FCH_S0I3_ENTRY_TIME_H_OFFSET);
	entry_time = entry_time << 32 | ioread32(dev->fch_virt_addr + FCH_S0I3_ENTRY_TIME_L_OFFSET);

	exit_time = ioread32(dev->fch_virt_addr + FCH_S0I3_EXIT_TIME_H_OFFSET);
	exit_time = exit_time << 32 | ioread32(dev->fch_virt_addr + FCH_S0I3_EXIT_TIME_L_OFFSET);

	/* It's in 48MHz. We need to convert it */
	residency = exit_time - entry_time;
	do_div(residency, 48);

	seq_puts(s, "=== S0ix statistics ===\n");
	seq_printf(s, "S0ix Entry Time: %lld\n", entry_time);
	seq_printf(s, "S0ix Exit Time: %lld\n", exit_time);
	seq_printf(s, "Residency Time: %lld\n", residency);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(s0ix_stats);

static int amd_pmc_idlemask_read(struct amd_pmc_dev *pdev, struct device *dev,
				 struct seq_file *s)
{
	u32 val;
	int rc;

	switch (pdev->cpu_id) {
	case AMD_CPU_ID_CZN:
		/* we haven't yet read SMU version */
		if (!pdev->major) {
			rc = amd_pmc_get_smu_version(pdev);
			if (rc)
				return rc;
		}
		if (pdev->major > 56 || (pdev->major >= 55 && pdev->minor >= 37))
			val = amd_pmc_reg_read(pdev, AMD_PMC_SCRATCH_REG_CZN);
		else
			return -EINVAL;
		break;
	case AMD_CPU_ID_YC:
	case AMD_CPU_ID_CB:
	case AMD_CPU_ID_PS:
		val = amd_pmc_reg_read(pdev, AMD_PMC_SCRATCH_REG_YC);
		break;
	case PCI_DEVICE_ID_AMD_1AH_M20H_ROOT:
	case PCI_DEVICE_ID_AMD_1AH_M60H_ROOT:
		val = amd_pmc_reg_read(pdev, AMD_PMC_SCRATCH_REG_1AH);
		break;
	default:
		return -EINVAL;
	}

	if (dev)
		pm_pr_dbg("SMU idlemask s0i3: 0x%x\n", val);

	if (s)
		seq_printf(s, "SMU idlemask : 0x%x\n", val);

	return 0;
}

static int amd_pmc_idlemask_show(struct seq_file *s, void *unused)
{
	return amd_pmc_idlemask_read(s->private, NULL, s);
}
DEFINE_SHOW_ATTRIBUTE(amd_pmc_idlemask);

static void amd_pmc_dbgfs_unregister(struct amd_pmc_dev *dev)
{
	debugfs_remove_recursive(dev->dbgfs_dir);
}

static void amd_pmc_dbgfs_register(struct amd_pmc_dev *dev)
{
	dev->dbgfs_dir = debugfs_create_dir("amd_pmc", NULL);
	debugfs_create_file("smu_fw_info", 0644, dev->dbgfs_dir, dev,
			    &smu_fw_info_fops);
	debugfs_create_file("s0ix_stats", 0644, dev->dbgfs_dir, dev,
			    &s0ix_stats_fops);
	debugfs_create_file("amd_pmc_idlemask", 0644, dev->dbgfs_dir, dev,
			    &amd_pmc_idlemask_fops);
}

static char *amd_pmc_get_msg_port(struct amd_pmc_dev *dev)
{
	switch (dev->msg_port) {
	case MSG_PORT_PMC:
		return "PMC";
	case MSG_PORT_S2D:
		return "S2D";
	default:
		return "Invalid message port";
	}
}

static void amd_pmc_dump_registers(struct amd_pmc_dev *dev)
{
	u32 value, message, argument, response;

	if (dev->msg_port == MSG_PORT_S2D) {
		message = dev->stb_arg.msg;
		argument = dev->stb_arg.arg;
		response = dev->stb_arg.resp;
	} else {
		message = dev->smu_msg;
		argument = AMD_PMC_REGISTER_ARGUMENT;
		response = AMD_PMC_REGISTER_RESPONSE;
	}

	value = amd_pmc_reg_read(dev, response);
	dev_dbg(dev->dev, "AMD_%s_REGISTER_RESPONSE:%x\n", amd_pmc_get_msg_port(dev), value);

	value = amd_pmc_reg_read(dev, argument);
	dev_dbg(dev->dev, "AMD_%s_REGISTER_ARGUMENT:%x\n", amd_pmc_get_msg_port(dev), value);

	value = amd_pmc_reg_read(dev, message);
	dev_dbg(dev->dev, "AMD_%s_REGISTER_MESSAGE:%x\n", amd_pmc_get_msg_port(dev), value);
}

int amd_pmc_send_cmd(struct amd_pmc_dev *dev, u32 arg, u32 *data, u8 msg, bool ret)
{
	int rc;
	u32 val, message, argument, response;

	guard(mutex)(&dev->lock);

	if (dev->msg_port == MSG_PORT_S2D) {
		message = dev->stb_arg.msg;
		argument = dev->stb_arg.arg;
		response = dev->stb_arg.resp;
	} else {
		message = dev->smu_msg;
		argument = AMD_PMC_REGISTER_ARGUMENT;
		response = AMD_PMC_REGISTER_RESPONSE;
	}

	/* Wait until we get a valid response */
	rc = readx_poll_timeout(ioread32, dev->regbase + response,
				val, val != 0, PMC_MSG_DELAY_MIN_US,
				PMC_MSG_DELAY_MIN_US * RESPONSE_REGISTER_LOOP_MAX);
	if (rc) {
		dev_err(dev->dev, "failed to talk to SMU\n");
		return rc;
	}

	/* Write zero to response register */
	amd_pmc_reg_write(dev, response, 0);

	/* Write argument into response register */
	amd_pmc_reg_write(dev, argument, arg);

	/* Write message ID to message ID register */
	amd_pmc_reg_write(dev, message, msg);

	/* Wait until we get a valid response */
	rc = readx_poll_timeout(ioread32, dev->regbase + response,
				val, val != 0, PMC_MSG_DELAY_MIN_US,
				PMC_MSG_DELAY_MIN_US * RESPONSE_REGISTER_LOOP_MAX);
	if (rc) {
		dev_err(dev->dev, "SMU response timed out\n");
		return rc;
	}

	switch (val) {
	case AMD_PMC_RESULT_OK:
		if (ret) {
			/* PMFW may take longer time to return back the data */
			usleep_range(DELAY_MIN_US, 10 * DELAY_MAX_US);
			*data = amd_pmc_reg_read(dev, argument);
		}
		break;
	case AMD_PMC_RESULT_CMD_REJECT_BUSY:
		dev_err(dev->dev, "SMU not ready. err: 0x%x\n", val);
		rc = -EBUSY;
		break;
	case AMD_PMC_RESULT_CMD_UNKNOWN:
		dev_err(dev->dev, "SMU cmd unknown. err: 0x%x\n", val);
		rc = -EINVAL;
		break;
	case AMD_PMC_RESULT_CMD_REJECT_PREREQ:
	case AMD_PMC_RESULT_FAILED:
	default:
		dev_err(dev->dev, "SMU cmd failed. err: 0x%x\n", val);
		rc = -EIO;
		break;
	}

	amd_pmc_dump_registers(dev);
	return rc;
}

static int amd_pmc_get_os_hint(struct amd_pmc_dev *dev)
{
	switch (dev->cpu_id) {
	case AMD_CPU_ID_PCO:
		return MSG_OS_HINT_PCO;
	case AMD_CPU_ID_RN:
	case AMD_CPU_ID_YC:
	case AMD_CPU_ID_CB:
	case AMD_CPU_ID_PS:
	case PCI_DEVICE_ID_AMD_1AH_M20H_ROOT:
	case PCI_DEVICE_ID_AMD_1AH_M60H_ROOT:
		return MSG_OS_HINT_RN;
	}
	return -EINVAL;
}

static int amd_pmc_wa_irq1(struct amd_pmc_dev *pdev)
{
	struct device *d;
	int rc;

	/* cezanne platform firmware has a fix in 64.66.0 */
	if (pdev->cpu_id == AMD_CPU_ID_CZN) {
		if (!pdev->major) {
			rc = amd_pmc_get_smu_version(pdev);
			if (rc)
				return rc;
		}

		if (pdev->major > 64 || (pdev->major == 64 && pdev->minor > 65))
			return 0;
	}

	d = bus_find_device_by_name(&serio_bus, NULL, "serio0");
	if (!d)
		return 0;
	if (device_may_wakeup(d)) {
		dev_info_once(d, "Disabling IRQ1 wakeup source to avoid platform firmware bug\n");
		disable_irq_wake(1);
		device_set_wakeup_enable(d, false);
	}
	put_device(d);

	return 0;
}

static int amd_pmc_verify_czn_rtc(struct amd_pmc_dev *pdev, u32 *arg)
{
	struct rtc_device *rtc_device;
	time64_t then, now, duration;
	struct rtc_wkalrm alarm;
	struct rtc_time tm;
	int rc;

	/* we haven't yet read SMU version */
	if (!pdev->major) {
		rc = amd_pmc_get_smu_version(pdev);
		if (rc)
			return rc;
	}

	if (pdev->major < 64 || (pdev->major == 64 && pdev->minor < 53))
		return 0;

	rtc_device = rtc_class_open("rtc0");
	if (!rtc_device)
		return 0;
	rc = rtc_read_alarm(rtc_device, &alarm);
	if (rc)
		return rc;
	if (!alarm.enabled) {
		dev_dbg(pdev->dev, "alarm not enabled\n");
		return 0;
	}
	rc = rtc_read_time(rtc_device, &tm);
	if (rc)
		return rc;
	then = rtc_tm_to_time64(&alarm.time);
	now = rtc_tm_to_time64(&tm);
	duration = then-now;

	/* in the past */
	if (then < now)
		return 0;

	/* will be stored in upper 16 bits of s0i3 hint argument,
	 * so timer wakeup from s0i3 is limited to ~18 hours or less
	 */
	if (duration <= 4 || duration > U16_MAX)
		return -EINVAL;

	*arg |= (duration << 16);
	rc = rtc_alarm_irq_enable(rtc_device, 0);
	pm_pr_dbg("wakeup timer programmed for %lld seconds\n", duration);

	return rc;
}

static void amd_pmc_s2idle_prepare(void)
{
	struct amd_pmc_dev *pdev = &pmc;
	int rc;
	u8 msg;
	u32 arg = 1;

	/* Reset and Start SMU logging - to monitor the s0i3 stats */
	amd_pmc_setup_smu_logging(pdev);

	/* Activate CZN specific platform bug workarounds */
	if (pdev->cpu_id == AMD_CPU_ID_CZN && !disable_workarounds) {
		rc = amd_pmc_verify_czn_rtc(pdev, &arg);
		if (rc) {
			dev_err(pdev->dev, "failed to set RTC: %d\n", rc);
			return;
		}
	}

	msg = amd_pmc_get_os_hint(pdev);
	rc = amd_pmc_send_cmd(pdev, arg, NULL, msg, false);
	if (rc) {
		dev_err(pdev->dev, "suspend failed: %d\n", rc);
		return;
	}

	rc = amd_stb_write(pdev, AMD_PMC_STB_S2IDLE_PREPARE);
	if (rc)
		dev_err(pdev->dev, "error writing to STB: %d\n", rc);
}

static void amd_pmc_s2idle_check(void)
{
	struct amd_pmc_dev *pdev = &pmc;
	struct smu_metrics table;
	int rc;

	/* Avoid triggering OVP */
	if (!get_metrics_table(pdev, &table) && table.s0i3_last_entry_status)
		msleep(2500);

	/* Dump the IdleMask before we add to the STB */
	amd_pmc_idlemask_read(pdev, pdev->dev, NULL);

	rc = amd_stb_write(pdev, AMD_PMC_STB_S2IDLE_CHECK);
	if (rc)
		dev_err(pdev->dev, "error writing to STB: %d\n", rc);
}

static int amd_pmc_dump_data(struct amd_pmc_dev *pdev)
{
	if (pdev->cpu_id == AMD_CPU_ID_PCO)
		return -ENODEV;

	return amd_pmc_send_cmd(pdev, 0, NULL, SMU_MSG_LOG_DUMP_DATA, false);
}

static void amd_pmc_s2idle_restore(void)
{
	struct amd_pmc_dev *pdev = &pmc;
	int rc;
	u8 msg;

	msg = amd_pmc_get_os_hint(pdev);
	rc = amd_pmc_send_cmd(pdev, 0, NULL, msg, false);
	if (rc)
		dev_err(pdev->dev, "resume failed: %d\n", rc);

	/* Let SMU know that we are looking for stats */
	amd_pmc_dump_data(pdev);

	rc = amd_stb_write(pdev, AMD_PMC_STB_S2IDLE_RESTORE);
	if (rc)
		dev_err(pdev->dev, "error writing to STB: %d\n", rc);

	/* Notify on failed entry */
	amd_pmc_validate_deepest(pdev);

	amd_pmc_process_restore_quirks(pdev);
}

static struct acpi_s2idle_dev_ops amd_pmc_s2idle_dev_ops = {
	.prepare = amd_pmc_s2idle_prepare,
	.check = amd_pmc_s2idle_check,
	.restore = amd_pmc_s2idle_restore,
};

static int amd_pmc_suspend_handler(struct device *dev)
{
	struct amd_pmc_dev *pdev = dev_get_drvdata(dev);
	int rc;

	/*
	 * Must be called only from the same set of dev_pm_ops handlers
	 * as i8042_pm_suspend() is called: currently just from .suspend.
	 */
	if (pdev->disable_8042_wakeup && !disable_workarounds) {
		rc = amd_pmc_wa_irq1(pdev);
		if (rc) {
			dev_err(pdev->dev, "failed to adjust keyboard wakeup: %d\n", rc);
			return rc;
		}
	}

	return 0;
}

static const struct dev_pm_ops amd_pmc_pm = {
	.suspend = amd_pmc_suspend_handler,
};

static const struct pci_device_id pmc_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_PS) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_CB) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_YC) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_CZN) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_RN) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_PCO) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_RV) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_SP) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_SHP) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_1AH_M20H_ROOT) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_1AH_M60H_ROOT) },
	{ }
};

static int amd_pmc_probe(struct platform_device *pdev)
{
	struct amd_pmc_dev *dev = &pmc;
	struct pci_dev *rdev;
	u32 base_addr_lo, base_addr_hi;
	u64 base_addr;
	int err;
	u32 val;

	dev->dev = &pdev->dev;
	rdev = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0, 0));
	if (!rdev || !pci_match_id(pmc_pci_ids, rdev)) {
		err = -ENODEV;
		goto err_pci_dev_put;
	}

	dev->cpu_id = rdev->device;
	if (dev->cpu_id == AMD_CPU_ID_SP || dev->cpu_id == AMD_CPU_ID_SHP) {
		dev_warn_once(dev->dev, "S0i3 is not supported on this hardware\n");
		err = -ENODEV;
		goto err_pci_dev_put;
	}

	dev->rdev = rdev;
	err = amd_smn_read(0, AMD_PMC_BASE_ADDR_LO, &val);
	if (err) {
		dev_err(dev->dev, "error reading 0x%x\n", AMD_PMC_BASE_ADDR_LO);
		err = pcibios_err_to_errno(err);
		goto err_pci_dev_put;
	}

	base_addr_lo = val & AMD_PMC_BASE_ADDR_HI_MASK;
	err = amd_smn_read(0, AMD_PMC_BASE_ADDR_HI, &val);
	if (err) {
		dev_err(dev->dev, "error reading 0x%x\n", AMD_PMC_BASE_ADDR_HI);
		err = pcibios_err_to_errno(err);
		goto err_pci_dev_put;
	}

	base_addr_hi = val & AMD_PMC_BASE_ADDR_LO_MASK;
	base_addr = ((u64)base_addr_hi << 32 | base_addr_lo);

	dev->regbase = devm_ioremap(dev->dev, base_addr + AMD_PMC_BASE_ADDR_OFFSET,
				    AMD_PMC_MAPPING_SIZE);
	if (!dev->regbase) {
		err = -ENOMEM;
		goto err_pci_dev_put;
	}

	err = devm_mutex_init(dev->dev, &dev->lock);
	if (err)
		goto err_pci_dev_put;

	/* Get num of IP blocks within the SoC */
	amd_pmc_get_ip_info(dev);

	platform_set_drvdata(pdev, dev);
	if (IS_ENABLED(CONFIG_SUSPEND)) {
		err = acpi_register_lps0_dev(&amd_pmc_s2idle_dev_ops);
		if (err)
			dev_warn(dev->dev, "failed to register LPS0 sleep handler, expect increased power consumption\n");
		if (!disable_workarounds)
			amd_pmc_quirks_init(dev);
	}

	amd_pmc_dbgfs_register(dev);
	err = amd_stb_s2d_init(dev);
	if (err)
		goto err_pci_dev_put;

	if (IS_ENABLED(CONFIG_AMD_MP2_STB))
		amd_mp2_stb_init(dev);
	pm_report_max_hw_sleep(U64_MAX);
	return 0;

err_pci_dev_put:
	pci_dev_put(rdev);
	return err;
}

static void amd_pmc_remove(struct platform_device *pdev)
{
	struct amd_pmc_dev *dev = platform_get_drvdata(pdev);

	if (IS_ENABLED(CONFIG_SUSPEND))
		acpi_unregister_lps0_dev(&amd_pmc_s2idle_dev_ops);
	amd_pmc_dbgfs_unregister(dev);
	pci_dev_put(dev->rdev);
	if (IS_ENABLED(CONFIG_AMD_MP2_STB))
		amd_mp2_stb_deinit(dev);
}

static const struct acpi_device_id amd_pmc_acpi_ids[] = {
	{"AMDI0005", 0},
	{"AMDI0006", 0},
	{"AMDI0007", 0},
	{"AMDI0008", 0},
	{"AMDI0009", 0},
	{"AMDI000A", 0},
	{"AMDI000B", 0},
	{"AMD0004", 0},
	{"AMD0005", 0},
	{ }
};
MODULE_DEVICE_TABLE(acpi, amd_pmc_acpi_ids);

static struct platform_driver amd_pmc_driver = {
	.driver = {
		.name = "amd_pmc",
		.acpi_match_table = amd_pmc_acpi_ids,
		.dev_groups = pmc_groups,
		.pm = pm_sleep_ptr(&amd_pmc_pm),
	},
	.probe = amd_pmc_probe,
	.remove = amd_pmc_remove,
};
module_platform_driver(amd_pmc_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("AMD PMC Driver");

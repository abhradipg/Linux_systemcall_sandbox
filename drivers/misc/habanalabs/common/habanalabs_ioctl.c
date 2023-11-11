// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright 2016-2022 HabanaLabs, Ltd.
 * All Rights Reserved.
 */

#define pr_fmt(fmt)	"habanalabs: " fmt

#include <uapi/misc/habanalabs.h>
#include "habanalabs.h"

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

static u32 hl_debug_struct_size[HL_DEBUG_OP_TIMESTAMP + 1] = {
	[HL_DEBUG_OP_ETR] = sizeof(struct hl_debug_params_etr),
	[HL_DEBUG_OP_ETF] = sizeof(struct hl_debug_params_etf),
	[HL_DEBUG_OP_STM] = sizeof(struct hl_debug_params_stm),
	[HL_DEBUG_OP_FUNNEL] = 0,
	[HL_DEBUG_OP_BMON] = sizeof(struct hl_debug_params_bmon),
	[HL_DEBUG_OP_SPMU] = sizeof(struct hl_debug_params_spmu),
	[HL_DEBUG_OP_TIMESTAMP] = 0

};

static int device_status_info(struct hl_device *hdev, struct hl_info_args *args)
{
	struct hl_info_device_status dev_stat = {0};
	u32 size = args->return_size;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;

	if ((!size) || (!out))
		return -EINVAL;

	dev_stat.status = hl_device_status(hdev);

	return copy_to_user(out, &dev_stat,
			min((size_t)size, sizeof(dev_stat))) ? -EFAULT : 0;
}

static int hw_ip_info(struct hl_device *hdev, struct hl_info_args *args)
{
	struct hl_info_hw_ip_info hw_ip = {0};
	u32 size = args->return_size;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;
	struct asic_fixed_properties *prop = &hdev->asic_prop;
	u64 sram_kmd_size, dram_kmd_size, dram_available_size;

	if ((!size) || (!out))
		return -EINVAL;

	sram_kmd_size = (prop->sram_user_base_address -
				prop->sram_base_address);
	dram_kmd_size = (prop->dram_user_base_address -
				prop->dram_base_address);

	hw_ip.device_id = hdev->asic_funcs->get_pci_id(hdev);
	hw_ip.sram_base_address = prop->sram_user_base_address;
	hw_ip.dram_base_address =
			hdev->mmu_enable && prop->dram_supports_virtual_memory ?
			prop->dmmu.start_addr : prop->dram_user_base_address;
	hw_ip.tpc_enabled_mask = prop->tpc_enabled_mask & 0xFF;
	hw_ip.tpc_enabled_mask_ext = prop->tpc_enabled_mask;

	hw_ip.sram_size = prop->sram_size - sram_kmd_size;

	dram_available_size = prop->dram_size - dram_kmd_size;

	if (hdev->mmu_enable == MMU_EN_ALL)
		hw_ip.dram_size = DIV_ROUND_DOWN_ULL(dram_available_size,
				prop->dram_page_size) * prop->dram_page_size;
	else
		hw_ip.dram_size = dram_available_size;

	if (hw_ip.dram_size > PAGE_SIZE)
		hw_ip.dram_enabled = 1;

	hw_ip.dram_page_size = prop->dram_page_size;
	hw_ip.device_mem_alloc_default_page_size = prop->device_mem_alloc_default_page_size;
	hw_ip.num_of_events = prop->num_of_events;

	memcpy(hw_ip.cpucp_version, prop->cpucp_info.cpucp_version,
		min(VERSION_MAX_LEN, HL_INFO_VERSION_MAX_LEN));

	memcpy(hw_ip.card_name, prop->cpucp_info.card_name,
		min(CARD_NAME_MAX_LEN, HL_INFO_CARD_NAME_MAX_LEN));

	hw_ip.cpld_version = le32_to_cpu(prop->cpucp_info.cpld_version);
	hw_ip.module_id = le32_to_cpu(prop->cpucp_info.card_location);

	hw_ip.psoc_pci_pll_nr = prop->psoc_pci_pll_nr;
	hw_ip.psoc_pci_pll_nf = prop->psoc_pci_pll_nf;
	hw_ip.psoc_pci_pll_od = prop->psoc_pci_pll_od;
	hw_ip.psoc_pci_pll_div_factor = prop->psoc_pci_pll_div_factor;

	hw_ip.decoder_enabled_mask = prop->decoder_enabled_mask;
	hw_ip.mme_master_slave_mode = prop->mme_master_slave_mode;
	hw_ip.first_available_interrupt_id = prop->first_available_user_interrupt;
	hw_ip.number_of_user_interrupts = prop->user_interrupt_count;

	hw_ip.edma_enabled_mask = prop->edma_enabled_mask;
	hw_ip.server_type = prop->server_type;

	return copy_to_user(out, &hw_ip,
		min((size_t) size, sizeof(hw_ip))) ? -EFAULT : 0;
}

static int hw_events_info(struct hl_device *hdev, bool aggregate,
			struct hl_info_args *args)
{
	u32 size, max_size = args->return_size;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;
	void *arr;

	if ((!max_size) || (!out))
		return -EINVAL;

	arr = hdev->asic_funcs->get_events_stat(hdev, aggregate, &size);

	return copy_to_user(out, arr, min(max_size, size)) ? -EFAULT : 0;
}

static int events_info(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	u32 max_size = args->return_size;
	u64 events_mask;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;

	if ((max_size < sizeof(u64)) || (!out))
		return -EINVAL;

	mutex_lock(&hpriv->notifier_event.lock);
	events_mask = hpriv->notifier_event.events_mask;
	hpriv->notifier_event.events_mask = 0;
	mutex_unlock(&hpriv->notifier_event.lock);

	return copy_to_user(out, &events_mask, sizeof(u64)) ? -EFAULT : 0;
}

static int dram_usage_info(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	struct hl_device *hdev = hpriv->hdev;
	struct hl_info_dram_usage dram_usage = {0};
	u32 max_size = args->return_size;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;
	struct asic_fixed_properties *prop = &hdev->asic_prop;
	u64 dram_kmd_size;

	if ((!max_size) || (!out))
		return -EINVAL;

	dram_kmd_size = (prop->dram_user_base_address -
				prop->dram_base_address);
	dram_usage.dram_free_mem = (prop->dram_size - dram_kmd_size) -
					atomic64_read(&hdev->dram_used_mem);
	if (hpriv->ctx)
		dram_usage.ctx_dram_mem =
			atomic64_read(&hpriv->ctx->dram_phys_mem);

	return copy_to_user(out, &dram_usage,
		min((size_t) max_size, sizeof(dram_usage))) ? -EFAULT : 0;
}

static int hw_idle(struct hl_device *hdev, struct hl_info_args *args)
{
	struct hl_info_hw_idle hw_idle = {0};
	u32 max_size = args->return_size;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;

	if ((!max_size) || (!out))
		return -EINVAL;

	hw_idle.is_idle = hdev->asic_funcs->is_device_idle(hdev,
					hw_idle.busy_engines_mask_ext,
					HL_BUSY_ENGINES_MASK_EXT_SIZE, NULL);
	hw_idle.busy_engines_mask =
			lower_32_bits(hw_idle.busy_engines_mask_ext[0]);

	return copy_to_user(out, &hw_idle,
		min((size_t) max_size, sizeof(hw_idle))) ? -EFAULT : 0;
}

static int debug_coresight(struct hl_device *hdev, struct hl_ctx *ctx, struct hl_debug_args *args)
{
	struct hl_debug_params *params;
	void *input = NULL, *output = NULL;
	int rc;

	params = kzalloc(sizeof(*params), GFP_KERNEL);
	if (!params)
		return -ENOMEM;

	params->reg_idx = args->reg_idx;
	params->enable = args->enable;
	params->op = args->op;

	if (args->input_ptr && args->input_size) {
		input = kzalloc(hl_debug_struct_size[args->op], GFP_KERNEL);
		if (!input) {
			rc = -ENOMEM;
			goto out;
		}

		if (copy_from_user(input, u64_to_user_ptr(args->input_ptr),
					args->input_size)) {
			rc = -EFAULT;
			dev_err(hdev->dev, "failed to copy input debug data\n");
			goto out;
		}

		params->input = input;
	}

	if (args->output_ptr && args->output_size) {
		output = kzalloc(args->output_size, GFP_KERNEL);
		if (!output) {
			rc = -ENOMEM;
			goto out;
		}

		params->output = output;
		params->output_size = args->output_size;
	}

	rc = hdev->asic_funcs->debug_coresight(hdev, ctx, params);
	if (rc) {
		dev_err(hdev->dev,
			"debug coresight operation failed %d\n", rc);
		goto out;
	}

	if (output && copy_to_user((void __user *) (uintptr_t) args->output_ptr,
					output, args->output_size)) {
		dev_err(hdev->dev, "copy to user failed in debug ioctl\n");
		rc = -EFAULT;
		goto out;
	}


out:
	kfree(params);
	kfree(output);
	kfree(input);

	return rc;
}

static int device_utilization(struct hl_device *hdev, struct hl_info_args *args)
{
	struct hl_info_device_utilization device_util = {0};
	u32 max_size = args->return_size;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;
	int rc;

	if ((!max_size) || (!out))
		return -EINVAL;

	rc = hl_device_utilization(hdev, &device_util.utilization);
	if (rc)
		return -EINVAL;

	return copy_to_user(out, &device_util,
		min((size_t) max_size, sizeof(device_util))) ? -EFAULT : 0;
}

static int get_clk_rate(struct hl_device *hdev, struct hl_info_args *args)
{
	struct hl_info_clk_rate clk_rate = {0};
	u32 max_size = args->return_size;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;
	int rc;

	if ((!max_size) || (!out))
		return -EINVAL;

	rc = hl_fw_get_clk_rate(hdev, &clk_rate.cur_clk_rate_mhz, &clk_rate.max_clk_rate_mhz);
	if (rc)
		return rc;

	return copy_to_user(out, &clk_rate, min_t(size_t, max_size, sizeof(clk_rate)))
										? -EFAULT : 0;
}

static int get_reset_count(struct hl_device *hdev, struct hl_info_args *args)
{
	struct hl_info_reset_count reset_count = {0};
	u32 max_size = args->return_size;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;

	if ((!max_size) || (!out))
		return -EINVAL;

	reset_count.hard_reset_cnt = hdev->reset_info.hard_reset_cnt;
	reset_count.soft_reset_cnt = hdev->reset_info.compute_reset_cnt;

	return copy_to_user(out, &reset_count,
		min((size_t) max_size, sizeof(reset_count))) ? -EFAULT : 0;
}

static int time_sync_info(struct hl_device *hdev, struct hl_info_args *args)
{
	struct hl_info_time_sync time_sync = {0};
	u32 max_size = args->return_size;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;

	if ((!max_size) || (!out))
		return -EINVAL;

	time_sync.device_time = hdev->asic_funcs->get_device_time(hdev);
	time_sync.host_time = ktime_get_raw_ns();

	return copy_to_user(out, &time_sync,
		min((size_t) max_size, sizeof(time_sync))) ? -EFAULT : 0;
}

static int pci_counters_info(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	struct hl_device *hdev = hpriv->hdev;
	struct hl_info_pci_counters pci_counters = {0};
	u32 max_size = args->return_size;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;
	int rc;

	if ((!max_size) || (!out))
		return -EINVAL;

	rc = hl_fw_cpucp_pci_counters_get(hdev, &pci_counters);
	if (rc)
		return rc;

	return copy_to_user(out, &pci_counters,
		min((size_t) max_size, sizeof(pci_counters))) ? -EFAULT : 0;
}

static int clk_throttle_info(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;
	struct hl_device *hdev = hpriv->hdev;
	struct hl_info_clk_throttle clk_throttle = {0};
	ktime_t end_time, zero_time = ktime_set(0, 0);
	u32 max_size = args->return_size;
	int i;

	if ((!max_size) || (!out))
		return -EINVAL;

	mutex_lock(&hdev->clk_throttling.lock);

	clk_throttle.clk_throttling_reason = hdev->clk_throttling.current_reason;

	for (i = 0 ; i < HL_CLK_THROTTLE_TYPE_MAX ; i++) {
		if (!(hdev->clk_throttling.aggregated_reason & BIT(i)))
			continue;

		clk_throttle.clk_throttling_timestamp_us[i] =
			ktime_to_us(hdev->clk_throttling.timestamp[i].start);

		if (ktime_compare(hdev->clk_throttling.timestamp[i].end, zero_time))
			end_time = hdev->clk_throttling.timestamp[i].end;
		else
			end_time = ktime_get();

		clk_throttle.clk_throttling_duration_ns[i] =
			ktime_to_ns(ktime_sub(end_time,
				hdev->clk_throttling.timestamp[i].start));

	}
	mutex_unlock(&hdev->clk_throttling.lock);

	return copy_to_user(out, &clk_throttle,
		min((size_t) max_size, sizeof(clk_throttle))) ? -EFAULT : 0;
}

static int cs_counters_info(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;
	struct hl_info_cs_counters cs_counters = {0};
	struct hl_device *hdev = hpriv->hdev;
	struct hl_cs_counters_atomic *cntr;
	u32 max_size = args->return_size;

	cntr = &hdev->aggregated_cs_counters;

	if ((!max_size) || (!out))
		return -EINVAL;

	cs_counters.total_out_of_mem_drop_cnt =
			atomic64_read(&cntr->out_of_mem_drop_cnt);
	cs_counters.total_parsing_drop_cnt =
			atomic64_read(&cntr->parsing_drop_cnt);
	cs_counters.total_queue_full_drop_cnt =
			atomic64_read(&cntr->queue_full_drop_cnt);
	cs_counters.total_device_in_reset_drop_cnt =
			atomic64_read(&cntr->device_in_reset_drop_cnt);
	cs_counters.total_max_cs_in_flight_drop_cnt =
			atomic64_read(&cntr->max_cs_in_flight_drop_cnt);
	cs_counters.total_validation_drop_cnt =
			atomic64_read(&cntr->validation_drop_cnt);

	if (hpriv->ctx) {
		cs_counters.ctx_out_of_mem_drop_cnt =
				atomic64_read(
				&hpriv->ctx->cs_counters.out_of_mem_drop_cnt);
		cs_counters.ctx_parsing_drop_cnt =
				atomic64_read(
				&hpriv->ctx->cs_counters.parsing_drop_cnt);
		cs_counters.ctx_queue_full_drop_cnt =
				atomic64_read(
				&hpriv->ctx->cs_counters.queue_full_drop_cnt);
		cs_counters.ctx_device_in_reset_drop_cnt =
				atomic64_read(
			&hpriv->ctx->cs_counters.device_in_reset_drop_cnt);
		cs_counters.ctx_max_cs_in_flight_drop_cnt =
				atomic64_read(
			&hpriv->ctx->cs_counters.max_cs_in_flight_drop_cnt);
		cs_counters.ctx_validation_drop_cnt =
				atomic64_read(
				&hpriv->ctx->cs_counters.validation_drop_cnt);
	}

	return copy_to_user(out, &cs_counters,
		min((size_t) max_size, sizeof(cs_counters))) ? -EFAULT : 0;
}

static int sync_manager_info(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	struct hl_device *hdev = hpriv->hdev;
	struct asic_fixed_properties *prop = &hdev->asic_prop;
	struct hl_info_sync_manager sm_info = {0};
	u32 max_size = args->return_size;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;

	if ((!max_size) || (!out))
		return -EINVAL;

	if (args->dcore_id >= HL_MAX_DCORES)
		return -EINVAL;

	sm_info.first_available_sync_object =
			prop->first_available_user_sob[args->dcore_id];
	sm_info.first_available_monitor =
			prop->first_available_user_mon[args->dcore_id];
	sm_info.first_available_cq =
			prop->first_available_cq[args->dcore_id];

	return copy_to_user(out, &sm_info, min_t(size_t, (size_t) max_size,
			sizeof(sm_info))) ? -EFAULT : 0;
}

static int total_energy_consumption_info(struct hl_fpriv *hpriv,
			struct hl_info_args *args)
{
	struct hl_device *hdev = hpriv->hdev;
	struct hl_info_energy total_energy = {0};
	u32 max_size = args->return_size;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;
	int rc;

	if ((!max_size) || (!out))
		return -EINVAL;

	rc = hl_fw_cpucp_total_energy_get(hdev,
			&total_energy.total_energy_consumption);
	if (rc)
		return rc;

	return copy_to_user(out, &total_energy,
		min((size_t) max_size, sizeof(total_energy))) ? -EFAULT : 0;
}

static int pll_frequency_info(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	struct hl_device *hdev = hpriv->hdev;
	struct hl_pll_frequency_info freq_info = { {0} };
	u32 max_size = args->return_size;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;
	int rc;

	if ((!max_size) || (!out))
		return -EINVAL;

	rc = hl_fw_cpucp_pll_info_get(hdev, args->pll_index, freq_info.output);
	if (rc)
		return rc;

	return copy_to_user(out, &freq_info,
		min((size_t) max_size, sizeof(freq_info))) ? -EFAULT : 0;
}

static int power_info(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	struct hl_device *hdev = hpriv->hdev;
	u32 max_size = args->return_size;
	struct hl_power_info power_info = {0};
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;
	int rc;

	if ((!max_size) || (!out))
		return -EINVAL;

	rc = hl_fw_cpucp_power_get(hdev, &power_info.power);
	if (rc)
		return rc;

	return copy_to_user(out, &power_info,
		min((size_t) max_size, sizeof(power_info))) ? -EFAULT : 0;
}

static int open_stats_info(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	struct hl_device *hdev = hpriv->hdev;
	u32 max_size = args->return_size;
	struct hl_open_stats_info open_stats_info = {0};
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;

	if ((!max_size) || (!out))
		return -EINVAL;

	open_stats_info.last_open_period_ms = jiffies64_to_msecs(
		hdev->last_open_session_duration_jif);
	open_stats_info.open_counter = hdev->open_counter;
	open_stats_info.is_compute_ctx_active = hdev->is_compute_ctx_active;
	open_stats_info.compute_ctx_in_release = hdev->compute_ctx_in_release;

	return copy_to_user(out, &open_stats_info,
		min((size_t) max_size, sizeof(open_stats_info))) ? -EFAULT : 0;
}

static int dram_pending_rows_info(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	struct hl_device *hdev = hpriv->hdev;
	u32 max_size = args->return_size;
	u32 pend_rows_num = 0;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;
	int rc;

	if ((!max_size) || (!out))
		return -EINVAL;

	rc = hl_fw_dram_pending_row_get(hdev, &pend_rows_num);
	if (rc)
		return rc;

	return copy_to_user(out, &pend_rows_num,
			min_t(size_t, max_size, sizeof(pend_rows_num))) ? -EFAULT : 0;
}

static int dram_replaced_rows_info(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	struct hl_device *hdev = hpriv->hdev;
	u32 max_size = args->return_size;
	struct cpucp_hbm_row_info info = {0};
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;
	int rc;

	if ((!max_size) || (!out))
		return -EINVAL;

	rc = hl_fw_dram_replaced_row_get(hdev, &info);
	if (rc)
		return rc;

	return copy_to_user(out, &info, min_t(size_t, max_size, sizeof(info))) ? -EFAULT : 0;
}

static int last_err_open_dev_info(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	struct hl_info_last_err_open_dev_time info = {0};
	struct hl_device *hdev = hpriv->hdev;
	u32 max_size = args->return_size;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;

	if ((!max_size) || (!out))
		return -EINVAL;

	info.timestamp = ktime_to_ns(hdev->last_successful_open_ktime);

	return copy_to_user(out, &info, min_t(size_t, max_size, sizeof(info))) ? -EFAULT : 0;
}

static int cs_timeout_info(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	struct hl_info_cs_timeout_event info = {0};
	struct hl_device *hdev = hpriv->hdev;
	u32 max_size = args->return_size;
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;

	if ((!max_size) || (!out))
		return -EINVAL;

	info.seq = hdev->last_error.cs_timeout.seq;
	info.timestamp = ktime_to_ns(hdev->last_error.cs_timeout.timestamp);

	return copy_to_user(out, &info, min_t(size_t, max_size, sizeof(info))) ? -EFAULT : 0;
}

static int razwi_info(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	struct hl_device *hdev = hpriv->hdev;
	u32 max_size = args->return_size;
	struct hl_info_razwi_event info = {0};
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;

	if ((!max_size) || (!out))
		return -EINVAL;

	info.timestamp = ktime_to_ns(hdev->last_error.razwi.timestamp);
	info.addr = hdev->last_error.razwi.addr;
	info.engine_id_1 = hdev->last_error.razwi.engine_id_1;
	info.engine_id_2 = hdev->last_error.razwi.engine_id_2;
	info.no_engine_id = hdev->last_error.razwi.non_engine_initiator;
	info.error_type = hdev->last_error.razwi.type;

	return copy_to_user(out, &info, min_t(size_t, max_size, sizeof(info))) ? -EFAULT : 0;
}

static int undefined_opcode_info(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	struct hl_device *hdev = hpriv->hdev;
	u32 max_size = args->return_size;
	struct hl_info_undefined_opcode_event info = {0};
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;

	if ((!max_size) || (!out))
		return -EINVAL;

	info.timestamp = ktime_to_ns(hdev->last_error.undef_opcode.timestamp);
	info.engine_id = hdev->last_error.undef_opcode.engine_id;
	info.cq_addr = hdev->last_error.undef_opcode.cq_addr;
	info.cq_size = hdev->last_error.undef_opcode.cq_size;
	info.stream_id = hdev->last_error.undef_opcode.stream_id;
	info.cb_addr_streams_len = hdev->last_error.undef_opcode.cb_addr_streams_len;
	memcpy(info.cb_addr_streams, hdev->last_error.undef_opcode.cb_addr_streams,
			sizeof(info.cb_addr_streams));

	return copy_to_user(out, &info, min_t(size_t, max_size, sizeof(info))) ? -EFAULT : 0;
}

static int dev_mem_alloc_page_sizes_info(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	void __user *out = (void __user *) (uintptr_t) args->return_pointer;
	struct hl_info_dev_memalloc_page_sizes info = {0};
	struct hl_device *hdev = hpriv->hdev;
	u32 max_size = args->return_size;

	if ((!max_size) || (!out))
		return -EINVAL;

	/*
	 * Future ASICs that will support multiple DRAM page sizes will support only "powers of 2"
	 * pages (unlike some of the ASICs before supporting multiple page sizes).
	 * For this reason for all ASICs that not support multiple page size the function will
	 * return an empty bitmask indicating that multiple page sizes is not supported.
	 */
	info.page_order_bitmask = hdev->asic_prop.dmmu.supported_pages_mask;

	return copy_to_user(out, &info, min_t(size_t, max_size, sizeof(info))) ? -EFAULT : 0;
}

static int eventfd_register(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	int rc;

	/* check if there is already a registered on that process */
	mutex_lock(&hpriv->notifier_event.lock);
	if (hpriv->notifier_event.eventfd) {
		mutex_unlock(&hpriv->notifier_event.lock);
		return -EINVAL;
	}

	hpriv->notifier_event.eventfd = eventfd_ctx_fdget(args->eventfd);
	if (IS_ERR(hpriv->notifier_event.eventfd)) {
		rc = PTR_ERR(hpriv->notifier_event.eventfd);
		hpriv->notifier_event.eventfd = NULL;
		mutex_unlock(&hpriv->notifier_event.lock);
		return rc;
	}

	mutex_unlock(&hpriv->notifier_event.lock);
	return 0;
}

static int eventfd_unregister(struct hl_fpriv *hpriv, struct hl_info_args *args)
{
	mutex_lock(&hpriv->notifier_event.lock);
	if (!hpriv->notifier_event.eventfd) {
		mutex_unlock(&hpriv->notifier_event.lock);
		return -EINVAL;
	}

	eventfd_ctx_put(hpriv->notifier_event.eventfd);
	hpriv->notifier_event.eventfd = NULL;
	mutex_unlock(&hpriv->notifier_event.lock);
	return 0;
}

static int _hl_info_ioctl(struct hl_fpriv *hpriv, void *data,
				struct device *dev)
{
	enum hl_device_status status;
	struct hl_info_args *args = data;
	struct hl_device *hdev = hpriv->hdev;

	int rc;

	/*
	 * Information is returned for the following opcodes even if the device
	 * is disabled or in reset.
	 */
	switch (args->op) {
	case HL_INFO_HW_IP_INFO:
		return hw_ip_info(hdev, args);

	case HL_INFO_DEVICE_STATUS:
		return device_status_info(hdev, args);

	case HL_INFO_RESET_COUNT:
		return get_reset_count(hdev, args);

	case HL_INFO_HW_EVENTS:
		return hw_events_info(hdev, false, args);

	case HL_INFO_HW_EVENTS_AGGREGATE:
		return hw_events_info(hdev, true, args);

	case HL_INFO_CS_COUNTERS:
		return cs_counters_info(hpriv, args);

	case HL_INFO_CLK_THROTTLE_REASON:
		return clk_throttle_info(hpriv, args);

	case HL_INFO_SYNC_MANAGER:
		return sync_manager_info(hpriv, args);

	case HL_INFO_OPEN_STATS:
		return open_stats_info(hpriv, args);

	case HL_INFO_LAST_ERR_OPEN_DEV_TIME:
		return last_err_open_dev_info(hpriv, args);

	case HL_INFO_CS_TIMEOUT_EVENT:
		return cs_timeout_info(hpriv, args);

	case HL_INFO_RAZWI_EVENT:
		return razwi_info(hpriv, args);

	case HL_INFO_UNDEFINED_OPCODE_EVENT:
		return undefined_opcode_info(hpriv, args);

	case HL_INFO_DEV_MEM_ALLOC_PAGE_SIZES:
		return dev_mem_alloc_page_sizes_info(hpriv, args);

	case HL_INFO_GET_EVENTS:
		return events_info(hpriv, args);

	default:
		break;
	}

	if (!hl_device_operational(hdev, &status)) {
		dev_warn_ratelimited(dev,
			"Device is %s. Can't execute INFO IOCTL\n",
			hdev->status[status]);
		return -EBUSY;
	}

	switch (args->op) {
	case HL_INFO_DRAM_USAGE:
		rc = dram_usage_info(hpriv, args);
		break;

	case HL_INFO_HW_IDLE:
		rc = hw_idle(hdev, args);
		break;

	case HL_INFO_DEVICE_UTILIZATION:
		rc = device_utilization(hdev, args);
		break;

	case HL_INFO_CLK_RATE:
		rc = get_clk_rate(hdev, args);
		break;

	case HL_INFO_TIME_SYNC:
		return time_sync_info(hdev, args);

	case HL_INFO_PCI_COUNTERS:
		return pci_counters_info(hpriv, args);

	case HL_INFO_TOTAL_ENERGY:
		return total_energy_consumption_info(hpriv, args);

	case HL_INFO_PLL_FREQUENCY:
		return pll_frequency_info(hpriv, args);

	case HL_INFO_POWER:
		return power_info(hpriv, args);


	case HL_INFO_DRAM_REPLACED_ROWS:
		return dram_replaced_rows_info(hpriv, args);

	case HL_INFO_DRAM_PENDING_ROWS:
		return dram_pending_rows_info(hpriv, args);

	case HL_INFO_REGISTER_EVENTFD:
		return eventfd_register(hpriv, args);

	case HL_INFO_UNREGISTER_EVENTFD:
		return eventfd_unregister(hpriv, args);

	default:
		dev_err(dev, "Invalid request %d\n", args->op);
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int hl_info_ioctl(struct hl_fpriv *hpriv, void *data)
{
	return _hl_info_ioctl(hpriv, data, hpriv->hdev->dev);
}

static int hl_info_ioctl_control(struct hl_fpriv *hpriv, void *data)
{
	return _hl_info_ioctl(hpriv, data, hpriv->hdev->dev_ctrl);
}

static int hl_debug_ioctl(struct hl_fpriv *hpriv, void *data)
{
	struct hl_debug_args *args = data;
	struct hl_device *hdev = hpriv->hdev;
	enum hl_device_status status;

	int rc = 0;

	if (!hl_device_operational(hdev, &status)) {
		dev_warn_ratelimited(hdev->dev,
			"Device is %s. Can't execute DEBUG IOCTL\n",
			hdev->status[status]);
		return -EBUSY;
	}

	switch (args->op) {
	case HL_DEBUG_OP_ETR:
	case HL_DEBUG_OP_ETF:
	case HL_DEBUG_OP_STM:
	case HL_DEBUG_OP_FUNNEL:
	case HL_DEBUG_OP_BMON:
	case HL_DEBUG_OP_SPMU:
	case HL_DEBUG_OP_TIMESTAMP:
		if (!hdev->in_debug) {
			dev_err_ratelimited(hdev->dev,
				"Rejecting debug configuration request because device not in debug mode\n");
			return -EFAULT;
		}
		args->input_size = min(args->input_size, hl_debug_struct_size[args->op]);
		rc = debug_coresight(hdev, hpriv->ctx, args);
		break;

	case HL_DEBUG_OP_SET_MODE:
		rc = hl_device_set_debug_mode(hdev, hpriv->ctx, (bool) args->enable);
		break;

	default:
		dev_err(hdev->dev, "Invalid request %d\n", args->op);
		rc = -EINVAL;
		break;
	}

	return rc;
}

#define HL_IOCTL_DEF(ioctl, _func) \
	[_IOC_NR(ioctl)] = {.cmd = ioctl, .func = _func}

static const struct hl_ioctl_desc hl_ioctls[] = {
	HL_IOCTL_DEF(HL_IOCTL_INFO, hl_info_ioctl),
	HL_IOCTL_DEF(HL_IOCTL_CB, hl_cb_ioctl),
	HL_IOCTL_DEF(HL_IOCTL_CS, hl_cs_ioctl),
	HL_IOCTL_DEF(HL_IOCTL_WAIT_CS, hl_wait_ioctl),
	HL_IOCTL_DEF(HL_IOCTL_MEMORY, hl_mem_ioctl),
	HL_IOCTL_DEF(HL_IOCTL_DEBUG, hl_debug_ioctl)
};

static const struct hl_ioctl_desc hl_ioctls_control[] = {
	HL_IOCTL_DEF(HL_IOCTL_INFO, hl_info_ioctl_control)
};

static long _hl_ioctl(struct file *filep, unsigned int cmd, unsigned long arg,
		const struct hl_ioctl_desc *ioctl, struct device *dev)
{
	struct hl_fpriv *hpriv = filep->private_data;
	unsigned int nr = _IOC_NR(cmd);
	char stack_kdata[128] = {0};
	char *kdata = NULL;
	unsigned int usize, asize;
	hl_ioctl_t *func;
	u32 hl_size;
	int retcode;

	/* Do not trust userspace, use our own definition */
	func = ioctl->func;

	if (unlikely(!func)) {
		dev_dbg(dev, "no function\n");
		retcode = -ENOTTY;
		goto out_err;
	}

	hl_size = _IOC_SIZE(ioctl->cmd);
	usize = asize = _IOC_SIZE(cmd);
	if (hl_size > asize)
		asize = hl_size;

	cmd = ioctl->cmd;

	if (cmd & (IOC_IN | IOC_OUT)) {
		if (asize <= sizeof(stack_kdata)) {
			kdata = stack_kdata;
		} else {
			kdata = kzalloc(asize, GFP_KERNEL);
			if (!kdata) {
				retcode = -ENOMEM;
				goto out_err;
			}
		}
	}

	if (cmd & IOC_IN) {
		if (copy_from_user(kdata, (void __user *)arg, usize)) {
			retcode = -EFAULT;
			goto out_err;
		}
	} else if (cmd & IOC_OUT) {
		memset(kdata, 0, usize);
	}

	retcode = func(hpriv, kdata);

	if ((cmd & IOC_OUT) && copy_to_user((void __user *)arg, kdata, usize))
		retcode = -EFAULT;

out_err:
	if (retcode)
		dev_dbg(dev, "error in ioctl: pid=%d, cmd=0x%02x, nr=0x%02x\n",
			  task_pid_nr(current), cmd, nr);

	if (kdata != stack_kdata)
		kfree(kdata);

	return retcode;
}

long hl_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct hl_fpriv *hpriv = filep->private_data;
	struct hl_device *hdev = hpriv->hdev;
	const struct hl_ioctl_desc *ioctl = NULL;
	unsigned int nr = _IOC_NR(cmd);

	if (!hdev) {
		pr_err_ratelimited("Sending ioctl after device was removed! Please close FD\n");
		return -ENODEV;
	}

	if ((nr >= HL_COMMAND_START) && (nr < HL_COMMAND_END)) {
		ioctl = &hl_ioctls[nr];
	} else {
		dev_err(hdev->dev, "invalid ioctl: pid=%d, nr=0x%02x\n",
			task_pid_nr(current), nr);
		return -ENOTTY;
	}

	return _hl_ioctl(filep, cmd, arg, ioctl, hdev->dev);
}

long hl_ioctl_control(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct hl_fpriv *hpriv = filep->private_data;
	struct hl_device *hdev = hpriv->hdev;
	const struct hl_ioctl_desc *ioctl = NULL;
	unsigned int nr = _IOC_NR(cmd);

	if (!hdev) {
		pr_err_ratelimited("Sending ioctl after device was removed! Please close FD\n");
		return -ENODEV;
	}

	if (nr == _IOC_NR(HL_IOCTL_INFO)) {
		ioctl = &hl_ioctls_control[nr];
	} else {
		dev_err(hdev->dev_ctrl, "invalid ioctl: pid=%d, nr=0x%02x\n",
			task_pid_nr(current), nr);
		return -ENOTTY;
	}

	return _hl_ioctl(filep, cmd, arg, ioctl, hdev->dev_ctrl);
}

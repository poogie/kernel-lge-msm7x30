/* Copyright (c) 2010-2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */
#include <linux/interrupt.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <mach/clk.h>
#include <mach/dal_axi.h>
#include <mach/msm_bus.h>

#include "kgsl.h"
#include "kgsl_log.h"

static int kgsl_pwrctrl_gpuclk_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	char temp[20];
	int i, delta = 5000000;
	unsigned long val;
	struct kgsl_device *device = kgsl_device_from_dev(dev);
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;

	snprintf(temp, sizeof(temp), "%.*s",
			 (int)min(count, sizeof(temp) - 1), buf);
	strict_strtoul(temp, 0, &val);

	mutex_lock(&device->mutex);
	/* Find the best match for the requested freq, if it exists */

	for (i = 0; i < pwr->num_pwrlevels; i++)
		if (abs(pwr->pwrlevels[i].gpu_freq - val) < delta) {
			pwr->requested_pwrlevel = i;
			break;
	}

	if (i < pwr->num_pwrlevels &&
	    pwr->requested_pwrlevel != pwr->active_pwrlevel) {
		device->ftbl.device_idle(device, KGSL_TIMEOUT_DEFAULT);
		kgsl_pwrctrl_axi(device, KGSL_PWRFLAGS_AXI_OFF);
		kgsl_pwrctrl_clk(device, KGSL_PWRFLAGS_CLK_OFF);
		pwr->active_pwrlevel = pwr->requested_pwrlevel;
		kgsl_pwrctrl_clk(device, KGSL_PWRFLAGS_CLK_ON);
		kgsl_pwrctrl_axi(device, KGSL_PWRFLAGS_AXI_ON);
		KGSL_PWR_WARN(device, "pwr level changed to %d\n",
			pwr->active_pwrlevel);
	}
	pwr->requested_pwrlevel = -1;
	mutex_unlock(&device->mutex);

	return count;
}

static int kgsl_pwrctrl_gpuclk_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct kgsl_device *device = kgsl_device_from_dev(dev);
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	return sprintf(buf, "%d\n",
			pwr->pwrlevels[pwr->active_pwrlevel].gpu_freq);
}

static struct device_attribute gpuclk_attr = {
	.attr = { .name = "gpuclk", .mode = 0644, },
	.show = kgsl_pwrctrl_gpuclk_show,
	.store = kgsl_pwrctrl_gpuclk_store,
};

int kgsl_pwrctrl_init_sysfs(struct kgsl_device *device)
{
	return device_create_file(device->dev, &gpuclk_attr);
}

void kgsl_pwrctrl_uninit_sysfs(struct kgsl_device *device)
{
	device_remove_file(device->dev, &gpuclk_attr);
}

void kgsl_pwrctrl_clk(struct kgsl_device *device, unsigned int pwrflag)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;

	switch (pwrflag) {
	case KGSL_PWRFLAGS_CLK_OFF:
		if (pwr->power_flags & KGSL_PWRFLAGS_CLK_ON) {
			KGSL_PWR_INFO(device,
				"clocks off, device %d\n", device->id);
			if (pwr->grp_pclk)
				clk_disable(pwr->grp_pclk);
			clk_disable(pwr->grp_clk);
			if (pwr->imem_clk != NULL)
				clk_disable(pwr->imem_clk);
			if (pwr->imem_pclk != NULL)
				clk_disable(pwr->imem_pclk);
			if (pwr->clk_freq[KGSL_MIN_FREQ])
				clk_set_rate(pwr->grp_src_clk,
					pwr->pwrlevels[pwr->num_pwrlevels - 1].
						gpu_freq);
			pwr->power_flags &=
					~(KGSL_PWRFLAGS_CLK_ON);
			pwr->power_flags |= KGSL_PWRFLAGS_CLK_OFF;
		}
		return;
	case KGSL_PWRFLAGS_CLK_ON:
		if (pwr->power_flags & KGSL_PWRFLAGS_CLK_OFF) {
			KGSL_PWR_INFO(device,
				"clocks on, device %d\n", device->id);
			if (pwr->pwrlevels[0].gpu_freq > 0)
				clk_set_rate(pwr->grp_src_clk,
					pwr->clk_freq[KGSL_MAX_FREQ]);
			if (pwr->grp_pclk)
				clk_enable(pwr->grp_pclk);
			clk_enable(pwr->grp_clk);
			if (pwr->imem_clk != NULL)
				clk_enable(pwr->imem_clk);
			if (pwr->imem_pclk != NULL)
				clk_enable(pwr->imem_pclk);

			pwr->power_flags &=
				~(KGSL_PWRFLAGS_CLK_OFF);
			pwr->power_flags |= KGSL_PWRFLAGS_CLK_ON;
		}
		return;
	default:
		return;
	}
}

void kgsl_pwrctrl_axi(struct kgsl_device *device, unsigned int pwrflag)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;

	switch (pwrflag) {
	case KGSL_PWRFLAGS_AXI_OFF:
		if (pwr->power_flags & KGSL_PWRFLAGS_AXI_ON) {
			KGSL_PWR_INFO(device,
				"axi off, device %d\n", device->id);
			if (pwr->ebi1_clk)
				clk_disable(pwr->ebi1_clk);
			if (pwr->pcl)
				msm_bus_scale_client_update_request(pwr->pcl,
								BW_INIT);
			pwr->power_flags &=
				~(KGSL_PWRFLAGS_AXI_ON);
			pwr->power_flags |= KGSL_PWRFLAGS_AXI_OFF;
		}
		return;
	case KGSL_PWRFLAGS_AXI_ON:
		if (pwr->power_flags & KGSL_PWRFLAGS_AXI_OFF) {
			KGSL_PWR_INFO(device,
				"axi on, device %d\n", device->id);
			if (pwr->ebi1_clk)
				clk_enable(pwr->ebi1_clk);
			if (pwr->pcl)
				msm_bus_scale_client_update_request(pwr->pcl,
								BW_MAX);
			pwr->power_flags &=
				~(KGSL_PWRFLAGS_AXI_OFF);
			pwr->power_flags |= KGSL_PWRFLAGS_AXI_ON;
		}
		return;
	default:
		return;
	}
}


void kgsl_pwrctrl_pwrrail(struct kgsl_device *device, unsigned int pwrflag)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;

	switch (pwrflag) {
	case KGSL_PWRFLAGS_POWER_OFF:
		if (pwr->power_flags & KGSL_PWRFLAGS_POWER_ON) {
			KGSL_PWR_INFO(device,
				"power off, device %d\n", device->id);
			if (internal_pwr_rail_ctl(pwr->pwr_rail, false)) {
				KGSL_DRV_ERR(device,
					"call internal_pwr_rail_ctl failed\n");
				return;
			}
			if (pwr->gpu_reg)
				regulator_disable(pwr->gpu_reg);
			pwr->power_flags &=
					~(KGSL_PWRFLAGS_POWER_ON);
			pwr->power_flags |=
					KGSL_PWRFLAGS_POWER_OFF;
		}
		return;
	case KGSL_PWRFLAGS_POWER_ON:
		if (pwr->power_flags & KGSL_PWRFLAGS_POWER_OFF) {
			KGSL_PWR_INFO(device,
				"power on, device %d\n", device->id);
			if (internal_pwr_rail_ctl(pwr->pwr_rail, true)) {
				KGSL_PWR_ERR(device,
					"call internal_pwr_rail_ctl failed\n");
				return;
			}

			if (pwr->gpu_reg)
				regulator_enable(pwr->gpu_reg);
			pwr->power_flags &=
					~(KGSL_PWRFLAGS_POWER_OFF);
			pwr->power_flags |=
					KGSL_PWRFLAGS_POWER_ON;
		}
		return;
	default:
		return;
	}
}


void kgsl_pwrctrl_irq(struct kgsl_device *device, unsigned int pwrflag)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	switch (pwrflag) {
	case KGSL_PWRFLAGS_IRQ_ON:
		if (pwr->power_flags & KGSL_PWRFLAGS_IRQ_OFF) {
			KGSL_PWR_INFO(device,
				"irq on, device %d\n", device->id);
			pwr->power_flags &=
				~(KGSL_PWRFLAGS_IRQ_OFF);
			pwr->power_flags |= KGSL_PWRFLAGS_IRQ_ON;
			enable_irq(pwr->interrupt_num);
		}
		return;
	case KGSL_PWRFLAGS_IRQ_OFF:
		if (pwr->power_flags & KGSL_PWRFLAGS_IRQ_ON) {
			KGSL_PWR_INFO(device,
				"irq off, device %d\n", device->id);
			disable_irq(pwr->interrupt_num);
			pwr->power_flags &=
				~(KGSL_PWRFLAGS_IRQ_ON);
			pwr->power_flags |= KGSL_PWRFLAGS_IRQ_OFF;
		}
		return;
	default:
		return;
	}
}

int kgsl_pwrctrl_init(struct kgsl_device *device)
{
	int i, result = 0;
	struct clk *clk;
	struct platform_device *pdev = device->pdev;
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	struct kgsl_device_platform_data *pdata_dev = pdev->dev.platform_data;
	struct kgsl_device_pwr_data *pdata_pwr = &pdata_dev->pwr_data;
	const char *clk_names[KGSL_MAX_CLKS] = {pwr->src_clk_name,
						pdata_dev->clk.name.clk,
						pdata_dev->clk.name.pclk,
						pdata_dev->imem_clk_name.clk,
						pdata_dev->imem_clk_name.pclk};

	/*acquire clocks */
	for (i = 1; i < KGSL_MAX_CLKS; i++) {
		if (clk_names[i]) {
			clk = clk_get(&pdev->dev, clk_names[i]);
			if (IS_ERR(clk))
				goto clk_err;
			pwr->grp_clks[i] = clk;
		}
	}
	/* Make sure we have a source clk for freq setting */
	clk = clk_get(&pdev->dev, clk_names[0]);
	pwr->grp_clks[0] = (IS_ERR(clk)) ? pwr->grp_clks[1] : clk;

	/* put the AXI bus into asynchronous mode with the graphics cores */
	if (pdata_pwr->set_grp_async != NULL)
		pdata_pwr->set_grp_async();

	if (pdata_pwr->num_levels > KGSL_MAX_PWRLEVELS) {
		KGSL_PWR_ERR(device, "invalid power level count: %d\n",
					 pdata_pwr->num_levels);
		result = -EINVAL;
		goto done;
	}
	pwr->num_pwrlevels = pdata_pwr->num_levels;
	pwr->active_pwrlevel = pdata_pwr->init_level;
	for (i = 0; i < pdata_pwr->num_levels; i++) {
		pwr->pwrlevels[i].gpu_freq =
		(pdata_pwr->pwrlevel[i].gpu_freq > 0) ?
		clk_round_rate(pwr->grp_clks[0],
					   pdata_pwr->pwrlevel[i].
					   gpu_freq) : 0;
		pwr->pwrlevels[i].bus_freq =
			pdata_pwr->pwrlevel[i].bus_freq;
	}
	/* Do not set_rate for targets in sync with AXI */
	if (pwr->pwrlevels[0].gpu_freq > 0)
		clk_set_rate(pwr->grp_clks[0], pwr->
				pwrlevels[pwr->num_pwrlevels - 1].gpu_freq);

	pwr->gpu_reg = regulator_get(NULL, pwr->regulator_name);
	if (IS_ERR(pwr->gpu_reg))
		pwr->gpu_reg = NULL;
	if (internal_pwr_rail_mode(device->pwrctrl.pwr_rail,
						PWR_RAIL_CTL_MANUAL)) {
		KGSL_PWR_ERR(device, "internal_pwr_rail_mode failed\n");
		result = -EINVAL;
		goto done;
	}

	pwr->power_flags = KGSL_PWRFLAGS_CLK_OFF |
			KGSL_PWRFLAGS_AXI_OFF | KGSL_PWRFLAGS_POWER_OFF |
			KGSL_PWRFLAGS_IRQ_OFF;
	pwr->nap_allowed = pdata_pwr->nap_allowed;
	pwr->interval_timeout = pdata_pwr->idle_timeout;
	pwr->ebi1_clk = clk_get(NULL, "ebi1_kgsl_clk");
	if (IS_ERR(pwr->ebi1_clk))
		pwr->ebi1_clk = NULL;
	else
		clk_set_rate(pwr->ebi1_clk,
					 pwr->pwrlevels[pwr->active_pwrlevel].
						bus_freq);
	if (pdata_dev->clk.bus_scale_table != NULL) {
		pwr->pcl =
			msm_bus_scale_register_client(pdata_dev->clk.
							bus_scale_table);
		if (!pwr->pcl) {
			KGSL_PWR_ERR(device,
					"msm_bus_scale_register_client failed: "
					"id %d table %p", device->id,
					pdata_dev->clk.bus_scale_table);
			result = -EINVAL;
			goto done;
		}
	}

	/*acquire interrupt */
	pwr->interrupt_num =
		platform_get_irq_byname(pdev, pwr->irq_name);

	if (pwr->interrupt_num <= 0) {
		KGSL_PWR_ERR(device, "platform_get_irq_byname failed: %d\n",
					 pwr->interrupt_num);
		result = -EINVAL;
		goto done;
	}
	return result;

clk_err:
	result = PTR_ERR(clk);
	KGSL_PWR_ERR(device, "clk_get(%s) failed: %d\n",
				 clk_names[i], result);

done:
	return result;
}

void kgsl_pwrctrl_close(struct kgsl_device *device)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;

	KGSL_PWR_INFO(device, "close device %d\n", device->id);

	if (pwr->interrupt_num > 0) {
		if (pwr->have_irq) {
			free_irq(pwr->interrupt_num, NULL);
			pwr->have_irq = 0;
		}
		pwr->interrupt_num = 0;
	}

	clk_put(pwr->ebi1_clk);

	if (pwr->pcl)
		msm_bus_scale_unregister_client(pwr->pcl);

	pwr->pcl = 0;

	if (pwr->gpu_reg) {
		regulator_put(pwr->gpu_reg);
		pwr->gpu_reg = NULL;
	}

	if (pwr->grp_pclk) {
		clk_put(pwr->grp_pclk);
		pwr->grp_pclk = NULL;
	}

	if (pwr->grp_clk) {
		clk_put(pwr->grp_clk);
		pwr->grp_clk = NULL;
	}

	if (pwr->imem_clk != NULL) {
		clk_put(pwr->imem_clk);
		pwr->imem_clk = NULL;
	}

	pwr->grp_src_clk = NULL;
	pwr->power_flags = 0;
}

void kgsl_idle_check(struct work_struct *work)
{
	struct kgsl_device *device = container_of(work, struct kgsl_device,
							idle_check_ws);

	mutex_lock(&device->mutex);
	if (device->state & KGSL_STATE_HUNG) {
		device->requested_state = KGSL_STATE_NONE;
		goto done;
	}
	if (device->state & (KGSL_STATE_ACTIVE | KGSL_STATE_NAP)) {
		if (kgsl_pwrctrl_sleep(device) != 0)
			mod_timer(&device->idle_timer,
					jiffies +
					device->pwrctrl.interval_timeout);
	}
done:
	mutex_unlock(&device->mutex);
}

void kgsl_timer(unsigned long data)
{
	struct kgsl_device *device = (struct kgsl_device *) data;

	KGSL_PWR_INFO(device, "idle timer expired device %d\n", device->id);
	if (device->requested_state != KGSL_STATE_SUSPEND) {
		device->requested_state = KGSL_STATE_SLEEP;
		/* Have work run in a non-interrupt context. */
		queue_work(device->work_queue, &device->idle_check_ws);
	}
}

void kgsl_pre_hwaccess(struct kgsl_device *device)
{
	if (device->state & (KGSL_STATE_SLEEP | KGSL_STATE_NAP))
		kgsl_pwrctrl_wake(device);
}

void kgsl_check_suspended(struct kgsl_device *device)
{
	if (device->requested_state == KGSL_STATE_SUSPEND ||
				device->state == KGSL_STATE_SUSPEND) {
		mutex_unlock(&device->mutex);
		wait_for_completion(&device->hwaccess_gate);
		mutex_lock(&device->mutex);
	}
 }


/******************************************************************/
/* Caller must hold the device mutex. */
int kgsl_pwrctrl_sleep(struct kgsl_device *device)
{
	KGSL_PWR_INFO(device, "sleep device %d\n", device->id);

	/* Work through the legal state transitions */
	if (device->requested_state == KGSL_STATE_NAP) {
		if (device->ftbl.device_isidle(device))
			goto nap;
	} else if (device->requested_state == KGSL_STATE_SLEEP) {
		if (device->state == KGSL_STATE_NAP ||
			device->ftbl.device_isidle(device))
			goto sleep;
	}

	device->requested_state = KGSL_STATE_NONE;
	return -EBUSY;

sleep:
	kgsl_pwrctrl_irq(device, KGSL_PWRFLAGS_IRQ_OFF);
	kgsl_pwrctrl_axi(device, KGSL_PWRFLAGS_AXI_OFF);
	goto clk_off;

nap:
	kgsl_pwrctrl_irq(device, KGSL_PWRFLAGS_IRQ_OFF);
clk_off:
	kgsl_pwrctrl_clk(device, KGSL_PWRFLAGS_CLK_OFF);

	device->state = device->requested_state;
	device->requested_state = KGSL_STATE_NONE;
	KGSL_PWR_WARN(device, "state -> NAP/SLEEP(%d), device %d\n",
				  device->state, device->id);

	return 0;
}


/******************************************************************/
/* Caller must hold the device mutex. */
void kgsl_pwrctrl_wake(struct kgsl_device *device)
{
	if (device->state == KGSL_STATE_SUSPEND)
		return;

	/* Turn on the core clocks */
	kgsl_pwrctrl_clk(device, KGSL_PWRFLAGS_CLK_ON);
	if (device->state != KGSL_STATE_NAP) {
		kgsl_pwrctrl_axi(device, KGSL_PWRFLAGS_AXI_ON);
	}
	/* Enable state before turning on irq */
	device->state = KGSL_STATE_ACTIVE;
	kgsl_pwrctrl_irq(device, KGSL_PWRFLAGS_IRQ_ON);

	/* Re-enable HW access */
	device->state = KGSL_STATE_ACTIVE;
	KGSL_PWR_WARN(device, "state -> ACTIVE, device %d\n", device->id);
	mod_timer(&device->idle_timer,
				jiffies + device->pwrctrl.interval_timeout);

	wake_lock(&device->idle_wakelock);
	KGSL_PWR_INFO(device, "wake return for device %d\n", device->id);
}


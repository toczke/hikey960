/*
 * venc_regulator.c - Kirin 960 Hardware Video Encoder (VENC) Power & Clock Manager
 * Modern Linux 7.x replacement for legacy venc_regulator.S
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/mutex.h>

#include "hi_type.h"
#include "hi_drv_venc.h"
#include "hi_drv_mem.h"
#include "drv_venc_efl.h"
#include "venc_regulator.h"

#define VENC_CLOCK_NAME      "clk_venc"
#define VENC_REGULATOR_NAME  "ldo_venc"

/* Globals referenced by core assembly */
HI_U32 g_voltHold = 0;
void *g_hisi_mmu_domain = NULL;

static struct device    *g_pVencDev = NULL;
static struct regulator *g_pVencRegulator = NULL;
static struct clk       *g_PvencClk = NULL;
static HI_U32            g_vencClkRate_l = 200000000;  /* 200 MHz */
static HI_U32            g_vencClkRate_h = 480000000;  /* 480 MHz */
static bool              g_vencPowerOn = false;
static DEFINE_MUTEX(g_venc_reg_mutex);

HI_S32 Venc_Regulator_Init(struct platform_device *pdev)
{
	struct device *dev;
	struct device_node *np;
	struct resource res;
	VeduEfl_DTS_CONFIG_S info;
	u32 irq_vedu = 0, irq_mmu = 0;
	HI_S32 ret;

	if (!pdev || !pdev->dev.of_node) {
		pr_err("[VENC] Venc_Regulator_Init: invalid device\n");
		return HI_FAILURE;
	}

	dev = &pdev->dev;
	g_pVencDev = dev;
	np = dev->of_node;

	/* Get VENC regulator */
	g_pVencRegulator = devm_regulator_get(dev, VENC_REGULATOR_NAME);
	if (IS_ERR(g_pVencRegulator)) {
		pr_warn("[VENC] devm_regulator_get(%s) returned %ld, proceeding\n",
			VENC_REGULATOR_NAME, PTR_ERR(g_pVencRegulator));
		g_pVencRegulator = NULL;
	}

	/* Get VENC clock */
	g_PvencClk = devm_clk_get(dev, VENC_CLOCK_NAME);
	if (IS_ERR(g_PvencClk)) {
		pr_err("[VENC] devm_clk_get(%s) failed: %ld\n",
			VENC_CLOCK_NAME, PTR_ERR(g_PvencClk));
		return HI_FAILURE;
	}

	/* Read clock rates from device tree */
	of_property_read_u32_index(np, "enc_clk_rate", 0, &g_vencClkRate_l);
	of_property_read_u32_index(np, "enc_clk_rate", 1, &g_vencClkRate_h);

	/* Parse IRQs */
	irq_vedu = irq_of_parse_and_map(np, 0);
	irq_mmu  = irq_of_parse_and_map(np, 1);
	if (!irq_mmu)
		irq_mmu = irq_vedu + 1;

	/* Parse register resource */
	memset(&res, 0, sizeof(res));
	if (of_address_to_resource(np, 0, &res) != 0) {
		res.start = 0xe8900000;
		res.end = 0xe891ffff;
	}

	/* Populate hardware DTS configuration for drv_venc_efl */
	memset(&info, 0, sizeof(info));
	info.IsFPGA = 0;
	info.VeduIrqNum = irq_vedu;
	info.MmuIrqNum = irq_mmu;
	info.VedusecIrqNum = irq_vedu;
	info.MmusecIrqNum = irq_mmu;
	info.VencRegBaseAddr = (HI_U32)res.start;
	info.VencRegRange = (HI_U32)resource_size(&res);
	info.normalRate = g_vencClkRate_l ? g_vencClkRate_l : 200000000;
	info.highRate = g_vencClkRate_h ? g_vencClkRate_h : 480000000;
	/* SmmuPageBaseAddr must be non-zero to pass VENC_SetDtsConfig sanity check */
	info.SmmuPageBaseAddr = (HI_U64)res.start ? (HI_U64)res.start : 0x1000;

	ret = VENC_SetDtsConfig(&info);
	if (ret != HI_SUCCESS) {
		pr_err("[VENC] VENC_SetDtsConfig failed: %d\n", ret);
		return HI_FAILURE;
	}

	pr_info("[VENC] Regulator & Clock initialized (vedu_irq=%u, mmu_irq=%u, reg=0x%x..0x%x)\n",
		irq_vedu, irq_mmu, (u32)res.start, (u32)res.end);

	return HI_SUCCESS;
}
EXPORT_SYMBOL(Venc_Regulator_Init);

HI_VOID Venc_Regulator_Deinit(struct platform_device *pdev)
{
	Venc_Regulator_Disable(HI_TRUE);
	g_PvencClk = NULL;
	g_pVencRegulator = NULL;
	g_pVencDev = NULL;
}
EXPORT_SYMBOL(Venc_Regulator_Deinit);

HI_S32 Venc_Regulator_Enable(HI_VOID)
{
	int ret;

	mutex_lock(&g_venc_reg_mutex);
	if (!g_vencPowerOn) {
		if (g_pVencRegulator) {
			ret = regulator_enable(g_pVencRegulator);
			if (ret)
				pr_warn("[VENC] regulator_enable failed: %d\n", ret);
		}

		if (g_PvencClk) {
			ret = clk_prepare_enable(g_PvencClk);
			if (ret)
				pr_warn("[VENC] clk_prepare_enable failed: %d\n", ret);
			if (g_vencClkRate_h)
				clk_set_rate(g_PvencClk, g_vencClkRate_h);
		}
		g_vencPowerOn = true;
		pr_info("[VENC] Hardware power and clock enabled (rate=%u Hz)\n", g_vencClkRate_h);
	}
	mutex_unlock(&g_venc_reg_mutex);
	return HI_SUCCESS;
}
EXPORT_SYMBOL(Venc_Regulator_Enable);

HI_S32 Venc_Regulator_Disable(HI_BOOL disVolthold)
{
	mutex_lock(&g_venc_reg_mutex);
	if (g_vencPowerOn) {
		if (g_PvencClk)
			clk_disable_unprepare(g_PvencClk);

		if (g_pVencRegulator)
			regulator_disable(g_pVencRegulator);

		g_vencPowerOn = false;
		pr_info("[VENC] Hardware power and clock disabled\n");
	}
	mutex_unlock(&g_venc_reg_mutex);
	return HI_SUCCESS;
}
EXPORT_SYMBOL(Venc_Regulator_Disable);

HI_VOID Venc_SetRate(HI_BOOL isHighRate)
{
	HI_U32 rate = isHighRate ? g_vencClkRate_h : g_vencClkRate_l;

	if (g_PvencClk && rate > 0)
		clk_set_rate(g_PvencClk, rate);
}
EXPORT_SYMBOL(Venc_SetRate);

/*
 *  linux/drivers/mmc/host/sdhci.c - Secure Digital Host Controller Interface driver
 *
 *  Copyright (C) 2005-2008 Pierre Ossman, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * Thanks to the following companies for their support:
 *
 *     - JMicron (hardware and technical support)
 */

#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/swiotlb.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>

#include <linux/leds.h>

#include <linux/mmc/mmc.h>
#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/slot-gpio.h>

#include "sdhci.h"

#define DRIVER_NAME "sdhci"

#define DBG(f, x...) \
        pr_debug("%s: " DRIVER_NAME ": " f, mmc_hostname(host->mmc), ## x)

#define SDHCI_DUMP(f, x...) \
        pr_err("%s: " DRIVER_NAME ": " f, mmc_hostname(host->mmc), ## x)

#define MAX_TUNING_LOOP 40

static unsigned int debug_quirks = 0;
static unsigned int debug_quirks2;

static void sdhci_finish_data(struct sdhci_host *);

void sdhci_dumpregs(struct sdhci_host *host)
{
	SDHCI_DUMP("============ SDHCI REGISTER DUMP ===========\n");

	SDHCI_DUMP("Sys addr:  0x%08x | Version:  0x%08x\n",
		   sdhci_readl(host, SDHCI_DMA_ADDRESS),
		   sdhci_readw(host, SDHCI_HOST_VERSION));
	SDHCI_DUMP("Blk size:  0x%08x | Blk cnt:  0x%08x\n",
		   sdhci_readw(host, SDHCI_BLOCK_SIZE),
		   sdhci_readw(host, SDHCI_BLOCK_COUNT));
	SDHCI_DUMP("Argument:  0x%08x | Trn mode: 0x%08x\n",
		   sdhci_readl(host, SDHCI_ARGUMENT),
		   sdhci_readw(host, SDHCI_TRANSFER_MODE));
	SDHCI_DUMP("Present:   0x%08x | Host ctl: 0x%08x\n",
		   sdhci_readl(host, SDHCI_PRESENT_STATE),
		   sdhci_readb(host, SDHCI_HOST_CONTROL));
	SDHCI_DUMP("Power:     0x%08x | Blk gap:  0x%08x\n",
		   sdhci_readb(host, SDHCI_POWER_CONTROL),
		   sdhci_readb(host, SDHCI_BLOCK_GAP_CONTROL));
	SDHCI_DUMP("Wake-up:   0x%08x | Clock:    0x%08x\n",
		   sdhci_readb(host, SDHCI_WAKE_UP_CONTROL),
		   sdhci_readw(host, SDHCI_CLOCK_CONTROL));
	SDHCI_DUMP("Timeout:   0x%08x | Int stat: 0x%08x\n",
		   sdhci_readb(host, SDHCI_TIMEOUT_CONTROL),
		   sdhci_readl(host, SDHCI_INT_STATUS));
	SDHCI_DUMP("Int enab:  0x%08x | Sig enab: 0x%08x\n",
		   sdhci_readl(host, SDHCI_INT_ENABLE),
		   sdhci_readl(host, SDHCI_SIGNAL_ENABLE));
	SDHCI_DUMP("AC12 err:  0x%08x | Slot int: 0x%08x\n",
		   sdhci_readw(host, SDHCI_ACMD12_ERR),
		   sdhci_readw(host, SDHCI_SLOT_INT_STATUS));
	SDHCI_DUMP("Caps:      0x%08x | Caps_1:   0x%08x\n",
		   sdhci_readl(host, SDHCI_CAPABILITIES),
		   sdhci_readl(host, SDHCI_CAPABILITIES_1));
	SDHCI_DUMP("Cmd:       0x%08x | Max curr: 0x%08x\n",
		   sdhci_readw(host, SDHCI_COMMAND),
		   sdhci_readl(host, SDHCI_MAX_CURRENT));
	SDHCI_DUMP("Resp[0]:   0x%08x | Resp[1]:  0x%08x\n",
		   sdhci_readl(host, SDHCI_RESPONSE),
		   sdhci_readl(host, SDHCI_RESPONSE + 4));
	SDHCI_DUMP("Resp[2]:   0x%08x | Resp[3]:  0x%08x\n",
		   sdhci_readl(host, SDHCI_RESPONSE + 8),
		   sdhci_readl(host, SDHCI_RESPONSE + 12));
	SDHCI_DUMP("Host ctl2: 0x%08x\n",
		   sdhci_readw(host, SDHCI_HOST_CONTROL2));

	if (host->flags & SDHCI_USE_ADMA) {
		if (host->flags & SDHCI_USE_64_BIT_DMA) {
			SDHCI_DUMP("ADMA Err:  0x%08x | ADMA Ptr: 0x%08x%08x\n",
				   sdhci_readl(host, SDHCI_ADMA_ERROR),
				   sdhci_readl(host, SDHCI_ADMA_ADDRESS_HI),
				   sdhci_readl(host, SDHCI_ADMA_ADDRESS));
		} else {
			SDHCI_DUMP("ADMA Err:  0x%08x | ADMA Ptr: 0x%08x\n",
				   sdhci_readl(host, SDHCI_ADMA_ERROR),
				   sdhci_readl(host, SDHCI_ADMA_ADDRESS));
		}
	}

	SDHCI_DUMP("============================================\n");
}
EXPORT_SYMBOL_GPL(sdhci_dumpregs);

static u16 sdhci_get_preset_value(struct sdhci_host *host)
{
        u16 preset = 0;

        switch (host->timing) {
        case MMC_TIMING_UHS_SDR12:
                preset = sdhci_readw(host, SDHCI_PRESET_FOR_SDR12);
                break;
        case MMC_TIMING_UHS_SDR25:
                preset = sdhci_readw(host, SDHCI_PRESET_FOR_SDR25);
                break;
        case MMC_TIMING_UHS_SDR50:
                preset = sdhci_readw(host, SDHCI_PRESET_FOR_SDR50);
                break;
        case MMC_TIMING_UHS_SDR104:
        case MMC_TIMING_MMC_HS200:
                preset = sdhci_readw(host, SDHCI_PRESET_FOR_SDR104);
                break;
        case MMC_TIMING_UHS_DDR50:
        case MMC_TIMING_MMC_DDR52:
                preset = sdhci_readw(host, SDHCI_PRESET_FOR_DDR50);
                break;
        case MMC_TIMING_MMC_HS400:
                preset = sdhci_readw(host, SDHCI_PRESET_FOR_HS400);
                break;
        default:
                pr_warn("%s: Invalid UHS-I mode selected\n",
                        mmc_hostname(host->mmc));
                preset = sdhci_readw(host, SDHCI_PRESET_FOR_SDR12);
                break;
        }
        return preset;
}

void sdhci_set_bus_width(struct sdhci_host *host, int width)
{
        u8 ctrl;

        ctrl = sdhci_readb(host, SDHCI_HOST_CONTROL);
        if (width == MMC_BUS_WIDTH_8) {
                ctrl &= ~SDHCI_CTRL_4BITBUS;
                ctrl |= SDHCI_CTRL_8BITBUS;
        } else {
                if (host->mmc->caps & MMC_CAP_8_BIT_DATA)
                        ctrl &= ~SDHCI_CTRL_8BITBUS;
                if (width == MMC_BUS_WIDTH_4)
                        ctrl |= SDHCI_CTRL_4BITBUS;
                else
                        ctrl &= ~SDHCI_CTRL_4BITBUS;
        }
        sdhci_writeb(host, ctrl, SDHCI_HOST_CONTROL);
}
EXPORT_SYMBOL_GPL(sdhci_set_bus_width);

void sdhci_set_uhs_signaling(struct sdhci_host *host, unsigned timing)
{
        u16 ctrl_2;

        ctrl_2 = sdhci_readw(host, SDHCI_HOST_CONTROL2);
        /* Select Bus Speed Mode for host */
        ctrl_2 &= ~SDHCI_CTRL_UHS_MASK;
        if ((timing == MMC_TIMING_MMC_HS200) ||
            (timing == MMC_TIMING_UHS_SDR104))
                ctrl_2 |= SDHCI_CTRL_UHS_SDR104;
        else if (timing == MMC_TIMING_UHS_SDR12)
                ctrl_2 |= SDHCI_CTRL_UHS_SDR12;
        else if (timing == MMC_TIMING_UHS_SDR25)
                ctrl_2 |= SDHCI_CTRL_UHS_SDR25;
        else if (timing == MMC_TIMING_UHS_SDR50)
                ctrl_2 |= SDHCI_CTRL_UHS_SDR50;
        else if ((timing == MMC_TIMING_UHS_DDR50) ||
                 (timing == MMC_TIMING_MMC_DDR52))
                ctrl_2 |= SDHCI_CTRL_UHS_DDR50;
        else if (timing == MMC_TIMING_MMC_HS400)
                ctrl_2 |= SDHCI_CTRL_HS400; /* Non-standard */
        sdhci_writew(host, ctrl_2, SDHCI_HOST_CONTROL2);
}
EXPORT_SYMBOL_GPL(sdhci_set_uhs_signaling);

static void sdhci_runtime_pm_bus_off(struct sdhci_host *host)
{
        if (!host->bus_on)
                return;
        host->bus_on = false;
        pm_runtime_put_noidle(host->mmc->parent);
}

void sdhci_reset(struct sdhci_host *host, u8 mask)
{
        ktime_t timeout;

        sdhci_writeb(host, mask, SDHCI_SOFTWARE_RESET);

        if (mask & SDHCI_RESET_ALL) {
                host->clock = 0;
                /* Reset-all turns off SD Bus Power */
                if (host->quirks2 & SDHCI_QUIRK2_CARD_ON_NEEDS_BUS_ON)
                        sdhci_runtime_pm_bus_off(host);
        }

        /* Wait max 100 ms */
        timeout = ktime_add_ms(ktime_get(), 100);

        /* hw clears the bit when it's done */
        while (1) {
                bool timedout = ktime_after(ktime_get(), timeout);

                if (!(sdhci_readb(host, SDHCI_SOFTWARE_RESET) & mask))
                        break;
                if (timedout) {
                        pr_err("%s: Reset 0x%x never completed.\n",
                                mmc_hostname(host->mmc), (int)mask);
                        sdhci_dumpregs(host);
                        return;
                }
                udelay(10);
        }
}
EXPORT_SYMBOL_GPL(sdhci_reset);

u16 sdhci_calc_clk(struct sdhci_host *host, unsigned int clock,
		   unsigned int *actual_clock)
{
	int div = 0; /* Initialized for compiler warning */
	int real_div = div, clk_mul = 1;
	u16 clk = 0;
	bool switch_base_clk = false;

	if (host->version >= SDHCI_SPEC_300) {
		if (host->preset_enabled) {
			u16 pre_val;

			clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
			pre_val = sdhci_get_preset_value(host);
			div = (pre_val & SDHCI_PRESET_SDCLK_FREQ_MASK)
				>> SDHCI_PRESET_SDCLK_FREQ_SHIFT;
			if (host->clk_mul &&
				(pre_val & SDHCI_PRESET_CLKGEN_SEL_MASK)) {
				clk = SDHCI_PROG_CLOCK_MODE;
				real_div = div + 1;
				clk_mul = host->clk_mul;
			} else {
				real_div = max_t(int, 1, div << 1);
			}
			goto clock_set;
		}

		/*
		 * Check if the Host Controller supports Programmable Clock
		 * Mode.
		 */
		if (host->clk_mul) {
			for (div = 1; div <= 1024; div++) {
				if ((host->max_clk * host->clk_mul / div)
					<= clock)
					break;
			}
			if ((host->max_clk * host->clk_mul / div) <= clock) {
				/*
				 * Set Programmable Clock Mode in the Clock
				 * Control register.
				 */
				clk = SDHCI_PROG_CLOCK_MODE;
				real_div = div;
				clk_mul = host->clk_mul;
				div--;
			} else {
				/*
				 * Divisor can be too small to reach clock
				 * speed requirement. Then use the base clock.
				 */
				switch_base_clk = true;
			}
		}

		if (!host->clk_mul || switch_base_clk) {
			/* Version 3.00 divisors must be a multiple of 2. */
			if (host->max_clk <= clock)
				div = 1;
			else {
				for (div = 2; div < SDHCI_MAX_DIV_SPEC_300;
				     div += 2) {
					if ((host->max_clk / div) <= clock)
						break;
				}
			}
			real_div = div;
			div >>= 1;
			if ((host->quirks2 & SDHCI_QUIRK2_CLOCK_DIV_ZERO_BROKEN)
				&& !div && host->max_clk <= 25000000)
				div = 1;
		}
	} else {
		/* Version 2.00 divisors must be a power of 2. */
		for (div = 1; div < SDHCI_MAX_DIV_SPEC_200; div *= 2) {
			if ((host->max_clk / div) <= clock)
				break;
		}
		real_div = div;
		div >>= 1;
	}

clock_set:
	if (real_div)
		*actual_clock = (host->max_clk * clk_mul) / real_div;
	clk |= (div & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT;
	clk |= ((div & SDHCI_DIV_HI_MASK) >> SDHCI_DIV_MASK_LEN)
		<< SDHCI_DIVIDER_HI_SHIFT;

	return clk;
}
EXPORT_SYMBOL_GPL(sdhci_calc_clk);

static const struct mmc_host_ops sdhci_ops = {
 /*       .request        = sdhci_request,
        .post_req       = sdhci_post_req,
        .pre_req        = sdhci_pre_req,
        .set_ios        = sdhci_set_ios,
        .get_cd         = sdhci_get_cd,
        .get_ro         = sdhci_get_ro,
        .hw_reset       = sdhci_hw_reset,
        .enable_sdio_irq = sdhci_enable_sdio_irq,
        .start_signal_voltage_switch    = sdhci_start_signal_voltage_switch,
        .prepare_hs400_tuning           = sdhci_prepare_hs400_tuning,
        .execute_tuning                 = sdhci_execute_tuning,
        .card_event                     = sdhci_card_event,
        .card_busy      = sdhci_card_busy,
*/
};




void sdhci_enable_clk(struct sdhci_host *host, u16 clk)
{
        ktime_t timeout;

        clk |= SDHCI_CLOCK_INT_EN;
        sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

        /* Wait max 20 ms */
        timeout = ktime_add_ms(ktime_get(), 20);
        while (1) {
                bool timedout = ktime_after(ktime_get(), timeout);

                clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
                if (clk & SDHCI_CLOCK_INT_STABLE)
                        break;
                if (timedout) {
                        pr_err("%s: Internal clock never stabilised.\n",
                               mmc_hostname(host->mmc));
                        sdhci_dumpregs(host);
                        return;
                }
                udelay(10);
        }

        clk |= SDHCI_CLOCK_CARD_EN;
        sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
}
EXPORT_SYMBOL_GPL(sdhci_enable_clk);

void sdhci_set_clock(struct sdhci_host *host, unsigned int clock)
{
        u16 clk;

        host->mmc->actual_clock = 0;

        sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);

        if (clock == 0)
                return;

        clk = sdhci_calc_clk(host, clock, &host->mmc->actual_clock);
        sdhci_enable_clk(host, clk);
}
EXPORT_SYMBOL_GPL(sdhci_set_clock);

struct sdhci_host *sdhci_alloc_host(struct device *dev,
        size_t priv_size)
{
        struct mmc_host *mmc;
        struct sdhci_host *host;

        WARN_ON(dev == NULL);

 //       mmc = mmc_alloc_host(sizeof(struct sdhci_host) + priv_size, dev);
        if (!mmc)
                return ERR_PTR(-ENOMEM);

        host = mmc_priv(mmc);
        host->mmc = mmc;
        host->mmc_host_ops = sdhci_ops;
        mmc->ops = &host->mmc_host_ops;

        host->flags = SDHCI_SIGNALING_330;

        host->cqe_ier     = SDHCI_CQE_INT_MASK;
        host->cqe_err_ier = SDHCI_CQE_INT_ERR_MASK;

        host->tuning_delay = -1;

        host->sdma_boundary = SDHCI_DEFAULT_BOUNDARY_ARG;

        return host;
}

EXPORT_SYMBOL_GPL(sdhci_alloc_host);

/*
 *  linux/drivers/mmc/core/sd.c
 *
 *  Copyright (C) 2003-2004 Russell King, All Rights Reserved.
 *  SD support Copyright (C) 2004 Ian Molton, All Rights Reserved.
 *  Copyright (C) 2005-2007 Pierre Ossman, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/err.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/pm_runtime.h>

#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>

#include "core.h"
#include "card.h"
#include "host.h"
#include "bus.h"
#include "mmc_ops.h"
#include "sd.h"
#include "sd_ops.h"


/* Get host's max current setting at its current voltage */
static u32 sd_get_host_max_current(struct mmc_host *host)
{
        u32 voltage, max_current;

        voltage = 1 << host->ios.vdd;
        switch (voltage) {
        case MMC_VDD_165_195:
                max_current = host->max_current_180;
                break;
        case MMC_VDD_29_30:
        case MMC_VDD_30_31:
                max_current = host->max_current_300;
                break;
        case MMC_VDD_32_33:
        case MMC_VDD_33_34:
                max_current = host->max_current_330;
                break;
        default:
                max_current = 0;
        }

        return max_current;
}

/*
 * Fetch CID from card.
 */
int mmc_sd_get_cid(struct mmc_host *host, u32 ocr, u32 *cid, u32 *rocr)
{
        int err;
        u32 max_current;
        int retries = 10;
        u32 pocr = ocr;

try_again:
        if (!retries) {
                ocr &= ~SD_OCR_S18R;
                pr_warn("%s: Skipping voltage switch\n", mmc_hostname(host));
        }

        /*
         * Since we're changing the OCR value, we seem to
         * need to tell some cards to go back to the idle
         * state.  We wait 1ms to give cards time to
         * respond.
         */
        mmc_go_idle(host);

        /*
         * If SD_SEND_IF_COND indicates an SD 2.0
         * compliant card and we should set bit 30
         * of the ocr to indicate that we can handle
         * block-addressed SDHC cards.
         */
        err = mmc_send_if_cond(host, ocr);
        if (!err)
                ocr |= SD_OCR_CCS;
        /*
         * If the host supports one of UHS-I modes, request the card
         * to switch to 1.8V signaling level. If the card has failed
         * repeatedly to switch however, skip this.
         */
        if (retries && mmc_host_uhs(host))
                ocr |= SD_OCR_S18R;

        /*
         * If the host can supply more than 150mA at current voltage,
         * XPC should be set to 1.
         */
        max_current = sd_get_host_max_current(host);
        if (max_current > 150)
                ocr |= SD_OCR_XPC;

        err = mmc_send_app_op_cond(host, ocr, rocr);
        if (err)
                return err;

        /*
         * In case CCS and S18A in the response is set, start Signal Voltage
         * Switch procedure. SPI mode doesn't support CMD11.
         */
        if (!mmc_host_is_spi(host) && rocr &&
           ((*rocr & 0x41000000) == 0x41000000)) {
                err = mmc_set_uhs_voltage(host, pocr);
                if (err == -EAGAIN) {
                        retries--;
                        goto try_again;
                } else if (err) {
                        retries = 0;
                        goto try_again;
                }
        }
        err = mmc_send_cid(host, cid);
        return err;
}











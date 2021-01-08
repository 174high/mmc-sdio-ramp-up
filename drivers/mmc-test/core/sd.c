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

static const unsigned int tran_exp[] = {
        10000,          100000,         1000000,        10000000,
        0,              0,              0,              0
};

static const unsigned char tran_mant[] = {
        0,      10,     12,     13,     15,     20,     25,     30,
        35,     40,     45,     50,     55,     60,     70,     80,
};
        
static const unsigned int taac_exp[] = {
        1,      10,     100,    1000,   10000,  100000, 1000000, 10000000,
};

static const unsigned int taac_mant[] = {
        0,      10,     12,     13,     15,     20,     25,     30,
        35,     40,     45,     50,     55,     60,     70,     80,
};
                
static const unsigned int sd_au_size[] = {
        0,              SZ_16K / 512,           SZ_32K / 512,   SZ_64K / 512,
        SZ_128K / 512,  SZ_256K / 512,          SZ_512K / 512,  SZ_1M / 512,
        SZ_2M / 512,    SZ_4M / 512,            SZ_8M / 512,    (SZ_8M + SZ_4M) / 512,
        SZ_16M / 512,   (SZ_16M + SZ_8M) / 512, SZ_32M / 512,   SZ_64M / 512,
};

#define UNSTUFF_BITS(resp,start,size)                                   \
        ({                                                              \
                const int __size = size;                                \
                const u32 __mask = (__size < 32 ? 1 << __size : 0) - 1; \
                const int __off = 3 - ((start) / 32);                   \
                const int __shft = (start) & 31;                        \
                u32 __res;                                              \
                                                                        \
                __res = resp[__off] >> __shft;                          \
                if (__size + __shft > 32)                               \
                        __res |= resp[__off-1] << ((32 - __shft) % 32); \
                __res & __mask;                                         \
        })

/*
 * Given the decoded CSD structure, decode the raw CID to our CID structure.
 */
void mmc_decode_cid(struct mmc_card *card)
{
        u32 *resp = card->raw_cid;

        /*
         * SD doesn't currently have a version field so we will
         * have to assume we can parse this.
         */
        card->cid.manfid                = UNSTUFF_BITS(resp, 120, 8);
        card->cid.oemid                 = UNSTUFF_BITS(resp, 104, 16);
        card->cid.prod_name[0]          = UNSTUFF_BITS(resp, 96, 8);
        card->cid.prod_name[1]          = UNSTUFF_BITS(resp, 88, 8);
        card->cid.prod_name[2]          = UNSTUFF_BITS(resp, 80, 8);
        card->cid.prod_name[3]          = UNSTUFF_BITS(resp, 72, 8);
        card->cid.prod_name[4]          = UNSTUFF_BITS(resp, 64, 8);
        card->cid.hwrev                 = UNSTUFF_BITS(resp, 60, 4);
        card->cid.fwrev                 = UNSTUFF_BITS(resp, 56, 4);
        card->cid.serial                = UNSTUFF_BITS(resp, 24, 32);
        card->cid.year                  = UNSTUFF_BITS(resp, 12, 8);
        card->cid.month                 = UNSTUFF_BITS(resp, 8, 4);

        card->cid.year += 2000; /* SD cards year offset */
}


MMC_DEV_ATTR(cid, "%08x%08x%08x%08x\n", card->raw_cid[0], card->raw_cid[1],
        card->raw_cid[2], card->raw_cid[3]);
MMC_DEV_ATTR(csd, "%08x%08x%08x%08x\n", card->raw_csd[0], card->raw_csd[1],
        card->raw_csd[2], card->raw_csd[3]);
MMC_DEV_ATTR(scr, "%08x%08x\n", card->raw_scr[0], card->raw_scr[1]);
MMC_DEV_ATTR(ssr,
        "%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x\n",
                card->raw_ssr[0], card->raw_ssr[1], card->raw_ssr[2],
                card->raw_ssr[3], card->raw_ssr[4], card->raw_ssr[5],
                card->raw_ssr[6], card->raw_ssr[7], card->raw_ssr[8],
                card->raw_ssr[9], card->raw_ssr[10], card->raw_ssr[11],
                card->raw_ssr[12], card->raw_ssr[13], card->raw_ssr[14],
                card->raw_ssr[15]);
MMC_DEV_ATTR(date, "%02d/%04d\n", card->cid.month, card->cid.year);
MMC_DEV_ATTR(erase_size, "%u\n", card->erase_size << 9);
MMC_DEV_ATTR(preferred_erase_size, "%u\n", card->pref_erase << 9);
MMC_DEV_ATTR(fwrev, "0x%x\n", card->cid.fwrev);
MMC_DEV_ATTR(hwrev, "0x%x\n", card->cid.hwrev);
MMC_DEV_ATTR(manfid, "0x%06x\n", card->cid.manfid);
MMC_DEV_ATTR(name, "%s\n", card->cid.prod_name);
MMC_DEV_ATTR(oemid, "0x%04x\n", card->cid.oemid);
MMC_DEV_ATTR(serial, "0x%08x\n", card->cid.serial);
MMC_DEV_ATTR(ocr, "0x%08x\n", card->ocr);
MMC_DEV_ATTR(rca, "0x%04x\n", card->rca);

static ssize_t mmc_dsr_show(struct device *dev,
                           struct device_attribute *attr,
                           char *buf)
{
       struct mmc_card *card = mmc_dev_to_card(dev);
       struct mmc_host *host = card->host;

       if (card->csd.dsr_imp && host->dsr_req)
               return sprintf(buf, "0x%x\n", host->dsr);
       else
               /* return default DSR value */
               return sprintf(buf, "0x%x\n", 0x404);
}


static DEVICE_ATTR(dsr, S_IRUGO, mmc_dsr_show, NULL);

static struct attribute *sd_std_attrs[] = {
        &dev_attr_cid.attr,
        &dev_attr_csd.attr,
        &dev_attr_scr.attr,
        &dev_attr_ssr.attr,
        &dev_attr_date.attr,
        &dev_attr_erase_size.attr,
        &dev_attr_preferred_erase_size.attr,
        &dev_attr_fwrev.attr,
        &dev_attr_hwrev.attr,
        &dev_attr_manfid.attr,
        &dev_attr_name.attr,
        &dev_attr_oemid.attr,
        &dev_attr_serial.attr,
        &dev_attr_ocr.attr,
        &dev_attr_rca.attr,
        &dev_attr_dsr.attr,
        NULL,
};
ATTRIBUTE_GROUPS(sd_std);

struct device_type sd_type = {
        .groups = sd_std_groups,
};

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
 * Fetches and decodes switch information
 */
static int mmc_read_switch(struct mmc_card *card)
{
        int err;
        u8 *status;

        if (card->scr.sda_vsn < SCR_SPEC_VER_1)
                return 0;

        if (!(card->csd.cmdclass & CCC_SWITCH)) {
                pr_warn("%s: card lacks mandatory switch function, performance might suffer\n",
                        mmc_hostname(card->host));
                return 0;
        }

        status = kmalloc(64, GFP_KERNEL);
        if (!status)
                return -ENOMEM;

        /*
         * Find out the card's support bits with a mode 0 operation.
         * The argument does not matter, as the support bits do not
         * change with the arguments.
         */
        err = mmc_sd_switch(card, 0, 0, 0, status);
        if (err) {
                /*
                 * If the host or the card can't do the switch,
                 * fail more gracefully.
                 */
                if (err != -EINVAL && err != -ENOSYS && err != -EFAULT)
                        goto out;

                pr_warn("%s: problem reading Bus Speed modes\n",
                        mmc_hostname(card->host));
                err = 0;

                goto out;
        }

        if (status[13] & SD_MODE_HIGH_SPEED)
                card->sw_caps.hs_max_dtr = HIGH_SPEED_MAX_DTR;

        if (card->scr.sda_spec3) {
                card->sw_caps.sd3_bus_mode = status[13];
                /* Driver Strengths supported by the card */
                card->sw_caps.sd3_drv_type = status[9];
	        card->sw_caps.sd3_curr_limit = status[7] | status[6] << 8;
        }

out:
        kfree(status);

        return err;
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


int mmc_sd_get_csd(struct mmc_host *host, struct mmc_card *card)
{
        int err;

        /*
         * Fetch CSD from card.
         */
 /*       err = mmc_send_csd(card, card->raw_csd);
        if (err)
                return err;

        err = mmc_decode_csd(card);
        if (err)
                return err;
*/
        return 0;
}

/*
 * Given a 64-bit response, decode to our card SCR structure.
 */
static int mmc_decode_scr(struct mmc_card *card)
{
        struct sd_scr *scr = &card->scr;
        unsigned int scr_struct;
        u32 resp[4];

        resp[3] = card->raw_scr[1];
        resp[2] = card->raw_scr[0];

        scr_struct = UNSTUFF_BITS(resp, 60, 4);
        if (scr_struct != 0) {
                pr_err("%s: unrecognised SCR structure version %d\n",
                        mmc_hostname(card->host), scr_struct);
                return -EINVAL;
        }

        scr->sda_vsn = UNSTUFF_BITS(resp, 56, 4);
        scr->bus_widths = UNSTUFF_BITS(resp, 48, 4);
        if (scr->sda_vsn == SCR_SPEC_VER_2)
                /* Check if Physical Layer Spec v3.0 is supported */
                scr->sda_spec3 = UNSTUFF_BITS(resp, 47, 1);

        if (UNSTUFF_BITS(resp, 55, 1))
                card->erased_byte = 0xFF;
        else
                card->erased_byte = 0x0;

        if (scr->sda_spec3)
                scr->cmds = UNSTUFF_BITS(resp, 32, 2);
        return 0;
}

/*
 * Fetch and process SD Status register.
 */
static int mmc_read_ssr(struct mmc_card *card)
{
        unsigned int au, es, et, eo;
        __be32 *raw_ssr;
        int i;

        if (!(card->csd.cmdclass & CCC_APP_SPEC)) {
                pr_warn("%s: card lacks mandatory SD Status function\n",
                        mmc_hostname(card->host));
                return 0;
        }

        raw_ssr = kmalloc(sizeof(card->raw_ssr), GFP_KERNEL);
        if (!raw_ssr)
                return -ENOMEM;

        if (mmc_app_sd_status(card, raw_ssr)) {
                pr_warn("%s: problem reading SD Status register\n",
                        mmc_hostname(card->host));
                kfree(raw_ssr);
                return 0;
        }

        for (i = 0; i < 16; i++) 
                card->raw_ssr[i] = be32_to_cpu(raw_ssr[i]);

        kfree(raw_ssr);

        /*
         * UNSTUFF_BITS only works with four u32s so we have to offset the
         * bitfield positions accordingly.
         */
        au = UNSTUFF_BITS(card->raw_ssr, 428 - 384, 4);
        if (au) {
                if (au <= 9 || card->scr.sda_spec3) {
                        card->ssr.au = sd_au_size[au];
                        es = UNSTUFF_BITS(card->raw_ssr, 408 - 384, 16);
                        et = UNSTUFF_BITS(card->raw_ssr, 402 - 384, 6);
                        if (es && et) {
                                eo = UNSTUFF_BITS(card->raw_ssr, 400 - 384, 2);
                                card->ssr.erase_timeout = (et * 1000) / es;
                                card->ssr.erase_offset = eo * 1000;
                        }
                } else {
                        pr_warn("%s: SD Status: Invalid Allocation Unit size\n",
                                mmc_hostname(card->host));
          }
        }

        return 0;
}

static int mmc_sd_get_ro(struct mmc_host *host)
{
        int ro;

        /*
         * Some systems don't feature a write-protect pin and don't need one.
         * E.g. because they only have micro-SD card slot. For those systems
         * assume that the SD card is always read-write.
         */
        if (host->caps2 & MMC_CAP2_NO_WRITE_PROTECT)
                return 0;

        if (!host->ops->get_ro)
                return -1;

        ro = host->ops->get_ro(host);

        return ro;
}


int mmc_sd_setup_card(struct mmc_host *host, struct mmc_card *card,
	bool reinit)
{
	int err;

	if (!reinit) {
		/*
		 * Fetch SCR from card.
		 */
		err = mmc_app_send_scr(card);
		if (err)
			return err;

		err = mmc_decode_scr(card);
		if (err)
			return err;

		/*
		 * Fetch and process SD Status register.
		 */
		err = mmc_read_ssr(card);
		if (err)
			return err;

		/* Erase init depends on CSD and SSR */
		mmc_init_erase(card);

		/*
		 * Fetch switch information from card.
		 */
		err = mmc_read_switch(card);
		if (err)
			return err;
	}

	/*
	 * For SPI, enable CRC as appropriate.
	 * This CRC enable is located AFTER the reading of the
	 * card registers because some SDHC cards are not able
	 * to provide valid CRCs for non-512-byte blocks.
	 */
	if (mmc_host_is_spi(host)) {
		err = mmc_spi_set_crc(host, use_spi_crc);
		if (err)
			return err;
	}

	/*
	 * Check if read-only switch is active.
	 */
	if (!reinit) {
		int ro = mmc_sd_get_ro(host);

		if (ro < 0) {
			pr_warn("%s: host does not support reading read-only switch, assuming write-enable\n",
				mmc_hostname(host));
		} else if (ro > 0) {
			mmc_card_set_readonly(card);
		}
	}

	return 0;
}

/*
 * Test if the card supports high-speed mode and, if so, switch to it.
 */
int mmc_sd_switch_hs(struct mmc_card *card)
{
        int err;
        u8 *status;

        if (card->scr.sda_vsn < SCR_SPEC_VER_1)
                return 0;

        if (!(card->csd.cmdclass & CCC_SWITCH))
                return 0;

        if (!(card->host->caps & MMC_CAP_SD_HIGHSPEED))
                return 0;

        if (card->sw_caps.hs_max_dtr == 0)
                return 0;

        status = kmalloc(64, GFP_KERNEL);
        if (!status)
                return -ENOMEM;

        err = mmc_sd_switch(card, 1, 0, 1, status);
        if (err)
                goto out;

        if ((status[16] & 0xF) != 1) {
                pr_warn("%s: Problem switching card into high-speed mode!\n",
                        mmc_hostname(card->host));
                err = 0;
        } else {
                err = 1;
        }

out:
        kfree(status);

        return err;
}

unsigned mmc_sd_get_max_clock(struct mmc_card *card)
{
        unsigned max_dtr = (unsigned int)-1;

        if (mmc_card_hs(card)) {
                if (max_dtr > card->sw_caps.hs_max_dtr)
                        max_dtr = card->sw_caps.hs_max_dtr;
        } else if (max_dtr > card->csd.max_dtr) {
                max_dtr = card->csd.max_dtr;
        }

        return max_dtr;
}






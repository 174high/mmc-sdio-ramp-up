/*
 *  linux/drivers/mmc/sdio.c
 *
 *  Copyright 2006-2007 Pierre Ossman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include <linux/err.h>
#include <linux/pm_runtime.h>

#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/init.h>

#include "core.h"
//#include "card.h"
#include "host.h"
#include "bus.h"
//#include "quirks.h"
#include "sd.h"
//#include "sdio_bus.h"
#include "mmc_ops.h"
//#include "sd_ops.h"
#include "sdio_ops.h"
//#include "sdio_cis.h"

static const struct mmc_bus_ops mmc_sdio_ops = {
  /*      .remove = mmc_sdio_remove,
        .detect = mmc_sdio_detect,
        .pre_suspend = mmc_sdio_pre_suspend,
        .suspend = mmc_sdio_suspend,
        .resume = mmc_sdio_resume,
        .runtime_suspend = mmc_sdio_runtime_suspend,
        .runtime_resume = mmc_sdio_runtime_resume,
        .alive = mmc_sdio_alive,
        .hw_reset = mmc_sdio_hw_reset,
        .sw_reset = mmc_sdio_sw_reset, */
};

/*
 * Handle the detection and initialisation of a card.
 *
 * In the case of a resume, "oldcard" will contain the card
 * we're trying to reinitialise.
 */
static int mmc_sdio_init_card(struct mmc_host *host, u32 ocr,
                              struct mmc_card *oldcard, int powered_resume)
{
        struct mmc_card *card;
        int err;
        int retries = 10;
        u32 rocr = 0;
        u32 ocr_card = ocr;

        WARN_ON(!host->claimed);

        /* to query card if 1.8V signalling is supported */
        if (mmc_host_uhs(host))
                ocr |= R4_18V_PRESENT;

try_again:
        if (!retries) {
                pr_warn("%s: Skipping voltage switch\n", mmc_hostname(host));
                ocr &= ~R4_18V_PRESENT;
        }

        /*
         * Inform the card of the voltage
         */
        if (!powered_resume) {
                err = mmc_send_io_op_cond(host, ocr, &rocr);
                if (err)
                        goto err;
        }
        /*
         * For SPI, enable CRC as appropriate.
         */
        if (mmc_host_is_spi(host)) {
                err = mmc_spi_set_crc(host, use_spi_crc);
                if (err)
                        goto err;
        }

        /*
         * Allocate card structure.
         */
        card = mmc_alloc_card(host, NULL);
        if (IS_ERR(card)) {
                err = PTR_ERR(card);
                goto err;
        }

        if ((rocr & R4_MEMORY_PRESENT) &&
            mmc_sd_get_cid(host, ocr & rocr, card->raw_cid, NULL) == 0) {
       /*         card->type = MMC_TYPE_SD_COMBO;

                if (oldcard && (oldcard->type != MMC_TYPE_SD_COMBO ||
                    memcmp(card->raw_cid, oldcard->raw_cid, sizeof(card->raw_cid)) != 0)) {
                        mmc_remove_card(card);
                        return -ENOENT;
                } */
        } else {
          /*      card->type = MMC_TYPE_SDIO;

                if (oldcard && oldcard->type != MMC_TYPE_SDIO) {
                        mmc_remove_card(card);
                        return -ENOENT;
                } */
        } 
        /*
         * Call the optional HC's init_card function to handle quirks.
         */
    //    if (host->ops->init_card)
    //            host->ops->init_card(host, card);

        /*
         * If the host and card support UHS-I mode request the card
         * to switch to 1.8V signaling level.  No 1.8v signalling if
         * UHS mode is not enabled to maintain compatibility and some
         * systems that claim 1.8v signalling in fact do not support
         * it. Per SDIO spec v3, section 3.1.2, if the voltage is already
         * 1.8v, the card sets S18A to 0 in the R4 response. So it will
         * fails to check rocr & R4_18V_PRESENT,  but we still need to
         * try to init uhs card. sdio_read_cccr will take over this task
         * to make sure which speed mode should work.
         */
   /*     if (!powered_resume && (rocr & ocr & R4_18V_PRESENT)) {
                err = mmc_set_uhs_voltage(host, ocr_card);
                if (err == -EAGAIN) {
                        mmc_sdio_resend_if_cond(host, card);
                        retries--;
                        goto try_again;
                } else if (err) {
                        ocr &= ~R4_18V_PRESENT;
                }
        }
  */
        /*
         * For native busses:  set card RCA and quit open drain mode.
         */
    //    if (!powered_resume && !mmc_host_is_spi(host)) {
    //            err = mmc_send_relative_addr(host, &card->rca);
    //            if (err)
    //                    goto remove;

                /*
                 * Update oldcard with the new RCA received from the SDIO
                 * device -- we're doing this so that it's updated in the
                 * "card" struct when oldcard overwrites that later.
                 */
    //            if (oldcard)
    //                    oldcard->rca = card->rca;
    //    }
        /*
         * Read CSD, before selecting the card
         */
     /*   if (!oldcard && card->type == MMC_TYPE_SD_COMBO) {
                err = mmc_sd_get_csd(host, card);
                if (err)
                        return err;

                mmc_decode_cid(card);
        }
 */
        /*
         * Select card, as all following commands rely on that.
         */
    /*    if (!powered_resume && !mmc_host_is_spi(host)) {
                err = mmc_select_card(card);
                if (err)
                        goto remove;
        }

        if (card->quirks & MMC_QUIRK_NONSTD_SDIO) {
      */          /*
                 * This is non-standard SDIO device, meaning it doesn't
                 * have any CIA (Common I/O area) registers present.
                 * It's host's responsibility to fill cccr and cis
                 * structures in init_card().
                 */
  /*              mmc_set_clock(host, card->cis.max_dtr);

                if (card->cccr.high_speed) {
                        mmc_set_timing(card->host, MMC_TIMING_SD_HS);
                }

                goto finish;
        } */
        /*
         * Read the common registers. Note that we should try to
         * validate whether UHS would work or not.
         */
    /*    err = sdio_read_cccr(card, ocr);
        if (err) {
                mmc_sdio_resend_if_cond(host, card);
                if (ocr & R4_18V_PRESENT) {  */
                        /* Retry init sequence, but without R4_18V_PRESENT. */
     /*                   retries = 0;
                        goto try_again;
                } else {
                        goto remove;
                }
        }
*/
        /*
         * Read the common CIS tuples.
         */
     /*   err = sdio_read_common_cis(card);
        if (err)
                goto remove;

        if (oldcard) {
                int same = (card->cis.vendor == oldcard->cis.vendor &&
                            card->cis.device == oldcard->cis.device);
                mmc_remove_card(card);
                if (!same)
                        return -ENOENT;

                card = oldcard;
        }
        card->ocr = ocr_card;
        mmc_fixup_device(card, sdio_fixup_methods);

        if (card->type == MMC_TYPE_SD_COMBO) {
                err = mmc_sd_setup_card(host, card, oldcard != NULL);
       */         /* handle as SDIO-only card if memory init failed */
     /*           if (err) {
                        mmc_go_idle(host);
                        if (mmc_host_is_spi(host))
  */                              /* should not fail, as it worked previously */
   /*                             mmc_spi_set_crc(host, use_spi_crc);
                        card->type = MMC_TYPE_SDIO;
                } else
                        card->dev.type = &sd_type;
        }
*/
        /*
         * If needed, disconnect card detection pull-up resistor.
         */
  //      err = sdio_disable_cd(card);
  //      if (err)
   //             goto remove;

        /* Initialization sequence for UHS-I cards */
        /* Only if card supports 1.8v and UHS signaling */
    /*    if ((ocr & R4_18V_PRESENT) && card->sw_caps.sd3_bus_mode) {
                err = mmc_sdio_init_uhs_card(card);
                if (err)
                        goto remove;
        } else {
      */          /*
                 * Switch to high-speed (if supported).
                 */
    /*            err = sdio_enable_hs(card);
                if (err > 0)
                        mmc_set_timing(card->host, MMC_TIMING_SD_HS);
                else if (err)
                        goto remove;
  */
                /*
                 * Change to the card's maximum speed.
                 */
  //              mmc_set_clock(host, mmc_sdio_get_max_clock(card));

                /*
                 * Switch to wider bus (if supported).
                 */
/*                err = sdio_enable_4bit_bus(card);
         if (err)
                        goto remove;
        }

        if (host->caps2 & MMC_CAP2_AVOID_3_3V &&
            host->ios.signal_voltage == MMC_SIGNAL_VOLTAGE_330) {
                pr_err("%s: Host failed to negotiate down from 3.3V\n",
                        mmc_hostname(host));
                err = -EINVAL;
                goto remove;
        }
finish:
        if (!oldcard)
                host->card = card;
        return 0;

remove:
        if (!oldcard)
                mmc_remove_card(card);
*/
err:
        return err;
}

/*
 * Starting point for SDIO card init.
 */
int mmc_attach_sdio(struct mmc_host *host)
{
        int err, i, funcs;
        u32 ocr, rocr;
        struct mmc_card *card;

        WARN_ON(!host->claimed);

        err = mmc_send_io_op_cond(host, 0, &ocr);
        if (err)
                return err;

        mmc_attach_bus(host, &mmc_sdio_ops);
         if (host->ocr_avail_sdio)
                host->ocr_avail = host->ocr_avail_sdio;


        rocr = mmc_select_voltage(host, ocr);
        /*
         * Can we support the voltage(s) of the card(s)?
         */
        if (!rocr) {
                 err = -EINVAL;
                 goto err;
         }

        /*
         * Detect and init the card.
         */
        err = mmc_sdio_init_card(host, rocr, NULL, 0);
        if (err)
                goto err;

        card = host->card;
 
        /*
         * Enable runtime PM only if supported by host+card+board
         */
 //       if (host->caps & MMC_CAP_POWER_OFF_CARD) {
                /*
                 * Do not allow runtime suspend until after SDIO function
                 * devices are added.
                 */
  //              pm_runtime_get_noresume(&card->dev);

                /*
                 * Let runtime PM core know our card is active
                 */
  //              err = pm_runtime_set_active(&card->dev);
  //              if (err)
  //                      goto remove;

                /*
                 * Enable runtime PM for this card
                 */
  //              pm_runtime_enable(&card->dev);
  //      }

        /*
         * The number of functions on the card is encoded inside
         * the ocr.
         */
   //     funcs = (ocr & 0x70000000) >> 28;
  //      card->sdio_funcs = 0;

        /*
         * Initialize (but don't add) all present functions.
         */
   //     for (i = 0; i < funcs; i++, card->sdio_funcs++) {
  //              err = sdio_init_func(host->card, i + 1);
   //             if (err)
   //                     goto remove;

                /*
                 * Enable Runtime PM for this func (if supported)
                 */
     //           if (host->caps & MMC_CAP_POWER_OFF_CARD)
      //                  pm_runtime_enable(&card->sdio_func[i]->dev);
     //   }

        /*
         * First add the card to the driver model...
         */
    //    mmc_release_host(host);
  //      err = mmc_add_card(host->card);
   //     if (err)
   //             goto remove_added;

        /*
         * ...then the SDIO functions.
         */
   /*      for (i = 0;i < funcs;i++) {
                err = sdio_add_func(host->card->sdio_func[i]);
                if (err)
                        goto remove_added;
        }

        if (host->caps & MMC_CAP_POWER_OFF_CARD)
                pm_runtime_put(&card->dev);

        mmc_claim_host(host);
        return 0;

remove:
        mmc_release_host(host);
remove_added:
     */   /*
         * The devices are being deleted so it is not necessary to disable
         * runtime PM. Similarly we also don't pm_runtime_put() the SDIO card
         * because it needs to be active to remove any function devices that
         * were probed, and after that it gets deleted.
         */
     /*   mmc_sdio_remove(host);
        mmc_claim_host(host); */
err:
 //       mmc_detach_bus(host);

        pr_err("%s: error %d whilst initialising SDIO card\n",
                mmc_hostname(host), err);

        return err;
}


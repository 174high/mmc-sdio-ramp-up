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
//#include "host.h"
//#include "bus.h"
//#include "quirks.h"
//#include "sd.h"
//#include "sdio_bus.h"
//#include "mmc_ops.h"
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
 * Starting point for SDIO card init.
 */
int mmc_attach_sdio(struct mmc_host *host)
{
        int err, i, funcs;
        u32 ocr, rocr;
        struct mmc_card *card;

        WARN_ON(!host->claimed);

        err = mmc_send_io_op_cond(host, 0, &ocr);
//        if (err)
//                return err;

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
 //       err = mmc_sdio_init_card(host, rocr, NULL, 0);
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


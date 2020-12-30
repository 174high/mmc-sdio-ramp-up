/*
 *  linux/drivers/mmc/core/host.c
 *
 *  Copyright (C) 2003 Russell King, All Rights Reserved.
 *  Copyright (C) 2007-2008 Pierre Ossman
 *  Copyright (C) 2010 Linus Walleij
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  MMC host class device management
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pagemap.h>
#include <linux/export.h>
#include <linux/leds.h>
#include <linux/slab.h>

#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/slot-gpio.h>

#include "core.h"
#include "host.h"
#include "slot-gpio.h"

#define cls_dev_to_mmc_host(d)  container_of(d, struct mmc_host, class_dev)

static DEFINE_IDA(mmc_host_ida);

static void mmc_host_classdev_release(struct device *dev)
{
        struct mmc_host *host = cls_dev_to_mmc_host(dev);
        ida_simple_remove(&mmc_host_ida, host->index);
        kfree(host);
}

static struct class mmc_host_class = {
        .name           = "mmc_host",
        .dev_release    = mmc_host_classdev_release,
};


/**
 *      mmc_alloc_host - initialise the per-host structure.
 *      @extra: sizeof private data structure
 *      @dev: pointer to host device model structure
 *
 *      Initialise the per-host structure.
 */
struct mmc_host *mmc_alloc_host(int extra, struct device *dev)
{
        int err;
        int alias_id;
        struct mmc_host *host;

        host = kzalloc(sizeof(struct mmc_host) + extra, GFP_KERNEL);
        if (!host)
                return NULL;

        /* scanning will be enabled when we're ready */
        host->rescan_disable = 1;
        host->parent = dev;

        alias_id = mmc_get_reserved_index(host);
  /*      if (alias_id >= 0)
                err = ida_simple_get(&mmc_host_ida, alias_id,
                                        alias_id + 1, GFP_KERNEL);
        else
                err = ida_simple_get(&mmc_host_ida,
                                        mmc_first_nonreserved_index(),
                                        0, GFP_KERNEL);
      if (err < 0) {
                kfree(host);
                return NULL;
        }

        host->index = err;

        dev_set_name(&host->class_dev, "mmc%d", host->index);

        host->class_dev.parent = dev;
        host->class_dev.class = &mmc_host_class;
        device_initialize(&host->class_dev);
        device_enable_async_suspend(&host->class_dev);

        if (mmc_gpio_alloc(host)) {
                put_device(&host->class_dev);
                ida_simple_remove(&mmc_host_ida, host->index);
                kfree(host);
                return NULL;
        }

        spin_lock_init(&host->lock);
        init_waitqueue_head(&host->wq); */
    //    INIT_DELAYED_WORK(&host->detect, mmc_rescan);
    //    INIT_DELAYED_WORK(&host->sdio_irq_work, sdio_irq_work);
    //    timer_setup(&host->retune_timer, mmc_retune_timer, 0);

        /*
         * By default, hosts do not support SGIO or large requests.
         * They have to set these according to their abilities.
         */
        host->max_segs = 1;
        host->max_seg_size = PAGE_SIZE;

        host->max_req_size = PAGE_SIZE;
        host->max_blk_size = 512;
        host->max_blk_count = PAGE_SIZE / 512;

        host->fixed_drv_type = -EINVAL;
        host->ios.power_delay_ms = 10;

        return host;
}

EXPORT_SYMBOL(mmc_alloc_host);

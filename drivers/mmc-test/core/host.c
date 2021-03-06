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
#include "pwrseq.h"
#include "sdio_ops.h"

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

void mmc_retune_enable(struct mmc_host *host)
{
        host->can_retune = 1;
        if (host->retune_period)
                mod_timer(&host->retune_timer,
                          jiffies + host->retune_period * HZ);
}



void mmc_retune_unpause(struct mmc_host *host)
{
        if (host->retune_paused) {
                host->retune_paused = 0;
                mmc_retune_release(host);
        }
}       
EXPORT_SYMBOL(mmc_retune_unpause);

void mmc_retune_disable(struct mmc_host *host)
{
        mmc_retune_unpause(host);
        host->can_retune = 0;
        del_timer_sync(&host->retune_timer);
        host->retune_now = 0;
        host->need_retune = 0;
}

void mmc_retune_hold(struct mmc_host *host)
{
        if (!host->hold_retune)
                host->retune_now = 1;
        host->hold_retune += 1;
}

void mmc_retune_release(struct mmc_host *host)
{
        if (host->hold_retune)
                host->hold_retune -= 1;
        else
                WARN_ON(1);
}
EXPORT_SYMBOL(mmc_retune_release);

static void mmc_retune_timer(struct timer_list *t)
{
        struct mmc_host *host = from_timer(host, t, retune_timer);

        mmc_retune_needed(host);
}

/**
 *      mmc_remove_host - remove host hardware
 *      @host: mmc host
 *
 *      Unregister and remove all cards associated with this host,
 *      and power down the MMC bus. No new requests will be issued
 *      after this function has returned.
 */
void mmc_remove_host(struct mmc_host *host)
{
        if (!(host->pm_caps & MMC_PM_IGNORE_PM_NOTIFY))
                mmc_unregister_pm_notifier(host);
        mmc_stop_host(host);

#ifdef CONFIG_DEBUG_FS
        mmc_remove_host_debugfs(host);
#endif

        device_del(&host->class_dev);

        led_trigger_unregister_simple(host->led);
}

EXPORT_SYMBOL(mmc_remove_host);

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
        if (alias_id >= 0)
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
        init_waitqueue_head(&host->wq); 
        INIT_DELAYED_WORK(&host->detect, mmc_rescan);
        INIT_DELAYED_WORK(&host->sdio_irq_work, sdio_irq_work);
        timer_setup(&host->retune_timer, mmc_retune_timer, 0);

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

int mmc_retune(struct mmc_host *host)
{
        bool return_to_hs400 = false;
        int err;

        if (host->retune_now)
                host->retune_now = 0;
        else
                return 0;

        if (!host->need_retune || host->doing_retune || !host->card)
                return 0;
 
        host->need_retune = 0;

        host->doing_retune = 1;

        if (host->ios.timing == MMC_TIMING_MMC_HS400) {
                err = mmc_hs400_to_hs200(host->card);
                if (err)
                        goto out;

                return_to_hs400 = true;
        }

        err = mmc_execute_tuning(host->card);
        if (err)
                goto out;

        if (return_to_hs400)
                err = mmc_hs200_to_hs400(host->card);
out:
        host->doing_retune = 0;

        return err;
}

/**
 *      mmc_free_host - free the host structure
 *      @host: mmc host
 *
 *      Free the host once all references to it have been dropped.
 */
void mmc_free_host(struct mmc_host *host)
{
        mmc_pwrseq_free(host);
        put_device(&host->class_dev);
}

EXPORT_SYMBOL(mmc_free_host);

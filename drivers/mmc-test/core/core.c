/*
 *  linux/drivers/mmc/core/core.c
 *
 *  Copyright (C) 2003-2004 Russell King, All Rights Reserved.
 *  SD support Copyright (C) 2004 Ian Molton, All Rights Reserved.
 *  Copyright (C) 2005-2008 Pierre Ossman, All Rights Reserved.
 *  MMCv4 support Copyright (C) 2006 Philip Langdale, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/pagemap.h>
#include <linux/err.h>
#include <linux/leds.h>
#include <linux/scatterlist.h>
#include <linux/log2.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeup.h>
#include <linux/suspend.h>
#include <linux/fault-inject.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/of.h>

#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/mmc/slot-gpio.h>

#define CREATE_TRACE_POINTS
#include <trace/events/mmc.h>

#include "core.h"

/* The max erase timeout, used when host->max_busy_timeout isn't specified */
#define MMC_ERASE_TIMEOUT_MS    (60 * 1000) /* 60 s */

static const unsigned freqs[] = { 400000, 300000, 200000, 100000 };

static int __mmc_max_reserved_idx = -1;

/*
 * mmc_get_reserved_index() - get the index reserved for this host
 * Return: The index reserved for this host or negative error value
 *         if no index is reserved for this host 
 */
int mmc_get_reserved_index(struct mmc_host *host)
{
        return of_alias_get_id(host->parent->of_node, "mmc");
}
EXPORT_SYMBOL(mmc_get_reserved_index);

/*
 * mmc_first_nonreserved_index() - get the first index that
 * is not reserved
 */
int mmc_first_nonreserved_index(void)
{
        return __mmc_max_reserved_idx + 1;
}
EXPORT_SYMBOL(mmc_first_nonreserved_index);

void mmc_rescan(struct work_struct *work)
{
        struct mmc_host *host =
                container_of(work, struct mmc_host, detect.work);
        int i;

        if (host->rescan_disable)
                return;

        /* If there is a non-removable card registered, only scan once */
        if (!mmc_card_is_removable(host) && host->rescan_entered)
                return;
        host->rescan_entered = 1;

        if (host->trigger_card_event && host->ops->card_event) {
                mmc_claim_host(host);
  //              host->ops->card_event(host);
  //              mmc_release_host(host);
  //              host->trigger_card_event = false;
        }

  //      mmc_bus_get(host);

        /*
         * if there is a _removable_ card registered, check whether it is
         * still present
         */
  //      if (host->bus_ops && !host->bus_dead && mmc_card_is_removable(host))
  //              host->bus_ops->detect(host);

  //      host->detect_change = 0;

        /*
         * Let mmc_bus_put() free the bus/bus_ops if we've found that
         * the card is no longer present.
         */
//        mmc_bus_put(host);
//        mmc_bus_get(host);

        /* if there still is a card present, stop here */
/*        if (host->bus_ops != NULL) {
                mmc_bus_put(host);
                goto out;
        }
*/
        /*
         * Only we can add a new handler, so it's safe to
         * release the lock here.
         */
/*        mmc_bus_put(host);

        mmc_claim_host(host);
        if (mmc_card_is_removable(host) && host->ops->get_cd &&
                        host->ops->get_cd(host) == 0) {
                mmc_power_off(host);
                mmc_release_host(host);
                goto out;
        }

        for (i = 0; i < ARRAY_SIZE(freqs); i++) {
                if (!mmc_rescan_try_freq(host, max(freqs[i], host->f_min)))
                        break;
                if (freqs[i] <= host->f_min)
                        break;
        }
        mmc_release_host(host);

 out:
        if (host->caps & MMC_CAP_NEEDS_POLL)
                mmc_schedule_delayed_work(&host->detect, HZ); 
*/
}

/*      
 * Allow claiming an already claimed host if the context is the same or there is
 * no context but the task is the same.
 */
static inline bool mmc_ctx_matches(struct mmc_host *host, struct mmc_ctx *ctx,
                                   struct task_struct *task)
{
        return host->claimer == ctx ||
               (!ctx && task && host->claimer->task == task);
}

static inline void mmc_ctx_set_claimer(struct mmc_host *host,
                                       struct mmc_ctx *ctx,
                                       struct task_struct *task)
{
        if (!host->claimer) {
                if (ctx)
                        host->claimer = ctx;
                else
                        host->claimer = &host->default_ctx;
        }
        if (task)
                host->claimer->task = task;
}

/**
 *      __mmc_claim_host - exclusively claim a host
 *      @host: mmc host to claim
 *      @ctx: context that claims the host or NULL in which case the default
 *      context will be used
 *      @abort: whether or not the operation should be aborted
 *
 *      Claim a host for a set of operations.  If @abort is non null and
 *      dereference a non-zero value then this will return prematurely with
 *      that non-zero value without acquiring the lock.  Returns zero
 *      with the lock held otherwise.
 */
int __mmc_claim_host(struct mmc_host *host, struct mmc_ctx *ctx,
                     atomic_t *abort)
{
        struct task_struct *task = ctx ? NULL : current;
        DECLARE_WAITQUEUE(wait, current);
        unsigned long flags;
        int stop;
        bool pm = false;

        might_sleep();

        add_wait_queue(&host->wq, &wait);
        spin_lock_irqsave(&host->lock, flags);
        while (1) {
    set_current_state(TASK_UNINTERRUPTIBLE);
                stop = abort ? atomic_read(abort) : 0;
                if (stop || !host->claimed || mmc_ctx_matches(host, ctx, task))
                        break;
                spin_unlock_irqrestore(&host->lock, flags);
                schedule();
                spin_lock_irqsave(&host->lock, flags);
        }
        set_current_state(TASK_RUNNING);
        if (!stop) {
                host->claimed = 1;
                mmc_ctx_set_claimer(host, ctx, task);
                host->claim_cnt += 1;
                if (host->claim_cnt == 1)
                        pm = true;
        } else
                wake_up(&host->wq);
        spin_unlock_irqrestore(&host->lock, flags);
        remove_wait_queue(&host->wq, &wait);

        if (pm)
                pm_runtime_get_sync(mmc_dev(host));

        return stop;
}
EXPORT_SYMBOL(__mmc_claim_host);



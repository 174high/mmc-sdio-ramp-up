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
#include "host.h"

#include "pwrseq.h"

#include "sdio_ops.h"

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

/*
 * Increase reference count of bus operator
 */
static inline void mmc_bus_get(struct mmc_host *host)
{
        unsigned long flags; 

        spin_lock_irqsave(&host->lock, flags);
        host->bus_refs++;
        spin_unlock_irqrestore(&host->lock, flags);
}

/*
 * Cleanup when the last reference to the bus operator is dropped.
 */
static void __mmc_release_bus(struct mmc_host *host)
{
        WARN_ON(!host->bus_dead);

        host->bus_ops = NULL;
}

/*
 * Decrease reference count of bus operator and free it if 
 * it is the last reference. 
 */ 
static inline void mmc_bus_put(struct mmc_host *host)
{   
        unsigned long flags;
    
        spin_lock_irqsave(&host->lock, flags);
        host->bus_refs--;
        if ((host->bus_refs == 0) && host->bus_ops)
                __mmc_release_bus(host);
        spin_unlock_irqrestore(&host->lock, flags);
}

/*
 * Internal function that does the actual ios call to the host driver,
 * optionally printing some debug output.
 */
static inline void mmc_set_ios(struct mmc_host *host)
{
        struct mmc_ios *ios = &host->ios;

        pr_debug("%s: clock %uHz busmode %u powermode %u cs %u Vdd %u "
                "width %u timing %u\n", 
                 mmc_hostname(host), ios->clock, ios->bus_mode,
                 ios->power_mode, ios->chip_select, ios->vdd,
                 1 << ios->bus_width, ios->timing);

        host->ops->set_ios(host, ios); 
} 

/*
 * Set initial state after a power cycle or a hw_reset.
 */
void mmc_set_initial_state(struct mmc_host *host)
{
        if (host->cqe_on)
                host->cqe_ops->cqe_off(host);

        mmc_retune_disable(host);

        if (mmc_host_is_spi(host))
                host->ios.chip_select = MMC_CS_HIGH;
        else
                host->ios.chip_select = MMC_CS_DONTCARE;
        host->ios.bus_mode = MMC_BUSMODE_PUSHPULL;
        host->ios.bus_width = MMC_BUS_WIDTH_1;
        host->ios.timing = MMC_TIMING_LEGACY;
        host->ios.drv_type = 0;
        host->ios.enhanced_strobe = false;

        /*
         * Make sure we are in non-enhanced strobe mode before we
         * actually enable it in ext_csd.
         */
        if ((host->caps2 & MMC_CAP2_HS400_ES) &&
             host->ops->hs400_enhanced_strobe)
                host->ops->hs400_enhanced_strobe(host, &host->ios);

        mmc_set_ios(host); 
}

void mmc_power_off(struct mmc_host *host)
{
        if (host->ios.power_mode == MMC_POWER_OFF)
                return;

        mmc_pwrseq_power_off(host);

        host->ios.clock = 0; 
        host->ios.vdd = 0;

        host->ios.power_mode = MMC_POWER_OFF;
        /* Set initial state and call mmc_set_ios */
        mmc_set_initial_state(host);

        /*
         * Some configurations, such as the 802.11 SDIO card in the OLPC
         * XO-1.5, require a short delay after poweroff before the card
         * can be successfully turned on again.
         */ 
        mmc_delay(1);
} 

int mmc_set_signal_voltage(struct mmc_host *host, int signal_voltage)
{
        int err = 0;
        int old_signal_voltage = host->ios.signal_voltage;

        host->ios.signal_voltage = signal_voltage;
        if (host->ops->start_signal_voltage_switch)
                err = host->ops->start_signal_voltage_switch(host, &host->ios);

        if (err)
                host->ios.signal_voltage = old_signal_voltage;

        return err;

}
  
void mmc_set_initial_signal_voltage(struct mmc_host *host)
{
        /* Try to set signal voltage to 3.3V but fall back to 1.8v or 1.2v */
        if (!mmc_set_signal_voltage(host, MMC_SIGNAL_VOLTAGE_330))
                dev_dbg(mmc_dev(host), "Initial signal voltage of 3.3v\n");
        else if (!mmc_set_signal_voltage(host, MMC_SIGNAL_VOLTAGE_180))
                dev_dbg(mmc_dev(host), "Initial signal voltage of 1.8v\n");
        else if (!mmc_set_signal_voltage(host, MMC_SIGNAL_VOLTAGE_120))
                dev_dbg(mmc_dev(host), "Initial signal voltage of 1.2v\n");
}

/*
 * Apply power to the MMC stack.  This is a two-stage process.
 * First, we enable power to the card without the clock running.
 * We then wait a bit for the power to stabilise.  Finally,
 * enable the bus drivers and clock to the card.
 *
 * We must _NOT_ enable the clock prior to power stablising.
 *
 * If a host does all the power sequencing itself, ignore the
 * initial MMC_POWER_UP stage.
 */
void mmc_power_up(struct mmc_host *host, u32 ocr)
{
        if (host->ios.power_mode == MMC_POWER_ON)
                return;

        mmc_pwrseq_pre_power_on(host);
        
        host->ios.vdd = fls(ocr) - 1;
        host->ios.power_mode = MMC_POWER_UP;
        /* Set initial state and call mmc_set_ios */
        mmc_set_initial_state(host);
        
        mmc_set_initial_signal_voltage(host);
        
        /*
         * This delay should be sufficient to allow the power supply
         * to reach the minimum voltage.
         */
        mmc_delay(host->ios.power_delay_ms);

     //   mmc_pwrseq_post_power_on(host);

     //   host->ios.clock = host->f_init;

    //    host->ios.power_mode = MMC_POWER_ON;
    //    mmc_set_ios(host);

        /*
         * This delay must be at least 74 clock sizes, or 1 ms, or the
         * time required to reach a stable voltage.
         */
  //      mmc_delay(host->ios.power_delay_ms);
}

static void mmc_hw_reset_for_init(struct mmc_host *host)
{               
        mmc_pwrseq_reset(host);

        if (!(host->caps & MMC_CAP_HW_RESET) || !host->ops->hw_reset)
                return;
        host->ops->hw_reset(host);
}

static inline void mmc_wait_ongoing_tfr_cmd(struct mmc_host *host)
{
 //       struct mmc_request *ongoing_mrq = READ_ONCE(host->ongoing_mrq);

        /*
         * If there is an ongoing transfer, wait for the command line to become
         * available.
         */
 //       if (ongoing_mrq && !completion_done(&ongoing_mrq->cmd_completion))
 //               wait_for_completion(&ongoing_mrq->cmd_completion);
}

static int __mmc_start_req(struct mmc_host *host, struct mmc_request *mrq)
{
        int err;
/*
        mmc_wait_ongoing_tfr_cmd(host);

        init_completion(&mrq->completion);
        mrq->done = mmc_wait_done;

        err = mmc_start_request(host, mrq);
        if (err) {
                mrq->cmd->error = err;
                mmc_complete_cmd(mrq);
                complete(&mrq->completion);
        }
 */   
        return err; 
}


static int mmc_rescan_try_freq(struct mmc_host *host, unsigned freq)
{
        host->f_init = freq; 

        pr_debug("%s: %s: trying to init card at %u Hz\n",
                mmc_hostname(host), __func__, host->f_init);

        mmc_power_up(host, host->ocr_avail);
 
        /*
         * Some eMMCs (with VCCQ always on) may not be reset after power up, so
         * do a hardware reset if possible.
         */
        mmc_hw_reset_for_init(host);

        /*
         * sdio_reset sends CMD52 to reset card.  Since we do not know
         * if the card is being re-initialized, just send it.  CMD52
         * should be ignored by SD/eMMC cards.
         * Skip it if we already know that we do not support SDIO commands
         */
        if (!(host->caps2 & MMC_CAP2_NO_SDIO))
                sdio_reset(host);

//        mmc_go_idle(host);

//        if (!(host->caps2 & MMC_CAP2_NO_SD))
//                mmc_send_if_cond(host, host->ocr_avail);

        /* Order's important: probe SDIO, then SD, then MMC */
  /*      if (!(host->caps2 & MMC_CAP2_NO_SDIO))
               if (!mmc_attach_sdio(host))
                {
                        return 0;
                }



        if (!(host->caps2 & MMC_CAP2_NO_SD))
                if (!mmc_attach_sd(host))
                        return 0;

        if (!(host->caps2 & MMC_CAP2_NO_MMC))
                if (!mmc_attach_mmc(host))
                        return 0;

        mmc_power_off(host);
    */    return -EIO;
}



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
                host->ops->card_event(host);
                mmc_release_host(host);
                host->trigger_card_event = false;
        }

        mmc_bus_get(host);

        /*
         * if there is a _removable_ card registered, check whether it is
         * still present
         */
        if (host->bus_ops && !host->bus_dead && mmc_card_is_removable(host))
                host->bus_ops->detect(host);

        host->detect_change = 0;

        /*
         * Let mmc_bus_put() free the bus/bus_ops if we've found that
         * the card is no longer present.
         */
        mmc_bus_put(host);
        mmc_bus_get(host);

        /* if there still is a card present, stop here */
        if (host->bus_ops != NULL) {
                mmc_bus_put(host);
//                goto out;
        }

        /*
         * Only we can add a new handler, so it's safe to
         * release the lock here.
         */
        mmc_bus_put(host);

        mmc_claim_host(host);
        if (mmc_card_is_removable(host) && host->ops->get_cd &&
                        host->ops->get_cd(host) == 0) {
                mmc_power_off(host);
                mmc_release_host(host);
 //               goto out;
        }

        for (i = 0; i < ARRAY_SIZE(freqs); i++) {
                if (!mmc_rescan_try_freq(host, max(freqs[i], host->f_min)))
                        break;
                if (freqs[i] <= host->f_min)
                        break;
        }
//        mmc_release_host(host);

// out:
 //       if (host->caps & MMC_CAP_NEEDS_POLL)
 //               mmc_schedule_delayed_work(&host->detect, HZ); 

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

/**
 *      mmc_release_host - release a host
 *      @host: mmc host to release
 *
 *      Release a MMC host, allowing others to claim the host
 *      for their operations.
 */
void mmc_release_host(struct mmc_host *host)
{
        unsigned long flags;

        WARN_ON(!host->claimed);

        spin_lock_irqsave(&host->lock, flags);
        if (--host->claim_cnt) {
                /* Release for nested claim */
                spin_unlock_irqrestore(&host->lock, flags);
        } else {
                host->claimed = 0;
                host->claimer->task = NULL;
                host->claimer = NULL;
                spin_unlock_irqrestore(&host->lock, flags);
                wake_up(&host->wq);
                pm_runtime_mark_last_busy(mmc_dev(host));
                pm_runtime_put_autosuspend(mmc_dev(host));
        }
}
EXPORT_SYMBOL(mmc_release_host);

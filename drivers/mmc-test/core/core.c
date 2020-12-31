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
#include "card.h"
#include "host.h"

#include "pwrseq.h"

#include "mmc_ops.h"
#include "sd_ops.h"
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
 * Control chip select pin on a host.
 */
void mmc_set_chip_select(struct mmc_host *host, int mode)
{
        host->ios.chip_select = mode;
        mmc_set_ios(host);
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

static inline void mmc_wait_ongoing_tfr_cmd(struct mmc_host *host)
{
        struct mmc_request *ongoing_mrq = READ_ONCE(host->ongoing_mrq);

        /*
         * If there is an ongoing transfer, wait for the command line to become
         * available.
         */
        if (ongoing_mrq && !completion_done(&ongoing_mrq->cmd_completion))
                wait_for_completion(&ongoing_mrq->cmd_completion);
}

static void mmc_mrq_pr_debug(struct mmc_host *host, struct mmc_request *mrq,
                             bool cqe)
{
        if (mrq->sbc) {
                pr_debug("<%s: starting CMD%u arg %08x flags %08x>\n",
                         mmc_hostname(host), mrq->sbc->opcode,
                         mrq->sbc->arg, mrq->sbc->flags);
        }

        if (mrq->cmd) {
                pr_debug("%s: starting %sCMD%u arg %08x flags %08x\n",
                         mmc_hostname(host), cqe ? "CQE direct " : "",
                         mrq->cmd->opcode, mrq->cmd->arg, mrq->cmd->flags);
        } else if (cqe) {
                pr_debug("%s: starting CQE transfer for tag %d blkaddr %u\n",
                         mmc_hostname(host), mrq->tag, mrq->data->blk_addr);
        }

        if (mrq->data) { 
                pr_debug("%s:     blksz %d blocks %d flags %08x "
                        "tsac %d ms nsac %d\n",
                        mmc_hostname(host), mrq->data->blksz,
                        mrq->data->blocks, mrq->data->flags,
                        mrq->data->timeout_ns / 1000000,
                        mrq->data->timeout_clks);
        }

        if (mrq->stop) {
                pr_debug("%s:     CMD%u arg %08x flags %08x\n",
                         mmc_hostname(host), mrq->stop->opcode,
                         mrq->stop->arg, mrq->stop->flags);
        }
}

static int mmc_mrq_prep(struct mmc_host *host, struct mmc_request *mrq)
{
        unsigned int i, sz = 0;
        struct scatterlist *sg;

        if (mrq->cmd) {
                mrq->cmd->error = 0;
                mrq->cmd->mrq = mrq;
                mrq->cmd->data = mrq->data;
        }
        if (mrq->sbc) {
                mrq->sbc->error = 0;
                mrq->sbc->mrq = mrq;
        }
        if (mrq->data) {
                if (mrq->data->blksz > host->max_blk_size || 
                    mrq->data->blocks > host->max_blk_count ||
                    mrq->data->blocks * mrq->data->blksz > host->max_req_size)
                        return -EINVAL; 
    
                for_each_sg(mrq->data->sg, sg, mrq->data->sg_len, i)
                        sz += sg->length;
                if (sz != mrq->data->blocks * mrq->data->blksz)
                        return -EINVAL;

                mrq->data->error = 0;
                mrq->data->mrq = mrq;
                if (mrq->stop) {
                        mrq->data->stop = mrq->stop;
                        mrq->stop->error = 0;
                        mrq->stop->mrq = mrq;
                }
        }

        return 0;
}

static void __mmc_start_request(struct mmc_host *host, struct mmc_request *mrq)
{
        int err;

        /* Assumes host controller has been runtime resumed by mmc_claim_host */
        err = mmc_retune(host);
        if (err) {
                mrq->cmd->error = err; 
// shijonn                mmc_request_done(host, mrq);
                return;
        }

        /*
         * For sdio rw commands we must wait for card busy otherwise some
         * sdio devices won't work properly.
         * And bypass I/O abort, reset and bus suspend operations.
         */
 //       if (sdio_is_io_busy(mrq->cmd->opcode, mrq->cmd->arg) &&
 //           host->ops->card_busy) {
//                int tries = 500; /* Wait aprox 500ms at maximum */
/*    
                while (host->ops->card_busy(host) && --tries)
                        mmc_delay(1);

                if (tries == 0) {
                        mrq->cmd->error = -EBUSY;
                        mmc_request_done(host, mrq);
                        return;
                }
        }

        if (mrq->cap_cmd_during_tfr) {
                host->ongoing_mrq = mrq;
  */              /*
                 * Retry path could come through here without having waiting on
                 * cmd_completion, so ensure it is reinitialised.
                 */
 /*               reinit_completion(&mrq->cmd_completion);
        }

        trace_mmc_request_start(host, mrq);

        if (host->cqe_on)
                host->cqe_ops->cqe_off(host);

        host->ops->request(host, mrq); */
}

int mmc_start_request(struct mmc_host *host, struct mmc_request *mrq)
{
        int err;

        init_completion(&mrq->cmd_completion);

        mmc_retune_hold(host);

        if (mmc_card_removed(host->card))
                return -ENOMEDIUM;

        mmc_mrq_pr_debug(host, mrq, false);

        WARN_ON(!host->claimed);

        err = mmc_mrq_prep(host, mrq);
        if (err)
                return err;

        led_trigger_event(host->led, LED_FULL);
        __mmc_start_request(host, mrq);

        return 0;
}
EXPORT_SYMBOL(mmc_start_request);

static void mmc_wait_done(struct mmc_request *mrq)
{
        complete(&mrq->completion);
}

static inline void mmc_complete_cmd(struct mmc_request *mrq)
{       
        if (mrq->cap_cmd_during_tfr && !completion_done(&mrq->cmd_completion))
                complete_all(&mrq->cmd_completion);
} 

static int __mmc_start_req(struct mmc_host *host, struct mmc_request *mrq)
{
        int err;

        mmc_wait_ongoing_tfr_cmd(host);

        init_completion(&mrq->completion);
        mrq->done = mmc_wait_done;

        err = mmc_start_request(host, mrq);
        if (err) {
                mrq->cmd->error = err;
                mmc_complete_cmd(mrq);
                complete(&mrq->completion);
        }
 
        return err;
}

void mmc_wait_for_req_done(struct mmc_host *host, struct mmc_request *mrq)
{
        struct mmc_command *cmd;

        while (1) {
                wait_for_completion(&mrq->completion);

                cmd = mrq->cmd;

                /*
                 * If host has timed out waiting for the sanitize
                 * to complete, card might be still in programming state
                 * so let's try to bring the card out of programming
                 * state.
                 */
                if (cmd->sanitize_busy && cmd->error == -ETIMEDOUT) {
                        if (!mmc_interrupt_hpi(host->card)) {
                                pr_warn("%s: %s: Interrupted sanitize\n",
                                        mmc_hostname(host), __func__);
                                cmd->error = 0;
                                break;
                        } else {
                                pr_err("%s: %s: Failed to interrupt sanitize\n",
                                       mmc_hostname(host), __func__);
                        } 
                }
                if (!cmd->error || !cmd->retries ||
                    mmc_card_removed(host->card))
                        break;
                
                mmc_retune_recheck(host);
                
                pr_debug("%s: req failed (CMD%u): %d, retrying...\n",
                         mmc_hostname(host), cmd->opcode, cmd->error);
                cmd->retries--;
                cmd->error = 0;
                __mmc_start_request(host, mrq);
        }

        mmc_retune_release(host);
}
EXPORT_SYMBOL(mmc_wait_for_req_done);


/**
 *      mmc_wait_for_req - start a request and wait for completion
 *      @host: MMC host to start command
 *      @mrq: MMC request to start
 *
 *      Start a new MMC custom command request for a host, and wait
 *      for the command to complete. In the case of 'cap_cmd_during_tfr'
 *      requests, the transfer is ongoing and the caller can issue further
 *      commands that do not use the data lines, and then wait by calling
 *      mmc_wait_for_req_done(). 
 *      Does not attempt to parse the response.
 */
void mmc_wait_for_req(struct mmc_host *host, struct mmc_request *mrq)
{
        __mmc_start_req(host, mrq);

        if (!mrq->cap_cmd_during_tfr)
               mmc_wait_for_req_done(host, mrq);
}
EXPORT_SYMBOL(mmc_wait_for_req);

/**
 *      mmc_wait_for_cmd - start a command and wait for completion
 *      @host: MMC host to start command
 *      @cmd: MMC command to start
 *      @retries: maximum number of retries
 *
 *      Start a new MMC command for a host, and wait for the command
 *      to complete.  Return any error that occurred while the command
 *      was executing.  Do not attempt to parse the response.
 */
int mmc_wait_for_cmd(struct mmc_host *host, struct mmc_command *cmd, int retries)
{
        struct mmc_request mrq = {};

        WARN_ON(!host->claimed);

        memset(cmd->resp, 0, sizeof(cmd->resp));
        cmd->retries = retries;

        mrq.cmd = cmd;
        cmd->data = NULL;

        mmc_wait_for_req(host, &mrq);

        return cmd->error;
}

EXPORT_SYMBOL(mmc_wait_for_cmd);

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

        mmc_pwrseq_post_power_on(host);

        host->ios.clock = host->f_init;

        host->ios.power_mode = MMC_POWER_ON;
        mmc_set_ios(host);

        /*
         * This delay must be at least 74 clock sizes, or 1 ms, or the
         * time required to reach a stable voltage.
         */
        mmc_delay(host->ios.power_delay_ms);
}

static void mmc_hw_reset_for_init(struct mmc_host *host)
{               
        mmc_pwrseq_reset(host);

        if (!(host->caps & MMC_CAP_HW_RESET) || !host->ops->hw_reset)
                return;
        host->ops->hw_reset(host);
}

/**
 *      mmc_set_data_timeout - set the timeout for a data command
 *      @data: data phase for command
 *      @card: the MMC card associated with the data transfer
 *
 *      Computes the data timeout parameters according to the
 *      correct algorithm given the card type.
 */
void mmc_set_data_timeout(struct mmc_data *data, const struct mmc_card *card)
{
        unsigned int mult;

        /*
         * SDIO cards only define an upper 1 s limit on access.
         */
        if (mmc_card_sdio(card)) {
                data->timeout_ns = 1000000000;
                data->timeout_clks = 0;
                return;
        }

        /*
         * SD cards use a 100 multiplier rather than 10
         */
        mult = mmc_card_sd(card) ? 100 : 10;

        /*
         * Scale up the multiplier (and therefore the timeout) by
         * the r2w factor for writes.
         */
        if (data->flags & MMC_DATA_WRITE)
                mult <<= card->csd.r2w_factor;

        data->timeout_ns = card->csd.taac_ns * mult;
        data->timeout_clks = card->csd.taac_clks * mult;

        /*
         * SD cards also have an upper limit on the timeout.
         */
        if (mmc_card_sd(card)) {
                unsigned int timeout_us, limit_us;

                timeout_us = data->timeout_ns / 1000;
                if (card->host->ios.clock)
                        timeout_us += data->timeout_clks * 1000 /
                                (card->host->ios.clock / 1000);

                if (data->flags & MMC_DATA_WRITE)
                        /*
                         * The MMC spec "It is strongly recommended
                         * for hosts to implement more than 500ms
                         * timeout value even if the card indicates
                         * the 250ms maximum busy length."  Even the
                         * previous value of 300ms is known to be
                         * insufficient for some cards.
                         */
                        limit_us = 3000000;
                else
                        limit_us = 100000;

                /*
                 * SDHC cards always use these fixed values.
                 */
                if (timeout_us > limit_us) {
                        data->timeout_ns = limit_us * 1000;
                        data->timeout_clks = 0;
                }

                /* assign limit value if invalid */
                if (timeout_us == 0)
                        data->timeout_ns = limit_us * 1000;
        }
        /*
         * Some cards require longer data read timeout than indicated in CSD.
         * Address this by setting the read timeout to a "reasonably high"
         * value. For the cards tested, 600ms has proven enough. If necessary,
         * this value can be increased if other problematic cards require this.
         */
        if (mmc_card_long_read_time(card) && data->flags & MMC_DATA_READ) {
                data->timeout_ns = 600000000;
                data->timeout_clks = 0;
        }

        /*
         * Some cards need very high timeouts if driven in SPI mode.
         * The worst observed timeout was 900ms after writing a
         * continuous stream of data until the internal logic
         * overflowed.
         */
         if (mmc_host_is_spi(card->host)) {
                if (data->flags & MMC_DATA_WRITE) {
                        if (data->timeout_ns < 1000000000)
                                data->timeout_ns = 1000000000;  /* 1s */
                } else {
                        if (data->timeout_ns < 100000000)
                                data->timeout_ns =  100000000;  /* 100ms */
                }
        } 
}
EXPORT_SYMBOL(mmc_set_data_timeout);


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

        mmc_go_idle(host);

        if (!(host->caps2 & MMC_CAP2_NO_SD))
                mmc_send_if_cond(host, host->ocr_avail);

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

/*
 * Sets the host clock to the highest possible frequency that
 * is below "hz".
 */
void mmc_set_clock(struct mmc_host *host, unsigned int hz)
{
        WARN_ON(hz && hz < host->f_min);

        if (hz > host->f_max)
                hz = host->f_max;

        host->ios.clock = hz;
        mmc_set_ios(host);
}

int mmc_hs400_to_hs200(struct mmc_card *card)
{
        struct mmc_host *host = card->host;
        unsigned int max_dtr;
        int err;
        u8 val;

        /* Reduce frequency to HS */
        max_dtr = card->ext_csd.hs_max_dtr;
        mmc_set_clock(host, max_dtr);

        /* Switch HS400 to HS DDR */
        val = EXT_CSD_TIMING_HS;
        err = __mmc_switch(card, EXT_CSD_CMD_SET_NORMAL, EXT_CSD_HS_TIMING,
                           val, card->ext_csd.generic_cmd6_time, 0,
                           true, false, true);
    /*    if (err)
                goto out_err;

        mmc_set_timing(host, MMC_TIMING_MMC_DDR52);

        err = mmc_switch_status(card);
        if (err)
                goto out_err;
   */
        /* Switch HS DDR to HS */
    /*    err = __mmc_switch(card, EXT_CSD_CMD_SET_NORMAL, EXT_CSD_BUS_WIDTH,
                           EXT_CSD_BUS_WIDTH_8, card->ext_csd.generic_cmd6_time,
                           0, true, false, true);
        if (err)
                goto out_err;

        mmc_set_timing(host, MMC_TIMING_MMC_HS);

        if (host->ops->hs400_downgrade)
               host->ops->hs400_downgrade(host);

        err = mmc_switch_status(card);
        if (err)
                goto out_err;
    */
        /* Switch HS to HS200 */
    /*    val = EXT_CSD_TIMING_HS200 |
              card->drive_strength << EXT_CSD_DRV_STR_SHIFT;
        err = __mmc_switch(card, EXT_CSD_CMD_SET_NORMAL, EXT_CSD_HS_TIMING,
                           val, card->ext_csd.generic_cmd6_time, 0,
                           true, false, true);
        if (err)
                goto out_err;

        mmc_set_timing(host, MMC_TIMING_MMC_HS200);
   */
        /*
         * For HS200, CRC errors are not a reliable way to know the switch
         * failed. If there really is a problem, we would expect tuning will
         * fail and the result ends up the same.
         */
     /*   err = __mmc_switch_status(card, false);
        if (err)
                goto out_err;

        mmc_set_bus_speed(card);
    */
        /* Prepare tuning for HS400 mode. */
     //   if (host->ops->prepare_hs400_tuning)
     //           host->ops->prepare_hs400_tuning(host, &host->ios);

        return 0;

out_err:
        pr_err("%s: %s failed, error %d\n", mmc_hostname(card->host),
               __func__, err);
 
        return err;
}

/*
 * Select timing parameters for host.
 */
void mmc_set_timing(struct mmc_host *host, unsigned int timing)
{
        host->ios.timing = timing;
        mmc_set_ios(host);
}

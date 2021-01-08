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

bool use_spi_crc = 1;
module_param(use_spi_crc, bool, 0);

static int mmc_schedule_delayed_work(struct delayed_work *work,
                                     unsigned long delay)
{
        /*
         * We use the system_freezable_wq, because of two reasons.
         * First, it allows several works (not the same work item) to be
         * executed simultaneously. Second, the queue becomes frozen when
         * userspace becomes frozen during system PM.
         */
        return queue_delayed_work(system_freezable_wq, work, delay);
}

#ifdef CONFIG_FAIL_MMC_REQUEST

/*
 * Internal function. Inject random data errors.
 * If mmc_data is NULL no errors are injected.
 */
static void mmc_should_fail_request(struct mmc_host *host,
                                    struct mmc_request *mrq)
{
        struct mmc_command *cmd = mrq->cmd;
        struct mmc_data *data = mrq->data;
        static const int data_errors[] = {
                -ETIMEDOUT,
                -EILSEQ,
                -EIO,
        };

        if (!data)
                return;

        if ((cmd && cmd->error) || data->error ||
            !should_fail(&host->fail_mmc_request, data->blksz * data->blocks))
                return;

        data->error = data_errors[prandom_u32() % ARRAY_SIZE(data_errors)];
        data->bytes_xfered = (prandom_u32() % (data->bytes_xfered >> 9)) << 9;
}

#else /* CONFIG_FAIL_MMC_REQUEST */

static inline void mmc_should_fail_request(struct mmc_host *host,
                                           struct mmc_request *mrq)
{
}

#endif /* CONFIG_FAIL_MMC_REQUEST */

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
 * Change data bus width of a host.
 */
void mmc_set_bus_width(struct mmc_host *host, unsigned int width)
{
        host->ios.bus_width = width;
        mmc_set_ios(host);
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

static inline void mmc_complete_cmd(struct mmc_request *mrq)
{
        if (mrq->cap_cmd_during_tfr && !completion_done(&mrq->cmd_completion))
                complete_all(&mrq->cmd_completion);
}


/**
 *	mmc_request_done - finish processing an MMC request
 *	@host: MMC host which completed request
 *	@mrq: MMC request which request
 *
 *	MMC drivers should call this function when they have completed
 *	their processing of a request.
 */
void mmc_request_done(struct mmc_host *host, struct mmc_request *mrq)
{
	struct mmc_command *cmd = mrq->cmd;
	int err = cmd->error;

	/* Flag re-tuning needed on CRC errors */
	if ((cmd->opcode != MMC_SEND_TUNING_BLOCK &&
	    cmd->opcode != MMC_SEND_TUNING_BLOCK_HS200) &&
	    (err == -EILSEQ || (mrq->sbc && mrq->sbc->error == -EILSEQ) ||
	    (mrq->data && mrq->data->error == -EILSEQ) ||
	    (mrq->stop && mrq->stop->error == -EILSEQ)))
		mmc_retune_needed(host);

	if (err && cmd->retries && mmc_host_is_spi(host)) {
		if (cmd->resp[0] & R1_SPI_ILLEGAL_COMMAND)
			cmd->retries = 0;
	}

	if (host->ongoing_mrq == mrq)
		host->ongoing_mrq = NULL;

	mmc_complete_cmd(mrq);

	trace_mmc_request_done(host, mrq);

	/*
	 * We list various conditions for the command to be considered
	 * properly done:
	 *
	 * - There was no error, OK fine then
	 * - We are not doing some kind of retry
	 * - The card was removed (...so just complete everything no matter
	 *   if there are errors or retries)
	 */
	if (!err || !cmd->retries || mmc_card_removed(host->card)) {
		mmc_should_fail_request(host, mrq);

		if (!host->ongoing_mrq)
			led_trigger_event(host->led, LED_OFF);

		if (mrq->sbc) {
			pr_debug("%s: req done <CMD%u>: %d: %08x %08x %08x %08x\n",
				mmc_hostname(host), mrq->sbc->opcode,
				mrq->sbc->error,
				mrq->sbc->resp[0], mrq->sbc->resp[1],
				mrq->sbc->resp[2], mrq->sbc->resp[3]);
		}

		pr_debug("%s: req done (CMD%u): %d: %08x %08x %08x %08x\n",
			mmc_hostname(host), cmd->opcode, err,
			cmd->resp[0], cmd->resp[1],
			cmd->resp[2], cmd->resp[3]);

		if (mrq->data) {
			pr_debug("%s:     %d bytes transferred: %d\n",
				mmc_hostname(host),
				mrq->data->bytes_xfered, mrq->data->error);
		}

		if (mrq->stop) {
			pr_debug("%s:     (CMD%u): %d: %08x %08x %08x %08x\n",
				mmc_hostname(host), mrq->stop->opcode,
				mrq->stop->error,
				mrq->stop->resp[0], mrq->stop->resp[1],
				mrq->stop->resp[2], mrq->stop->resp[3]);
		}
	}
	/*
	 * Request starter must handle retries - see
	 * mmc_wait_for_req_done().
	 */
	if (mrq->done)
		mrq->done(mrq);
}

EXPORT_SYMBOL(mmc_request_done);


static void __mmc_start_request(struct mmc_host *host, struct mmc_request *mrq)
{
        int err;

        /* Assumes host controller has been runtime resumed by mmc_claim_host */
        err = mmc_retune(host);
        if (err) {
                mrq->cmd->error = err; 
                mmc_request_done(host, mrq);
                return;
        }

        /*
         * For sdio rw commands we must wait for card busy otherwise some
         * sdio devices won't work properly.
         * And bypass I/O abort, reset and bus suspend operations.
         */
        if (sdio_is_io_busy(mrq->cmd->opcode, mrq->cmd->arg) &&
            host->ops->card_busy) {
                int tries = 500; /* Wait aprox 500ms at maximum */
   
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
                 /*
                 * Retry path could come through here without having waiting on
                 * cmd_completion, so ensure it is reinitialised.
                 */
                reinit_completion(&mrq->cmd_completion);
        }

        trace_mmc_request_start(host, mrq);

        if (host->cqe_on)
                host->cqe_ops->cqe_off(host);

        host->ops->request(host, mrq); 
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
        if (!(host->caps2 & MMC_CAP2_NO_SDIO))
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
        return -EIO;
}

int mmc_select_drive_strength(struct mmc_card *card, unsigned int max_dtr,
                              int card_drv_type, int *drv_type)
{
        struct mmc_host *host = card->host;
        int host_drv_type = SD_DRIVER_TYPE_B;

        *drv_type = 0;

        if (!host->ops->select_drive_strength)
                return 0;

        /* Use SD definition of driver strength for hosts */ 
        if (host->caps & MMC_CAP_DRIVER_TYPE_A)
                host_drv_type |= SD_DRIVER_TYPE_A;
    
        if (host->caps & MMC_CAP_DRIVER_TYPE_C)
                host_drv_type |= SD_DRIVER_TYPE_C;
    
        if (host->caps & MMC_CAP_DRIVER_TYPE_D)
                host_drv_type |= SD_DRIVER_TYPE_D;

        /*
         * The drive strength that the hardware can support
         * depends on the board design.  Pass the appropriate
         * information and let the hardware specific code
         * return what is possible given the options
         */
        return host->ops->select_drive_strength(card, max_dtr,
                                                host_drv_type,
                                                card_drv_type,
                                                drv_type);
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
                goto out;
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


/*
 * Select timing parameters for host.
 */
void mmc_set_timing(struct mmc_host *host, unsigned int timing)
{
        host->ios.timing = timing;
        mmc_set_ios(host);
}

/*
 * Assign a mmc bus handler to a host. Only one bus handler may control a
 * host at any given time.
 */     
void mmc_attach_bus(struct mmc_host *host, const struct mmc_bus_ops *ops)
{
        unsigned long flags;

        WARN_ON(!host->claimed);

        spin_lock_irqsave(&host->lock, flags);

        WARN_ON(host->bus_ops);
        WARN_ON(host->bus_refs);

        host->bus_ops = ops;
        host->bus_refs = 1;
        host->bus_dead = 0;

        spin_unlock_irqrestore(&host->lock, flags);
}

void mmc_power_cycle(struct mmc_host *host, u32 ocr)
{
        mmc_power_off(host);
        /* Wait at least 1 ms according to SD spec */
        mmc_delay(1);
        mmc_power_up(host, ocr);
}

/*
 * Mask off any voltages we don't support and select
 * the lowest voltage
 */
u32 mmc_select_voltage(struct mmc_host *host, u32 ocr)
{
        int bit;

        /*
         * Sanity check the voltages that the card claims to
         * support.
         */
        if (ocr & 0x7F) {
                dev_warn(mmc_dev(host),
                "card claims to support voltages below defined range\n");
                ocr &= ~0x7F;
        }

        ocr &= host->ocr_avail;
        if (!ocr) {
                dev_warn(mmc_dev(host), "no support for card's volts\n");
                return 0;
        }

        if (host->caps2 & MMC_CAP2_FULL_PWR_CYCLE) {
                bit = ffs(ocr) - 1;
                ocr &= 3 << bit;
                mmc_power_cycle(host, ocr);
        } else {
                bit = fls(ocr) - 1;
                ocr &= 3 << bit;
                if (bit != host->ios.vdd)
                        dev_warn(mmc_dev(host), "exceeding card's volts\n");
        }

        return ocr;
}

/*
 * This is a helper function, which fetches a runtime pm reference for the
 * card device and also claims the host.
 */
void mmc_get_card(struct mmc_card *card, struct mmc_ctx *ctx)
{
        pm_runtime_get_sync(&card->dev);
        __mmc_claim_host(card->host, ctx, NULL);
}
EXPORT_SYMBOL(mmc_get_card);

/*
 * This is a helper function, which releases the host and drops the runtime
 * pm reference for the card device.
 */
void mmc_put_card(struct mmc_card *card, struct mmc_ctx *ctx)
{
        struct mmc_host *host = card->host;

        WARN_ON(ctx && host->claimer != ctx);

        mmc_release_host(host);
        pm_runtime_mark_last_busy(&card->dev);
        pm_runtime_put_autosuspend(&card->dev);
}
EXPORT_SYMBOL(mmc_put_card);

static int mmc_of_get_func_num(struct device_node *node)
{
        u32 reg;
        int ret;

        ret = of_property_read_u32(node, "reg", &reg);
        if (ret < 0)
                return ret;

        return reg;
}

struct device_node *mmc_of_find_child_device(struct mmc_host *host,
                unsigned func_num)
{
        struct device_node *node;

        if (!host->parent || !host->parent->of_node)
                return NULL;

        for_each_child_of_node(host->parent->of_node, node) {
                if (mmc_of_get_func_num(node) == func_num)
                        return node;
        }

        return NULL;
}

int mmc_host_set_uhs_voltage(struct mmc_host *host)
{
        u32 clock;

        /*
         * During a signal voltage level switch, the clock must be gated
         * for 5 ms according to the SD spec
         */
        clock = host->ios.clock;
        host->ios.clock = 0;
        mmc_set_ios(host);

        if (mmc_set_signal_voltage(host, MMC_SIGNAL_VOLTAGE_180))
                return -EAGAIN;

        /* Keep clock gated for at least 10 ms, though spec only says 5 ms */
        mmc_delay(10);
        host->ios.clock = clock;
        mmc_set_ios(host);

        return 0;
}

int mmc_set_uhs_voltage(struct mmc_host *host, u32 ocr)
{
        struct mmc_command cmd = {};
        int err = 0;

        /*
         * If we cannot switch voltages, return failure so the caller
         * can continue without UHS mode
         */
        if (!host->ops->start_signal_voltage_switch)
                return -EPERM;
        if (!host->ops->card_busy)
                pr_warn("%s: cannot verify signal voltage switch\n",
                        mmc_hostname(host));

        cmd.opcode = SD_SWITCH_VOLTAGE;
        cmd.arg = 0;
        cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;

        err = mmc_wait_for_cmd(host, &cmd, 0);
        if (err)
                return err;

        if (!mmc_host_is_spi(host) && (cmd.resp[0] & R1_ERROR))
                return -EIO;

        /*
         * The card should drive cmd and dat[0:3] low immediately
         * after the response of cmd11, but wait 1 ms to be sure
         */
        mmc_delay(1);
        if (host->ops->card_busy && !host->ops->card_busy(host)) {
                err = -EAGAIN;
                goto power_cycle;
        }

        if (mmc_host_set_uhs_voltage(host)) {
                /*
                 * Voltages may not have been switched, but we've already
                 * sent CMD11, so a power cycle is required anyway
                 */
                err = -EAGAIN;
                goto power_cycle;
        }

        /* Wait for at least 1 ms according to spec */
        mmc_delay(1);

        /*
         * Failure to switch is indicated by the card holding
         * dat[0:3] low
         */
        if (host->ops->card_busy && host->ops->card_busy(host))
                err = -EAGAIN;

power_cycle:
        if (err) {
                pr_debug("%s: Signal voltage switch failed, "
                        "power cycling card\n", mmc_hostname(host));
                mmc_power_cycle(host, ocr);
        }

        return err;
}

void mmc_init_erase(struct mmc_card *card)
{
        unsigned int sz;

        if (is_power_of_2(card->erase_size))
                card->erase_shift = ffs(card->erase_size) - 1;
        else
                card->erase_shift = 0;
        
        /*
         * It is possible to erase an arbitrarily large area of an SD or MMC
         * card.  That is not desirable because it can take a long time
         * (minutes) potentially delaying more important I/O, and also the
         * timeout calculations become increasingly hugely over-estimated.
         * Consequently, 'pref_erase' is defined as a guide to limit erases
         * to that size and alignment.
         *
         * For SD cards that define Allocation Unit size, limit erases to one
         * Allocation Unit at a time.
         * For MMC, have a stab at ai good value and for modern cards it will
         * end up being 4MiB. Note that if the value is too small, it can end
         * up taking longer to erase. Also note, erase_size is already set to
         * High Capacity Erase Size if available when this function is called.
         */
        if (mmc_card_sd(card) && card->ssr.au) {
                card->pref_erase = card->ssr.au;
                card->erase_shift = ffs(card->ssr.au) - 1;
        } else if (card->erase_size) {
                sz = (card->csd.capacity << (card->csd.read_blkbits - 9)) >> 11;
                if (sz < 128)
                        card->pref_erase = 512 * 1024 / 512;
                else if (sz < 512)
                        card->pref_erase = 1024 * 1024 / 512;
                else if (sz < 1024)
                        card->pref_erase = 2 * 1024 * 1024 / 512;
                else    
                        card->pref_erase = 4 * 1024 * 1024 / 512;
                if (card->pref_erase < card->erase_size)
                        card->pref_erase = card->erase_size;
                else {
                        sz = card->pref_erase % card->erase_size;
                        if (sz)
                                card->pref_erase += card->erase_size - sz;
                }
        } else
                card->pref_erase = 0;
}

/*
 * Select appropriate driver type for host.
 */
void mmc_set_driver_type(struct mmc_host *host, unsigned int drv_type)
{
        host->ios.drv_type = drv_type;
        mmc_set_ios(host);
}

int mmc_execute_tuning(struct mmc_card *card)
{                       
        struct mmc_host *host = card->host;
        u32 opcode;
        int err;
        
        if (!host->ops->execute_tuning)
                return 0;

        if (host->cqe_on)
                host->cqe_ops->cqe_off(host);

        if (mmc_card_mmc(card))
                opcode = MMC_SEND_TUNING_BLOCK_HS200;
        else
                opcode = MMC_SEND_TUNING_BLOCK;

        err = host->ops->execute_tuning(host, opcode);
    
        if (err)
                pr_err("%s: tuning execution failed: %d\n",
                        mmc_hostname(host), err);
        else
                mmc_retune_enable(host);

        return err;
}

/**
 *      mmc_align_data_size - pads a transfer size to a more optimal value
 *      @card: the MMC card associated with the data transfer
 *      @sz: original transfer size
 *
 *      Pads the original data size with a number of extra bytes in
 *      order to avoid controller bugs and/or performance hits
 *      (e.g. some controllers revert to PIO for certain sizes).
 *
 *      Returns the improved size, which might be unmodified.
 *
 *      Note that this function is only relevant when issuing a
 *      single scatter gather entry.
 */
unsigned int mmc_align_data_size(struct mmc_card *card, unsigned int sz)
{
        /*
         * FIXME: We don't have a system for the controller to tell
         * the core about its problems yet, so for now we just 32-bit
         * align the size.
         */
        sz = ((sz + 3) / 4) * 4;

        return sz;
}
EXPORT_SYMBOL(mmc_align_data_size);

/*
 * Remove the current bus handler from a host.
 */
void mmc_detach_bus(struct mmc_host *host)
{               
        unsigned long flags;

        WARN_ON(!host->claimed);
        WARN_ON(!host->bus_ops);

        spin_lock_irqsave(&host->lock, flags);

        host->bus_dead = 1;

        spin_unlock_irqrestore(&host->lock, flags);

        mmc_bus_put(host);
}

static void _mmc_detect_change(struct mmc_host *host, unsigned long delay,
                                bool cd_irq)
{
        /*
         * If the device is configured as wakeup, we prevent a new sleep for
         * 5 s to give provision for user space to consume the event.
         */
        if (cd_irq && !(host->caps & MMC_CAP_NEEDS_POLL) &&
                device_can_wakeup(mmc_dev(host)))
                pm_wakeup_event(mmc_dev(host), 5000);

        host->detect_change = 1;
        mmc_schedule_delayed_work(&host->detect, delay);
}

/**
 *      mmc_detect_change - process change of state on a MMC socket
 *      @host: host which changed state.
 *      @delay: optional delay to wait before detection (jiffies)
 *
 *      MMC drivers should call this when they detect a card has been
 *      inserted or removed. The MMC layer will confirm that any
 *      present card is still functional, and initialize any newly
 *      inserted.
 */
void mmc_detect_change(struct mmc_host *host, unsigned long delay)
{
        _mmc_detect_change(host, delay, true);
}
EXPORT_SYMBOL(mmc_detect_change);


int _mmc_detect_card_removed(struct mmc_host *host)
{
        int ret;

        if (!host->card || mmc_card_removed(host->card))
                return 1;

        ret = host->bus_ops->alive(host);

        /*
         * Card detect status and alive check may be out of sync if card is
         * removed slowly, when card detect switch changes while card/slot
         * pads are still contacted in hardware (refer to "SD Card Mechanical
         * Addendum, Appendix C: Card Detection Switch"). So reschedule a
         * detect work 200ms later for this case.
         */
        if (!ret && host->ops->get_cd && !host->ops->get_cd(host)) {
                mmc_detect_change(host, msecs_to_jiffies(200));
                pr_debug("%s: card removed too slowly\n", mmc_hostname(host));
        }

        if (ret) { 
                mmc_card_set_removed(host->card);
                pr_debug("%s: card remove detected\n", mmc_hostname(host));
        }

        return ret;
}

/*
 * Change the bus mode (open drain/push-pull) of a host.
 */
void mmc_set_bus_mode(struct mmc_host *host, unsigned int mode)
{
        host->ios.bus_mode = mode;
        mmc_set_ios(host);
}


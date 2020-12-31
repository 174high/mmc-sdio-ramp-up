/*
 *  linux/drivers/mmc/core/mmc_ops.h
 *
 *  Copyright 2006-2007 Pierre Ossman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include <linux/slab.h>
#include <linux/export.h>
#include <linux/types.h>
#include <linux/scatterlist.h>

#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>

#include "core.h"
#include "host.h"
#include "mmc_ops.h"

#define MMC_OPS_TIMEOUT_MS      (10 * 60 * 1000) /* 10 minute timeout */



int mmc_go_idle(struct mmc_host *host)
{
        int err;
        struct mmc_command cmd = {};

        /*
         * Non-SPI hosts need to prevent chipselect going active during
         * GO_IDLE; that would put chips into SPI mode.  Remind them of
         * that in case of hardware that won't pull up DAT3/nCS otherwise.
         *
         * SPI hosts ignore ios.chip_select; it's managed according to
         * rules that must accommodate non-MMC slaves which this layer
         * won't even know about.
         */
        if (!mmc_host_is_spi(host)) {
                mmc_set_chip_select(host, MMC_CS_HIGH);
                mmc_delay(1);
        }

        cmd.opcode = MMC_GO_IDLE_STATE;
        cmd.arg = 0;
        cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_NONE | MMC_CMD_BC;

        err = mmc_wait_for_cmd(host, &cmd, 0);

        mmc_delay(1);

        if (!mmc_host_is_spi(host)) {
                mmc_set_chip_select(host, MMC_CS_DONTCARE);
                mmc_delay(1);
        }

        host->use_spi_crc = 0;

        return err;
}

int __mmc_send_status(struct mmc_card *card, u32 *status, unsigned int retries)
{
        int err;
        struct mmc_command cmd = {};

        cmd.opcode = MMC_SEND_STATUS;
        if (!mmc_host_is_spi(card->host))
                cmd.arg = card->rca << 16;
        cmd.flags = MMC_RSP_SPI_R2 | MMC_RSP_R1 | MMC_CMD_AC;

        err = mmc_wait_for_cmd(card->host, &cmd, retries);
        if (err)
                return err;

        /* NOTE: callers are required to understand the difference
         * between "native" and SPI format status words!
         */
        if (status)
                *status = cmd.resp[0];

        return 0;
}
EXPORT_SYMBOL_GPL(__mmc_send_status);

static int mmc_switch_status_error(struct mmc_host *host, u32 status)
{
        if (mmc_host_is_spi(host)) {
                if (status & R1_SPI_ILLEGAL_COMMAND)
                        return -EBADMSG;
        } else {
                if (R1_STATUS(status))
                        pr_warn("%s: unexpected status %#x after switch\n",
                                mmc_hostname(host), status);
                if (status & R1_SWITCH_ERROR)
                        return -EBADMSG;
        }
        return 0;
}

/* Caller must hold re-tuning */
int __mmc_switch_status(struct mmc_card *card, bool crc_err_fatal)
{
        u32 status;
        int err;

        err = mmc_send_status(card, &status);
        if (!crc_err_fatal && err == -EILSEQ)
                return 0;
        if (err)
                return err;

        return mmc_switch_status_error(card->host, status);
}

int mmc_switch_status(struct mmc_card *card)
{
        return __mmc_switch_status(card, true);
}

int mmc_send_status(struct mmc_card *card, u32 *status)
{               
        return __mmc_send_status(card, status, MMC_CMD_RETRIES);
}
EXPORT_SYMBOL_GPL(mmc_send_status);


static int mmc_poll_for_busy(struct mmc_card *card, unsigned int timeout_ms,
                        bool send_status, bool retry_crc_err)
{
        struct mmc_host *host = card->host;
        int err;
        unsigned long timeout;
        u32 status = 0;
        bool expired = false;
        bool busy = false;

        /* We have an unspecified cmd timeout, use the fallback value. */
        if (!timeout_ms)
                timeout_ms = MMC_OPS_TIMEOUT_MS;

        /*
         * In cases when not allowed to poll by using CMD13 or because we aren't
         * capable of polling by using ->card_busy(), then rely on waiting the
         * stated timeout to be sufficient.
         */
        if (!send_status && !host->ops->card_busy) {
                mmc_delay(timeout_ms);
                return 0;
        }

        timeout = jiffies + msecs_to_jiffies(timeout_ms) + 1;
        do {
                /*
                 * Due to the possibility of being preempted while polling,
                 * check the expiration time first.
                 */
                expired = time_after(jiffies, timeout);

                if (host->ops->card_busy) {
                        busy = host->ops->card_busy(host);
                } else {
                        err = mmc_send_status(card, &status);
                        if (retry_crc_err && err == -EILSEQ) {
                                busy = true;
                        } else if (err) {
                                return err;
                        } else {
                                err = mmc_switch_status_error(host, status);
                                if (err)
                                        return err;
                                busy = R1_CURRENT_STATE(status) == R1_STATE_PRG;
                        }
                }
    
                /* Timeout if the device still remains busy. */
                if (expired && busy) {
                        pr_err("%s: Card stuck being busy! %s\n",
                                mmc_hostname(host), __func__);
                        return -ETIMEDOUT;
                }  
        } while (busy);
    
        return 0;
}



/**
 *      __mmc_switch - modify EXT_CSD register
 *      @card: the MMC card associated with the data transfer
 *      @set: cmd set values
 *      @index: EXT_CSD register index
 *      @value: value to program into EXT_CSD register
 *      @timeout_ms: timeout (ms) for operation performed by register write,
 *                   timeout of zero implies maximum possible timeout
 *      @timing: new timing to change to
 *      @use_busy_signal: use the busy signal as response type
 *      @send_status: send status cmd to poll for busy
 *      @retry_crc_err: retry when CRC errors when polling with CMD13 for busy
 *
 *      Modifies the EXT_CSD register for selected card.
 */
int __mmc_switch(struct mmc_card *card, u8 set, u8 index, u8 value,
                unsigned int timeout_ms, unsigned char timing,
                bool use_busy_signal, bool send_status, bool retry_crc_err)
{
        struct mmc_host *host = card->host;
        int err;
        struct mmc_command cmd = {};
        bool use_r1b_resp = use_busy_signal;
        unsigned char old_timing = host->ios.timing;

        mmc_retune_hold(host);

        /*
         * If the cmd timeout and the max_busy_timeout of the host are both
         * specified, let's validate them. A failure means we need to prevent
         * the host from doing hw busy detection, which is done by converting
         * to a R1 response instead of a R1B.
         */
        if (timeout_ms && host->max_busy_timeout &&
                (timeout_ms > host->max_busy_timeout))
                use_r1b_resp = false;

        cmd.opcode = MMC_SWITCH;
        cmd.arg = (MMC_SWITCH_MODE_WRITE_BYTE << 24) |
                  (index << 16) |
                  (value << 8) |
                  set;
        cmd.flags = MMC_CMD_AC;
        if (use_r1b_resp) {
                cmd.flags |= MMC_RSP_SPI_R1B | MMC_RSP_R1B;
                 /*
                 * A busy_timeout of zero means the host can decide to use
                 * whatever value it finds suitable.
                 */
                cmd.busy_timeout = timeout_ms;
        } else {
                cmd.flags |= MMC_RSP_SPI_R1 | MMC_RSP_R1;
        }

        if (index == EXT_CSD_SANITIZE_START)
                cmd.sanitize_busy = true;

        err = mmc_wait_for_cmd(host, &cmd, MMC_CMD_RETRIES);
        if (err)
                goto out;
   
        /* No need to check card status in case of unblocking command */
        if (!use_busy_signal)
                goto out;

        /*If SPI or used HW busy detection above, then we don't need to poll. */
        if (((host->caps & MMC_CAP_WAIT_WHILE_BUSY) && use_r1b_resp) ||
                mmc_host_is_spi(host))
                goto out_tim;

        /* Let's try to poll to find out when the command is completed. */
        err = mmc_poll_for_busy(card, timeout_ms, send_status, retry_crc_err);
        if (err)
                goto out;

out_tim:
        /* Switch to new timing before check switch status. */
        if (timing)
                mmc_set_timing(host, timing);

        /*
         * WORKAROUND: for Sandisk eMMC cards, it might need certain delay
         * before sending CMD13 after CMD6
         */
        mdelay(1);

        if (send_status) {
                err = mmc_switch_status(card);
                if (err && timing)
                        mmc_set_timing(host, old_timing);
        }

out:
        mmc_retune_release(host);

        return err;
}







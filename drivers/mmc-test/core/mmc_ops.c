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

int mmc_spi_read_ocr(struct mmc_host *host, int highcap, u32 *ocrp)
{
        struct mmc_command cmd = {};
        int err;

        cmd.opcode = MMC_SPI_READ_OCR;
        cmd.arg = highcap ? (1 << 30) : 0;
        cmd.flags = MMC_RSP_SPI_R3;

        err = mmc_wait_for_cmd(host, &cmd, 0);

        *ocrp = cmd.resp[1];
        return err;
}

int mmc_spi_set_crc(struct mmc_host *host, int use_crc)
{
        struct mmc_command cmd = {};
        int err;

        cmd.opcode = MMC_SPI_CRC_ON_OFF;
        cmd.flags = MMC_RSP_SPI_R1;
        cmd.arg = use_crc;

        err = mmc_wait_for_cmd(host, &cmd, 0);
        if (!err)
                host->use_spi_crc = use_crc;
        return err;
}

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

static int mmc_send_hpi_cmd(struct mmc_card *card, u32 *status)
{
        struct mmc_command cmd = {};
        unsigned int opcode;
        int err;

        if (!card->ext_csd.hpi) {
                pr_warn("%s: Card didn't support HPI command\n",
                        mmc_hostname(card->host));
                return -EINVAL;
        }

        opcode = card->ext_csd.hpi_cmd;
        if (opcode == MMC_STOP_TRANSMISSION)
                cmd.flags = MMC_RSP_R1B | MMC_CMD_AC;
        else if (opcode == MMC_SEND_STATUS)
                cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;
    
        cmd.opcode = opcode;
        cmd.arg = card->rca << 16 | 1;
    
        err = mmc_wait_for_cmd(card->host, &cmd, 0);
        if (err) {
                pr_warn("%s: error %d interrupting operation. "
                        "HPI command response %#x\n", mmc_hostname(card->host),
                        err, cmd.resp[0]);
                return err;
        }
        if (status)
                *status = cmd.resp[0];

        return 0;
}

/**
 *      mmc_interrupt_hpi - Issue for High priority Interrupt
 *      @card: the MMC card associated with the HPI transfer
 *
 *      Issued High Priority Interrupt, and check for card status
 *      until out-of prg-state.
 */
int mmc_interrupt_hpi(struct mmc_card *card)
{
        int err;
        u32 status;
        unsigned long prg_wait;

        if (!card->ext_csd.hpi_en) {
                pr_info("%s: HPI enable bit unset\n", mmc_hostname(card->host));
                return 1;
        }

        err = mmc_send_status(card, &status);
        if (err) {
                pr_err("%s: Get card status fail\n", mmc_hostname(card->host));
                goto out;
        }

        switch (R1_CURRENT_STATE(status)) {
        case R1_STATE_IDLE:
        case R1_STATE_READY:
        case R1_STATE_STBY:
        case R1_STATE_TRAN:
                /*
                 * In idle and transfer states, HPI is not needed and the caller
                 * can issue the next intended command immediately
                 */
                goto out;
        case R1_STATE_PRG:
                break;
        default:
                /* In all other states, it's illegal to issue HPI */
                pr_debug("%s: HPI cannot be sent. Card state=%d\n",
                        mmc_hostname(card->host), R1_CURRENT_STATE(status));
                err = -EINVAL;
                goto out;
        }

        err = mmc_send_hpi_cmd(card, &status);
        if (err)
                goto out;

        prg_wait = jiffies + msecs_to_jiffies(card->ext_csd.out_of_int_time);
        do {
                err = mmc_send_status(card, &status);

                if (!err && R1_CURRENT_STATE(status) == R1_STATE_TRAN)
                        break;
                if (time_after(jiffies, prg_wait))
                        err = -ETIMEDOUT;
        } while (!err);

out:
        return err;
}

/*
 * NOTE: void *buf, caller for the buf is required to use DMA-capable
 * buffer or on-stack buffer (with some overhead in callee).
 */
static int
mmc_send_cxd_data(struct mmc_card *card, struct mmc_host *host,
                u32 opcode, void *buf, unsigned len)
{
        struct mmc_request mrq = {};
        struct mmc_command cmd = {};
        struct mmc_data data = {};
        struct scatterlist sg;

        mrq.cmd = &cmd;
        mrq.data = &data;

        cmd.opcode = opcode;
        cmd.arg = 0;

        /* NOTE HACK:  the MMC_RSP_SPI_R1 is always correct here, but we
         * rely on callers to never use this with "native" calls for reading
         * CSD or CID.  Native versions of those commands use the R2 type,
         * not R1 plus a data block.
         */
        cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;

        data.blksz = len;
        data.blocks = 1;
        data.flags = MMC_DATA_READ;
        data.sg = &sg;
        data.sg_len = 1;

        sg_init_one(&sg, buf, len);

        if (opcode == MMC_SEND_CSD || opcode == MMC_SEND_CID) {
                /*
                 * The spec states that CSR and CID accesses have a timeout
                 * of 64 clock cycles.
                 */
                data.timeout_ns = 0;
                data.timeout_clks = 64;
        } else
                mmc_set_data_timeout(&data, card);

        mmc_wait_for_req(host, &mrq);

        if (cmd.error) 
                return cmd.error;
        if (data.error)
                return data.error;

        return 0;
}


static int mmc_spi_send_cid(struct mmc_host *host, u32 *cid)
{
        int ret, i;
        __be32 *cid_tmp;

        cid_tmp = kzalloc(16, GFP_KERNEL);
        if (!cid_tmp)
                return -ENOMEM;

        ret = mmc_send_cxd_data(NULL, host, MMC_SEND_CID, cid_tmp, 16);
        if (ret)
                goto err;

        for (i = 0; i < 4; i++)
                cid[i] = be32_to_cpu(cid_tmp[i]);

err:
        kfree(cid_tmp);
        return ret; 
}

static int
mmc_send_cxd_native(struct mmc_host *host, u32 arg, u32 *cxd, int opcode)
{
        int err;
        struct mmc_command cmd = {};

        cmd.opcode = opcode;
        cmd.arg = arg;
        cmd.flags = MMC_RSP_R2 | MMC_CMD_AC;

        err = mmc_wait_for_cmd(host, &cmd, MMC_CMD_RETRIES);
        if (err)
                return err;

        memcpy(cxd, cmd.resp, sizeof(u32) * 4);

        return 0;
}


int mmc_send_cid(struct mmc_host *host, u32 *cid)
{
        if (mmc_host_is_spi(host))
                return mmc_spi_send_cid(host, cid);

        return mmc_send_cxd_native(host, 0, cid, MMC_ALL_SEND_CID);
}

static int _mmc_select_card(struct mmc_host *host, struct mmc_card *card)
{
        struct mmc_command cmd = {};

        cmd.opcode = MMC_SELECT_CARD;

        if (card) {
                cmd.arg = card->rca << 16;
                cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;
        } else {
                cmd.arg = 0;
                cmd.flags = MMC_RSP_NONE | MMC_CMD_AC;
        }

        return mmc_wait_for_cmd(host, &cmd, MMC_CMD_RETRIES);
}

int mmc_select_card(struct mmc_card *card)
{

        return _mmc_select_card(card->host, card);
}

int mmc_deselect_cards(struct mmc_host *host)
{
        return _mmc_select_card(host, NULL);
}


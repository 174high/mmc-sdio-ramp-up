#include <linux/busfreq-imx.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_data/mmc-esdhc-imx.h>
#include <linux/pm_runtime.h>
#include "sdhci-pltfm.h"
#include "sdhci-esdhc.h"
//#include "cqhci.h"



#define BIT(nr)                 (1 << (nr))

/*
 * The CMDTYPE of the CMD register (offset 0xE) should be set to
 * "11" when the STOP CMD12 is issued on imx53 to abort one
 * open ended multi-blk IO. Otherwise the TC INT wouldn't
 * be generated.
 * In exact block transfer, the controller doesn't complete the
 * operations automatically as required at the end of the
 * transfer and remains on hold if the abort command is not sent.
 * As a result, the TC flag is not asserted and SW received timeout
 * exception. Bit1 of Vendor Spec register is used to fix it.
 */
#define ESDHC_FLAG_MULTIBLK_NO_INT      BIT(1)
/*
 * The flag tells that the ESDHC controller is an USDHC block that is
 * integrated on the i.MX6 series.
 */
#define ESDHC_FLAG_USDHC                BIT(3)
/* The IP supports manual tuning process */
#define ESDHC_FLAG_MAN_TUNING           BIT(4)
/* The IP supports standard tuning process */
#define ESDHC_FLAG_STD_TUNING           BIT(5)
/* The IP has SDHCI_CAPABILITIES_1 register */
#define ESDHC_FLAG_HAVE_CAP1            BIT(6)
/*
 * The IP has erratum ERR004536
 * uSDHC: ADMA Length Mismatch Error occurs if the AHB read access is slow,
 * when reading data from the card
 * This flag is also set for i.MX25 and i.MX35 in order to get
 * SDHCI_QUIRK_BROKEN_ADMA, but for different reasons (ADMA capability bits).
 */
#define ESDHC_FLAG_ERR004536            BIT(7)
/* The IP supports HS200 mode */
#define ESDHC_FLAG_HS200                BIT(8)
/* The IP supports HS400 mode */
#define ESDHC_FLAG_HS400                BIT(9)

/* The IP state got lost in low power mode */
#define ESDHC_FLAG_STATE_LOST_IN_LPMODE BIT(10)

/* The IP has errata ERR010450
 * uSDHC: Due to the I/O timing limit, for SDR mode, SD card clock can't
 * exceed 150MHz, for DDR mode, SD card clock can't exceed 45MHz.
 */
#define ESDHC_FLAG_ERR010450            BIT(11)
/* need request bus freq during low power */
#define ESDHC_FLAG_BUSFREQ              BIT(12)
/* need request pmqos during low power */
#define ESDHC_FLAG_PMQOS                BIT(13)
/* The IP supports HS400ES mode */
#define ESDHC_FLAG_HS400_ES             BIT(14)
/* The IP lost clock rate in PM_RUNTIME */
#define ESDHC_FLAG_CLK_RATE_LOST_IN_PM_RUNTIME  BIT(15)
/* The IP has Host Controller Interface for Command Queuing */
#define ESDHC_FLAG_CQHCI               BIT(16)

struct esdhc_soc_data {
        unsigned int flags;
};

static struct esdhc_soc_data esdhc_imx25_data = {
        .flags = ESDHC_FLAG_ERR004536,
};

static struct esdhc_soc_data esdhc_imx35_data = {
        .flags = ESDHC_FLAG_ERR004536,
};

static struct esdhc_soc_data esdhc_imx51_data = {
        .flags = 0,
};

static struct esdhc_soc_data esdhc_imx53_data = {
        .flags = ESDHC_FLAG_MULTIBLK_NO_INT,
};

static struct esdhc_soc_data usdhc_imx6q_data = {
        .flags = ESDHC_FLAG_USDHC | ESDHC_FLAG_MAN_TUNING,
};

static struct esdhc_soc_data usdhc_imx6sl_data = {
        .flags = ESDHC_FLAG_USDHC | ESDHC_FLAG_STD_TUNING
                        | ESDHC_FLAG_HAVE_CAP1 | ESDHC_FLAG_ERR004536
                        | ESDHC_FLAG_HS200 | ESDHC_FLAG_BUSFREQ,
};

static struct esdhc_soc_data usdhc_imx6sx_data = {
        .flags = ESDHC_FLAG_USDHC | ESDHC_FLAG_STD_TUNING
                        | ESDHC_FLAG_HAVE_CAP1 | ESDHC_FLAG_HS200
                        | ESDHC_FLAG_STATE_LOST_IN_LPMODE
                        | ESDHC_FLAG_BUSFREQ,
};

static struct esdhc_soc_data usdhc_imx6ull_data = {
        .flags = ESDHC_FLAG_USDHC | ESDHC_FLAG_STD_TUNING
                        | ESDHC_FLAG_HAVE_CAP1 | ESDHC_FLAG_HS200
                        | ESDHC_FLAG_STATE_LOST_IN_LPMODE
                        | ESDHC_FLAG_ERR010450
                        | ESDHC_FLAG_BUSFREQ,
};

static struct esdhc_soc_data usdhc_imx7d_data = {
        .flags = ESDHC_FLAG_USDHC | ESDHC_FLAG_STD_TUNING
                        | ESDHC_FLAG_HAVE_CAP1 | ESDHC_FLAG_HS200
                        | ESDHC_FLAG_HS400 | ESDHC_FLAG_STATE_LOST_IN_LPMODE
                        | ESDHC_FLAG_BUSFREQ,
};

static struct esdhc_soc_data usdhc_imx7ulp_data = {
        .flags = ESDHC_FLAG_USDHC | ESDHC_FLAG_STD_TUNING
                        | ESDHC_FLAG_HAVE_CAP1 | ESDHC_FLAG_HS200
                        | ESDHC_FLAG_HS400
                        | ESDHC_FLAG_STATE_LOST_IN_LPMODE
                        | ESDHC_FLAG_PMQOS,
};

static struct esdhc_soc_data usdhc_imx8qxp_data = {
        .flags = ESDHC_FLAG_USDHC | ESDHC_FLAG_STD_TUNING
                        | ESDHC_FLAG_HAVE_CAP1 | ESDHC_FLAG_HS200
                        | ESDHC_FLAG_HS400 | ESDHC_FLAG_HS400_ES
                        | ESDHC_FLAG_CQHCI
                        | ESDHC_FLAG_STATE_LOST_IN_LPMODE
                        | ESDHC_FLAG_CLK_RATE_LOST_IN_PM_RUNTIME,
};

static struct esdhc_soc_data usdhc_imx8mm_data = {
        .flags = ESDHC_FLAG_USDHC | ESDHC_FLAG_STD_TUNING
                        | ESDHC_FLAG_HAVE_CAP1 | ESDHC_FLAG_HS200
                        | ESDHC_FLAG_HS400 | ESDHC_FLAG_HS400_ES
                        | ESDHC_FLAG_CQHCI
                        | ESDHC_FLAG_STATE_LOST_IN_LPMODE
                        | ESDHC_FLAG_BUSFREQ,
};

struct pltfm_imx_data {
        u32 scratchpad;
        struct pinctrl *pinctrl;
        struct pinctrl_state *pins_default;
        struct pinctrl_state *pins_100mhz;
        struct pinctrl_state *pins_200mhz;
        const struct esdhc_soc_data *socdata;
        struct esdhc_platform_data boarddata;
        struct clk *clk_ipg;
        struct clk *clk_ahb;
        struct clk *clk_per; 
        unsigned int actual_clock;
        enum {
                NO_CMD_PENDING,      /* no multiblock command pending */
                MULTIBLK_IN_PROCESS, /* exact multiblock cmd in process */
                WAIT_FOR_INT,        /* sent CMD12, waiting for response INT */
        } multiblock_status;
        u32 is_ddr;
        struct pm_qos_request pm_qos_req;
};

static const struct platform_device_id imx_esdhc_devtype[] = {
        {
                .name = "sdhci-esdhc-imx25",
                .driver_data = (kernel_ulong_t) &esdhc_imx25_data,
        }, {
                .name = "sdhci-esdhc-imx35",
                .driver_data = (kernel_ulong_t) &esdhc_imx35_data,
        }, {
                .name = "sdhci-esdhc-imx51",
                .driver_data = (kernel_ulong_t) &esdhc_imx51_data,
        }, {
                /* sentinel */
        }
};
MODULE_DEVICE_TABLE(platform, imx_esdhc_devtype);


static const struct of_device_id imx_esdhc_dt_ids[] = {
        { .compatible = "fsl,imx25-esdhc", .data = &esdhc_imx25_data, },
        { .compatible = "fsl,imx35-esdhc", .data = &esdhc_imx35_data, },
        { .compatible = "fsl,imx51-esdhc", .data = &esdhc_imx51_data, },
        { .compatible = "fsl,imx53-esdhc", .data = &esdhc_imx53_data, },
        { .compatible = "fsl,imx6sx-usdhc", .data = &usdhc_imx6sx_data, },
        { .compatible = "fsl,imx6sl-usdhc", .data = &usdhc_imx6sl_data, },
        { .compatible = "fsl,imx6q-usdhc", .data = &usdhc_imx6q_data, },
        { .compatible = "fsl,imx6ull-usdhc-test", .data = &usdhc_imx6ull_data, },
        { .compatible = "fsl,imx7d-usdhc", .data = &usdhc_imx7d_data, },
        { .compatible = "fsl,imx7ulp-usdhc", .data = &usdhc_imx7ulp_data, },
        { .compatible = "fsl,imx8qxp-usdhc", .data = &usdhc_imx8qxp_data, },
        { .compatible = "fsl,imx8mm-usdhc", .data = &usdhc_imx8mm_data, },
        { /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, imx_esdhc_dt_ids);

/*
static const struct dev_pm_ops sdhci_esdhc_pmops = {
        SET_SYSTEM_SLEEP_PM_OPS(sdhci_esdhc_suspend, sdhci_esdhc_resume)
        SET_RUNTIME_PM_OPS(sdhci_esdhc_runtime_suspend,
                                sdhci_esdhc_runtime_resume, NULL)
};
*/

static const struct sdhci_pltfm_data sdhci_esdhc_imx_pdata = {

	.quirks = ESDHC_DEFAULT_QUIRKS,	
//        .quirks = ESDHC_DEFAULT_QUIRKS | SDHCI_QUIRK_NO_HISPD_BIT
//                        | SDHCI_QUIRK_NO_ENDATTR_IN_NOPDESC
//                        | SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC
//                        | SDHCI_QUIRK_BROKEN_CARD_DETECTION,
//        .ops = &sdhci_esdhc_ops,
};

static int sdhci_esdhc_imx_probe(struct platform_device *pdev)
{
	const struct of_device_id *of_id =
                        of_match_device(imx_esdhc_dt_ids, &pdev->dev);
        struct sdhci_pltfm_host *pltfm_host;
        struct sdhci_host *host;
        struct cqhci_host *cq_host;
	int err;
 	struct pltfm_imx_data *imx_data;
        u32 status;

	pr_info("sdhci_esdhc_imx_probe shijonn \r\n");

	host = sdhci_pltfm_init(pdev, &sdhci_esdhc_imx_pdata,
                                sizeof(*imx_data));	


	return err;
}

static int sdhci_esdhc_imx_remove(struct platform_device *pdev)
{


	return 0 ;
}

static struct platform_driver sdhci_esdhc_imx_driver = {
        .driver         = {
                .name   = "sdhci-esdhc-imx-test",
                .of_match_table = imx_esdhc_dt_ids,
//                .pm     = &sdhci_esdhc_pmops,
        },
         .id_table       = imx_esdhc_devtype,
         .probe          = sdhci_esdhc_imx_probe,
         .remove         = sdhci_esdhc_imx_remove,
};

module_platform_driver(sdhci_esdhc_imx_driver);

MODULE_DESCRIPTION("SDHCI driver for Freescale i.MX eSDHC");
MODULE_AUTHOR("Wolfram Sang <kernel@pengutronix.de>");
MODULE_LICENSE("GPL v2");




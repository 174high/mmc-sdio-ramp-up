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

#define ESDHC_SYS_CTRL_DTOCV_MASK       0x0f
#define ESDHC_CTRL_D3CD                 0x08
#define ESDHC_BURST_LEN_EN_INCR         (1 << 27)
/* VENDOR SPEC register */
#define ESDHC_VENDOR_SPEC               0xc0
#define  ESDHC_VENDOR_SPEC_SDIO_QUIRK   (1 << 1)
#define  ESDHC_VENDOR_SPEC_VSELECT      (1 << 1)
#define  ESDHC_VENDOR_SPEC_FRC_SDCLK_ON (1 << 8)
#define ESDHC_WTMK_LVL                  0x44
#define  ESDHC_WTMK_DEFAULT_VAL         0x10401040
#define  ESDHC_WTMK_LVL_RD_WML_MASK     0x000000FF
#define  ESDHC_WTMK_LVL_RD_WML_SHIFT    0
#define  ESDHC_WTMK_LVL_WR_WML_MASK     0x00FF0000
#define  ESDHC_WTMK_LVL_WR_WML_SHIFT    16
#define  ESDHC_WTMK_LVL_WML_VAL_DEF     64
#define  ESDHC_WTMK_LVL_WML_VAL_MAX     128
#define ESDHC_MIX_CTRL                  0x48
#define  ESDHC_MIX_CTRL_DDREN           (1 << 3)
#define  ESDHC_MIX_CTRL_AC23EN          (1 << 7)
#define  ESDHC_MIX_CTRL_EXE_TUNE        (1 << 22)
#define  ESDHC_MIX_CTRL_SMPCLK_SEL      (1 << 23)
#define  ESDHC_MIX_CTRL_AUTO_TUNE_EN    (1 << 24)
#define  ESDHC_MIX_CTRL_FBCLK_SEL       (1 << 25)
#define  ESDHC_MIX_CTRL_HS400_EN        (1 << 26)
#define  ESDHC_MIX_CTRL_HS400_ES_EN     (1 << 27)
/* Bits 3 and 6 are not SDHCI standard definitions */
#define  ESDHC_MIX_CTRL_SDHCI_MASK      0xb7
/* Tuning bits */
#define  ESDHC_MIX_CTRL_TUNING_MASK     0x03c00000

/* dll control register */
#define ESDHC_DLL_CTRL                  0x60
#define ESDHC_DLL_OVERRIDE_VAL_SHIFT    9
#define ESDHC_DLL_OVERRIDE_EN_SHIFT     8

/* tune control register */
#define ESDHC_TUNE_CTRL_STATUS          0x68
#define  ESDHC_TUNE_CTRL_STEP           1
#define  ESDHC_TUNE_CTRL_MIN            0
#define  ESDHC_TUNE_CTRL_MAX            ((1 << 7) - 1)

/* strobe dll register */
#define ESDHC_STROBE_DLL_CTRL           0x70
#define ESDHC_STROBE_DLL_CTRL_ENABLE    (1 << 0)
#define ESDHC_STROBE_DLL_CTRL_RESET     (1 << 1)
#define ESDHC_STROBE_DLL_CTRL_SLV_DLY_TARGET_DEFAULT    0x7
#define ESDHC_STROBE_DLL_CTRL_SLV_DLY_TARGET_SHIFT      3
#define ESDHC_STROBE_DLL_CTRL_SLV_UPDATE_INT_DEFAULT    (4 << 20)

#define ESDHC_STROBE_DLL_STATUS         0x74
#define ESDHC_STROBE_DLL_STS_REF_LOCK   (1 << 1)
#define ESDHC_STROBE_DLL_STS_SLV_LOCK   0x1

#define ESDHC_VEND_SPEC2                0xc8
#define ESDHC_VEND_SPEC2_EN_BUSY_IRQ    (1 << 8)

#define ESDHC_TUNING_CTRL               0xcc
#define ESDHC_STD_TUNING_EN             (1 << 24)
/* NOTE: the minimum valid tuning start tap for mx6sl is 1 */
#define ESDHC_TUNING_START_TAP_DEFAULT  0x1
#define ESDHC_TUNING_START_TAP_MASK     0xff
#define ESDHC_TUNING_STEP_MASK          0x00070000
#define ESDHC_TUNING_STEP_SHIFT         16

/* pinctrl state */
#define ESDHC_PINCTRL_STATE_100MHZ      "state_100mhz"
#define ESDHC_PINCTRL_STATE_200MHZ      "state_200mhz"

/*
 * Our interpretation of the SDHCI_HOST_CONTROL register
 */
#define ESDHC_CTRL_4BITBUS              (0x1 << 1)
#define ESDHC_CTRL_8BITBUS              (0x2 << 1)
#define ESDHC_CTRL_BUSWIDTH_MASK        (0x3 << 1)

/*
 * There is an INT DMA ERR mismatch between eSDHC and STD SDHC SPEC:
 * Bit25 is used in STD SPEC, and is reserved in fsl eSDHC design,
 * but bit28 is used as the INT DMA ERR in fsl eSDHC design.
 * Define this macro DMA error INT for fsl eSDHC
 */
#define ESDHC_INT_VENDOR_SPEC_DMA_ERR   (1 << 28)

/* the address offset of CQHCI */
#define ESDHC_CQHCI_ADDR_OFFSET         0x100

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

static inline int esdhc_is_usdhc(struct pltfm_imx_data *data)
{
        return !!(data->socdata->flags & ESDHC_FLAG_USDHC);
}


/*
static const struct dev_pm_ops sdhci_esdhc_pmops = {
        SET_SYSTEM_SLEEP_PM_OPS(sdhci_esdhc_suspend, sdhci_esdhc_resume)
        SET_RUNTIME_PM_OPS(sdhci_esdhc_runtime_suspend,
                                sdhci_esdhc_runtime_resume, NULL)
};
*/

static const struct sdhci_pltfm_data sdhci_esdhc_imx_pdata = {

        .quirks = ESDHC_DEFAULT_QUIRKS | SDHCI_QUIRK_NO_HISPD_BIT
                        | SDHCI_QUIRK_NO_ENDATTR_IN_NOPDESC
                        | SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC
                        | SDHCI_QUIRK_BROKEN_CARD_DETECTION,
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
        if (IS_ERR(host))
                return PTR_ERR(host);

        pltfm_host = sdhci_priv(host);

        imx_data = sdhci_pltfm_priv(pltfm_host);	

	       imx_data->socdata = of_id ? of_id->data : (struct esdhc_soc_data *)
                                                  pdev->id_entry->driver_data;

        imx_data->clk_ipg = devm_clk_get(&pdev->dev, "ipg");
        if (IS_ERR(imx_data->clk_ipg)) {
                err = PTR_ERR(imx_data->clk_ipg);
                goto free_sdhci;
        }

        imx_data->clk_ahb = devm_clk_get(&pdev->dev, "ahb");
        if (IS_ERR(imx_data->clk_ahb)) {
                err = PTR_ERR(imx_data->clk_ahb);
                goto free_sdhci;
        }

        imx_data->clk_per = devm_clk_get(&pdev->dev, "per");
        if (IS_ERR(imx_data->clk_per)) {
                err = PTR_ERR(imx_data->clk_per);
                goto free_sdhci;
        }

        pltfm_host->clk = imx_data->clk_per;
        pltfm_host->clock = clk_get_rate(pltfm_host->clk);

	if (imx_data->socdata->flags & ESDHC_FLAG_BUSFREQ)
                request_bus_freq(BUS_FREQ_HIGH);

        if (imx_data->socdata->flags & ESDHC_FLAG_PMQOS)
                pm_qos_add_request(&imx_data->pm_qos_req,
                        PM_QOS_CPU_DMA_LATENCY, 0);

        err = clk_prepare_enable(imx_data->clk_per);
        if (err)
                goto free_sdhci;
        err = clk_prepare_enable(imx_data->clk_ipg);
        if (err)
                goto disable_per_clk;
        err = clk_prepare_enable(imx_data->clk_ahb);
        if (err)
                goto disable_ipg_clk;
		
	        imx_data->pinctrl = devm_pinctrl_get(&pdev->dev);
        if (IS_ERR(imx_data->pinctrl)) {
                err = PTR_ERR(imx_data->pinctrl);
                dev_warn(mmc_dev(host->mmc), "could not get pinctrl\n");
                imx_data->pins_default = ERR_PTR(-EINVAL);
        } else {
                imx_data->pins_default = pinctrl_lookup_state(imx_data->pinctrl,
                                                              PINCTRL_STATE_DEFAULT);
                if (IS_ERR(imx_data->pins_default))
                        dev_warn(mmc_dev(host->mmc), "could not get default state\n");
        }

        if (esdhc_is_usdhc(imx_data)) {
                host->quirks2 |= SDHCI_QUIRK2_PRESET_VALUE_BROKEN;
                host->mmc->caps |= MMC_CAP_1_8V_DDR | MMC_CAP_3_3V_DDR;
                if (!(imx_data->socdata->flags & ESDHC_FLAG_HS200))
                        host->quirks2 |= SDHCI_QUIRK2_BROKEN_HS200;

                /* clear tuning bits in case ROM has set it already */
                writel(0x0, host->ioaddr + ESDHC_MIX_CTRL);
                writel(0x0, host->ioaddr + SDHCI_ACMD12_ERR);
                writel(0x0, host->ioaddr + ESDHC_TUNE_CTRL_STATUS);
        }

        host->tuning_delay = 1;



disable_ahb_clk:
        clk_disable_unprepare(imx_data->clk_ahb);
disable_ipg_clk:
        clk_disable_unprepare(imx_data->clk_ipg);
disable_per_clk:
        clk_disable_unprepare(imx_data->clk_per);

        if (imx_data->socdata->flags & ESDHC_FLAG_BUSFREQ)
                release_bus_freq(BUS_FREQ_HIGH);

        if (imx_data->socdata->flags & ESDHC_FLAG_PMQOS)
                pm_qos_remove_request(&imx_data->pm_qos_req);
free_sdhci:
        sdhci_pltfm_free(pdev);
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




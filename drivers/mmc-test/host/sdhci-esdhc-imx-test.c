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
//#include "sdhci-pltfm.h"
//#include "sdhci-esdhc.h"
//#include "cqhci.h"



#define BIT(nr)                 (1 << (nr))

#define ESDHC_FLAG_ERR004536            BIT(7)

struct esdhc_soc_data {
        unsigned int flags;
};

static struct esdhc_soc_data esdhc_imx25_data = {
        .flags = ESDHC_FLAG_ERR004536,
};


static const struct of_device_id imx_esdhc_dt_ids[] = {
        { .compatible = "fsl,imx25-esdhc", .data = &esdhc_imx25_data, },
/*        { .compatible = "fsl,imx35-esdhc", .data = &esdhc_imx35_data, },
        { .compatible = "fsl,imx51-esdhc", .data = &esdhc_imx51_data, },
        { .compatible = "fsl,imx53-esdhc", .data = &esdhc_imx53_data, },
        { .compatible = "fsl,imx6sx-usdhc", .data = &usdhc_imx6sx_data, },
        { .compatible = "fsl,imx6sl-usdhc", .data = &usdhc_imx6sl_data, },
        { .compatible = "fsl,imx6q-usdhc", .data = &usdhc_imx6q_data, },
        { .compatible = "fsl,imx6ull-usdhc", .data = &usdhc_imx6ull_data, },
        { .compatible = "fsl,imx7d-usdhc", .data = &usdhc_imx7d_data, },
        { .compatible = "fsl,imx7ulp-usdhc", .data = &usdhc_imx7ulp_data, },
        { .compatible = "fsl,imx8qxp-usdhc", .data = &usdhc_imx8qxp_data, },
        { .compatible = "fsl,imx8mm-usdhc", .data = &usdhc_imx8mm_data, },
  */      { /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, imx_esdhc_dt_ids);

static struct platform_driver sdhci_esdhc_imx_driver = {
        .driver         = {
                .name   = "sdhci-esdhc-imx-test",
                .of_match_table = imx_esdhc_dt_ids,
//                .pm     = &sdhci_esdhc_pmops,
        },
//        .id_table       = imx_esdhc_devtype,
//        .probe          = sdhci_esdhc_imx_probe,
//        .remove         = sdhci_esdhc_imx_remove,
};

module_platform_driver(sdhci_esdhc_imx_driver);

MODULE_DESCRIPTION("SDHCI driver for Freescale i.MX eSDHC");
MODULE_AUTHOR("Wolfram Sang <kernel@pengutronix.de>");
MODULE_LICENSE("GPL v2");




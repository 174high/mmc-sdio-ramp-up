/*
 * sdhci-pltfm.c Support for SDHCI platform devices
 * Copyright (c) 2009 Intel Corporation
 *
 * Copyright (C) 2007, 2011, 2015 Freescale Semiconductor, Inc.
 * Copyright (c) 2009 MontaVista Software, Inc.
 *
 * Authors: Xiaobo Xie <X.Xie@freescale.com>
 *          Anton Vorontsov <avorontsov@ru.mvista.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* Supports:
 * SDHCI platform devices
 *
 * Inspired by sdhci-pci.c, by Pierre Ossman
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#ifdef CONFIG_PPC
#include <asm/machdep.h>
#endif
#include "sdhci-pltfm.h"

static const struct sdhci_ops sdhci_pltfm_ops = {
        .set_clock = sdhci_set_clock,
        .set_bus_width = sdhci_set_bus_width,
        .reset = sdhci_reset,
        .set_uhs_signaling = sdhci_set_uhs_signaling,
};

struct sdhci_host *sdhci_pltfm_init(struct platform_device *pdev,
                                    const struct sdhci_pltfm_data *pdata,
                                    size_t priv_size)
{
	struct sdhci_host *host;
        struct resource *iomem;
        void __iomem *ioaddr;
        int irq, ret;

	iomem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ioaddr = devm_ioremap_resource(&pdev->dev, iomem);
        if (IS_ERR(ioaddr)) {
                ret = PTR_ERR(ioaddr);
                goto err;
        }					

	irq = platform_get_irq(pdev, 0);
        if (irq < 0) {
                dev_err(&pdev->dev, "failed to get IRQ number\n");
                ret = irq;
                goto err;
        }

        host = sdhci_alloc_host(&pdev->dev,
                sizeof(struct sdhci_pltfm_host) + priv_size);

        if (IS_ERR(host)) {
                ret = PTR_ERR(host);
                goto err;
        }

        host->ioaddr = ioaddr;
        host->irq = irq;
        host->hw_name = dev_name(&pdev->dev);
        if (pdata && pdata->ops)
                host->ops = pdata->ops;
        else
                host->ops = &sdhci_pltfm_ops;
        if (pdata) {
                host->quirks = pdata->quirks;
                host->quirks2 = pdata->quirks2;
        }

        platform_set_drvdata(pdev, host);

        return host;
err:
        dev_err(&pdev->dev, "%s failed %d\n", __func__, ret);
        return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(sdhci_pltfm_init);

void sdhci_pltfm_free(struct platform_device *pdev)
{
        struct sdhci_host *host = platform_get_drvdata(pdev);

//       	sdhci_free_host(host);
}
EXPORT_SYMBOL_GPL(sdhci_pltfm_free);








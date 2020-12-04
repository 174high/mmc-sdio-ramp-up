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

struct sdhci_host *sdhci_pltfm_init(struct platform_device *pdev,
                                    const struct sdhci_pltfm_data *pdata,
                                    size_t priv_size)
{
	struct sdhci_host *host;
        struct resource *iomem;
        void __iomem *ioaddr;
        int irq, ret;


	iomem = platform_get_resource(pdev, IORESOURCE_MEM, 0);


}
EXPORT_SYMBOL_GPL(sdhci_pltfm_init);

void sdhci_pltfm_free(struct platform_device *pdev)
{
 //       struct sdhci_host *host = platform_get_drvdata(pdev);

 //       sdhci_free_host(host);
}
EXPORT_SYMBOL_GPL(sdhci_pltfm_free);








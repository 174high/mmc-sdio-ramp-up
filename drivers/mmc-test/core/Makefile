# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the kernel mmc core.
#

obj-$(CONFIG_MMC_LT)		+= mmc_core.o

mmc_core-y                      := core.o bus.o host.o \
				mmc_ops.o slot-gpio.o sd.o sd_ops.o sdio_bus.o \
				sdio.o  sdio_ops.o sdio_io.o sdio_irq.o                  \
				sdio_cis.o  \


mmc_core-$(CONFIG_OF)           += pwrseq.o
mmc_core-$(CONFIG_DEBUG_FS)     += debugfs.o

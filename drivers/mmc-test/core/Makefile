# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the kernel mmc core.
#

obj-$(CONFIG_MMC_LT)		+= mmc_core.o

mmc_core-y                      := core.o host.o \
				mmc_ops.o slot-gpio.o sd_ops.o \
				sdio_ops.o

mmc_core-$(CONFIG_OF)           += pwrseq.o


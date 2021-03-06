/*
 * drivers/mmc/host/esdhc.c
 *
 * Copyright (C) 2007-2011 Freescale Semiconductor, Inc. All Rights Reserved.
 *	Author: Chenghu Wu <b16972@freescale.com>
 *		Xiaobo Xie <X.Xie@freescale.com>
 *
 *	Freescale Enhanced Secure Digital Host Controller driver.
 *	Based on mpc837x/driver/mmc/host/esdhc.c done by Xiaobo Xie
 *      Ported to Coldfire platform by Chenghu Wu
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#define CONFIG_ESDHC_FORCE_PIO
#define USE_EDMA
#undef  USE_ADMA

#include <linux/module.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/uaccess.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/mmc/host.h>
#include <linux/platform_device.h>

#include <asm/dma.h>
#include <asm/page.h>
#include <mach/dmac.h>
#include <mach/dmainit.h>
#include <mach/cache.h>
#if defined(CONFIG_KINETIS_GPIO_INT)
#include <mach/gpio.h>
#endif

#include "esdhc.h"
#define DRIVER_NAME "esdhc"

#if defined(CONFIG_KINETIS_GPIO_INT)

#define KINETIS_CARD_DETECT_GPIO	\
	((KINETIS_GPIO_PORT_E * KINETIS_GPIO_PORT_PINS) + 28)
static int card_detect_extern_irq;

#else

//#if defined(CONFIG_ESDHC_DETECT_USE_EXTERN_IRQ1)
//#define card_detect_extern_irq (64 + 1)
//#elif defined(CONFIG_ESDHC_DETECT_USE_EXTERN_IRQ7)
//#define card_detect_extern_irq (64 + 7)
//#else
#define card_detect_extern_irq 91 /* K70 PORT E int */
//#endif
//
#endif

#undef ESDHC_DMA_KMALLOC

#define SYS_BUSCLOCK 120000000
#define ESDHC_DMA_SIZE	0x10000

#define ESDHC_ERR_RETRY	3

#undef MMC_ESDHC_DEBUG
#undef MMC_ESDHC_DEBUG_REG

#ifdef MMC_ESDHC_DEBUG
#define DBG(fmt, args...) printk(KERN_INFO "[%s] " fmt "\n", __func__, ## args)
#else
#define DBG(fmt, args...) do {} while (0)
#endif

struct timer_list	timer1;		/* Timer for timeouts */

static void esdhc_regs(struct esdhc_host *host)
{
	printk(KERN_INFO "========= REGISTER DUMP ==========\n");
	printk(KERN_INFO "DSADDR:   0x%08x | BLKATTR:   0x%08x\n"
			 "CMDARG:   0x%08x | XFERTYP:   0x%08x\n"
			 "CMDRSP0:  0x%08x | CMDRSP1:   0x%08x\n"
			 "CMDRSP2:  0x%08x | CMDRSP3:   0x%08x\n"
			 "DATPORT:  0x%08x | PRSSTAT:   0x%08x\n"
			 "PROCTL:   0x%08x | SYSCTL:    0x%08x\n"
			 "IRQSTAT:  0x%08x | IRQSTATEN: 0x%08x\n"
			 "IRQSIGEN: 0x%08x | AC12ERR:   0x%08x\n"
			 "HTCAPBLT: 0x%08x | WML:       0x%08x\n"
			 "FEVT:     0x%08x | ADMAES:    0x%08x\n"
			 "ADSADDR:  0x%08x | VENDOR:    0x%08x\n"
			 "MMCBOOT:  0x%08x | HOSTVER:   0x%08x\n",
			fsl_readl(host->ioaddr + 0x00), fsl_readl(host->ioaddr + 0x04),
			fsl_readl(host->ioaddr + 0x08), fsl_readl(host->ioaddr + 0x0c),
			fsl_readl(host->ioaddr + 0x10), fsl_readl(host->ioaddr + 0x14),
			fsl_readl(host->ioaddr + 0x18), fsl_readl(host->ioaddr + 0x1c),
			fsl_readl(host->ioaddr + 0x20), fsl_readl(host->ioaddr + 0x24),
			fsl_readl(host->ioaddr + 0x28), fsl_readl(host->ioaddr + 0x2c),
			fsl_readl(host->ioaddr + 0x30), fsl_readl(host->ioaddr + 0x34),
			fsl_readl(host->ioaddr + 0x38), fsl_readl(host->ioaddr + 0x3c),
			fsl_readl(host->ioaddr + 0x40), fsl_readl(host->ioaddr + 0x44),
			fsl_readl(host->ioaddr + 0x50), fsl_readl(host->ioaddr + 0x54),
			fsl_readl(host->ioaddr + 0x58), fsl_readl(host->ioaddr + 0xc0),
			fsl_readl(host->ioaddr + 0xc4), fsl_readl(host->ioaddr + 0xfc));
#if defined(USE_ADMA)
	{
		volatile struct adma_bd *chain;
		int i, inum = host->data ? host->data->blocks : 0;

		chain = host->dma_tx_buf;
		printk("ADMA chain:\n");
		for (i = 0; i < inum; i++) {
			printk("[%02d]: %p: adr=0x%08x,len=0x%04x,atr=0x%04x\n",
				i, &chain[i],
				chain[i].addr, chain[i].len, chain[i].attr);
		}
	}
#endif
	printk(KERN_INFO "==================================\n");
}
#ifdef MMC_ESDHC_DEBUG_REG
static void esdhc_dumpregs(struct esdhc_host *host)
{
	esdhc_regs(host);
}
#else
static void esdhc_dumpregs(struct esdhc_host *host)
{
	do {} while (0);
}
#endif

static unsigned int debug_nodma;
static unsigned int debug_forcedma;
static unsigned int debug_quirks;

#define ESDHC_QUIRK_CLOCK_BEFORE_RESET			(1<<0)
#define ESDHC_QUIRK_FORCE_DMA				(1<<1)
#define ESDHC_QUIRK_NO_CARD_NO_RESET			(1<<2)
#define ESDHC_QUIRK_SINGLE_POWER_WRITE			(1<<3)

static void esdhc_set_clock(struct esdhc_host *host, unsigned int clock);

static void esdhc_prepare_data(struct esdhc_host *, struct mmc_data *);
static void esdhc_finish_data(struct esdhc_host *);
static irqreturn_t esdhc_irq(int irq, void *dev_id);
static void esdhc_send_command(struct esdhc_host *, struct mmc_command *);
static void esdhc_finish_command(struct esdhc_host *);

/*****************************************************************************\
 *                                                                           *
 * Low level functions                                                       *
 *                                                                           *
\*****************************************************************************/

static void esdhc_reset(struct esdhc_host *host, u8 mask)
{
	unsigned long timeout;
	unsigned int sysctl;

	if (host->chip->quirks & ESDHC_QUIRK_NO_CARD_NO_RESET) {
		if (!(fsl_readl(host->ioaddr + ESDHC_PRESENT_STATE) &
			ESDHC_CARD_PRESENT))
			return;
	}

	DBG("esdhc_reset %x\n", mask);

	timeout = fsl_readl(host->ioaddr + ESDHC_SYSTEM_CONTROL);
	timeout = timeout | (mask << ESDHC_RESET_SHIFT);
	fsl_writel(host->ioaddr + ESDHC_SYSTEM_CONTROL, (timeout & ~ESDHC_CLOCK_SDCLKEN));
	fsl_writel(host->ioaddr + ESDHC_SYSTEM_CONTROL, timeout);

	if (mask & ESDHC_RESET_ALL) {
		host->clock = 0;
		host->bus_width = 0;
	}

	/* Wait max 100 ms */
	timeout = 10000;

	/* hw clears the bit when it's done */
	sysctl = (mask << ESDHC_RESET_SHIFT);
	while (fsl_readl(host->ioaddr + ESDHC_SYSTEM_CONTROL) & sysctl) {
		if (timeout == 0) {
			printk(KERN_ERR "%s: Reset 0x%x never completed.\n",
				mmc_hostname(host->mmc), (int)mask);
			esdhc_dumpregs(host);
			return;
		}
		timeout--;
		udelay(10);
	}

	while (!(fsl_readl(host->ioaddr + ESDHC_PRESENT_STATE) & ESDHC_SDSTB));

	timeout = fsl_readl(host->ioaddr + ESDHC_SYSTEM_CONTROL);
	fsl_writel(host->ioaddr + ESDHC_SYSTEM_CONTROL, timeout | ESDHC_CLOCK_SDCLKEN);
}

static void esdhc_init(struct esdhc_host *host)
{
	u32 intmask;
	/*reset eSDHC chip*/
	esdhc_reset(host, ESDHC_RESET_ALL);

	fsl_writel(host->ioaddr + ESDHC_VENDOR, 0);
	fsl_writel(host->ioaddr + ESDHC_WML, (1 << 16) | 2);

	intmask = fsl_readl(host->ioaddr + ESDHC_PRESENT_STATE);
	intmask = intmask & 0xF7000000;
	fsl_writel(host->ioaddr + ESDHC_PRESENT_STATE, intmask);

	intmask = fsl_readl(host->ioaddr + ESDHC_SYSTEM_CONTROL);
	intmask = intmask | ESDHC_CLOCK_INT_EN | ESDHC_CLOCK_INT_STABLE;
	fsl_writel(host->ioaddr + ESDHC_SYSTEM_CONTROL, intmask);

	intmask = fsl_readl(host->ioaddr + ESDHC_INT_STATUS);
	fsl_writel(host->ioaddr + ESDHC_INT_STATUS, intmask);

	intmask = fsl_readl(host->ioaddr + ESDHC_INT_ENABLE);
	intmask &= ~ESDHC_INT_DATA_TIMEOUT;

	fsl_writel(host->ioaddr + ESDHC_INT_ENABLE, intmask);
	fsl_writel(host->ioaddr + ESDHC_SIGNAL_ENABLE, intmask);
	/* Modelo does not support */
	/*MCF_ESDHC_SCR = MCF_ESDHC_SCR  | ESDHC_DMA_SNOOP | 0xC0;*/

	intmask = fsl_readl(host->ioaddr + ESDHC_PROTOCOL_CONTROL);
#if defined(USE_ADMA)
	intmask |= ESDHC_CTRL_DMAS_ADMA2;
#endif
	intmask &= ~ESDHC_CTRL_D3_DETEC;

	fsl_writel(host->ioaddr + ESDHC_PROTOCOL_CONTROL, intmask);
	DBG(" ### PROCTL: init %x\n", fsl_readl(host->ioaddr + ESDHC_PROTOCOL_CONTROL));

	fsl_writel(host->ioaddr + ESDHC_SYSTEM_CONTROL,
		fsl_readl(host->ioaddr + ESDHC_SYSTEM_CONTROL) | 0x08000000);
	while (fsl_readl(host->ioaddr + ESDHC_SYSTEM_CONTROL) & 0x08000000);
}

static void reset_regs(struct esdhc_host *host)
{
	u32 intmask;

	intmask = fsl_readl(host->ioaddr + ESDHC_INT_STATUS);
	fsl_writel(host->ioaddr + ESDHC_INT_STATUS, intmask);

	intmask = ESDHC_INT_DATA_END_BIT | ESDHC_INT_DATA_CRC |
		ESDHC_INT_DATA_TIMEOUT | ESDHC_INT_INDEX |
		ESDHC_INT_END_BIT | ESDHC_INT_CRC | ESDHC_INT_TIMEOUT |
		ESDHC_INT_DATA_AVAIL | ESDHC_INT_SPACE_AVAIL |
		ESDHC_INT_DMA_END | ESDHC_INT_DATA_END | ESDHC_INT_RESPONSE;
	fsl_writel(host->ioaddr + ESDHC_INT_ENABLE,
			intmask & ~ESDHC_INT_RESPONSE);
	fsl_writel(host->ioaddr + ESDHC_SIGNAL_ENABLE, intmask);

	if (host->bus_width == MMC_BUS_WIDTH_4) {
		intmask = fsl_readl(host->ioaddr + ESDHC_PROTOCOL_CONTROL);
#if defined(USE_ADMA)
		intmask |= ESDHC_CTRL_DMAS_ADMA2;
#endif
		intmask |= ESDHC_CTRL_4BITBUS;
		fsl_writel(host->ioaddr + ESDHC_PROTOCOL_CONTROL, intmask);
	}

	DBG(" ### PROCTL: reset regs %x\n", fsl_readl(host->ioaddr + ESDHC_PROTOCOL_CONTROL));
}

/*****************************************************************************
 *                                                                           *
 * Core functions                                                            *
 *                                                                           *
 *****************************************************************************/
/* Return the SG's virtual address */
static inline char *esdhc_sg_to_buffer(struct esdhc_host *host)
{
	DBG("cur_sg %p virt %p\n", host->cur_sg, sg_virt(host->cur_sg));
	return sg_virt(host->cur_sg);
}

static inline int esdhc_next_sg(struct esdhc_host *host)
{
	/*
	 * Skip to next SG entry.
	 */
	host->cur_sg = sg_next(host->cur_sg);
	host->num_sg--;

	/*
	 * Any entries left?
	 */
	if (host->num_sg > 0) {
		host->offset = 0;
		host->remain = host->cur_sg->length;
	}

	DBG("%s: host->remain %x  %x\n", __func__, host->remain, host->num_sg);
	return host->num_sg;
}

static void esdhc_read_block_pio(struct esdhc_host *host)
{

	int blksize, chunk_remain, rv;
	u32 data;
	char *buffer;

	DBG("PIO reading\n");

	blksize = host->data->blksz;
	chunk_remain = 0;
	data = 0;

	buffer = esdhc_sg_to_buffer(host) + host->offset;

#if defined(USE_EDMA)
	if (host->dma_run && kinetis_dma_ch_is_active(KINETIS_DMACH_ESDHC))
		return;

	host->dma_run = 1;
	rv = kinetis_dma_ch_init(KINETIS_DMACH_ESDHC);
	if (rv < 0)
		printk("%s: init err %d\n", __func__, rv);
	rv = kinetis_dma_ch_set_src(KINETIS_DMACH_ESDHC,
				    (u32)host->ioaddr + ESDHC_BUFFER, 0,
				    KINETIS_DMA_WIDTH_32BIT, 0);
	if (rv < 0)
		printk("%s: set src err %d\n", __func__, rv);
	rv = kinetis_dma_ch_set_dest(KINETIS_DMACH_ESDHC, (u32)buffer, 4,
				     KINETIS_DMA_WIDTH_32BIT, 0);
	if (rv < 0)
		printk("%s: set dest err %d\n", __func__, rv);
	rv = kinetis_dma_ch_set_nbytes(KINETIS_DMACH_ESDHC, blksize);
	if (rv < 0)
		printk("%s: set nbytes err %d\n", __func__, rv);
	rv = kinetis_dma_ch_set_iter_num(KINETIS_DMACH_ESDHC, 1);
	if (rv < 0)
		printk("%s: set iter err %d\n", __func__, rv);
	rv = kinetis_dma_ch_enable(KINETIS_DMACH_ESDHC, 1);
	if (rv < 0)
		printk("%s: enable err %d\n", __func__, rv);

	host->offset += blksize;
	host->remain -= blksize;

	if (!host->remain)
		esdhc_next_sg(host);
#else
	while (blksize) {
		int size;

		if (chunk_remain == 0) {
			data = fsl_readl(host->ioaddr + ESDHC_BUFFER);
			chunk_remain = min(blksize, 4);
		}

		size = min(host->remain, chunk_remain);

		chunk_remain -= size;
		blksize -= size;
		host->offset += size;
		host->remain -= size;

		while (size) {
			*buffer = data & 0xFF;
			buffer++;
			data >>= 8;
			size--;
		}

		rv = host->remain;
		if (rv == 0) {
			if (esdhc_next_sg(host) == 0) {
				BUG_ON(blksize != 0);
				return;
			}
			buffer = esdhc_sg_to_buffer(host);
		}
	}
#endif
}

static void esdhc_write_block_pio(struct esdhc_host *host)
{
	int blksize, chunk_remain, bytes, rv;
	u32 data;
	char *buffer;

	DBG("PIO writing\n");

	blksize = host->data->blksz;
	chunk_remain = 4;
	data = 0;

	bytes = 0;
	buffer = esdhc_sg_to_buffer(host) + host->offset;

#if defined(USE_EDMA)
	if (host->dma_run && kinetis_dma_ch_is_active(KINETIS_DMACH_ESDHC))
		return;

	host->dma_run = 1;
	rv = kinetis_dma_ch_init(KINETIS_DMACH_ESDHC);
	if (rv < 0)
		printk("%s: init err %d\n", __func__, rv);
	rv = kinetis_dma_ch_set_src(KINETIS_DMACH_ESDHC, (u32)buffer, 4,
				    KINETIS_DMA_WIDTH_32BIT, 0);
	if (rv < 0)
		printk("%s: set src err %d\n", __func__, rv);
	rv = kinetis_dma_ch_set_dest(KINETIS_DMACH_ESDHC,
				     (u32)host->ioaddr + ESDHC_BUFFER, 0,
				     KINETIS_DMA_WIDTH_32BIT, 0);
	if (rv < 0)
		printk("%s: set dest err %d\n", __func__, rv);
	rv = kinetis_dma_ch_set_nbytes(KINETIS_DMACH_ESDHC, blksize);
	if (rv < 0)
		printk("%s: set nbytes err %d\n", __func__, rv);
	rv = kinetis_dma_ch_set_iter_num(KINETIS_DMACH_ESDHC, 1);
	if (rv < 0)
		printk("%s: set iter err %d\n", __func__, rv);
	rv = kinetis_dma_ch_enable(KINETIS_DMACH_ESDHC, 1);
	if (rv < 0)
		printk("%s: enable err %d\n", __func__, rv);

	host->offset += blksize;
	host->remain -= blksize;

	if (!host->remain)
		esdhc_next_sg(host);
#else
	while (blksize) {
		int size;

		size = min(host->remain, chunk_remain);

		chunk_remain -= size;
		blksize -= size;
		host->offset += size;
		host->remain -= size;

		while (size) {
			data >>= 8;
			data |= (u32)*buffer << 24;
			buffer++;
			size--;
		}

		if (chunk_remain == 0) {
			fsl_writel(host->ioaddr + ESDHC_BUFFER, data);
			chunk_remain = min(blksize, 4);
		}

		rv = host->remain;
		if (rv == 0) {
			if (esdhc_next_sg(host) == 0) {
				BUG_ON(blksize != 0);
				return;
			}
			buffer = esdhc_sg_to_buffer(host);
		}
	}
#endif
}

static void esdhc_transfer_pio(struct esdhc_host *host)
{
	u32 mask;

	BUG_ON(!host->data);

	if (host->num_sg == 0)
		return;

	if (host->data->flags & MMC_DATA_READ)
		mask = ESDHC_DATA_AVAILABLE;
	else
		mask = ESDHC_SPACE_AVAILABLE;

	while (fsl_readl(host->ioaddr + ESDHC_PRESENT_STATE) & mask) {
		if (host->data->flags & MMC_DATA_READ)
			esdhc_read_block_pio(host);
		else
			esdhc_write_block_pio(host);

		if (host->num_sg == 0)
			break;
	}

	DBG("PIO transfer complete.\n");
}

static void esdhc_prepare_data(struct esdhc_host *host, struct mmc_data *data)
{
	u8 count;
	unsigned int blkattr = 0;
	unsigned int target_timeout, current_timeout;
	unsigned int sysctl;

	WARN_ON(host->data);

	if (data == NULL)
		return;

	DBG("blksz %04x blks %04x flags %08x",
		data->blksz, data->blocks, data->flags);
	DBG("tsac %d ms nsac %d clk",
		data->timeout_ns / 1000000, data->timeout_clks);

	/* Sanity checks */
	BUG_ON(data->blksz * data->blocks > 524288);
	BUG_ON(data->blksz > host->mmc->max_blk_size);
	BUG_ON(data->blocks > 65535);

	if (host->clock == 0)
		return;

	/* timeout in us */
	target_timeout = data->timeout_ns / 1000 +
		(data->timeout_clks * 1000000) / host->clock;

	/*
	 * Figure out needed cycles.
	 * We do this in steps in order to fit inside a 32 bit int.
	 * The first step is the minimum timeout, which will have a
	 * minimum resolution of 6 bits:
	 * (1) 2^13*1000 > 2^22,
	 * (2) host->timeout_clk < 2^16
	 *     =>
	 *     (1) / (2) > 2^6
	 */
	count = 0;
	host->timeout_clk = host->clock/1000;
	current_timeout = (1 << 13) * 1000 / host->timeout_clk;
	while (current_timeout < target_timeout) {
		count++;
		current_timeout <<= 1;
		if (count >= 0xF)
			break;
	}

	if (count >= 0xF) {
		DBG("%s:Timeout requested is too large!\n",
			mmc_hostname(host->mmc));
		count = 0xE;
	}

	if (data->blocks >= 0x50) {
		DBG("%s:Blocks %x are too large!\n",
			mmc_hostname(host->mmc),
			data->blocks);
		count = 0xE;
	}

	if ((data->blocks == 1) && (data->blksz >= 0x200)) {
		DBG("%s:Blocksize %x is too large\n",
			mmc_hostname(host->mmc),
			data->blksz);
		count = 0xE;
	}
	count = 0xE;

	sysctl = fsl_readl(host->ioaddr + ESDHC_SYSTEM_CONTROL);
	sysctl &= (~ESDHC_TIMEOUT_MASK);
	fsl_writel(host->ioaddr + ESDHC_SYSTEM_CONTROL,
		sysctl | (count<<ESDHC_TIMEOUT_SHIFT));

	/* Data transfer*/
	if (host->flags & ESDHC_USE_DMA) {
		int sg_count;
		unsigned int wml;
		unsigned int wml_value;
		unsigned int timeout;

		unsigned int val;
		val = fsl_readl(host->ioaddr + ESDHC_PRESENT_STATE);
		if (val & (ESDHC_DOING_READ | ESDHC_DATA_DLA)) {
			printk("### %s: is busy (0x%08x)\n", __func__, val);
			while (fsl_readl(host->ioaddr + ESDHC_PRESENT_STATE) &
					(ESDHC_DOING_READ | ESDHC_DATA_DLA));
		}

		/* DMA address eSDHC in Modelo must be 4 bytes aligned */
		if ((data->sg->offset & 0x3) == 0)
			host->offset = 0;
		else
			host->offset = 0x4 - (data->sg->offset & 0x3);

		sg_count = dma_map_sg(mmc_dev(host->mmc), data->sg,
					data->sg_len,
					(data->flags & MMC_DATA_READ)
					? DMA_FROM_DEVICE : DMA_TO_DEVICE);

		BUG_ON(sg_count != 1);

#if !defined(USE_ADMA)
		/* The data in SD card is little endian,
		   the SD controller is big endian */
		if ((data->flags & MMC_DATA_WRITE) == MMC_DATA_WRITE) {
			unsigned char *buffer = sg_virt(data->sg);
			unsigned char *buffer_tx =
				(unsigned char *)host->dma_tx_buf;
			/* Each sector is 512 Bytes, write 0x200 sectors */
			memcpy(buffer_tx, buffer, data->sg->length);
			fsl_writel(host->ioaddr + ESDHC_DMA_ADDRESS,
				(unsigned long)host->dma_tx_dmahandle);
		} else {
			DBG("READ 0x%x(%dx%x) to %x/%x 0x%x\n",
				data->blocks * data->blksz, data->blocks, data->blksz,
				host->dma_tx_dmahandle, sg_dma_address(data->sg),
				host->offset);
			fsl_writel(host->ioaddr + ESDHC_DMA_ADDRESS,
				(unsigned long)host->dma_tx_dmahandle);
		}
#endif

		/* Disable the BRR and BWR interrupt */
		timeout = fsl_readl(host->ioaddr + ESDHC_INT_ENABLE);
		timeout = timeout & (~(ESDHC_INT_DATA_AVAIL |
					ESDHC_INT_SPACE_AVAIL));
		fsl_writel(host->ioaddr + ESDHC_INT_ENABLE, timeout);

		timeout = fsl_readl(host->ioaddr + ESDHC_SIGNAL_ENABLE);
		timeout = timeout & (~(ESDHC_INT_DATA_AVAIL |
					ESDHC_INT_SPACE_AVAIL));
		fsl_writel(host->ioaddr + ESDHC_SIGNAL_ENABLE, timeout);

		wml_value = data->blksz / 4;
		if (data->flags & MMC_DATA_READ) {
			/* Read watermask level, max is 0x10*/
			if (wml_value > 0x10)
				wml_value = 0x10;
			wml = (wml_value & ESDHC_WML_MASK) |
				((0x10 & ESDHC_WML_MASK)
				 << ESDHC_WML_WRITE_SHIFT);
		} else {
			if (wml_value > 0x80)
				wml_value = 0x80;
			wml = (0x10 & ESDHC_WML_MASK) |
				(((wml_value) & ESDHC_WML_MASK)
				 << ESDHC_WML_WRITE_SHIFT);
		}

		fsl_writel(host->ioaddr + ESDHC_WML, wml);
	} else {
		unsigned long v;

		host->cur_sg = data->sg;
		host->num_sg = data->sg_len;

		host->offset = 0;
		host->remain = host->cur_sg->length;

		v  = fsl_readl(host->ioaddr + ESDHC_INT_ENABLE);
		v |= ESDHC_INT_DATA_AVAIL | ESDHC_INT_SPACE_AVAIL;
		fsl_writel(host->ioaddr + ESDHC_INT_ENABLE, v);

		v  = fsl_readl(host->ioaddr + ESDHC_SIGNAL_ENABLE);
		v |= ESDHC_INT_DATA_AVAIL | ESDHC_INT_SPACE_AVAIL;
		fsl_writel(host->ioaddr + ESDHC_SIGNAL_ENABLE, v);

		/* Generate request if blksize (or more) avail */
		v = fsl_readl(host->ioaddr + ESDHC_WML);
		if (data->flags & MMC_DATA_WRITE) {
			v &= ~(0xFFFF << 16);
			v |= (data->blksz >> 2) << 16;
		} else {
			v &= ~0xFFFF;
			v |= (data->blksz >> 2);
		}
		fsl_writel(host->ioaddr + ESDHC_WML, v);
	}

#if defined(USE_ADMA)
	{
		volatile struct adma_bd	*chain;
		unsigned char		*buf;
		int			i;

		host->dma_tx_blocks = data->blocks;
		chain = host->dma_tx_buf;
		buf = sg_virt(data->sg);

		for (i = 0; i < data->blocks; i++) {
			chain[i].addr = (u32)buf;
			chain[i].len  = data->blksz;
			chain[i].attr = ESDHC_ADMA_ACT_TRAN | ESDHC_ADMA_VALID;

			buf += data->blksz;
		}
		chain[i - 1].attr |= ESDHC_ADMA_END;

		fsl_writel(host->ioaddr + 0x58, (u32)chain);
	}
#endif

#if defined(USE_EDMA)
	kinetis_ps_cache_flush();
#endif

	/* We do not handle DMA boundaries */
	blkattr = data->blksz;
	blkattr |= (data->blocks << 16);
	fsl_writel(host->ioaddr + ESDHC_BLOCK_ATTR, blkattr);

	esdhc_dumpregs(host);
}

static unsigned int esdhc_set_transfer_mode(struct esdhc_host *host,
	struct mmc_data *data)
{
	u32 mode = 0;

	WARN_ON(host->data);

	if (data == NULL)
		return 0;

	mode = ESDHC_TRNS_BLK_CNT_EN;
	if (data->blocks > 1) {
		if (data->flags & MMC_DATA_READ)
			mode |= ESDHC_TRNS_MULTI | ESDHC_TRNS_ACMD12;
		else
			mode |= ESDHC_TRNS_MULTI;
	}
	if (data->flags & MMC_DATA_READ)
		mode |= ESDHC_TRNS_READ;
	if (host->flags & ESDHC_USE_DMA)
		mode |= ESDHC_TRNS_DMA;

	return mode;
}

static void esdhc_finish_data(struct esdhc_host *host)
{
	struct mmc_data *data;
	u16 blocks;
	unsigned long cache_flags;

	BUG_ON(!host->data);

#if defined(USE_EDMA)
	kinetis_ps_cache_flush();
#endif

	data = host->data;
	host->data = NULL;

	if (host->flags & ESDHC_USE_DMA) {
		//unsigned char *buffer = sg_dma_address(data->sg);
		unsigned char *buffer = sg_virt(data->sg);
		unsigned char C0, C1, C2, C3;
		int i;
		/* Data in SD card is little endian,
		   SD controller is big endian */

		kinetis_ps_cache_save(&cache_flags);

#if !defined(USE_ADMA)
		mdelay(100);
		memcpy(buffer, (void *)host->dma_tx_dmahandle, data->sg->length);
#endif

		if (((data->flags & MMC_DATA_READ) == MMC_DATA_READ)) {
			for (i = 0; i < data->sg->length; i = i + 4) {
				C0  = *(buffer + host->offset + i);
				C1  = *(buffer + host->offset + i + 1);
				C2  = *(buffer + host->offset + i + 2);
				C3  = *(buffer + host->offset + i + 3);
				*(buffer+i)   = C0;
				*(buffer+i+1) = C1;
				*(buffer+i+2) = C2;
				*(buffer+i+3) = C3;
			}
		}
		kinetis_ps_cache_restore(&cache_flags);
	}
	/*
	 * Controller doesn't count down when in single block mode.
	 */
	if ((data->blocks == 1) && (data->error == MMC_ERR_NONE))
		blocks = 0;
	else {
		blocks = fsl_readl(host->ioaddr + ESDHC_BLOCK_ATTR) >> 16;
		blocks = 0;
		if (data->flags & MMC_DATA_READ)
			data->stop = 0;
	}

	data->bytes_xfered = data->blksz * (data->blocks - blocks);

	if ((data->error == MMC_ERR_NONE) && blocks) {
		printk(KERN_ERR"%s: Controller signaled completion even "
		       "though there were blocks left.\n",
			mmc_hostname(host->mmc));
		data->error = MMC_ERR_FAILED;
	}

	if ((blocks == 0) && (data->error & MMC_ERR_TIMEOUT)) {
		printk(KERN_ERR "Controller transmitted completion even "
		       "though there was a timeout error.\n");
		data->error &= ~MMC_ERR_TIMEOUT;
	}

	if (data->stop) {
		DBG("%s data->stop %p\n", __func__, data->stop);
		/*
		 * The controller needs a reset of internal state machines
		 * upon error conditions.
		 */
		if (data->error != MMC_ERR_NONE) {
			printk("%s: The controller needs a "
				"reset of internal state machines\n",
				__func__);
			esdhc_reset(host, ESDHC_RESET_CMD);
			esdhc_reset(host, ESDHC_RESET_DATA);
			reset_regs(host);
		}

		esdhc_send_command(host, data->stop);
	} else
		tasklet_schedule(&host->finish_tasklet);
}

static void esdhc_send_command(struct esdhc_host *host, struct mmc_command *cmd)
{
	unsigned int flags;
	u32 mask;
	unsigned long timeout;

	WARN_ON(host->cmd);

	/* Wait max 10 ms */
	timeout = 1000;

	mask = ESDHC_CMD_INHIBIT;
	if ((cmd->data != NULL) || (cmd->flags & MMC_RSP_BUSY))
		mask |= ESDHC_DATA_INHIBIT;

	/* We shouldn't wait for data inihibit for stop commands, even
	   though they might use busy signaling */
	if (host->mrq->data && (cmd == host->mrq->data->stop))
		mask &= ~ESDHC_DATA_INHIBIT;

	while (fsl_readl(host->ioaddr + ESDHC_PRESENT_STATE) & mask) {
		if (timeout == 0) {
			printk(KERN_ERR "%s: Controller never released "
			       "inhibit bit(s).\n", mmc_hostname(host->mmc));
			esdhc_regs(host);
			cmd->error = MMC_ERR_FAILED;
			tasklet_schedule(&host->finish_tasklet);
			return;
		}
		timeout--;
		udelay(10);
	}

	mod_timer(&host->timer, jiffies + 15 * HZ);

	host->cmd = cmd;

	esdhc_prepare_data(host, cmd->data);

	fsl_writel(host->ioaddr + ESDHC_ARGUMENT, cmd->arg);

	flags = esdhc_set_transfer_mode(host, cmd->data);

	if ((cmd->flags & MMC_RSP_136) && (cmd->flags & MMC_RSP_BUSY)) {
		printk(KERN_ERR "%s: Unsupported response type!\n",
		       mmc_hostname(host->mmc));
		cmd->error = MMC_ERR_INVALID;
		tasklet_schedule(&host->finish_tasklet);
		return;
	}

	if (!(cmd->flags & MMC_RSP_PRESENT))
		flags |= ESDHC_CMD_RESP_NONE;
	else if (cmd->flags & MMC_RSP_136)
		flags |= ESDHC_CMD_RESP_LONG;
	else if (cmd->flags & MMC_RSP_BUSY)
		flags |= ESDHC_CMD_RESP_SHORT_BUSY;
	else
		flags |= ESDHC_CMD_RESP_SHORT;

	if (cmd->flags & MMC_RSP_CRC)
		flags |= ESDHC_CMD_CRC_EN;
	if (cmd->flags & MMC_RSP_OPCODE)
		flags |= ESDHC_CMD_INDEX_EN;
	if (cmd->data)
		flags |= ESDHC_CMD_DATA;

	fsl_writel(host->ioaddr + ESDHC_COMMAND,
		 ESDHC_MAKE_CMD(cmd->opcode, flags));
}

static void esdhc_finish_command(struct esdhc_host *host)
{
	int i;

	BUG_ON(host->cmd == NULL);

	if (host->cmd->flags & MMC_RSP_PRESENT) {
		if (host->cmd->flags & MMC_RSP_136) {
			/* CRC is stripped so we need to do some shifting. */
			for (i = 0; i < 4; i++) {
				host->cmd->resp[i] = fsl_readl(host->ioaddr +
						ESDHC_RESPONSE + (3-i)*4) << 8;
				if (i != 3)
					host->cmd->resp[i] |=
						(fsl_readl(host->ioaddr
							+ ESDHC_RESPONSE
							+ (2-i)*4) >> 24);
			}
		} else
			host->cmd->resp[0] = fsl_readl(host->ioaddr +
							ESDHC_RESPONSE);
	}

	host->cmd->error = MMC_ERR_NONE;

	if (host->cmd->data)
		host->data = host->cmd->data;
	else
		tasklet_schedule(&host->finish_tasklet);

	host->cmd = NULL;
}

#define MYCLOCK 0
static void esdhc_set_clock(struct esdhc_host *host, unsigned int clock)
{
#if MYCLOCK
	unsigned long sdrefclk, vco, bestmatch = -1, temp, diff;
	int dvs, sdclkfs, outdiv;
	int best_dvs, best_sdclkfs, best_outdiv;
#else
	int div, pre_div;
	unsigned long sys_busclock = SYS_BUSCLOCK;
#endif
	unsigned long timeout;
	u16 clk;

	DBG("esdhc_set_clock %x\n", clock);
	if (clock == host->clock)
		return;

	timeout = fsl_readl(host->ioaddr + ESDHC_SYSTEM_CONTROL);
	timeout = timeout & (~(ESDHC_CLOCK_MASK | ESDHC_CLOCK_SDCLKEN));
	fsl_writel(host->ioaddr + ESDHC_SYSTEM_CONTROL, timeout);

	if (clock == 0)
		goto out;

#if MYCLOCK
	/* TC: The VCO must obtain from u-boot */
	/*
	* First set the outdiv3 to min 1, then walk through all to
	* get closer value with SDCLKDIV and DIV combination
	*/
	vco = 500000000;
	MCF_CLOCK_PLL_DR &= 0xFFFF83FF;         /* Disable SD Clock */

	for (outdiv = 2; outdiv <= 32; outdiv++) {
		sdrefclk = vco / outdiv;

		for (sdclkfs = 2; sdclkfs < 257; sdclkfs <<= 1) {
			for (dvs = 1; dvs < 17; dvs++) {
				temp = sdrefclk / (sdclkfs * dvs);

				if (temp > clock)
					diff = temp - clock;
				else
					diff = clock - temp;

				if (diff <= bestmatch) {
					bestmatch = diff;
					best_outdiv = outdiv;
					best_sdclkfs = sdclkfs;
					best_dvs = dvs;

					if (bestmatch == 0)
						goto end;
				}
			}
		}
	}

end:
#ifdef CONFIG_M5441X
	best_outdiv = 3;
	best_sdclkfs = 2;
	best_dvs = 5;
#endif
	MCF_CLOCK_PLL_DR |= ((best_outdiv - 1) << 10);
	clk = ((best_sdclkfs >> 1) << 8) | ((best_dvs - 1) << 4);
#else

	if (sys_busclock / 16 > clock) {
		for (pre_div = 1; pre_div < 256; pre_div *= 2) {
			if ((sys_busclock / pre_div) < (clock*16))
				break;
		}
	} else
		pre_div = 1;

	for (div = 1; div <= 16; div++) {
		if ((sys_busclock / (div*pre_div)) <= clock)
			break;
	}

	pre_div >>= 1;
	div -= 1;

	clk = (div << ESDHC_DIVIDER_SHIFT) | (pre_div << ESDHC_PREDIV_SHIFT);
#endif

#if 0
	timeout = fsl_readl(host->ioaddr + ESDHC_SYSTEM_CONTROL);
	timeout = timeout | clk;

	fsl_writel(host->ioaddr + ESDHC_SYSTEM_CONTROL, timeout);

	/* Wait max 10 ms */
	timeout = 10;
	while (timeout) {
		timeout--;
		mdelay(1);
	}

	while (!(fsl_readl(host->ioaddr + ESDHC_PRESENT_STATE) & ESDHC_SDSTB));

	timeout = fsl_readl(host->ioaddr + ESDHC_SYSTEM_CONTROL);
	timeout = timeout | ESDHC_CLOCK_CARD_EN | ESDHC_CLOCK_SDCLKEN;
	fsl_writel(host->ioaddr + ESDHC_SYSTEM_CONTROL, timeout);
#else
	fsl_writel(host->ioaddr + ESDHC_SYSTEM_CONTROL,
			fsl_readl(host->ioaddr + ESDHC_SYSTEM_CONTROL) & (~8));
	fsl_writel(host->ioaddr + ESDHC_SYSTEM_CONTROL,
			(fsl_readl(host->ioaddr + ESDHC_SYSTEM_CONTROL) & (~0xffff0)) |
			(0xe << 16) | clk);

	while (!(fsl_readl(host->ioaddr + ESDHC_PRESENT_STATE) & ESDHC_SDSTB));
	fsl_writel(host->ioaddr + ESDHC_SYSTEM_CONTROL,
			fsl_readl(host->ioaddr + ESDHC_SYSTEM_CONTROL) | 0x8);
	fsl_writel(host->ioaddr + ESDHC_INT_STATUS,
			fsl_readl(host->ioaddr + ESDHC_INT_STATUS) | 0x100000);
#endif

	esdhc_dumpregs(host);

out:
	host->clock = clock;
	if (host->clock == 0) {
		timeout = fsl_readl(host->ioaddr + ESDHC_SYSTEM_CONTROL);
		timeout = timeout | ESDHC_CLOCK_DEFAULT;
		fsl_writel(host->ioaddr + ESDHC_SYSTEM_CONTROL, timeout);
	}
}

static void esdhc_set_power(struct esdhc_host *host, unsigned short power)
{
	if (host->power == power)
		return;

	if (power == (unsigned short)-1)
		host->power = power;
}

/*****************************************************************************\
 *                                                                           *
 * MMC callbacks                                                             *
 *                                                                           *
\*****************************************************************************/

static void esdhc_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct esdhc_host *host;
	unsigned long flags;

	DBG("esdhc_request %p\n", mrq->cmd);
	host = mmc_priv(mmc);

	spin_lock_irqsave(&host->lock, flags);

	WARN_ON(host->mrq != NULL);

	host->mrq = mrq;

	esdhc_send_command(host, mrq->cmd);

	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);
}

static void esdhc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct esdhc_host *host;
	unsigned long flags;
	u32 ctrl, irq_status_ena, irq_signal_ena;

	DBG("ios->power_mode %x, ios->bus_width %x\n",
		ios->power_mode, ios->bus_width);
	host = mmc_priv(mmc);

	spin_lock_irqsave(&host->lock, flags);

	/*
	 * Reset the chip on each power off.
	 * Should clear out any weird states.
	 */

	if (ios->power_mode == MMC_POWER_OFF) {
		fsl_writel(host->ioaddr + ESDHC_SIGNAL_ENABLE, 0);
		esdhc_init(host);
	}

	esdhc_set_clock(host, ios->clock);

	if (ios->power_mode == MMC_POWER_OFF)
		esdhc_set_power(host, -1);
	else
		esdhc_set_power(host, ios->vdd);

	ctrl = fsl_readl(host->ioaddr + ESDHC_PROTOCOL_CONTROL);

	if (ios->bus_width == MMC_BUS_WIDTH_4) {
		ctrl |= ESDHC_CTRL_4BITBUS;
		host->bus_width = MMC_BUS_WIDTH_4;

		 ctrl &= ~ESDHC_CTRL_D3_DETEC;

		/*when change the config of the CD,
		* will involve card remove interrupt
		* So try disable the card remove interrupt.
		*/
		irq_status_ena = fsl_readl(host->ioaddr + ESDHC_INT_ENABLE);
		irq_status_ena &= ~ESDHC_INT_CARD_REMOVE;
		fsl_writel(host->ioaddr + ESDHC_INT_ENABLE, irq_status_ena);

		irq_signal_ena = fsl_readl(host->ioaddr + ESDHC_SIGNAL_ENABLE);
		irq_signal_ena &= ~ESDHC_INT_CARD_REMOVE;
		fsl_writel(host->ioaddr + ESDHC_SIGNAL_ENABLE, irq_signal_ena);

		DBG("host->card_insert = 0x%x\n", host->card_insert);


	} else {
		ctrl &= ~ESDHC_CTRL_4BITBUS;
		host->bus_width = MMC_BUS_WIDTH_1;
	}

#if defined(USE_ADMA)
	ctrl |= ESDHC_CTRL_DMAS_ADMA2;
#endif
	fsl_writel(host->ioaddr + ESDHC_PROTOCOL_CONTROL, ctrl);
	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);

	esdhc_dumpregs(host);
}

static int esdhc_get_ro(struct mmc_host *mmc)
{
	return 0;
}

static const struct mmc_host_ops esdhc_ops = {
	.request	= esdhc_request,
	.set_ios	= esdhc_set_ios,
	.get_ro		= esdhc_get_ro,
};

/*****************************************************************************\
 *                                                                           *
 * Tasklets                                                                  *
 *                                                                           *
\*****************************************************************************/

static void esdhc_tasklet_card(unsigned long param)
{
	struct esdhc_host *host;

	host = (struct esdhc_host *)param;

	spin_lock(&host->lock);

	DBG("esdhc_tasklet_card\n");
	if (!(fsl_readl(host->ioaddr + ESDHC_PRESENT_STATE) &
				ESDHC_CARD_PRESENT)) {
		if (host->mrq) {
			printk(KERN_ERR "%s: Card removed during transfer!\n",
				mmc_hostname(host->mmc));
			printk(KERN_ERR "%s: Resetting controller.\n",
				mmc_hostname(host->mmc));

			esdhc_reset(host, ESDHC_RESET_CMD);
			esdhc_reset(host, ESDHC_RESET_DATA);

			host->mrq->cmd->error = MMC_ERR_FAILED;
			tasklet_schedule(&host->finish_tasklet);
		}
		host->card_insert = 0;
	} else {
		esdhc_reset(host, ESDHC_INIT_CARD);
		host->card_insert = 1;
	}

	host->card_insert = 1;
	spin_unlock(&host->lock);

	mmc_detect_change(host->mmc, msecs_to_jiffies(500));
}

static void esdhc_tasklet_finish(unsigned long param)
{
	struct esdhc_host *host;
	unsigned long flags;
	struct mmc_request *mrq;

	host = (struct esdhc_host *)param;
	DBG("esdhc_tasklet_finish\n");

	spin_lock_irqsave(&host->lock, flags);

	del_timer(&host->timer);

	mrq = host->mrq;

	/*
	 * The controller needs a reset of internal state machines
	 * upon error conditions.
	 */
	if ((mrq->cmd->error != MMC_ERR_NONE) ||
		(mrq->data && ((mrq->data->error != MMC_ERR_NONE) ||
		(mrq->data->stop &&
			(mrq->data->stop->error != MMC_ERR_NONE))))) {

		/* Some controllers need this kick or reset won't work here */
		if (host->chip->quirks & ESDHC_QUIRK_CLOCK_BEFORE_RESET) {
			unsigned int clock;

			/* This is to force an update */
			clock = host->clock;
			host->clock = 0;
			esdhc_set_clock(host, clock);
		}

		if (mrq->cmd->error != MMC_ERR_TIMEOUT) {
			esdhc_reset(host, ESDHC_RESET_CMD);
			esdhc_reset(host, ESDHC_RESET_DATA);
			reset_regs(host);
			esdhc_dumpregs(host);
		}
	}

	host->mrq = NULL;
	host->cmd = NULL;
	host->data = NULL;

	if (mrq->cmd && mrq->cmd->error == MMC_ERR_TIMEOUT) {
		mrq->cmd->error = -EBUSY;
	}
	else if (mrq->cmd && mrq->cmd->error != MMC_ERR_NONE) {
		 mrq->cmd->error = -EIO;
	}
	if (mrq->data && mrq->data->error == MMC_ERR_TIMEOUT) {
		mrq->data->error = -EBUSY;
	}
	else if (mrq->data && mrq->data->error != MMC_ERR_NONE) {
		 mrq->data->error = -EIO;
	}

	spin_unlock_irqrestore(&host->lock, flags);

	mmc_request_done(host->mmc, mrq);
}

static void esdhc_timeout_timer(unsigned long data)
{
	struct esdhc_host *host;
	unsigned long flags;

	host = (struct esdhc_host *)data;
	printk(KERN_INFO "esdhc_timeout_timer\n");
	esdhc_regs(host);

	spin_lock_irqsave(&host->lock, flags);

	if (host->mrq) {
		if (host->data) {
			host->data->error = MMC_ERR_TIMEOUT;
			esdhc_finish_data(host);
		} else {
			if (host->cmd)
				host->cmd->error = MMC_ERR_TIMEOUT;
			else
				host->mrq->cmd->error = MMC_ERR_TIMEOUT;
			tasklet_schedule(&host->finish_tasklet);
		}
	}

	mmiowb();
	fsl_writel(host->ioaddr + ESDHC_INT_STATUS, 0);
	spin_unlock_irqrestore(&host->lock, flags);
}

static void esdhc_timer1(unsigned long data)
{
	struct esdhc_host *host;
	host = (struct esdhc_host *)data;
	esdhc_reset(host, ESDHC_INIT_CARD);
	host->card_insert = 1;
	mmc_detect_change(host->mmc, msecs_to_jiffies(500));
}

/*****************************************************************************\
 *                                                                           *
 * Interrupt handling                                                        *
 *                                                                           *
\*****************************************************************************/

static void esdhc_cmd_irq(struct esdhc_host *host, u32 intmask)
{
	BUG_ON(intmask == 0);
	if (!host->cmd) {
		printk(KERN_ERR "%s: Got command interrupt even though no "
			"command operation was in progress.\n",
			mmc_hostname(host->mmc));
		esdhc_dumpregs(host);
		return;
	}

	if (intmask & ESDHC_INT_TIMEOUT) {
		host->cmd->error = MMC_ERR_TIMEOUT;
		DBG("esdhc_cmd_irq  MMC_ERR_TIMEOUT\n");
		tasklet_schedule(&host->finish_tasklet);
	} else if (intmask & ESDHC_INT_RESPONSE)
		esdhc_finish_command(host);
	else {
		if (intmask & ESDHC_INT_CRC)
			host->cmd->error = MMC_ERR_BADCRC;
		else if (intmask & (ESDHC_INT_END_BIT | ESDHC_INT_INDEX))
			host->cmd->error = MMC_ERR_FAILED;
		else
			host->cmd->error = MMC_ERR_INVALID;

		tasklet_schedule(&host->finish_tasklet);
	}
}

static void esdhc_data_irq(struct esdhc_host *host, u32 intmask)
{
	BUG_ON(intmask == 0);
	if (!host->data) {
		/*
		 * A data end interrupt is sent together with the response
		 * for the stop command.
		 */
		if ((intmask & ESDHC_INT_DATA_END) ||
		    (intmask & ESDHC_INT_DMA_END)) {
			return;
		}
		DBG("%s: Got data interrupt even though no "
			"data operation was in progress.\n",
			mmc_hostname(host->mmc));
		esdhc_dumpregs(host);

		return;
	}

	if (intmask & ESDHC_INT_DATA_TIMEOUT)
		host->data->error = MMC_ERR_TIMEOUT;
	else if (intmask & ESDHC_INT_DATA_CRC)
		host->data->error = MMC_ERR_BADCRC;
	else if (intmask & ESDHC_INT_DATA_END_BIT)
		host->data->error = MMC_ERR_FAILED;
	/*
	 * With NYK we observe ACMD12ERR with 'timeout' status sometimes.
	 * According to RM we should resend CMD12 manually in this case,
	 * we don't do this since these timeouts don't bring any problems
	 * for now.
	 */

	if (host->data->error != MMC_ERR_NONE) {
		if (host->retries < ESDHC_ERR_RETRY) {
			host->retries++;
			host->data->error = 0;
			tasklet_schedule(&host->finish_tasklet);
		} else {
			host->retries = 0;
			esdhc_finish_data(host);
		}
	} else {
		host->retries = 0;
		if (intmask & (ESDHC_INT_DATA_AVAIL | ESDHC_INT_SPACE_AVAIL))
			esdhc_transfer_pio(host);
		/*
		 * We currently don't do anything fancy with DMA
		 * boundaries, but as we can't disable the feature
		 * we need to at least restart the transfer.
		 */
		if ((host->flags & ESDHC_USE_DMA) && (intmask & ESDHC_INT_DMA_END)) {
			/*printk("dma end!!! %x\n",
				fsl_readl(host->ioaddr + ESDHC_DMA_ADDRESS));
				esdhc_finish_data(host);*/
			fsl_writel(host->ioaddr + ESDHC_DMA_ADDRESS,
				fsl_readl(host->ioaddr + ESDHC_DMA_ADDRESS));
		}
		if (intmask & ESDHC_INT_DATA_END)
			esdhc_finish_data(host);
	}
}

static irqreturn_t esdhc_detect_irq(int irq, void *dev_id)
{
	irqreturn_t result = IRQ_HANDLED;
	struct esdhc_host *host = dev_id;
#if !defined(CONFIG_KINETIS_GPIO_INT)
	u32  irq_status = 0;
#endif

	spin_lock(&host->lock);

#if defined(CONFIG_KINETIS_GPIO_INT)

	/*
	 * Experimentally, it takes that long for the SD Card detect
	 * signal to get stable. Of course, it is a bad thing to delay
	 * in an IRQ handler since it affects real-time responsivness
	 * of the kernel. This is why we don't attempt to determine
	 * the status (low/high) of the SD Card detect line.
	 * The upper layers of the stack will do that anyhow.

	udelay(2000);
	irq_status = gpio_get_value(KINETIS_CARD_DETECT_GPIO);

	 */
#else

	irq_status = *(volatile unsigned int *)0x400ff110 & (1 << 28);
	*(volatile unsigned int *)0x4004D0A0 = (1 << 28);/* clear PTE28 int */
	DBG("***Extern IRQ %x\n", irq_status);
	if (irq_status == 0x0) {
		DBG("***  Card insert interrupt Extern IRQ\n");
		esdhc_reset(host, ESDHC_INIT_CARD);
		host->card_insert = 1;
	} else /*irq_status == 0x2) */{
		DBG("***  Card removed interrupt Extern IRQ\n");
		if (host->mrq) {
			printk(KERN_ERR "%s: Card removed during transfer!\n",
				mmc_hostname(host->mmc));
			printk(KERN_ERR "%s: Resetting controller.\n",
				mmc_hostname(host->mmc));

			esdhc_reset(host, ESDHC_RESET_CMD);
			esdhc_reset(host, ESDHC_RESET_DATA);

			host->mrq->cmd->error = MMC_ERR_FAILED;
			tasklet_schedule(&host->finish_tasklet);
		}
		host->card_insert = 0;
	}
#endif

	mmc_detect_change(host->mmc, msecs_to_jiffies(500));

	result = IRQ_HANDLED;
	spin_unlock(&host->lock);

	return result;
}

static irqreturn_t esdhc_irq(int irq, void *dev_id)
{
	irqreturn_t result;
	struct esdhc_host *host = dev_id;
	u32 status;

	spin_lock(&host->lock);

	status = fsl_readl(host->ioaddr + ESDHC_INT_STATUS);
	DBG("%s: status %x\n", __func__, status);

	if (!status || status == 0xffffffff) {
		result = IRQ_NONE;
		goto out;
	}

	if (status & ESDHC_INT_CMD_MASK) {
		fsl_writel(host->ioaddr + ESDHC_INT_STATUS,
			status & ESDHC_INT_CMD_MASK);
		esdhc_cmd_irq(host, status & ESDHC_INT_CMD_MASK);
	}

	if (status & ESDHC_INT_DATA_MASK) {
		fsl_writel(host->ioaddr + ESDHC_INT_STATUS,
			status & ESDHC_INT_DATA_MASK);
		esdhc_data_irq(host, status & ESDHC_INT_DATA_MASK);
	}

	status &= ~(ESDHC_INT_CMD_MASK | ESDHC_INT_DATA_MASK);

	if (status) {
		printk(KERN_ERR "%s: Unexpected interrupt 0x%08x.\n",
			mmc_hostname(host->mmc), status);
		esdhc_dumpregs(host);

		fsl_writel(host->ioaddr + ESDHC_INT_STATUS, status);
	}

	result = IRQ_HANDLED;

	mmiowb();
out:
	spin_unlock(&host->lock);

	return result;
}

/*****************************************************************************\
 *                                                                           *
 * Suspend/resume                                                            *
 *                                                                           *
\*****************************************************************************/

#ifdef CONFIG_PM

static int esdhc_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct esdhc_chip *chip;
	int i, ret;

	chip = platform_get_drvdata(pdev);
	if (!chip)
		return 0;

	DBG("Suspending...");

	for (i = 0; i < chip->num_slots; i++) {
		if (!chip->hosts[i])
			continue;
		ret = mmc_suspend_host(chip->hosts[i]->mmc, state);
		if (ret) {
			for (i--; i >= 0; i--)
				mmc_resume_host(chip->hosts[i]->mmc);
			return ret;
		}
	}

	for (i = 0; i < chip->num_slots; i++) {
		if (!chip->hosts[i])
			continue;
		free_irq(chip->hosts[i]->irq, chip->hosts[i]);
	}

	return 0;
}

static int esdhc_resume(struct platform_device *pdev)
{
	struct esdhc_chip *chip;
	int i, ret;

	chip = platform_get_drvdata(pdev);
	if (!chip)
		return 0;

	DBG("Resuming...");

	for (i = 0; i < chip->num_slots; i++) {
		if (!chip->hosts[i])
			continue;
		ret = request_irq(chip->hosts[i]->irq, esdhc_irq,
			IRQF_SHARED, chip->hosts[i]->slot_descr,
			chip->hosts[i]);
		if (ret)
			return ret;
		esdhc_init(chip->hosts[i]);
		mmiowb();
		ret = mmc_resume_host(chip->hosts[i]->mmc);
		if (ret)
			return ret;
	}

	return 0;
}

#else

#define esdhc_suspend NULL
#define esdhc_resume NULL

#endif

/*****************************************************************************\
 *                                                                           *
 * Device probing/removal                                                    *
 *                                                                           *
\*****************************************************************************/

static int esdhc_probe_slot(struct platform_device *pdev, int slot)
{
	int ret;
	unsigned int version;
	struct esdhc_chip *chip;
	struct mmc_host *mmc;
	struct esdhc_host *host;
	struct resource *res;

	unsigned int caps;

	chip = platform_get_drvdata(pdev);
	BUG_ON(!chip);

	mmc = mmc_alloc_host(sizeof(struct esdhc_host), &(pdev->dev));
	if (!mmc) {
		printk(KERN_ERR "%s mmc_alloc_host failed %x\n",
			__func__, (unsigned int)mmc);
		return -ENOMEM;
	}

	host = mmc_priv(mmc);
	host->mmc = mmc;

	host->chip = chip;
	chip->hosts[slot] = host;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res == NULL) {
		printk(KERN_ERR "%s platform_get_resource MEM failed %x\n",
			__func__, (unsigned int)res);
		goto free;
	}

	host->addr = res->start;
	host->size = res->end - res->start + 1;

	host->irq =  platform_get_irq(pdev, 0);
	if (host->irq <= 0) {
		printk(KERN_ERR "%s platform_get_irq failed %x\n",
			__func__, host->irq);
		goto free;
	}

	printk(KERN_INFO "slot %d at 0x%08lx, irq %d\n",
			slot, host->addr, host->irq);

	snprintf(host->slot_descr, 20, "esdhc:slot%d", slot);

	ret = (int)request_mem_region(host->addr, host->size, DRIVER_NAME);
	if (!ret) {
		ret = -EBUSY;
		printk(KERN_INFO "%s request_mem_region failed %x\n",
			__func__, (unsigned int)res);
		goto release;
	}

	host->ioaddr = ioremap_nocache(host->addr, host->size);
	if (!host->ioaddr) {
		ret = -ENOMEM;
		printk(KERN_INFO "%s ioremap_nocache failed %x\n",
			__func__, (unsigned int)host->ioaddr);
		goto release;
	}

	esdhc_reset(host, ESDHC_RESET_ALL);

	version = fsl_readl(host->ioaddr + ESDHC_HOST_VERSION);
	if ((version & 1) != 0x01)
		printk(KERN_INFO "%s: Unknown controller version (%d). "
			"You may experience problems.\n", host->slot_descr,
			version);

	caps = fsl_readl(host->ioaddr + ESDHC_CAPABILITIES);
	printk(KERN_INFO "%s caps %x %x\n",
		__func__, caps, (unsigned int)MCF_ESDHC_HOSTCAPBLT);

#if defined(CONFIG_ESDHC_FORCE_PIO)
	debug_nodma = 1;
#endif
	if (debug_nodma)
		DBG("DMA forced off\n");
	else if (debug_forcedma) {
		DBG("DMA forced on\n");
		host->flags |= ESDHC_USE_DMA;
	} else if (chip->quirks & ESDHC_QUIRK_FORCE_DMA) {
		DBG("Controller force DMA capability\n");
		host->flags |= ESDHC_USE_DMA;
	} else if (!(caps & ESDHC_CAN_DO_DMA))
		DBG("Controller doesn't have DMA capability\n");
	else {
		host->flags |= ESDHC_USE_DMA;
		DBG("Controller have DMA capability\n");
	}

	/*
	 * Set host parameters.
	 */
#ifdef CONFIG_MPC5441X
	host->max_clk = 17000000;
#else
	host->max_clk = 50000000;
#endif

	/* if 4 bit , freq can be 50MHz */
	mmc->ops = &esdhc_ops;
	mmc->f_min = 400000;
	mmc->f_max = min((int)host->max_clk, 50000000);

	mmc->caps = MMC_CAP_4_BIT_DATA;

	mmc->ocr_avail = 0;
	if (caps & ESDHC_CAN_VDD_330)
		mmc->ocr_avail |= MMC_VDD_32_33|MMC_VDD_33_34;
	if (caps & ESDHC_CAN_VDD_300)
		mmc->ocr_avail |= MMC_VDD_29_30|MMC_VDD_30_31;
	if (caps & ESDHC_CAN_VDD_180)
		mmc->ocr_avail |= MMC_VDD_165_195;

	if (mmc->ocr_avail == 0) {
		printk(KERN_INFO "%s: Hardware doesn't report any "
			"support voltages.\n", host->slot_descr);
		ret = -ENODEV;
		goto unmap;
	}

	spin_lock_init(&host->lock);

	/*
	 * Maximum number of segments. Hardware cannot do scatter lists.
	 */
	if (host->flags & ESDHC_USE_DMA)
		mmc->max_hw_segs = 1;
	else
		mmc->max_hw_segs = 1; //16;

	/*
	 * Maximum number of sectors in one transfer. Limited by DMA boundary
	 * size (512KiB).
	 */
	mmc->max_req_size = 524288;

	/*
	 * Maximum segment size. Could be one segment with the maximum number
	 * of bytes.
	 */
	mmc->max_seg_size = mmc->max_req_size;

	/*
	 * Maximum block size. This varies from controller to controller and
	 * is specified in the capabilities register.
	 */
	mmc->max_blk_size = (caps & ESDHC_MAX_BLOCK_MASK) >>
					ESDHC_MAX_BLOCK_SHIFT;
	if (mmc->max_blk_size > 3) {
		printk(KERN_INFO "%s: Invalid maximum block size.\n",
			host->slot_descr);
		ret = -ENODEV;
		goto unmap;
	}
	mmc->max_blk_size = 512 << mmc->max_blk_size;

	/*
	 * Maximum block count: limited with BLKATTR[BLKCNT]
	 */
	mmc->max_blk_count = 0xFFFF;

	/*
	 * Init tasklets.
	 */
	tasklet_init(&host->card_tasklet,
		esdhc_tasklet_card, (unsigned long)host);
	tasklet_init(&host->finish_tasklet,
		esdhc_tasklet_finish, (unsigned long)host);

	setup_timer(&host->timer, esdhc_timeout_timer, (unsigned long)host);

	setup_timer(&timer1, esdhc_timer1, (unsigned long)host);
	mod_timer(&timer1, jiffies + 1 * HZ);

	esdhc_init(host);

	ret = request_irq(host->irq, esdhc_irq, IRQF_DISABLED,
		host->slot_descr, host);
	if (ret) {
		printk(KERN_INFO "%s: request irq fail %x\n", __func__, ret);
		goto untasklet;
	}

#if defined(CONFIG_KINETIS_GPIO_INT)
	ret = gpio_request(KINETIS_CARD_DETECT_GPIO, "GPIO");
	if (ret) {
		printk(KERN_INFO "%s: GPIO %d busy\n", __func__,
			KINETIS_CARD_DETECT_GPIO);
		goto gpio_busy;
	}
	gpio_direction_input(KINETIS_CARD_DETECT_GPIO);

	card_detect_extern_irq = gpio_to_irq(KINETIS_CARD_DETECT_GPIO);
	ret = request_irq(card_detect_extern_irq,
			esdhc_detect_irq,
			IRQF_VALID |
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			host->slot_descr, host);
#else
	ret = request_irq(card_detect_extern_irq,
			esdhc_detect_irq, IRQF_DISABLED,
			host->slot_descr, host);
#endif
	if (ret) {
		printk(KERN_INFO "%s: request irq fail %x\n", __func__, ret);
		goto untasklet1;
	}

	mmiowb();

	ret = mmc_add_host(mmc);
	if (ret) {
		printk(KERN_INFO "%s: mmc_add_host fail %x\n", __func__, ret);
		goto unaddhost;
	}

	printk(KERN_INFO "%s: ESDHC at 0x%08lx irq %d %s\n", mmc_hostname(mmc),
		host->addr, host->irq,
		(host->flags & ESDHC_USE_DMA) ? "DMA" : "PIO");

#if !defined(USE_ADMA)

#if defined(USE_EDMA)
	ret = kinetis_dma_ch_get(KINETIS_DMACH_ESDHC);
	if (ret < 0)
		printk("%s: failed get eDMA chan %d (%d)\n", __func__,
			KINETIS_DMACH_ESDHC, ret);
#endif

#ifdef ESDHC_DMA_KMALLOC
	host->dma_tx_buf = kmalloc(ESDHC_DMA_SIZE, GFP_DMA);
	host->dma_tx_dmahandle = virt_to_phys(host->dma_tx_buf);
#else
	host->dma_tx_buf = dma_alloc_coherent(NULL, ESDHC_DMA_SIZE,
		&host->dma_tx_dmahandle, GFP_DMA|GFP_KERNEL);
#endif

	if (((unsigned int)host->dma_tx_buf == 0) ||
	    ((unsigned int)host->dma_tx_dmahandle == 0))
		printk(KERN_ERR "%s DMA alloc error\n", __func__);
#else
	host->dma_tx_buf = dma_alloc_coherent(NULL,
					      8 * mmc->max_blk_count,
					      &host->dma_tx_dmahandle,
					      GFP_DMA | GFP_KERNEL);
#endif

#if defined(CONFIG_KINETIS_GPIO_INT)
	esdhc_reset(host, ESDHC_INIT_CARD);
	host->card_insert = 1;
	mmc_detect_change(host->mmc, msecs_to_jiffies(500));
#else
	/* clear Card Detect pending int */
	*(volatile unsigned int *)0x4004D0A0 = (1 << 28);
	DBG("cd_sw %x %x\n", *(volatile unsigned int *)0x400ff110,
		fsl_readl(host->ioaddr + ESDHC_INT_STATUS));
#endif

	return 0;

unaddhost:
	free_irq(card_detect_extern_irq, host);
untasklet1:
#if defined(CONFIG_KINETIS_GPIO_INT)
	gpio_free(KINETIS_CARD_DETECT_GPIO);
gpio_busy:
#endif
	free_irq(host->irq, host);
untasklet:
	tasklet_kill(&host->card_tasklet);
	tasklet_kill(&host->finish_tasklet);
unmap:
	iounmap(host->ioaddr);
release:
	release_mem_region(host->addr, host->size);
free:
	mmc_free_host(mmc);

	return ret;
}

static void esdhc_remove_slot(struct platform_device *pdev, int slot)
{
	struct esdhc_chip *chip;
	struct mmc_host *mmc;
	struct esdhc_host *host;

	chip = platform_get_drvdata(pdev);
	host = chip->hosts[slot];
	mmc = host->mmc;

	chip->hosts[slot] = NULL;

	mmc_remove_host(mmc);

	esdhc_reset(host, ESDHC_RESET_ALL);

#if defined(CONFIG_KINETIS_GPIO_INT)
	gpio_free(KINETIS_CARD_DETECT_GPIO);
#endif

	free_irq(card_detect_extern_irq, host);

	free_irq(host->irq, host);

	del_timer_sync(&host->timer);

	tasklet_kill(&host->card_tasklet);
	tasklet_kill(&host->finish_tasklet);

	iounmap(host->ioaddr);

	release_mem_region(host->addr, host->size);

	mmc_free_host(mmc);
	DBG("%s: Exit....\n", __func__);
}

static int __init esdhc_probe(struct platform_device *pdev)
{
	int ret, i;
	u8 slots;
	struct esdhc_chip *chip;

	BUG_ON(pdev == NULL);
#if 0
	/* Slew Rate */
	MCF_GPIO_SRCR_SDHC = 3;
	MCF_GPIO_SRCR_IRQ0 = 3;

	/* Port Configuration */
	MCF_GPIO_PAR_ESDHCH = 0xFF;     /* DAT[3:0] */
	MCF_GPIO_PAR_ESDHCL = 0x0F;     /* CMD, CLK */
#endif
	MCF_ESDHC_VSR = 2;              /* disabled adma and set 3.0V */

#if 0
	MCF_INTC2_ICR31 = 2;            /* SDHC irqstat */
#if defined(CONFIG_ESDHC_DETECT_USE_EXTERN_IRQ1)
	/*this is irq1 hardware work round*/
	MCF_GPIO_PAR_IRQ0H |= 0x3;

	MCF_EPORT_EPPAR   = MCF_EPORT_EPPAR | MCF_EPORT_EPPAR_EPPA1_BOTH;
	MCF_EPORT_EPIER   = MCF_EPORT_EPIER | MCF_EPORT_EPIER_EPIE1;

	MCF_INTC0_ICR1  = 7;           /* IRQ1 */
	DBG("MCF_INTC0_ICR1 %x MCF_EPORT_EPPAR %x "
		"MCF_EPORT_EPFR %x MCF_EPORT_EPIER %x "
		"MCF_INTC0_IMRL %x MCF_INTC0_INTFRCL %x "
		"MCF_INTC0_IPRL %x\n",
			MCF_INTC0_ICR1, MCF_EPORT_EPPAR, MCF_EPORT_EPFR,
			MCF_EPORT_EPIER, MCF_INTC0_IMRL, MCF_INTC0_INTFRCL,
			MCF_INTC0_IPRL);
#elif defined(CONFIG_ESDHC_DETECT_USE_EXTERN_IRQ7)
	MCF_GPIO_PAR_IRQ0H |= MCF_GPIO_PAR_IRQH_IRQ7;

	MCF_EPORT_EPPAR   = MCF_EPORT_EPPAR | MCF_EPORT_EPPAR_EPPA7_BOTH;
	MCF_EPORT_EPIER   = MCF_EPORT_EPIER | MCF_EPORT_EPIER_EPIE7;

	MCF_INTC0_ICR7  = 2;           /* IRQ7 */
	DBG("MCF_INTC0_ICR7 %x MCF_EPORT_EPPAR %x\n",
		MCF_INTC0_ICR7, MCF_EPORT_EPPAR);
#else
	MCF_GPIO_PAR_IRQ0H |= MCF_GPIO_PAR_IRQH_IRQ7;

	MCF_EPORT_EPPAR   = MCF_EPORT_EPPAR | MCF_EPORT_EPPAR_EPPA7_BOTH;
	MCF_EPORT_EPIER   = MCF_EPORT_EPIER | MCF_EPORT_EPIER_EPIE7;

	MCF_INTC0_ICR7  = 2;           /* IRQ7 */
	DBG("MCF_INTC0_ICR1 %x MCF_EPORT_EPPAR %x\n",
		MCF_INTC0_ICR7, MCF_EPORT_EPPAR);
#endif
#endif
	slots = ESDHC_SLOTS_NUMBER;
	DBG("found %d slot(s)\n", slots);
	if (slots == 0) {
		printk(KERN_INFO "%s: slot err %d\n", __func__, slots);
		return -ENODEV;
	}

	chip = kmalloc(sizeof(struct esdhc_chip) +
		sizeof(struct esdhc_host *) * slots, GFP_KERNEL);
	if (!chip) {
		ret = -ENOMEM;
		printk(KERN_ERR "%s: kmalloc fail %x\n", __func__,
			(unsigned int)chip);
		goto err;
	}

	memset(chip, 0,
		sizeof(struct esdhc_chip) +
		sizeof(struct esdhc_host *) * slots);

	chip->pdev = pdev;
	chip->quirks = 0;//ESDHC_QUIRK_NO_CARD_NO_RESET;

	if (debug_quirks)
		chip->quirks = debug_quirks;

	chip->num_slots = slots;
	platform_set_drvdata(pdev, chip);

	for (i = 0; i < slots; i++) {
		ret = esdhc_probe_slot(pdev, i);
		if (ret) {
			for (i--; i >= 0; i--)
				esdhc_remove_slot(pdev, i);
			goto free;
		}
	}

	return 0;

free:
	platform_set_drvdata(pdev, NULL);
	kfree(chip);

err:
	return ret;
}

static int esdhc_remove(struct platform_device *pdev)
{
	int i;
	struct esdhc_chip *chip;

	chip = platform_get_drvdata(pdev);

	if (chip) {
		for (i = 0; i < chip->num_slots; i++)
			esdhc_remove_slot(pdev, i);

		platform_set_drvdata(pdev, NULL);

		kfree(chip);
	}

	return 0;
}

/*-------------------------------------------------------------------------*/
static struct platform_driver esdhc_driver = {
	.probe	=	esdhc_probe,
	.remove =	esdhc_remove,
	.suspend =	esdhc_suspend,
	.resume	=	esdhc_resume,
	.driver	= {
			.name   = DRIVER_NAME,
			.owner  = THIS_MODULE,
	},
};

/*****************************************************************************\
 *                                                                           *
 * Driver init/exit                                                          *
 *                                                                           *
\*****************************************************************************/

static int __init esdhc_drv_init(void)
{
	printk(KERN_INFO DRIVER_NAME
		": Freescale Enhanced Secure Digital Host"
		" Controller driver\n");

	return platform_driver_register(&esdhc_driver);
}

static void __exit esdhc_drv_exit(void)
{
	printk(KERN_INFO DRIVER_NAME
		": Freescale Enhanced Secure Digital Host"
		" Controller driver exit\n");
	platform_driver_unregister(&esdhc_driver);
}

module_init(esdhc_drv_init);
module_exit(esdhc_drv_exit);

module_param(debug_nodma, uint, 0444);
module_param(debug_forcedma, uint, 0444);
module_param(debug_quirks, uint, 0444);

MODULE_AUTHOR("Chenghu Wu<b16972@freescale.com>");
MODULE_DESCRIPTION("Enhanced Secure Digital Host Controller driver");
MODULE_LICENSE("GPL");

MODULE_PARM_DESC(debug_nodma, "Forcefully disable DMA transfers.");
MODULE_PARM_DESC(debug_forcedma, "Forcefully enable DMA transfers.");
MODULE_PARM_DESC(debug_quirks, "Force certain quirks.");

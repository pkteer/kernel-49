/*
 * MTD SPI driver for ST M25Pxx flash chips
 *
 * Author: Mike Lavender, mike@steroidmicros.com
 *
 * Copyright (c) 2005, Intec Automation Inc.
 *
 * Some parts are based on lart.c by Abraham Van Der Merwe
 *
 * Cleaned up and generalized based on mtd_dataflash.c
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/sched.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/semaphore.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include "ralink_gpio.h"
#include "ralink-flash.h"
#include "ralink_spi_bbu.h"

/*
 * #define SPI_DEBUG
 * #define TEST_CS1_FLASH
 */

#define BBU_MAX_BUSY_MS		2000
#define BBU_MAX_ERASE_MS	4000
#define BBU_MAX_WRITE_MS	2000

#if defined(CONFIG_MTD_SPI_READ_FAST)
#define RD_MODE_FAST
#endif

#define SPI_BBU_MAX_XFER	32

/****************************************************************************
 * SPI FLASH elementray definition and function
 ****************************************************************************/

#define FLASH_PAGESIZE		256

/* Flash opcodes. */
#define OPCODE_WREN		6	/* Write enable */
#define OPCODE_WRDI		4	/* Write disable */
#define OPCODE_RDSR		5	/* Read status register */
#define OPCODE_WRSR		1	/* Write status register */
#define OPCODE_READ		3	/* Read data bytes */
#define OPCODE_PP		2	/* Page program */
#define OPCODE_SE		0xD8	/* Sector erase */
#define OPCODE_RES		0xAB	/* Read Electronic Signature */
#define OPCODE_RDID		0x9F	/* Read JEDEC ID */
#define OPCODE_FAST_READ	0x0B	/* Read data bytes */
#define OPCODE_DOR		0x3B	/* Dual Output Read */
#define OPCODE_QOR		0x6B	/* Quad Output Read */
#define OPCODE_DIOR		0xBB	/* Dual IO High Performance Read */
#define OPCODE_QIOR		0xEB	/* Quad IO High Performance Read */

/* Status Register bits. */
#define SR_WIP			0x01	/* Write in progress */
#define SR_WEL			0x02	/* Write enable latch */
#define SR_BP0			0x04	/* Block protect 0 */
#define SR_BP1			0x08	/* Block protect 1 */
#define SR_BP2			0x10	/* Block protect 2 */
#define SR_BP3			0x20	/* Block protect 3 */
#define SR_SRWD			0x80	/* SR write protect */

#define OPCODE_BRRD		0x16
#define OPCODE_BRWR		0x17
#define OPCODE_RDCR		0x35

extern u32 get_surfboard_sysclk(void);

#if !defined(SPI_DEBUG)
#define ra_inl(addr)  (*(volatile u32 *)(addr))
#define ra_outl(addr, value)  (*(volatile u32 *)(addr) = (value))
#define ra_dbg(args...) do {} while(0)
/*#define ra_dbg(args...) do { printk(args); } while(0)*/
#else
#define ra_dbg(args...) do { printk(args); } while(0)
#define _ra_inl(addr)  (*(volatile u32 *)(addr))
#define _ra_outl(addr, value)  (*(volatile u32 *)(addr) = (value))
u32 ra_inl(u32 addr)
{
	u32 retval = _ra_inl(addr);
	pr_info("%s(%x) => %x\n", __func__, addr, retval);
	return retval;
}

u32 ra_outl(u32 addr, u32 val)
{
	_ra_outl(addr, val);
	pr_info("%s(%x, %x)\n", __func__, addr, val);
	return val;
}
#endif /* SPI_DEBUG */

#define ra_aor(addr, a_mask, o_value)  ra_outl(addr, (ra_inl(addr) & (a_mask)) | (o_value))
#define ra_and(addr, a_mask)  ra_aor(addr, a_mask, 0)
#define ra_or(addr, o_value)  ra_aor(addr, -1, o_value)

#define SPIC_READ_BYTES		(1 << 0)
#define SPIC_WRITE_BYTES	(1 << 1)

#define MODE_NORMAL		0
#define MODE_ATOMIC		1

#if defined(CONFIG_RALINK_SLIC_CONNECT_SPI_CS1)
/* SPI_SEM used in vport */
DEFINE_SEMAPHORE(SPI_SEM);
EXPORT_SYMBOL(SPI_SEM);
#endif

static u32 ra_spic_clk_div = 4;

static inline int bbu_spic_busy_wait_panic(void)
{
	unsigned int i;

	for (i = 0; i < BBU_MAX_BUSY_MS * 100; i++) {
		if ((ra_inl(SPI_REG_CTL) & SPI_CTL_BUSY) == 0)
			return 0;

		touch_softlockup_watchdog();
		udelay(10);
	}

	return -1;
}

static int bbu_spic_busy_wait(void)
{
	const unsigned long end = jiffies + msecs_to_jiffies(BBU_MAX_BUSY_MS);

	if (in_interrupt() || oops_in_progress)
		return bbu_spic_busy_wait_panic();

	do {
		if ((ra_inl(SPI_REG_CTL) & SPI_CTL_BUSY) == 0)
			return 0;

		cond_resched();
	} while (time_before(jiffies, end));

	pr_err("%s: SPI controller wait timed out\n", __func__);

	return -1;
}

static inline u32 bbu_spic_master(void)
{
	u32 reg_val;

	/* disable DOR/QOR/MORE_BUF, enable prefetch, select CS0 */
	reg_val = 0x8880;

	/* set clock */
	reg_val |= (ra_spic_clk_div << 16);

#if defined(TEST_CS1_FLASH) && !defined(CONFIG_RALINK_SLIC_CONNECT_SPI_CS1)
	reg_val |= (1 << 29);
#endif

	ra_outl(SPI_REG_MASTER, reg_val);

	return reg_val;
}

static void spic_init(void)
{
	u32 clk_sys, clk_div, clk_out;

	/*
	 * MT7621 sys_clk 220 MHz
	 * MT7628 sys_clk 193, 191 MHz
	 */
	clk_sys = get_surfboard_sysclk() / 1000000;
#if defined(CONFIG_MTD_SPI_FAST_CLOCK)
	clk_out = 50;
#else
	clk_out = 33;
#endif
	clk_div = clk_sys / clk_out;
	if ((clk_sys % clk_out) != 0)
		clk_div += 1;
	if (clk_div < 3)
		clk_div = 3;

	ra_spic_clk_div = clk_div - 2;

#if defined(TEST_CS1_FLASH) && !defined(CONFIG_RALINK_SLIC_CONNECT_SPI_CS1)
#if defined(CONFIG_RALINK_MT7628)
	ra_and(RALINK_REG_GPIOMODE, ~(3 << 4));
#endif
#endif

	bbu_spic_master();

	pr_info("MediaTek BBU SPI flash driver, SPI clock: %uMHz\n",
		clk_sys / clk_div);
}

struct chip_info {
	char		*name;
	u8		id;
	u32		jedec_id;
	unsigned int	sector_size;
	unsigned int	n_sectors;
	char		addr4b;
};

static struct chip_info chips_data[] = {
	/* REVISIT: fill in JEDEC ids, for parts that have them */
	{ "FL016AIF",		0x01, 0x02140000, 64 * 1024, 32,  0 },
	{ "FL064AIF",		0x01, 0x02160000, 64 * 1024, 128, 0 },

	{ "S25FL032P",		0x01, 0x02154D00, 64 * 1024, 64,  0 },
	{ "S25FL064P",		0x01, 0x02164D00, 64 * 1024, 128, 0 },
	{ "S25FL128P",		0x01, 0x20180301, 64 * 1024, 256, 0 },
	{ "S25FL129P",		0x01, 0x20184D01, 64 * 1024, 256, 0 },
	{ "S25FL256S",		0x01, 0x02194D01, 64 * 1024, 512, 1 },

	{ "S25FL116K",		0x01, 0x40150140, 64 * 1024, 32,  0 },
	{ "S25FL132K",		0x01, 0x40160140, 64 * 1024, 64,  0 },
	{ "S25FL164K",		0x01, 0x40170140, 64 * 1024, 128, 0 },

	{ "EN25F16",		0x1c, 0x31151c31, 64 * 1024, 32,  0 },
	{ "EN25F32",		0x1c, 0x31161c31, 64 * 1024, 64,  0 },
	{ "EN25Q32B",		0x1c, 0x30161c30, 64 * 1024, 64,  0 },
	{ "EN25F64",		0x1c, 0x20171c20, 64 * 1024, 128, 0 }, /* EN25P64 */
	{ "EN25Q64",		0x1c, 0x30171c30, 64 * 1024, 128, 0 },
	{ "EN25QH64A",		0x1c, 0x70171c70, 64 * 1024, 128, 0 },
	{ "EN25Q128",		0x1c, 0x30181c30, 64 * 1024, 256, 0 },
	{ "EN25QH128A",		0x1c, 0x70181c70, 64 * 1024, 256, 0 },
	{ "EN25QH256A",		0x1c, 0x70191c70, 64 * 1024, 512, 1 },

	{ "AT26DF161",		0x1f, 0x46000000, 64 * 1024, 32,  0 },
	{ "AT25DF321",		0x1f, 0x47000000, 64 * 1024, 64,  0 },

	{ "N25Q032A",		0x20, 0xba161000, 64 * 1024, 64,  0 },
	{ "N25Q064A",		0x20, 0xba171000, 64 * 1024, 128, 0 },
	{ "N25Q128A",		0x20, 0xba181000, 64 * 1024, 256, 0 },
	{ "N25Q256A",		0x20, 0xba191000, 64 * 1024, 512, 1 },
	{ "XM25QH256B",		0x20, 0x60192060, 64 * 1024, 512, 1 },
	{ "MT25QL512AB",	0x20, 0xba201044, 64 * 1024, 1024, 1 },

	{ "F25L32QA",		0x8c, 0x41168c41, 64 * 1024, 64,  0 }, /* ESMT */
	{ "F25L64QA",		0x8c, 0x41170000, 64 * 1024, 128, 0 }, /* ESMT */

	{ "MX25L1605D",		0xc2, 0x2015c220, 64 * 1024, 32,  0 },
	{ "MX25L3205D",		0xc2, 0x2016c220, 64 * 1024, 64,  0 },
	{ "MX25L6406E",		0xc2, 0x2017c220, 64 * 1024, 128, 0 },
	{ "MX25L12835F",	0xc2, 0x2018c220, 64 * 1024, 256, 0 },
	{ "MX25L25635F",	0xc2, 0x2019c220, 64 * 1024, 512, 1 },
	{ "MX25L51245G",	0xc2, 0x201ac220, 64 * 1024, 1024, 1 },

	{ "GD25Q32B",		0xc8, 0x40160000, 64 * 1024, 64,  0 },
	{ "GD25Q64B",		0xc8, 0x40170000, 64 * 1024, 128, 0 },
	{ "GD25Q64CSIG",	0xc8, 0x4017c840, 64 * 1024, 128, 0 },
	{ "GD25Q128C",		0xc8, 0x40180000, 64 * 1024, 256, 0 },
	{ "GD25Q128CSIG",	0xc8, 0x4018c840, 64 * 1024, 256, 0 },
	{ "GD25Q256CSIG",	0xc8, 0x4019c840, 64 * 1024, 512, 1 },

	{ "W25X32VS",		0xef, 0x30160000, 64 * 1024, 64,  0 },
	{ "W25Q32BV",		0xef, 0x40160000, 64 * 1024, 64,  0 },
	{ "W25Q64BV",		0xef, 0x40170000, 64 * 1024, 128, 0 }, /* S25FL064K */
	{ "W25Q128FV",		0xef, 0x40180000, 64 * 1024, 256, 0 },
	{ "W25Q256FV",		0xef, 0x40190000, 64 * 1024, 512, 1 },

#if defined(CONFIG_RT2880_FLASH_64M)
	{ "Unknown",		0x00, 0xffffffff, 64 * 1024, 1024, 1 },
#elif defined(CONFIG_RT2880_FLASH_32M) || defined(CONFIG_RT2880_FLASH_AUTO)
	{ "Unknown",		0x00, 0xffffffff, 64 * 1024, 512, 1 },
#elif defined(CONFIG_RT2880_FLASH_16M)
	{ "Unknown",		0x00, 0xffffffff, 64 * 1024, 256, 0 },
#elif defined(CONFIG_RT2880_FLASH_8M)
	{ "Unknown",		0x00, 0xffffffff, 64 * 1024, 128, 0 },
#else
	{ "Unknown",		0x00, 0xffffffff, 64 * 1024, 64,  0 },
#endif
};

struct flash_info {
	struct mutex		lock;
	struct mtd_info		mtd;
	struct chip_info	*chip;
};

static struct flash_info *flash;

static int bbu_mb_spic_trans(const u8 code, const u32 addr, u8 *buf,
			     const size_t n_tx, const size_t n_rx, int flag)
{
	u32 reg_mb, reg_ctl, reg_opcode, reg_data;
	int i, q, r;

	if (flag != SPIC_READ_BYTES && flag != SPIC_WRITE_BYTES)
		return -1;

	if (!flash)
		return -1;

	bbu_spic_busy_wait();

	reg_ctl = ra_inl(SPI_REG_CTL);
	reg_ctl &= ~SPI_CTL_TXRXCNT_MASK;
	reg_ctl &= ~SPI_CTL_ADDREXT_MASK;

	/* step 1. set opcode & address */
	if (flash->chip->addr4b) {
		reg_ctl |= (((u32)code << 24) & SPI_CTL_ADDREXT_MASK);
		reg_opcode = addr;
	} else
		reg_opcode = ((u32)code << 24) | (addr & 0xffffff);

	ra_outl(SPI_REG_OPCODE, reg_opcode);

	reg_mb = ra_inl(SPI_REG_MOREBUF);
	reg_mb &= ~SPI_MBCTL_TXRXCNT_MASK;
	reg_mb &= ~SPI_MBCTL_CMD_MASK;

	/* step 2. set cmd bit count to 32 (or 40) */
	if (flash->chip->addr4b)
		reg_mb |= ((5 << 3) << 24);
	else
		reg_mb |= ((4 << 3) << 24);

	/* step 3. set rx (miso_bit_cnt) and tx (mosi_bit_cnt) bit count */
	reg_mb |= ((n_rx << 3) << 12);
	reg_mb |=  (n_tx << 3);

	ra_outl(SPI_REG_MOREBUF, reg_mb);

#if defined(RD_MODE_FAST)
	/* clear data bit for dummy bits in Fast IO Read */
	if (flag & SPIC_READ_BYTES)
		ra_outl(SPI_REG_DATA0, 0);
#endif

	/* step 4. write DI/DO data #0 ~ #7 */
	if (flag & SPIC_WRITE_BYTES) {
		if (!buf)
			return -1;

		reg_data = 0;
		for (i = 0; i < n_tx; i++) {
			r = i % 4;
			if (r == 0)
				reg_data = 0;
			reg_data |= (*(buf + i) << (r * 8));
			if (r == 3 || (i + 1) == n_tx) {
				q = i / 4;
				ra_outl(SPI_REG_DATA(q), reg_data);
			}
		}
	}

	/* step 5. kick */
	ra_outl(SPI_REG_CTL, reg_ctl | SPI_CTL_START);

	/* step 6. wait spi_master_busy */
	bbu_spic_busy_wait();

	/* step 7. read DI/DO data #0 */
	if (flag & SPIC_READ_BYTES) {
		if (!buf)
			return -1;

		reg_data = 0;
		for (i = 0; i < n_rx; i++) {
			r = i % 4;
			if (r == 0) {
				q = i / 4;
				reg_data = ra_inl(SPI_REG_DATA(q));
			}
			*(buf + i) = (u8)(reg_data >> (r * 8));
		}
	}

	return 0;
}

static int bbu_spic_trans(const u8 code, const u32 addr, u8 *buf,
			  const size_t n_tx, const size_t n_rx, int flag)
{
	int addr4b = 0;
	u32 reg_ctl, reg_opcode, reg_data;

	bbu_spic_busy_wait();

	reg_ctl = ra_inl(SPI_REG_CTL);
	reg_ctl &= ~SPI_CTL_TXRXCNT_MASK;
	reg_ctl &= ~SPI_CTL_ADDREXT_MASK;

	/* step 1. set opcode & address */
	if ((reg_ctl & SPI_CTL_SIZE_MASK) == SPI_CTL_SIZE_MASK) {
		reg_ctl |= (addr & SPI_CTL_ADDREXT_MASK);
		addr4b = 1;
	}

	reg_opcode = ((addr & 0xffffff) << 8) | code;

#if defined(RD_MODE_FAST)
	/* clear data bit for dummy bits in Quad/Dual/Fast IO Read */
	if (flag & SPIC_READ_BYTES)
		ra_outl(SPI_REG_DATA0, 0);
#endif

	/* step 2. write DI/DO data #0 */
	if (flag & SPIC_WRITE_BYTES) {
		if (!buf)
			return -1;

		reg_data = 0;
		switch (n_tx) {
		case 8:
			reg_data |= (*(buf + 3) << 24);
		case 7:
			reg_data |= (*(buf + 2) << 16);
		case 6:
			reg_data |= (*(buf + 1) << 8);
		case 5:
			reg_data |= *buf;
			break;
		case 2:
			reg_opcode &= 0xff;
			if (addr4b) {
				reg_ctl &= ~SPI_CTL_ADDREXT_MASK;
				reg_ctl |= (*buf << 24);
			} else {
				reg_opcode |= (*buf << 24);
			}
			break;
		default:
			pr_err("%s: does not support write of length %d\n",
				__func__, n_tx);
			return -1;
		}

		ra_outl(SPI_REG_DATA0, reg_data);
	}

	ra_outl(SPI_REG_OPCODE, reg_opcode);

	/* step 3. set mosi_byte_cnt */
	reg_ctl |= (n_rx << 4);
	if (addr4b && n_tx >= 4)
		reg_ctl |= (n_tx + 1);
	else
		reg_ctl |= n_tx;
	ra_outl(SPI_REG_CTL, reg_ctl);

	/* step 4. kick */
	ra_outl(SPI_REG_CTL, reg_ctl | SPI_CTL_START);

	/* step 5. wait spi_master_busy */
	bbu_spic_busy_wait();

	/* step 6. read DI/DO data #0 */
	if (flag & SPIC_READ_BYTES) {
		if (!buf)
			return -1;

		reg_data = ra_inl(SPI_REG_DATA0);
		switch (n_rx) {
		case 4:
			*(buf + 3) = (u8)(reg_data >> 24);
		case 3:
			*(buf + 2) = (u8)(reg_data >> 16);
		case 2:
			*(buf + 1) = (u8)(reg_data >> 8);
		case 1:
			*buf = (u8)reg_data;
			break;
		default:
			pr_err("%s: read of length %d\n", __func__, n_rx);
			return -1;
		}
	}

	return 0;
}

/*
 * Set write enable latch with Write Enable command.
 * Returns negative if error occurred.
 */
static inline int raspi_write_enable(void)
{
	return bbu_spic_trans(OPCODE_WREN, 0, NULL, 1, 0, 0);
}

static inline int raspi_write_disable(void)
{
	return bbu_spic_trans(OPCODE_WRDI, 0, NULL, 1, 0, 0);
}

/*
 * Read the status register, returning its value in the location
 */
static inline int raspi_read_rg(u8 code, u8 *val)
{
	return bbu_spic_trans(code, 0, val, 1, 1, SPIC_READ_BYTES);
}

/*
 * write status register
 */
static int raspi_write_rg(u8 code, u8 *val)
{
	u32 address = (*val) << 24;

	/* put the value to be written in address register, so it will be transfered */
	return bbu_spic_trans(code, address, val, 2, 0, SPIC_WRITE_BYTES);
}

/*
 * read SPI flash device ID
 */
static int raspi_read_devid(u8 *rxbuf, const size_t n_rx)
{
	int retval;

	retval = bbu_spic_trans(OPCODE_RDID, 0, rxbuf, 1, n_rx, SPIC_READ_BYTES);
	if (retval)
		pr_err("%s: read returned %x\n", __func__, retval);

	return retval;
}

static inline int raspi_read_sr(u8 *val)
{
	return raspi_read_rg(OPCODE_RDSR, val);
}

static inline int raspi_write_sr(u8 *val)
{
	return raspi_write_rg(OPCODE_WRSR, val);
}

static int raspi_wait_write_ready(const unsigned int sleep_ms);

static int raspi_4byte_mode(int enable)
{
	int retval;
	u32 reg_ctl, reg_qctl;
	struct chip_info *chip = flash->chip;

	raspi_wait_write_ready(10);

	reg_ctl = ra_inl(SPI_REG_CTL);
	reg_qctl = ra_inl(SPI_REG_Q_CTL);

	if (enable) {
		reg_ctl |= SPI_CTL_SIZE_MASK;
		reg_qctl |= SPI_QCTL_FSADSZ_MASK;
	} else {
		reg_ctl &= ~SPI_CTL_SIZE_MASK;
		reg_ctl |= (0x2 << 19);
		reg_qctl &= ~SPI_QCTL_FSADSZ_MASK;
		reg_qctl |= (0x2 << 8);
	}

	ra_outl(SPI_REG_CTL, reg_ctl);
	ra_outl(SPI_REG_Q_CTL, reg_qctl);

	if (chip->id == 0x1) {
		/* Spansion */
		u8 br, br_cfn;

		br = (enable)? 0x81 : 0x0;
		raspi_write_rg(OPCODE_BRWR, &br);
		raspi_wait_write_ready(10);
		raspi_read_rg(OPCODE_BRRD, &br_cfn);

		if (br_cfn != br) {
			pr_err("%s: 4B mode set failed\n", __func__);
			return -1;
		}
	} else {
		u8 code;

		code = enable ? 0xB7 : 0xE9; /* B7: enter 4B, E9: exit 4B */

		/* for XMC XM25QH256B, EX4B is 29h */
		if (!enable &&
		     chip->id == 0x20 &&
		    (chip->jedec_id >> 24) == 0x60)
			code = 0x29;

		retval = bbu_spic_trans(code, 0, NULL, 1, 0, 0);

		/* for Winbond's W25Q256FV, need to clear extend address register */
		if ((!enable) && (chip->id == 0xef)) {
			code = 0x0;
			raspi_write_enable();
			raspi_write_rg(0xc5, &code);
		}

		if (retval != 0) {
			pr_err("%s: 4B mode set failed\n", __func__);
			return -1;
		}
	}

	return 0;
}

#if defined(CONFIG_MTD_SPI_FAST_CLOCK)
static void raspi_drive_strength(void)
{
	u8 code = 0;

	if (flash->chip->id == 0xef) {
		/* set Winbond DVP[1:0] as 10 (driving strength 50%) */
		if (raspi_read_rg(0x15, &code) == 0) {
			/* Winbond DVP[1:0] is 11 by default (driving strength 25%) */
			if ((code & 0x60) == 0x60) {
				code &= ~0x60;
				code |= 0x40;
				raspi_write_enable();
				raspi_write_rg(0x11, &code);
			}
		}
	}
}
#endif

/*
 * Set all sectors (global) unprotected if they are protected.
 * Returns negative if error occurred.
 */
static int raspi_unprotect(void)
{
	u8 sr_bp, sr = 0;

	if (raspi_read_sr(&sr) < 0) {
		pr_err("%s: read failed (%x)\n", __func__, sr);
		return -1;
	}

	sr_bp = SR_BP0 | SR_BP1 | SR_BP2;
	if (flash->chip->addr4b)
		sr_bp |= SR_BP3;

	if ((sr & sr_bp) != 0) {
		sr = 0;
		raspi_write_enable();
		raspi_write_sr(&sr);
	}
	return 0;
}

/*
 * Service routine to read status register until ready, or timeout occurs.
 * Returns non-zero if error.
 */
static inline int raspi_wait_write_ready_panic(const unsigned int sleep_ms)
{
	unsigned int i;

	for (i = 0; i < sleep_ms; i++) {
		u8 sr = 0;

		if (raspi_read_sr(&sr) < 0)
			return -EIO;

		if (!(sr & SR_WIP))
			return 0;

		touch_softlockup_watchdog();
		mdelay(1);
	}

	return -1;
}

static int raspi_wait_write_ready(const unsigned int sleep_ms)
{
	const unsigned long end = jiffies + msecs_to_jiffies(sleep_ms);
	u8 sr = 0;

	if (in_interrupt() || oops_in_progress)
		return raspi_wait_write_ready_panic(sleep_ms);

	/* one chip guarantees max 5 msec wait here after page writes,
	 * but potentially three seconds (!) after page erase.
	 */
	do {
		if (raspi_read_sr(&sr) < 0) {
			pr_err("%s: failed to read status\n", __func__);
			return -EIO;
		}

		if (!(sr & SR_WIP))
			return 0;

		usleep_range(20, 50);
	} while (time_before(jiffies, end));

	pr_err("%s: %u ms. wait timed out (0x%02hhx)\n", __func__, sleep_ms, sr);

	return -EIO;
}

/*
 * Erase one sector of flash memory at offset ``offset'' which is any
 * address within the sector which should be erased.
 *
 * Returns 0 if successful, non-zero otherwise.
 */
static int raspi_erase_sector(u32 offset)
{
	/* Wait until finished previous write command. */
	if (raspi_wait_write_ready(10))
		return -EIO;

	/* Send write enable, then erase commands. */
	raspi_write_enable();
	bbu_spic_trans(OPCODE_SE, offset, NULL, 4, 0, 0);
	raspi_wait_write_ready(BBU_MAX_ERASE_MS);

	return 0;
}

/*
 * SPI device driver setup and teardown
 */
struct chip_info *chip_prob(void)
{
	struct chip_info *info;
	u8 buf[4] = {0};
	u32 jedec;
	int i, table_size;

	raspi_read_devid(buf, 4);

	jedec = (u32)(((u32)buf[1] << 24) |
		      ((u32)buf[2] << 16) |
		      ((u32)buf[3] <<  8));

	ra_dbg("device ID: %x %x %x %x\n", buf[0], buf[1], buf[2], buf[3]);

	table_size = ARRAY_SIZE(chips_data);

	for (i = 0; i < table_size - 1; i++) {
		info = &chips_data[i];
		if (info->id == buf[0]) {
			if ((info->jedec_id & 0xffffff00) == jedec)
				return info;
		}
	}

	/* use last stub item */
	info = &chips_data[table_size - 1];

	/* update stub item */
	info->id = buf[0];
	info->jedec_id = ((u32)buf[1] << 24) | ((u32)buf[2] << 16);

	switch (buf[2]) {
	case 0x15:
		info->n_sectors = 32;
		break;
	case 0x16:
		info->n_sectors = 64;
		break;
	case 0x17:
		info->n_sectors = 128;
		break;
	case 0x18:
		info->n_sectors = 256;
		break;
	case 0x19:
		info->n_sectors = 512;
		break;
	case 0x1a:
	case 0x20:
		info->n_sectors = 1024;
		break;
	}

	info->addr4b = (info->n_sectors > 256) ? 1 : 0;

	pr_warn("unrecognized SPI chip ID: %x (%x), "
		"please update the SPI driver\n", buf[0], jedec);

	return info;
}

/*
 * MTD implementation
 */

/*
 * Erase an address range on the flash chip.  The address range may extend
 * one or more erase sectors.  Return an error is there is a problem erasing.
 */
static int ramtd_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	u32 addr, len;
	int exit_code = 0;

	/* sanity checks */
	if (instr->addr + instr->len > flash->mtd.size)
		return -EINVAL;

	addr = instr->addr;
	len = instr->len;

	mutex_lock(&flash->lock);

#if defined(CONFIG_RALINK_SLIC_CONNECT_SPI_CS1)
	down(&SPI_SEM);
#endif

	bbu_spic_master();

	/* wait until finished previous command. */
	if (raspi_wait_write_ready(10)) {
		instr->state = MTD_ERASE_FAILED;
#if defined(CONFIG_RALINK_SLIC_CONNECT_SPI_CS1)
		up(&SPI_SEM);
#endif
		mutex_unlock(&flash->lock);
		return -EIO;
	}

	raspi_unprotect();

	if (flash->chip->addr4b)
		raspi_4byte_mode(1);

	/* now erase those sectors */
	while (len > 0) {
		if (raspi_erase_sector(addr)) {
			exit_code = -EIO;
			break;
		}
		addr += mtd->erasesize;
		len -= mtd->erasesize;
	}

	if (flash->chip->addr4b)
		raspi_4byte_mode(0);

#if defined(CONFIG_RALINK_SLIC_CONNECT_SPI_CS1)
	up(&SPI_SEM);
#endif

	instr->state = (exit_code == 0) ? MTD_ERASE_DONE : MTD_ERASE_FAILED;

	mutex_unlock(&flash->lock);

	if (exit_code == 0)
		mtd_erase_callback(instr);

	return exit_code;
}

/*
 * Read an address range from the flash chip.  The address range
 * may be any size provided it is within the physical boundaries.
 */
static int ramtd_read(struct mtd_info *mtd, loff_t from, size_t len,
		      size_t *retlen, u_char *buf)
{
	int rc;
	size_t rdlen = 0;
	u32 reg_master;
#if defined(RD_MODE_FAST)
	size_t n_tx = 1;
	u8 code = OPCODE_FAST_READ;
#else
	size_t n_tx = 0;
	u8 code = OPCODE_READ;
#endif

	/* sanity checks */
	if (len == 0)
		return 0;

	if (from + len > flash->mtd.size)
		return -EINVAL;

	/* Byte count starts at zero. */
	if (retlen)
		*retlen = 0;

	mutex_lock(&flash->lock);

#if defined(CONFIG_RALINK_SLIC_CONNECT_SPI_CS1)
	down(&SPI_SEM);
#endif

	reg_master = bbu_spic_master();

	/* Wait till previous write/erase is done. */
	if (raspi_wait_write_ready(10)) {
#if defined(CONFIG_RALINK_SLIC_CONNECT_SPI_CS1)
		up(&SPI_SEM);
#endif
		mutex_unlock(&flash->lock);
		return -EIO;
	}

	if (flash->chip->addr4b)
		raspi_4byte_mode(1);

	/* SPI mode = more byte mode */
	ra_outl(SPI_REG_MASTER, (reg_master | 0x4));

	while (rdlen < len) {
		size_t r_part = len - rdlen;

		if (r_part > SPI_BBU_MAX_XFER)
			r_part = SPI_BBU_MAX_XFER;

		rc = bbu_mb_spic_trans(code, from, (buf + rdlen), n_tx, r_part,
				       SPIC_READ_BYTES);
		if (rc != 0) {
			pr_err("%s: failed\n", __func__);
			break;
		}

		from += r_part;
		rdlen += r_part;
	}

	/* SPI mode = normal */
	ra_outl(SPI_REG_MASTER, reg_master);

	if (flash->chip->addr4b)
		raspi_4byte_mode(0);

#if defined(CONFIG_RALINK_SLIC_CONNECT_SPI_CS1)
	up(&SPI_SEM);
#endif

	mutex_unlock(&flash->lock);

	if (retlen)
		*retlen = rdlen;

	if (rdlen != len)
		return -EIO;

	return 0;
}

inline int ramtd_lock(struct mtd_info *mtd, loff_t to, uint64_t len)
{
	return 0;
}

inline int ramtd_unlock(struct mtd_info *mtd, loff_t to, uint64_t len)
{
	return 0;
}

/*
 * Write an address range to the flash chip.  Data must be written in
 * FLASH_PAGESIZE chunks.  The address range may be any size provided
 * it is within the physical boundaries.
 */
static int _ramtd_write(struct mtd_info *mtd, loff_t to, size_t len,
			size_t *retlen, const u_char *buf, const int atomic)
{
	u32 page_offset, page_size, reg_master;
	int rc = 0, exit_code = 0;
	int wrto, wrlen;
	char *wrbuf;
	int count = 0;

	if (retlen)
		*retlen = 0;

	/* sanity checks */
	if (len == 0)
		return 0;

	if (to + len > flash->mtd.size)
		return -EINVAL;

	if (likely(!atomic)) {
		mutex_lock(&flash->lock);
#if defined(CONFIG_RALINK_SLIC_CONNECT_SPI_CS1)
		down(&SPI_SEM);
#endif
	}

	reg_master = bbu_spic_master();

	/* wait until finished previous write command. */
	if (raspi_wait_write_ready(10)) {
		exit_code = -EIO;
		goto mtd_write_exit_unlock;
	}

	raspi_unprotect();

	if (flash->chip->addr4b)
		raspi_4byte_mode(1);

	/* what page do we start with? */
	page_offset = to % FLASH_PAGESIZE;

	/* write everything in PAGESIZE chunks */
	while (len > 0) {
		page_size = min_t(size_t, len, FLASH_PAGESIZE-page_offset);
		page_offset = 0;

		/* write the next page to flash */
		wrto = to;
		wrlen = page_size;
		wrbuf = (char *)buf;

		while (wrlen > 0) {
			int w_part = (wrlen > SPI_BBU_MAX_XFER) ? SPI_BBU_MAX_XFER : wrlen;

			raspi_wait_write_ready(BBU_MAX_WRITE_MS);
			raspi_write_enable();

			ra_outl(SPI_REG_MASTER, (reg_master | 0x4));
			rc = bbu_mb_spic_trans(OPCODE_PP, wrto, wrbuf, w_part, 0,
					       SPIC_WRITE_BYTES);
			ra_outl(SPI_REG_MASTER, reg_master);

			if (rc != 0)
				break;

			wrlen -= w_part;
			wrto  += w_part;
			wrbuf += w_part;
		}

		rc = page_size - wrlen;
		if (rc > 0) {
			if (retlen)
				*retlen += rc;

			if (rc < page_size) {
				exit_code = -EIO;
				pr_err("%s: returned 0x%x, page_size: 0x%x\n",
				       __func__, rc, page_size);
				goto mtd_write_exit_4byte;
			}
		}

		len -= page_size;
		to += page_size;
		buf += page_size;
		count++;
		if ((count & 0xf) == 0)
			raspi_wait_write_ready(BBU_MAX_WRITE_MS / 4);
	}

	raspi_wait_write_ready(BBU_MAX_WRITE_MS);

mtd_write_exit_4byte:
	if (flash->chip->addr4b)
		raspi_4byte_mode(0);

mtd_write_exit_unlock:
	if (likely(!atomic)) {
#if defined(CONFIG_RALINK_SLIC_CONNECT_SPI_CS1)
		up(&SPI_SEM);
#endif
		mutex_unlock(&flash->lock);
	}

	return exit_code;
}

static int ramtd_write(struct mtd_info *mtd, loff_t to, size_t len,
		       size_t *retlen, const u_char *buf)
{
	return _ramtd_write(mtd, to, len, retlen, buf, MODE_NORMAL);
}

/*
 * Write an address range to the flash chip.  Data must be written in
 * FLASH_PAGESIZE chunks.  The address range may be any size provided
 * it is within the physical boundaries.
 *
 * Write out some data during a kernel panic
 *
 * This is used by the mtdoops driver to save the dying messages from a
 * kernel which has panic'd.
 *
 * This routine ignores all of the locking used throughout the rest of the
 * driver, in order to ensure that the data gets written out no matter what
 * state this driver (and the flash chip itself)
 * was in when the kernel crashed.
 *
 * The implementation of this routine is intentionally similar to
 * ramtd_write(), in order to ease code maintenance.
 */
static int ramtd_write_panic(struct mtd_info *mtd, loff_t to, size_t len,
			     size_t *retlen, const u_char *buf)
{
	return _ramtd_write(mtd, to, len, retlen, buf, MODE_ATOMIC);
}

static int __init raspi_init(void)
{
	struct chip_info *chip;
#if defined(SPI_DEBUG)
	unsigned i;
#endif
	spic_init();

	chip = chip_prob();

	flash = kzalloc(sizeof(*flash), GFP_KERNEL);
	if (!flash)
		return -ENOMEM;

	mutex_init(&flash->lock);

	flash->chip = chip;
	flash->mtd.name = "raspi";

	flash->mtd.type = MTD_NORFLASH;
	flash->mtd.writesize = 1;
	flash->mtd.flags = MTD_CAP_NORFLASH;
	flash->mtd.size = chip->sector_size * chip->n_sectors;
	flash->mtd.erasesize = chip->sector_size;
	flash->mtd._erase = ramtd_erase;
	flash->mtd._read = ramtd_read;
	flash->mtd._write = ramtd_write;
	flash->mtd._panic_write = ramtd_write_panic;
	flash->mtd._lock = ramtd_lock;
	flash->mtd._unlock = ramtd_unlock;

#if defined(CONFIG_MTD_SPI_FAST_CLOCK)
	/* tune flash chip output driving strength */
	raspi_drive_strength();
#endif

	pr_info("SPI flash chip: %s (%02x %04x) (%u Kbytes)\n",
		chip->name, chip->id, chip->jedec_id,
		(uint32_t)flash->mtd.size / 1024);

#if defined(SPI_DEBUG)
	ra_dbg("mtd .name = %s, .size = 0x%.8x (%uM) "
			".erasesize = 0x%.8x (%uK) .numeraseregions = %d\n",
		flash->mtd.name,
		(uint32_t)flash->mtd.size,
		(uint32_t)flash->mtd.size / (1024 * 1024),
		flash->mtd.erasesize, flash->mtd.erasesize / 1024,
		flash->mtd.numeraseregions);

	if (flash->mtd.numeraseregions)
		for (i = 0; i < flash->mtd.numeraseregions; i++)
			ra_dbg("mtd.eraseregions[%d] = { .offset = 0x%.8x, "
				".erasesize = 0x%.8x (%uK), "
				".numblocks = %d }\n",
				i, (uint32_t)flash->mtd.eraseregions[i].offset,
				flash->mtd.eraseregions[i].erasesize,
				flash->mtd.eraseregions[i].erasesize / 1024,
				flash->mtd.eraseregions[i].numblocks);
#endif

	/* register the partitions */
	return mtd_device_parse_register(&flash->mtd, NULL, NULL, NULL, 0);
}

static void __exit raspi_exit(void)
{
	if (flash) {
		mtd_device_unregister(&flash->mtd);
		kfree(flash);
		flash = NULL;
	}
}

module_init(raspi_init);
module_exit(raspi_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Steven Liu");
MODULE_DESCRIPTION("MediaTek MTD SPI driver for flash chips");

#ifndef __ASM_MACH_MIPS_TC3262_RT_MMAP_H
#define __ASM_MACH_MIPS_TC3262_RT_MMAP_H

/* EN7512(3)/EN7516 */

#define PHYS_TO_K1(physaddr)		KSEG1ADDR(physaddr)
#define sysRegRead(phys)		(*(volatile unsigned int *)PHYS_TO_K1(phys))
#define sysRegWrite(phys, val)		((*(volatile unsigned int *)PHYS_TO_K1(phys)) = (val))

#define RALINK_SYSCTL_BASE		0xBFB00000
#define RALINK_TIMER_BASE		0xBFBF0100
#define RALINK_MEMCTRL_BASE		0xBFB20000
#define RALINK_PIO_BASE			0xBFBF0200
#define RALINK_I2C_BASE			0xBFBF8000
#define RALINK_UART_LITE_BASE		0xBFBF0000
#define RALINK_UART_LITE2_BASE		0xBFBF0300
#define RALINK_PCM_BASE			0xBFBD0000
#define RALINK_GDMA_BASE		0xBFB30000
#define RALINK_FRAME_ENGINE_BASE	0xBFB50000
#define RALINK_ETH_SW_BASE		0xBFB58000
#define RALINK_CRYPTO_ENGINE_BASE	0xBFB70000
#define RALINK_PCI_BASE			0xBFB80000
#define RALINK_PCI_PHY0_BASE		0xBFAF2000
#define RALINK_PCI_PHY1_BASE		0xBFAC0000
#define RALINK_USB_HOST_BASE		0x1FB90000
#define RALINK_USB_IPPC_BASE		0x1FA80700
#define RALINK_XHCI_HOST_BASE		0xBFB90000
#define RALINK_XHCI_UPHY_BASE		0xBFA80000
#define RALINK_SFC_BASE			0xBFA10000
#define RALINK_CHIP_SCU_BASE		0xBFA20000
#define RALINK_11N_MAC_BASE		0xBFB00000 // Unused

#ifdef CONFIG_MIPS_TC3262_1004K
/* GIC */
#define RALINK_GIC_BASE			0x1F8C0000
#define RALINK_GIC_ADDRSPACE_SZ		0x20000

/* GCMP */
#define RALINK_GCMP_BASE		0x1F8E0000
#define RALINK_GCMP_ADDRSPACE_SZ	0x8000

/* CM */
#define CM_GCR_REG0_BASE_VALUE		0x1C000000	/* CM region 0 base address (Palmbus) */
#define CM_GCR_REG0_MASK_VALUE		0x0000FC00	/* CM region 0 mask (64M) */

#define CM_GCR_REG1_BASE_VALUE		0x20000000	/* CM region 1 base address (PCIe) */
#define CM_GCR_REG1_MASK_VALUE		0x0000F000	/* CM region 1 mask (256M) */
#else
/* Interrupt Controller */
#define RALINK_INTCL_BASE		0xBFB40000
#define RALINK_INTCTL_UARTLITE		(1<<0)
#define RALINK_INTCTL_PIO		(1<<10)
#define RALINK_INTCTL_PCM		(1<<11)
#define RALINK_INTCTL_DMA		(1<<14)
#define RALINK_INTCTL_GSW		(1<<15)
#define RALINK_INTCTL_UHST		(1<<17)
#define RALINK_INTCTL_FE		(1<<21)
#define RALINK_INTCTL_QDMA		(1<<22)
#define RALINK_INTCTL_PCIE0		(1<<23)
#define RALINK_INTCTL_PCIE1		(1<<24)

/* Reset Control Register */
#define RALINK_INTC_RST			(1<<9)
#endif

/* Reset Control Register */
#define RALINK_I2S1_RST			(1<<0)
#define RALINK_FE_QDMA_LAN_RST		(1<<1)
#define RALINK_FE_QDMA_WAN_RST		(1<<2)
#define RALINK_PCM2_RST			(1<<4)
#define RALINK_PTM_MAC_RST		(1<<5)
#define RALINK_CRYPTO_RST		(1<<6)
#define RALINK_SAR_RST			(1<<7)
#define RALINK_TIMER_RST		(1<<8)
#define RALINK_BONDING_RST		(1<<10)
#define RALINK_PCM1_RST			(1<<11)
#define RALINK_UART_RST			(1<<12)
#define RALINK_PIO_RST			(1<<13)
#define RALINK_DMA_RST			(1<<14)
#define RALINK_I2C_RST			(1<<16)
#define RALINK_I2S2_RST			(1<<17)
#define RALINK_SPI_RST			(1<<18)
#define RALINK_UARTL_RST		(1<<19)
#define RALINK_FE_RST			(1<<21)
#define RALINK_UHST_RST			(1<<22)
#define RALINK_ESW_RST			(1<<23)
#define RALINK_SFC2_RST			(1<<25)
#define RALINK_PCIE0_RST		(1<<26)
#define RALINK_PCIE1_RST		(1<<27)
#define RALINK_PCIEHB_RST		(1<<29)

#endif
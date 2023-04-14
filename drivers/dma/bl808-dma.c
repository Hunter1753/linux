// SPDX-License-Identifier: GPL-2.0
/**
 * Based on bflb_dma.c, by Bouffalolab team
 * Based on bcm2835-dma.c
 * Based on altera-msgdma.c
 */

#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/of.h>
#include <linux/of_dma.h>

#define BL808_DMA_INTSTATUS_OFFSET         (0x0)  /* DMA_IntStatus */
#define BL808_DMA_INTTCSTATUS_OFFSET       (0x4)  /* DMA_IntTCStatus */
#define BL808_DMA_INTTCCLEAR_OFFSET        (0x8)  /* DMA_IntTCClear */
#define BL808_DMA_INTERRORSTATUS_OFFSET    (0xC)  /* DMA_IntErrorStatus */
#define BL808_DMA_INTERRCLR_OFFSET         (0x10) /* DMA_IntErrClr */
#define BL808_DMA_RAWINTTCSTATUS_OFFSET    (0x14) /* DMA_RawIntTCStatus */
#define BL808_DMA_RAWINTERRORSTATUS_OFFSET (0x18) /* DMA_RawIntErrorStatus */
#define BL808_DMA_ENBLDCHNS_OFFSET         (0x1C) /* DMA_EnbldChns */
#define BL808_DMA_SOFTBREQ_OFFSET          (0x20) /* DMA_SoftBReq */
#define BL808_DMA_SOFTSREQ_OFFSET          (0x24) /* DMA_SoftSReq */
#define BL808_DMA_SOFTLBREQ_OFFSET         (0x28) /* DMA_SoftLBReq */
#define BL808_DMA_SOFTLSREQ_OFFSET         (0x2C) /* DMA_SoftLSReq */
#define BL808_DMA_TOP_CONFIG_OFFSET        (0x30) /* DMA_Top_Config */
#define BL808_DMA_SYNC_OFFSET              (0x34) /* DMA_Sync */

#define BL808_DMA_CxSRCADDR_OFFSET (0x00) /* DMA_CxSrcAddr */
#define BL808_DMA_CxDSTADDR_OFFSET (0x04) /* DMA_CxDstAddr */
#define BL808_DMA_CxLLI_OFFSET     (0x08) /* DMA_CxLLI */
#define BL808_DMA_CxCONTROL_OFFSET (0x0C) /* DMA_CxControl */
#define BL808_DMA_CxCONFIG_OFFSET  (0x10) /* DMA_CxConfig */

/* Register Bitfield definitions *****************************************************/

/* 0x0 : BL808_DMA_IntStatus */
#define BL808_DMA_INTSTATUS_SHIFT UL(0)
#define BL808_DMA_INTSTATUS_MASK  (0xff << BL808_DMA_INTSTATUS_SHIFT)

/* 0x4 : BL808_DMA_IntTCStatus */
#define BL808_DMA_INTTCSTATUS_SHIFT UL(0)
#define BL808_DMA_INTTCSTATUS_MASK  (0xff << BL808_DMA_INTTCSTATUS_SHIFT)

/* 0x8 : BL808_DMA_IntTCClear */
#define BL808_DMA_INTTCCLEAR_SHIFT UL(0)
#define BL808_DMA_INTTCCLEAR_MASK  (0xff << BL808_DMA_INTTCCLEAR_SHIFT)

/* 0xC : BL808_DMA_IntErrorStatus */
#define BL808_DMA_INTERRORSTATUS_SHIFT UL(0)
#define BL808_DMA_INTERRORSTATUS_MASK  (0xff << BL808_DMA_INTERRORSTATUS_SHIFT)

/* 0x10 : BL808_DMA_IntErrClr */
#define BL808_DMA_INTERRCLR_SHIFT UL(0)
#define BL808_DMA_INTERRCLR_MASK  (0xff << BL808_DMA_INTERRCLR_SHIFT)

/* 0x14 : BL808_DMA_RawIntTCStatus */
#define BL808_DMA_RAWINTTCSTATUS_SHIFT UL(0)
#define BL808_DMA_RAWINTTCSTATUS_MASK  (0xff << BL808_DMA_RAWINTTCSTATUS_SHIFT)

/* 0x18 : BL808_DMA_RawIntErrorStatus */
#define BL808_DMA_RAWINTERRORSTATUS_SHIFT UL(0)
#define BL808_DMA_RAWINTERRORSTATUS_MASK  (0xff << BL808_DMA_RAWINTERRORSTATUS_SHIFT)

/* 0x1C : BL808_DMA_EnbldChns */
#define BL808_DMA_ENABLEDCHANNELS_SHIFT UL(0)
#define BL808_DMA_ENABLEDCHANNELS_MASK  (0xff << BL808_DMA_ENABLEDCHANNELS_SHIFT)

/* 0x20 : BL808_DMA_SoftBReq */
#define BL808_DMA_SOFTBREQ_SHIFT UL(0)
#define BL808_DMA_SOFTBREQ_MASK  (0xffffffff << BL808_DMA_SOFTBREQ_SHIFT)

/* 0x24 : BL808_DMA_SoftSReq */
#define BL808_DMA_SOFTSREQ_SHIFT UL(0)
#define BL808_DMA_SOFTSREQ_MASK  (0xffffffff << BL808_DMA_SOFTSREQ_SHIFT)

/* 0x28 : BL808_DMA_SoftLBReq */
#define BL808_DMA_SOFTLBREQ_SHIFT UL(0)
#define BL808_DMA_SOFTLBREQ_MASK  (0xffffffff << BL808_DMA_SOFTLBREQ_SHIFT)

/* 0x2C : BL808_DMA_SoftLSReq */
#define BL808_DMA_SOFTLSREQ_SHIFT UL(0)
#define BL808_DMA_SOFTLSREQ_MASK  (0xffffffff << BL808_DMA_SOFTLSREQ_SHIFT)

/* 0x30 : BL808_DMA_Top_Config */
#define BL808_DMA_E BIT(0)
#define BL808_DMA_M BIT(1)

/* 0x34 : BL808_DMA_Sync */
#define BL808_DMA_SYNC_SHIFT UL(0)
#define BL808_DMA_SYNC_MASK  (0xffffffff << BL808_DMA_SYNC_SHIFT)

/* 0x100 : BL808_DMA_CxSrcAddr */
#define BL808_DMA_SRCADDR_SHIFT UL(0)
#define BL808_DMA_SRCADDR_MASK  (0xffffffff << BL808_DMA_SRCADDR_SHIFT)

/* 0x104 : BL808_DMA_CxDstAddr */
#define BL808_DMA_DSTADDR_SHIFT UL(0)
#define BL808_DMA_DSTADDR_MASK  (0xffffffff << BL808_DMA_DSTADDR_SHIFT)

/* 0x108 : BL808_DMA_CxLLI */
#define BL808_DMA_LLI_SHIFT UL(0)
#define BL808_DMA_LLI_MASK  (0xffffffff << BL808_DMA_LLI_SHIFT)

/* 0x10C : BL808_DMA_CxControl */
#define BL808_DMA_TRANSFERSIZE_SHIFT UL(0)
#define BL808_DMA_TRANSFERSIZE_MASK  (0xfff << BL808_DMA_TRANSFERSIZE_SHIFT)
#define BL808_DMA_SBSIZE_SHIFT       UL(12)
#define BL808_DMA_SBSIZE_MASK        (0x3 << BL808_DMA_SBSIZE_SHIFT)
#define BL808_DMA_DST_MIN_MODE       BIT(14)
#define BL808_DMA_DBSIZE_SHIFT       UL(15)
#define BL808_DMA_DBSIZE_MASK        (0x3 << BL808_DMA_DBSIZE_SHIFT)
#define BL808_DMA_DST_ADD_MODE       BIT(17)
#define BL808_DMA_SWIDTH_SHIFT       UL(18)
#define BL808_DMA_SWIDTH_MASK        (0x3 << BL808_DMA_SWIDTH_SHIFT)
#define BL808_DMA_DWIDTH_SHIFT       UL(21)
#define BL808_DMA_DWIDTH_MASK        (0x3 << BL808_DMA_DWIDTH_SHIFT)
#define BL808_DMA_FIX_CNT_SHIFT      UL(23)
#define BL808_DMA_FIX_CNT_MASK       (0x7 << BL808_DMA_FIX_CNT_SHIFT)
#define BL808_DMA_SI                 BIT(26)
#define BL808_DMA_DI                 BIT(27)
#define BL808_DMA_PROT_SHIFT         UL(28)
#define BL808_DMA_PROT_MASK          (0x7 << BL808_DMA_PROT_SHIFT)
#define BL808_DMA_I                  BIT(31)

/* 0x110 : BL808_DMA_CxConfig */
#define BL808_DMA_E                   BIT(0)
#define BL808_DMA_SRCPERIPHERAL_SHIFT UL(1)
#define BL808_DMA_SRCPERIPHERAL_MASK  (0x1f << BL808_DMA_SRCPERIPHERAL_SHIFT)
#define BL808_DMA_DSTPERIPHERAL_SHIFT UL(6)
#define BL808_DMA_DSTPERIPHERAL_MASK  (0x1f << BL808_DMA_DSTPERIPHERAL_SHIFT)
#define BL808_DMA_FLOWCNTRL_SHIFT     UL(11)
#define BL808_DMA_FLOWCNTRL_MASK      (0x7 << BL808_DMA_FLOWCNTRL_SHIFT)
#define BL808_DMA_IE                  BIT(14)
#define BL808_DMA_ITC                 BIT(15)
#define BL808_DMA_L                   BIT(16)
#define BL808_DMA_A                   BIT(17)
#define BL808_DMA_H                   BIT(18)
#define BL808_DMA_LLICOUNTER_SHIFT    UL(20)
#define BL808_DMA_LLICOUNTER_MASK     (0x3ff << BL808_DMA_LLICOUNTER_SHIFT)

#define BL808_DMA_SUPPORTED_PERIPHERALS_UART  BIT(0)
#define BL808_DMA_SUPPORTED_PERIPHERALS_I2C   BIT(1)
#define BL808_DMA_SUPPORTED_PERIPHERALS_SPI   BIT(2)
#define BL808_DMA_SUPPORTED_PERIPHERALS_ADC   BIT(3)
#define BL808_DMA_SUPPORTED_PERIPHERALS_IR    BIT(4)
#define BL808_DMA_SUPPORTED_PERIPHERALS_GPIO  BIT(5)
#define BL808_DMA_SUPPORTED_PERIPHERALS_Audio BIT(6)
#define BL808_DMA_SUPPORTED_PERIPHERALS_I2S   BIT(7)
#define BL808_DMA_SUPPORTED_PERIPHERALS_PDM   BIT(8)
#define BL808_DMA_SUPPORTED_PERIPHERALS_DBI   BIT(9)
#define BL808_DMA_SUPPORTED_PERIPHERALS_DSI   BIT(10)

#define BL808_DMA_SUPPORTED_PERIPHERALS_COM 	(BL808_DMA_SUPPORTED_PERIPHERALS_UART | \
												 BL808_DMA_SUPPORTED_PERIPHERALS_I2C | \
												 BL808_DMA_SUPPORTED_PERIPHERALS_SPI)
#define BL808_DMA_SUPPORTED_PERIPHERALS_DMA 	(BL808_DMA_SUPPORTED_PERIPHERALS_COM | \
												 BL808_DMA_SUPPORTED_PERIPHERALS_ADC | \
												 BL808_DMA_SUPPORTED_PERIPHERALS_IR | \
												 BL808_DMA_SUPPORTED_PERIPHERALS_GPIO | \
												 BL808_DMA_SUPPORTED_PERIPHERALS_Audio | \
												 BL808_DMA_SUPPORTED_PERIPHERALS_I2S | \
												 BL808_DMA_SUPPORTED_PERIPHERALS_PDM)
#define BL808_DMA_SUPPORTED_PERIPHERALS_DMAMM	(BL808_DMA_SUPPORTED_PERIPHERALS_COM | \
												 BL808_DMA_SUPPORTED_PERIPHERALS_DBI | \
												 BL808_DMA_SUPPORTED_PERIPHERALS_DSI)

struct bl808_dma_adapter_data {
	u8 channels;
	/**
	 * BIT: Peripheral
	 *   0: UART
	 *   1: I2C
	 *   2: SPI
	 *   3: ADC
	 *   4: IR
	 *   5: GPIO
	 *   6: Audio
	 *   7: I2S
	 *   8: PDM
	 *   9: DBI
	 *  10: DSI
	 */
	u16 supported_peripherals;
};

/**
 * struct bl808_dmadev - BL808 DMA controller
 * @ddev: DMA device
 * @base: base address of register map
 * @zero_page: bus address of zero page (to detect transactions copying from
 *	zero page and avoid accessing memory if so)
 */
struct bl808_dmadev {
	struct dma_device ddev;
	void __iomem *base;
	dma_addr_t zero_page;
};

struct bl808_dma_cb {
	uint32_t info;
	uint32_t src;
	uint32_t dst;
	uint32_t length;
	uint32_t stride;
	uint32_t next;
	uint32_t pad[2];
};

struct bl808_dma_cb_entry {
	struct bl808_dma_cb *cb;
	dma_addr_t paddr;
};

struct bl808_dma_chan {
	struct virt_dma_chan vc;

	struct dma_slave_config	cfg;
	unsigned int dreq;

	int ch;
	struct bl808_dma_desc *desc;
	struct dma_pool *cb_pool;

	void __iomem *chan_base;
	int irq_number;
	unsigned int irq_flags;

	bool is_lite_channel;
};

struct bl808_dma_desc {
	struct bl808_dma_chan *c;
	struct virt_dma_desc vd;
	enum dma_transfer_direction dir;

	unsigned int frames;
	size_t size;

	bool cyclic;

	struct bl808_dma_cb_entry cb_list[];
};

static void bl808_dma_free(struct bl808_dmadev *od)
{
	struct bl808_dma_chan *c, *next;

	list_for_each_entry_safe(c, next, &od->ddev.channels, vc.chan.device_node) {
		list_del(&c->vc.chan.device_node);
		tasklet_kill(&c->vc.task);
	}

	dma_unmap_page_attrs(od->ddev.dev, od->zero_page, PAGE_SIZE, DMA_TO_DEVICE, DMA_ATTR_SKIP_CPU_SYNC);
}

static int bl808_dma_probe(struct platform_device *pdev)
{
	struct bl808_dmadev *od;
	struct resource *res;
	void __iomem *base;

	return -1;
}

static int bl808_dma_remove(struct platform_device *pdev)
{
	struct bl808_dma_dev *od = platform_get_drvdata(pdev);

	dma_async_device_unregister(&od->ddev);
	bl808_dma_free(od);

	return 0;
}

static const struct bl808_dma_adapter_data bl808_dma0_data = {
	.channels = 8,
	.supported_peripherals = BL808_DMA_SUPPORTED_PERIPHERALS_DMA,
};

static const struct bl808_dma_adapter_data bl808_dma1_data = {
	.channels = 4,
	.supported_peripherals = BL808_DMA_SUPPORTED_PERIPHERALS_DMA,
};

static const struct bl808_dma_adapter_data bl808_dma2_data = {
	.channels = 8,
	.supported_peripherals = BL808_DMA_SUPPORTED_PERIPHERALS_DMAMM,
};

static const struct of_device_id bl808_dma_of_match[] = {
		{ .compatible = "bflb,bl808-dma0", .data = &bl808_dma0_data},
		{ .compatible = "bflb,bl808-dma1", .data = &bl808_dma1_data},
		{ .compatible = "bflb,bl808-dma2", .data = &bl808_dma2_data},
		{},
};
MODULE_DEVICE_TABLE(of, bl808_dma_of_match);

static struct platform_driver bl808_dma_driver = {
		.probe		= bl808_dma_probe,
		.remove		= bl808_dma_remove,
		.driver		= {
				.name	= "bl808-dma",
				.of_match_table = bl808_dma_of_match,
		},
};

module_platform_driver(bl808_dma_driver);

MODULE_AUTHOR("Alessandro Guttrof <hunter1753@gmail.com>");
MODULE_DESCRIPTION("bl808 dma engine driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:bl808-dma");
// SPDX-License-Identifier: GPL-2.0
/**
 * Based on bflb_dma.c, by Bouffalolab team
 * Based on bcm2835-dma.c
 * Based on altera-msgdma.c
 * Based on xilinx_dma.c
 */

#include "bl808-dma.h"

void bl808_dma2_isr(int irq, void *data)
{
	struct bl808_dma_dev *dma_dev = data;
    u32 val;

	val = bl808_dma_readl(dma_dev, BL808_DMA_INTSTATUS_OFFSET);

    bl808_dma_writel(dma_dev, BL808_DMA_INTTCCLEAR_OFFSET, val);

    for (u8 i = 0; i < 8; i++) {
        if (val & (1 << i)) {
            dma_callback[1][i].handler(dma_callback[2][i].arg);
        }
    }
}

static int bl808_dma2_probe(struct platform_device *pdev)
{
	struct bl808_dma_device *od;
	struct resource *res;
	void __iomem *base;

	return -1;
}

static const struct of_device_id bl808_dma_of_match[] = {
		{ .compatible = "bflb,bl808-dma2" },
		{},
};
MODULE_DEVICE_TABLE(of, bl808_dma_of_match);

static struct platform_driver bl808_dma_driver = {
		.probe		= bl808_dma2_probe,
		.remove		= bl808_dma_remove,
		.driver		= {
				.name	= "bl808-dma2",
				.of_match_table = bl808_dma_of_match,
		},
};

module_platform_driver(bl808_dma_driver);

MODULE_AUTHOR("Alessandro Guttrof <hunter1753@gmail.com>");
MODULE_DESCRIPTION("bl808 dma2 engine driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:bl808-dma-mm");
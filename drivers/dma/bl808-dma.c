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

struct bl808_cb_entry {
	struct bl808_dma_cb *cb;
	dma_addr_t paddr;
};

struct bl808_chan {
	struct virt_dma_chan vc;

	struct dma_slave_config	cfg;
	unsigned int dreq;

	int ch;
	struct bl808_desc *desc;
	struct dma_pool *cb_pool;

	void __iomem *chan_base;
	int irq_number;
	unsigned int irq_flags;

	bool is_lite_channel;
};

struct bl808_desc {
	struct bl808_chan *c;
	struct virt_dma_desc vd;
	enum dma_transfer_direction dir;

	unsigned int frames;
	size_t size;

	bool cyclic;

	struct bl808_cb_entry cb_list[];
};

static int bl808_dma_probe(struct platform_device *pdev){
	struct bl808_dmadev *od;
	struct resource *res;
	void __iomem *base;
	int rc;
	int i, j;
	int irq[BL808_DMA_MAX_DMA_CHAN_SUPPORTED + 1];
	int irq_flags;
	uint32_t chans_available;
	char chan_name[BL808_DMA_CHAN_NAME_SIZE];

	if (!pdev->dev.dma_mask)
		pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;

	rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (rc) {
		dev_err(&pdev->dev, "Unable to set DMA mask\n");
		return rc;
	}

	od = devm_kzalloc(&pdev->dev, sizeof(*od), GFP_KERNEL);
	if (!od)
		return -ENOMEM;

	dma_set_max_seg_size(&pdev->dev, 0x3FFFFFFF);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	od->base = base;

	dma_cap_set(DMA_SLAVE, od->ddev.cap_mask);
	dma_cap_set(DMA_PRIVATE, od->ddev.cap_mask);
	dma_cap_set(DMA_CYCLIC, od->ddev.cap_mask);
	dma_cap_set(DMA_MEMCPY, od->ddev.cap_mask);
	od->ddev.device_alloc_chan_resources = bl808_dma_alloc_chan_resources;
	od->ddev.device_free_chan_resources = bl808_dma_free_chan_resources;
	od->ddev.device_tx_status = bl808_dma_tx_status;
	od->ddev.device_issue_pending = bl808_dma_issue_pending;
	od->ddev.device_prep_dma_cyclic = bl808_dma_prep_dma_cyclic;
	od->ddev.device_prep_slave_sg = bl808_dma_prep_slave_sg;
	od->ddev.device_prep_dma_memcpy = bl808_dma_prep_dma_memcpy;
	od->ddev.device_config = bl808_dma_slave_config;
	od->ddev.device_terminate_all = bl808_dma_terminate_all;
	od->ddev.device_synchronize = bl808_dma_synchronize;
	od->ddev.src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	od->ddev.dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	od->ddev.directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV) |
			      BIT(DMA_MEM_TO_MEM);
	od->ddev.residue_granularity = DMA_RESIDUE_GRANULARITY_BURST;
	od->ddev.descriptor_reuse = true;
	od->ddev.dev = &pdev->dev;
	INIT_LIST_HEAD(&od->ddev.channels);

	platform_set_drvdata(pdev, od);

	od->zero_page = dma_map_page_attrs(od->ddev.dev, ZERO_PAGE(0), 0,
					   PAGE_SIZE, DMA_TO_DEVICE,
					   DMA_ATTR_SKIP_CPU_SYNC);
	if (dma_mapping_error(od->ddev.dev, od->zero_page)) {
		dev_err(&pdev->dev, "Failed to map zero page\n");
		return -ENOMEM;
	}

	/* Request DMA channel mask from device tree */
	if (of_property_read_u32(pdev->dev.of_node,
			"brcm,dma-channel-mask",
			&chans_available)) {
		dev_err(&pdev->dev, "Failed to get channel mask\n");
		rc = -EINVAL;
		goto err_no_dma;
	}

	/* get irqs for each channel that we support */
	for (i = 0; i <= BL808_DMA_MAX_DMA_CHAN_SUPPORTED; i++) {
		/* skip masked out channels */
		if (!(chans_available & (1 << i))) {
			irq[i] = -1;
			continue;
		}

		/* get the named irq */
		snprintf(chan_name, sizeof(chan_name), "dma%i", i);
		irq[i] = platform_get_irq_byname(pdev, chan_name);
		if (irq[i] >= 0)
			continue;

		/* legacy device tree case handling */
		dev_warn_once(&pdev->dev,
			      "missing interrupt-names property in device tree - legacy interpretation is used\n");
		/*
		 * in case of channel >= 11
		 * use the 11th interrupt and that is shared
		 */
		irq[i] = platform_get_irq(pdev, i < 11 ? i : 11);
	}

	/* get irqs for each channel */
	for (i = 0; i <= BL808_DMA_MAX_DMA_CHAN_SUPPORTED; i++) {
		/* skip channels without irq */
		if (irq[i] < 0)
			continue;

		/* check if there are other channels that also use this irq */
		irq_flags = 0;
		for (j = 0; j <= BL808_DMA_MAX_DMA_CHAN_SUPPORTED; j++)
			if ((i != j) && (irq[j] == irq[i])) {
				irq_flags = IRQF_SHARED;
				break;
			}

		/* initialize the channel */
		rc = bl808_dma_chan_init(od, i, irq[i], irq_flags);
		if (rc)
			goto err_no_dma;
	}

	dev_dbg(&pdev->dev, "Initialized %i DMA channels\n", i);

	/* Device-tree DMA controller registration */
	rc = of_dma_controller_register(pdev->dev.of_node,
			bl808_dma_xlate, od);
	if (rc) {
		dev_err(&pdev->dev, "Failed to register DMA controller\n");
		goto err_no_dma;
	}

	rc = dma_async_device_register(&od->ddev);
	if (rc) {
		dev_err(&pdev->dev,
			"Failed to register slave DMA engine device: %d\n", rc);
		goto err_no_dma;
	}

	dev_dbg(&pdev->dev, "Load BL808 DMA engine driver\n");

	return 0;

err_no_dma:
	bl808_dma_free(od);
	return rc;
}

static int bl808_dma_remove(struct platform_device *pdev)
{
    struct bl808_dma_dev *od = platform_get_drvdata(pdev);

        dma_async_device_unregister(&od->ddev);
	    bl808_dma_free(od);

        return 0;
}

static const struct of_device_id bl808_dma_of_match[] = {
        { .compatible = "bflb,bl808-dma" },
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
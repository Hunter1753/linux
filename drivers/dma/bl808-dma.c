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


// SPDX-License-Identifier: GPL-2.0
/**
 * Based on bflb_i2c.c, by Bouffalolab team
 * Based on i2c-bcm2835.c
 * Based on i2c-altera.c
*/

#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define BL808_I2C_CONFIG        0x0
#define BL808_I2C_STS           0x4
/* sub addr fields 0-3 */
#define BL808_I2C_SUB_ADDR      0x8
#define BL808_I2C_BUS_BUSY      0xc
/* length of start condition phase 0-3 */
#define BL808_I2C_PRD_START     0x10
/* length of stop condition phase 0-3 */
#define BL808_I2C_PRD_STOP      0x14
/* length of data condition phase 0-3 */
#define BL808_I2C_PRD_DATA      0x18

#define BL808_I2C_FIFO_CONFIG_0 0x80
#define BL808_I2C_FIFO_CONFIG_1 0x84
#define BL808_I2C_FIFO_WDATA    0x88
#define BL808_I2C_FIFO_RDATA    0x8c

/*
 * all fields are MSL
 */

/* 
 * Enable signal of master function
 * Asserting this bit will trigger the transaction, and should be
 * de-asserted after finish
 */
#define BL808_I2C_CONFIG_M_EN                   BIT(0)
/* 
 * Transfer direction
 * 0: write 1: read
 */
#define BL808_I2C_CONFIG_PKT_DIR                BIT(1)
/* enable input deglitch function */
#define BL808_I2C_CONFIG_DEG_EN                 BIT(2)
/* 
 * enable scl sync
 * needed for multimaster and clock stretching
 */
#define BL808_I2C_CONFIG_SCL_SYNC_EN            BIT(3)
#define BL808_I2C_CONFIG_SUB_ADDR_EN            BIT(4)
/*
 * sub address field byte count low
 * 00: 1 byte 01: 2 byte 10: 3 byte 11: 4 byte
 */
#define BL808_I2C_CONFIG_SUB_ADDR_BC_SHIFT      UL(5)
#define BL808_I2C_CONFIG_SUB_ADDR_BC_MASK       (0x3 << BL808_I2C_CONFIG_SUB_ADDR_BC_SHIFT)
#define BL808_I2C_CONFIG_10B_ADDR_EN            BIT(7)
/* target addr */
#define BL808_I2C_CONFIG_SLV_ADDR_SHIFT         UL(8)
#define BL808_I2C_CONFIG_SLV_ADDR_MASK          (0x3ff << BL808_I2C_CONFIG_SLV_ADDR_SHIFT)
/* packet length in byte */
#define BL808_I2C_CONFIG_PKT_LEN_SHIFT          UL(20)
#define BL808_I2C_CONFIG_PKT_LEN_MASK           (0xff << BL808_I2C_CONFIG_PKT_LEN_SHIFT)
/* de-glitch function cycle count */
#define BL808_I2C_CONFIG_DEG_CNT_SHIFT          UL(28)
#define BL808_I2C_CONFIG_DEG_CNT_MASK           (0xf << BL808_I2C_CONFIG_DEG_CNT_SHIFT)

/* transfer end interrupt */
#define BL808_I2C_STS_END_INT                   BIT(0)
/*
 * TX FIFO ready interrupt
 * autoclear on data push
 */
#define BL808_I2C_STS_TXF_INT                   BIT(1)
/*
 * RX FIFO ready interrupt
 * autoclear on data pop
 */
#define BL808_I2C_STS_RXF_INT                   BIT(2)
/* NACK received interrupt */
#define BL808_I2C_STS_NAK_INT                   BIT(3)
/* arbitration lost interrupt */
#define BL808_I2C_STS_ARB_INT                   BIT(4)
/*
 * TX/RX FIFO error interrupt
 * autoclear on FIFO over/underflow error flag
 * cleared
 */
#define BL808_I2C_STS_FER_INT                   BIT(5)
/* interrupt masks */
#define BL808_I2C_STS_END_MASK                  BIT(8)
#define BL808_I2C_STS_TXF_MASK                  BIT(9)
#define BL808_I2C_STS_RXF_MASK                  BIT(10)
#define BL808_I2C_STS_NAK_MASK                  BIT(11)
#define BL808_I2C_STS_ARB_MASK                  BIT(12)
#define BL808_I2C_STS_FER_MASK                  BIT(13)
#define BL808_I2C_STS_ALL_MASK                  (BL808_I2C_STS_END_MASK | \
                                                 BL808_I2C_STS_TXF_MASK | \
                                                 BL808_I2C_STS_RXF_MASK | \
                                                 BL808_I2C_STS_NAK_MASK | \
                                                 BL808_I2C_STS_ARB_MASK | \
                                                 BL808_I2C_STS_FER_MASK)
/* interrupt clears */
#define BL808_I2C_STS_END_CLR                   BIT(16)
#define BL808_I2C_STS_NAK_CLR                   BIT(19)
#define BL808_I2C_STS_ARB_CLR                   BIT(20)
/* interrupt enables */
#define BL808_I2C_STS_END_EN                    BIT(24)
#define BL808_I2C_STS_TXF_EN                    BIT(25)
#define BL808_I2C_STS_RXF_EN                    BIT(26)
#define BL808_I2C_STS_NAK_EN                    BIT(27)
#define BL808_I2C_STS_ARB_EN                    BIT(28)
#define BL808_I2C_STS_FER_EN                    BIT(29)
#define BL808_I2C_STS_ALL_EN                    (BL808_I2C_STS_END_EN | \
                                                 BL808_I2C_STS_TXF_EN | \
                                                 BL808_I2C_STS_RXF_EN | \
                                                 BL808_I2C_STS_NAK_EN | \
                                                 BL808_I2C_STS_ARB_EN | \
                                                 BL808_I2C_STS_FER_EN)

#define BL808_I2C_SUB_ADDR_B0_SHIFT             UL(0)
#define BL808_I2C_SUB_ADDR_B0_MASK              (0xff <<  BL808_I2C_SUB_ADDR_B0_SHIFT)
#define BL808_I2C_SUB_ADDR_B1_SHIFT             UL(8)
#define BL808_I2C_SUB_ADDR_B1_MASK              (0xff <<  BL808_I2C_SUB_ADDR_B1_SHIFT)
#define BL808_I2C_SUB_ADDR_B2_SHIFT             UL(16)
#define BL808_I2C_SUB_ADDR_B2_MASK              (0xff <<  BL808_I2C_SUB_ADDR_B2_SHIFT)
#define BL808_I2C_SUB_ADDR_B3_SHIFT             UL(24)
#define BL808_I2C_SUB_ADDR_B3_MASK              (0xff <<  BL808_I2C_SUB_ADDR_B3_SHIFT)

#define BL808_I2C_BUS_BUSY_IND                  BIT(0)
#define BL808_I2C_BUS_BUSY_CLR                  BIT(1)

#define BL808_I2C_PRD_S_PH_0_SHIFT              UL(0)
#define BL808_I2C_PRD_S_PH_0_MASK               (0xff <<  BL808_I2C_PRD_S_PH_0_SHIFT)
#define BL808_I2C_PRD_S_PH_1_SHIFT              UL(8)
#define BL808_I2C_PRD_S_PH_1_MASK               (0xff <<  BL808_I2C_PRD_S_PH_1_SHIFT)
#define BL808_I2C_PRD_S_PH_2_SHIFT              UL(16)
#define BL808_I2C_PRD_S_PH_2_MASK               (0xff <<  BL808_I2C_PRD_S_PH_2_SHIFT)
#define BL808_I2C_PRD_S_PH_3_SHIFT              UL(24)
#define BL808_I2C_PRD_S_PH_3_MASK               (0xff <<  BL808_I2C_PRD_S_PH_3_SHIFT)

#define BL808_I2C_PRD_P_PH_0_SHIFT              UL(0)
#define BL808_I2C_PRD_P_PH_0_MASK               (0xff <<  BL808_I2C_PRD_P_PH_0_SHIFT)
#define BL808_I2C_PRD_P_PH_1_SHIFT              UL(8)
#define BL808_I2C_PRD_P_PH_1_MASK               (0xff <<  BL808_I2C_PRD_P_PH_1_SHIFT)
#define BL808_I2C_PRD_P_PH_2_SHIFT              UL(16)
#define BL808_I2C_PRD_P_PH_2_MASK               (0xff <<  BL808_I2C_PRD_P_PH_2_SHIFT)
#define BL808_I2C_PRD_P_PH_3_SHIFT              UL(24)
#define BL808_I2C_PRD_P_PH_3_MASK               (0xff <<  BL808_I2C_PRD_P_PH_3_SHIFT)

#define BL808_I2C_PRD_D_PH_0_SHIFT              UL(0)
#define BL808_I2C_PRD_D_PH_0_MASK               (0xff <<  BL808_I2C_PRD_D_PH_0_SHIFT)
#define BL808_I2C_PRD_D_PH_1_SHIFT              UL(8)
#define BL808_I2C_PRD_D_PH_1_MASK               (0xff <<  BL808_I2C_PRD_D_PH_1_SHIFT)
#define BL808_I2C_PRD_D_PH_2_SHIFT              UL(16)
#define BL808_I2C_PRD_D_PH_2_MASK               (0xff <<  BL808_I2C_PRD_D_PH_2_SHIFT)
#define BL808_I2C_PRD_D_PH_3_SHIFT              UL(24)
#define BL808_I2C_PRD_D_PH_3_MASK               (0xff <<  BL808_I2C_PRD_D_PH_3_SHIFT)

#define BL808_I2C_FIFO_CONFIG_0_DMA_TX_EN       BIT(0)
#define BL808_I2C_FIFO_CONFIG_0_DMA_RX_EN       BIT(1)
#define BL808_I2C_FIFO_CONFIG_0_TX_FIFO_CLR     BIT(2)
#define BL808_I2C_FIFO_CONFIG_0_RX_FIFO_CLR     BIT(3)
#define BL808_I2C_FIFO_CONFIG_0_TX_FIFO_OVFLW   BIT(4)
#define BL808_I2C_FIFO_CONFIG_0_TX_FIFO_UDFLW   BIT(5)
#define BL808_I2C_FIFO_CONFIG_0_RX_FIFO_OVFLW   BIT(6)
#define BL808_I2C_FIFO_CONFIG_0_RX_FIFO_UDFLW   BIT(7)

#define BL808_I2C_FIFO_CONFIG_1_TX_FIFO_CNT_SHIFT UL(0)
#define BL808_I2C_FIFO_CONFIG_1_TX_FIFO_CNT_MASK  (0x3 << BL808_I2C_FIFO_CONFIG_1_TX_FIFO_CNT_SHIFT)
#define BL808_I2C_FIFO_CONFIG_1_RX_FIFO_CNT_SHIFT UL(8)
#define BL808_I2C_FIFO_CONFIG_1_RX_FIFO_CNT_MASK  (0x3 << BL808_I2C_FIFO_CONFIG_1_RX_FIFO_CNT_SHIFT)
#define BL808_I2C_FIFO_CONFIG_1_TX_FIFO_TH      BIT(16)
#define BL808_I2C_FIFO_CONFIG_1_RX_FIFO_TH      BIT(24)

struct bl808_i2c_dev {
        struct device *dev;
        void __iomem *regs;
        int irq;
        struct i2c_adapter adapter;
        struct completion completion;
        struct i2c_msg *curr_msg;
        struct clk *bus_clk;
        int num_msgs;
        u32 msg_err;
        u8 *msg_buf;
        u16 msg_buf_remaining;
};

static inline void bl808_i2c_writel(struct bl808_i2c_dev *i2c_dev,
				      u32 reg, u32 val)
{
	writel(val, i2c_dev->regs + reg);
}

static inline u32 bl808_i2c_readl(struct bl808_i2c_dev *i2c_dev, u32 reg)
{
	return readl(i2c_dev->regs + reg);
}

#define to_clk_bl808_i2c(_hw) container_of(_hw, struct clk_bl808_i2c, hw)
struct clk_bl808_i2c {
	struct clk_hw hw;
	struct bl808_i2c_dev *i2c_dev;
};

static u32 clk_bl808_i2c_calc_divider(unsigned long rate, unsigned long parent_rate) {
        u32 divider;
        
         /* TODO: use parent rate */
        divider = ((parent_rate/4)/rate) -1;

        return divider;
}

static int clk_bl808_i2c_set_rate(struct clk_hw *hw, unsigned long rate, unsigned long parent_rate) {
        struct clk_bl808_i2c *div = to_clk_bl808_i2c(hw);
        u32 val;
        u32 divider;

        divider = clk_bl808_i2c_calc_divider(rate, parent_rate);

        if(divider == 0) {
                return -EINVAL;
        }

        if(divider > 0xff) {
                divider = 0xff;
                dev_warn(div->i2c_dev->dev, "requested rate %lu is slower than minmum, setting to slowest possible rate\n", rate);
        }

        dev_dbg(div->i2c_dev->dev, "requested rate: %lu, parent rate: %lu, divider 0x%x\n", rate, parent_rate, divider);

        val =  (divider & 0xff) << BL808_I2C_PRD_S_PH_0_SHIFT;
        val |= (divider & 0xff) << BL808_I2C_PRD_S_PH_1_SHIFT;
        val |= (divider & 0xff) << BL808_I2C_PRD_S_PH_2_SHIFT;
        val |= (divider & 0xff) << BL808_I2C_PRD_S_PH_3_SHIFT;

        bl808_i2c_writel(div->i2c_dev, BL808_I2C_PRD_START, val);
        bl808_i2c_writel(div->i2c_dev, BL808_I2C_PRD_DATA, val);
        bl808_i2c_writel(div->i2c_dev, BL808_I2C_PRD_STOP, val);

        return 0;
}

static long clk_bl808_i2c_round_rate(struct clk_hw *hw, unsigned long rate, unsigned long *parent_rate) {
	u32 divider = clk_bl808_i2c_calc_divider(rate, *parent_rate);

	return BL808_TEMP_PARENT_CLK/((divider + 1) * 4);
}

static unsigned long clk_bl808_i2c_recalc_rate(struct clk_hw *hw, unsigned long parent_rate) {
        u32 val;
        u32 divider;
	struct clk_bl808_i2c *div = to_clk_bl808_i2c(hw);

        val = bl808_i2c_readl(div->i2c_dev, BL808_I2C_PRD_START);

        divider = val & 0xff;

	return BL808_TEMP_PARENT_CLK/((divider + 1) * 4);
}

static const struct clk_ops clk_bl808_i2c_ops = {
	.set_rate = clk_bl808_i2c_set_rate,
        .round_rate = clk_bl808_i2c_round_rate,
        .recalc_rate = clk_bl808_i2c_recalc_rate,
};

static struct clk *bl808_i2c_register_div(struct device *dev, struct clk *mclk, struct bl808_i2c_dev *i2c_dev)
{
	struct clk_init_data init;
	struct clk_bl808_i2c *priv;
	char name[32];
	const char *mclk_name;

	snprintf(name, sizeof(name), "%s_div", dev_name(dev));

	mclk_name = __clk_get_name(mclk);

	init.ops = &clk_bl808_i2c_ops;
	init.name = name;
	init.parent_names = (const char* []) { mclk_name };
	init.num_parents = 1;
	init.flags = 0;

	priv = devm_kzalloc(dev, sizeof(struct clk_bl808_i2c), GFP_KERNEL);
	if (priv == NULL)
		return ERR_PTR(-ENOMEM);

	priv->hw.init = &init;
	priv->i2c_dev = i2c_dev;

	clk_hw_register_clkdev(&priv->hw, "div", dev_name(dev));
	return devm_clk_register(dev, &priv->hw);
}

static void bl808_fill_tx_fifo(struct bl808_i2c_dev *i2c_dev) {
        u32 val;
        u32 temp = 0;
        u32 tx_fifo_free;

        val = bl808_i2c_readl(i2c_dev, BL808_I2C_FIFO_CONFIG_1);

        tx_fifo_free = (val & BL808_I2C_FIFO_CONFIG_1_TX_FIFO_CNT_MASK) >> BL808_I2C_FIFO_CONFIG_1_TX_FIFO_CNT_SHIFT;

        while (tx_fifo_free > 0) {

                if (i2c_dev->msg_buf_remaining >= 4) {
                        for(u8 i = 0; i < 4; i++) {
                                temp += i2c_dev->msg_buf[i] << ((i % 4) * 8);
                        }
                        i2c_dev->msg_buf += 4;
                        i2c_dev->msg_buf_remaining -= 4;
                } else if (i2c_dev->msg_buf_remaining > 0) {
                        for(u8 i = 0; i < i2c_dev->msg_buf_remaining; i++) {
                                temp += i2c_dev->msg_buf[i] << ((i % 4) * 8);
                        }
                        i2c_dev->msg_buf += i2c_dev->msg_buf_remaining;
                        i2c_dev->msg_buf_remaining -= i2c_dev->msg_buf_remaining;
                }
                
                bl808_i2c_writel(i2c_dev, BL808_I2C_FIFO_WDATA, temp);

                val = bl808_i2c_readl(i2c_dev, BL808_I2C_FIFO_CONFIG_1);
                tx_fifo_free = (val & BL808_I2C_FIFO_CONFIG_1_TX_FIFO_CNT_MASK) >> BL808_I2C_FIFO_CONFIG_1_TX_FIFO_CNT_SHIFT;
        }
}

static void bl808_drain_rx_fifo(struct bl808_i2c_dev *i2c_dev) {
        u32 val;
        u32 temp = 0;
        u32 rx_fifo_free;

        val = bl808_i2c_readl(i2c_dev, BL808_I2C_FIFO_CONFIG_1);

        rx_fifo_free = (val & BL808_I2C_FIFO_CONFIG_1_RX_FIFO_CNT_MASK) >> BL808_I2C_FIFO_CONFIG_1_RX_FIFO_CNT_SHIFT;

        while (rx_fifo_free > 0) {
                temp = bl808_i2c_readl(i2c_dev, BL808_I2C_FIFO_RDATA);
                if (i2c_dev->msg_buf_remaining >= 4) {
                        i2c_dev->msg_buf[0] = (temp >>  0) & 0xff;
                        i2c_dev->msg_buf[1] = (temp >>  8) & 0xff;
                        i2c_dev->msg_buf[2] = (temp >> 16) & 0xff;
                        i2c_dev->msg_buf[3] = (temp >> 24) & 0xff;

                        i2c_dev->msg_buf += 4;
                        i2c_dev->msg_buf_remaining -= i2c_dev->msg_buf_remaining;
                } else if (i2c_dev->msg_buf_remaining > 0) {
                        for (u8 i = 0; i < i2c_dev->msg_buf_remaining; i++) {
                                i2c_dev->msg_buf[i] = (u8)(temp >> (i * 8)) & 0xff;
                        }

                        i2c_dev->msg_buf += i2c_dev->msg_buf_remaining;
                        i2c_dev->msg_buf_remaining -= i2c_dev->msg_buf_remaining;
                }

                val = bl808_i2c_readl(i2c_dev, BL808_I2C_FIFO_CONFIG_1);
                rx_fifo_free = (val & BL808_I2C_FIFO_CONFIG_1_RX_FIFO_CNT_MASK) >> BL808_I2C_FIFO_CONFIG_1_RX_FIFO_CNT_SHIFT;
        }
}

static void bl808_i2c_addr_config(struct bl808_i2c_dev *i2c_dev, u16 target_addr, u16 sub_addr, u8 sub_addr_size, bool is_addr_10bit) {
        u32 val;
        val = bl808_i2c_readl(i2c_dev, BL808_I2C_CONFIG);

        if(sub_addr_size > 0) {
                val |= BL808_I2C_CONFIG_SUB_ADDR_EN;
                val &= ~BL808_I2C_CONFIG_SUB_ADDR_BC_MASK;
                val |= ((sub_addr_size -1) >> BL808_I2C_CONFIG_SUB_ADDR_BC_SHIFT);
        } else {
                val &= ~BL808_I2C_CONFIG_SUB_ADDR_EN;
        }

        val &= ~BL808_I2C_CONFIG_SLV_ADDR_MASK;
        val |= (target_addr << BL808_I2C_CONFIG_SLV_ADDR_SHIFT);

        if(is_addr_10bit) {
                val |= BL808_I2C_CONFIG_10B_ADDR_EN;
        } else {
                val &= ~BL808_I2C_CONFIG_10B_ADDR_EN;
        }

        val &= ~BL808_I2C_CONFIG_SCL_SYNC_EN;
        bl808_i2c_writel(i2c_dev, BL808_I2C_SUB_ADDR, sub_addr);
        bl808_i2c_writel(i2c_dev, BL808_I2C_CONFIG, val);
}

static void bl808_i2c_set_dir(struct bl808_i2c_dev *i2c_dev, bool is_in) {
        u32 val;
        val = bl808_i2c_readl(i2c_dev, BL808_I2C_CONFIG);

        if(is_in) {
                val |= BL808_I2C_CONFIG_PKT_DIR;
        } else {
                val &= ~BL808_I2C_CONFIG_PKT_DIR;
        }

        bl808_i2c_writel(i2c_dev, BL808_I2C_CONFIG, val);
}

static void bl808_i2c_set_datalen(struct bl808_i2c_dev *i2c_dev, u16 data_len) {
        u32 val;
        val = bl808_i2c_readl(i2c_dev, BL808_I2C_CONFIG);
        val &= ~BL808_I2C_CONFIG_PKT_LEN_MASK;
        val |= ((data_len - 1) << BL808_I2C_CONFIG_PKT_LEN_SHIFT) & BL808_I2C_CONFIG_PKT_LEN_MASK;
        bl808_i2c_writel(i2c_dev, BL808_I2C_CONFIG, val);
}

static void bl808_i2c_enable(struct bl808_i2c_dev *i2c_dev){
        u32 val;
        val = bl808_i2c_readl(i2c_dev, BL808_I2C_CONFIG);
        val |= BL808_I2C_CONFIG_M_EN;
        bl808_i2c_writel(i2c_dev, BL808_I2C_CONFIG, val);
}

static void bl808_i2c_disable(struct bl808_i2c_dev *i2c_dev){
        u32 val;
        /* disable i2c */
        val = bl808_i2c_readl(i2c_dev, BL808_I2C_CONFIG);
        val &= ~BL808_I2C_CONFIG_M_EN;
        bl808_i2c_writel(i2c_dev, BL808_I2C_CONFIG, val);
        /* Clear I2C fifo */
        val = bl808_i2c_readl(i2c_dev, BL808_I2C_FIFO_CONFIG_0);
        val |= BL808_I2C_FIFO_CONFIG_0_TX_FIFO_CLR;
        val |= BL808_I2C_FIFO_CONFIG_0_RX_FIFO_CLR;
        bl808_i2c_writel(i2c_dev, BL808_I2C_FIFO_CONFIG_0, val);
        /* Clear I2C interrupt status */
        val = bl808_i2c_readl(i2c_dev, BL808_I2C_STS);
        val |= BL808_I2C_STS_END_CLR;
        val |= BL808_I2C_STS_NAK_CLR;
        val |= BL808_I2C_STS_ARB_CLR;
        bl808_i2c_writel(i2c_dev, BL808_I2C_STS, val);
}

static void bl808_i2c_enable_interrupts(struct bl808_i2c_dev *i2c_dev, u32 interrupts) {
        u32 val;

        val = bl808_i2c_readl(i2c_dev, BL808_I2C_STS);

        val |= (interrupts & (BL808_I2C_STS_END_EN |
                BL808_I2C_STS_TXF_EN |
                BL808_I2C_STS_RXF_EN |
                BL808_I2C_STS_NAK_EN |
                BL808_I2C_STS_ARB_EN |
                BL808_I2C_STS_FER_EN));

        bl808_i2c_writel(i2c_dev, BL808_I2C_STS, val);
}

static void bl808_i2c_unmask_interrupts(struct bl808_i2c_dev *i2c_dev, u32 interrupts) {
        u32 val;

        val = bl808_i2c_readl(i2c_dev, BL808_I2C_STS);

        val &= ~(interrupts & (BL808_I2C_STS_END_MASK |
                BL808_I2C_STS_TXF_MASK |
                BL808_I2C_STS_RXF_MASK |
                BL808_I2C_STS_NAK_MASK |
                BL808_I2C_STS_ARB_MASK |
                BL808_I2C_STS_FER_MASK));

        bl808_i2c_writel(i2c_dev, BL808_I2C_STS, val);
}

static void bl808_i2c_disable_interrupts(struct bl808_i2c_dev *i2c_dev, u32 interrupts) {
        u32 val;

        val = bl808_i2c_readl(i2c_dev, BL808_I2C_STS);

        val &= ~(interrupts & (BL808_I2C_STS_END_EN |
                BL808_I2C_STS_TXF_EN |
                BL808_I2C_STS_RXF_EN |
                BL808_I2C_STS_NAK_EN |
                BL808_I2C_STS_ARB_EN |
                BL808_I2C_STS_FER_EN));

        bl808_i2c_writel(i2c_dev, BL808_I2C_STS, val);
}

static void bl808_i2c_mask_interrupts(struct bl808_i2c_dev *i2c_dev, u32 interrupts) {
        u32 val;

        val = bl808_i2c_readl(i2c_dev, BL808_I2C_STS);

        val |= (interrupts & (BL808_I2C_STS_END_MASK |
                BL808_I2C_STS_TXF_MASK |
                BL808_I2C_STS_RXF_MASK |
                BL808_I2C_STS_NAK_MASK |
                BL808_I2C_STS_ARB_MASK |
                BL808_I2C_STS_FER_MASK));

        bl808_i2c_writel(i2c_dev, BL808_I2C_STS, val);
}

static void bl808_i2c_clear_interrupts(struct bl808_i2c_dev *i2c_dev) {
        u32 val;

        val = bl808_i2c_readl(i2c_dev, BL808_I2C_STS);

        val |= (BL808_I2C_STS_END_CLR |
                BL808_I2C_STS_NAK_CLR |
                BL808_I2C_STS_ARB_CLR);

        bl808_i2c_writel(i2c_dev, BL808_I2C_STS, val);
}

static void bl808_i2c_clear_fifo_err(struct bl808_i2c_dev *i2c_dev) {
        u32 val;

        val = bl808_i2c_readl(i2c_dev, BL808_I2C_FIFO_CONFIG_0);

        val |= (BL808_I2C_FIFO_CONFIG_0_RX_FIFO_CLR |
                BL808_I2C_FIFO_CONFIG_0_TX_FIFO_CLR);

        bl808_i2c_writel(i2c_dev,  BL808_I2C_FIFO_CONFIG_0, val);
}

static void bl808_i2c_init(struct bl808_i2c_dev *i2c_dev) {

        bl808_i2c_disable(i2c_dev);

        bl808_i2c_mask_interrupts(i2c_dev, BL808_I2C_STS_ALL_MASK);

        bl808_i2c_disable_interrupts(i2c_dev, BL808_I2C_STS_ALL_EN);
}

static int bl808_i2c_start_transfer(struct bl808_i2c_dev *i2c_dev) {

        struct i2c_msg *msg = i2c_dev->curr_msg;
        struct i2c_msg *nxt_msg;
        bool is_ten_bit = (msg->flags & I2C_M_TEN);
        u16 subaddr = 0;
        u16 subaddr_size = 0;
        bool combined_message = false;

        if (!i2c_dev->num_msgs)
		return -EINVAL;

	i2c_dev->num_msgs--;
	i2c_dev->msg_buf = msg->buf;
	i2c_dev->msg_buf_remaining = msg->len;

        /* linux handles sub address via data bytes */
        if (i2c_dev->num_msgs > 0) {
                nxt_msg = i2c_dev->curr_msg + 1;
                combined_message = (msg->len <= 4) && !(msg->flags & I2C_M_RD) && (nxt_msg->flags & I2C_M_RD) && (msg->addr == nxt_msg->addr);
                if (combined_message) {
                        subaddr = 0;
                        for(u8 i = 0; i < msg->len; i++) {
                                subaddr += msg->buf[i] << (i * 8);
                        }
                        
                        subaddr_size = msg->len;
                        i2c_dev->curr_msg++;
                        msg = i2c_dev->curr_msg;
                        i2c_dev->num_msgs--;
                        i2c_dev->msg_buf = msg->buf;
                        i2c_dev->msg_buf_remaining = msg->len;
                } else {
                        subaddr = 0;
                        subaddr_size = 0;
                }
        }

        bl808_i2c_addr_config(i2c_dev, msg->addr, subaddr, subaddr_size, is_ten_bit);


        if (msg->len > 256) {
                return -EINVAL;
        }

        bl808_i2c_set_datalen(i2c_dev, msg->len);

	
        if (msg->flags & I2C_M_RD) {
                bl808_i2c_set_dir(i2c_dev, true);
                bl808_i2c_unmask_interrupts(i2c_dev, BL808_I2C_STS_ALL_MASK & ~BL808_I2C_STS_TXF_MASK);
                bl808_i2c_enable_interrupts(i2c_dev, BL808_I2C_STS_ALL_EN & ~BL808_I2C_STS_TXF_EN);
        } else {
                bl808_i2c_set_dir(i2c_dev, false);
                bl808_i2c_unmask_interrupts(i2c_dev, BL808_I2C_STS_ALL_MASK & ~BL808_I2C_STS_RXF_MASK);
                bl808_i2c_enable_interrupts(i2c_dev, BL808_I2C_STS_ALL_EN & ~BL808_I2C_STS_RXF_EN);
        }

        
        bl808_i2c_enable(i2c_dev);

        return 0;
}

static void bl808_i2c_finish_transfer(struct bl808_i2c_dev *i2c_dev){
        i2c_dev->curr_msg = NULL;
	i2c_dev->num_msgs = 0;

	i2c_dev->msg_buf = NULL;
	i2c_dev->msg_buf_remaining = 0;
}

static irqreturn_t bl808_i2c_isr(int this_isq, void *data) {
        struct bl808_i2c_dev *i2c_dev = data;
        u32 val;
        int ret;

        val = bl808_i2c_readl(i2c_dev, BL808_I2C_STS);

        if (!i2c_dev->curr_msg) {
                dev_err(i2c_dev->dev, "Unexpected interrupt (no running transfer)\n");
                goto complete;
        }

        if (val & BL808_I2C_STS_ARB_INT) {
                dev_dbg(i2c_dev->dev, "Arbitration lost\n");
                i2c_dev->msg_err = -EAGAIN;
                goto complete;
        } else if (val & BL808_I2C_STS_NAK_INT) {
                dev_dbg(i2c_dev->dev, "Could not get ACK\n");
                i2c_dev->msg_err = -ENXIO;
                goto complete;
        } else if (val & BL808_I2C_STS_FER_INT) {
                val = bl808_i2c_readl(i2c_dev, BL808_I2C_FIFO_CONFIG_0);
                if (val & BL808_I2C_FIFO_CONFIG_0_RX_FIFO_OVFLW) {
                        dev_err(i2c_dev->dev, "RX FIFO Overflow\n");
                } else if (val & BL808_I2C_FIFO_CONFIG_0_RX_FIFO_UDFLW) {
                        dev_err(i2c_dev->dev, "RX FIFO Underflow\n");
                } else if (val & BL808_I2C_FIFO_CONFIG_0_TX_FIFO_OVFLW) {
                        dev_err(i2c_dev->dev, "TX FIFO Overflow\n");
                } else if (val & BL808_I2C_FIFO_CONFIG_0_TX_FIFO_UDFLW) {
                        dev_err(i2c_dev->dev, "TX FIFO Underflow\n");
                }

                i2c_dev->msg_err = -EIO;
                bl808_i2c_clear_fifo_err(i2c_dev);

                goto complete;
        } else if (val & BL808_I2C_STS_END_INT) {
                if (i2c_dev->curr_msg->flags & I2C_M_RD) {
                        bl808_drain_rx_fifo(i2c_dev);
                }

                if (i2c_dev->msg_buf_remaining){
                        dev_err(i2c_dev->dev, "got end interrupt but msg_buf_remaining. %u\n", i2c_dev->msg_buf_remaining);
			i2c_dev->msg_err = -EREMOTEIO;
                } else{
			i2c_dev->msg_err = 0;
                }

                goto complete;
        } else if (val & BL808_I2C_STS_RXF_INT) {
                if(!i2c_dev->msg_buf_remaining) {
                        dev_err(i2c_dev->dev, "wants receive data to be popped, but no where to put\n");
                        i2c_dev->msg_err = -EREMOTEIO;
                        goto complete;
                }

                bl808_drain_rx_fifo(i2c_dev);

                if (i2c_dev->num_msgs && !i2c_dev->msg_buf_remaining) {
                        i2c_dev->curr_msg++;
                        ret = bl808_i2c_start_transfer(i2c_dev);
                        if (ret) {
                                i2c_dev->msg_err = ret;
                                goto complete;
                        }
                }

                return IRQ_HANDLED;
        } else if (val & BL808_I2C_STS_TXF_INT) {
                if(!i2c_dev->msg_buf_remaining) {
                        dev_dbg(i2c_dev->dev, "tx fifo free but nothing to tx anymore, masking\n");
                        bl808_i2c_mask_interrupts(i2c_dev, BL808_I2C_STS_TXF_MASK);
                        bl808_i2c_disable_interrupts(i2c_dev, BL808_I2C_STS_TXF_EN);
                        return IRQ_HANDLED;
                }

                bl808_fill_tx_fifo(i2c_dev);

                if (i2c_dev->num_msgs && !i2c_dev->msg_buf_remaining) {
                        i2c_dev->curr_msg++;
                        ret = bl808_i2c_start_transfer(i2c_dev);
                        if (ret) {
                                i2c_dev->msg_err = ret;
                                goto complete;
                        }
                }

                return IRQ_HANDLED;
        }

        dev_warn(i2c_dev->dev, "Unexpected interrupt: 0x%x\n", val);
        bl808_i2c_clear_interrupts(i2c_dev);

        return IRQ_HANDLED;

complete:
        bl808_i2c_disable(i2c_dev);
        bl808_i2c_clear_interrupts(i2c_dev);
        bl808_i2c_mask_interrupts(i2c_dev, BL808_I2C_STS_ALL_MASK);
        bl808_i2c_disable_interrupts(i2c_dev, BL808_I2C_STS_ALL_EN);
        complete(&i2c_dev->completion);

        return IRQ_HANDLED;
}

static int bl808_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[],
			    int num)
{
        struct bl808_i2c_dev *i2c_dev = i2c_get_adapdata(adap);
        unsigned long time_left;
        int ret;

        i2c_dev->curr_msg = msgs;
	i2c_dev->num_msgs = num;
	reinit_completion(&i2c_dev->completion);
        
        ret = bl808_i2c_start_transfer(i2c_dev);
        if (ret) {
                return i2c_dev->msg_err;
        }

        time_left = wait_for_completion_timeout(&i2c_dev->completion,
						adap->timeout);

	bl808_i2c_finish_transfer(i2c_dev);

        if (time_left == 0) {
                bl808_i2c_disable(i2c_dev);
                bl808_i2c_clear_interrupts(i2c_dev);
                bl808_i2c_mask_interrupts(i2c_dev, BL808_I2C_STS_ALL_MASK);
                bl808_i2c_disable_interrupts(i2c_dev, BL808_I2C_STS_ALL_EN);
                /* maybe reset bus here? */
                dev_err(i2c_dev->dev, "i2c transfer timed out\n");
		return -ETIMEDOUT;
        }

        if (!i2c_dev->msg_err){
                return num;
        }

        dev_dbg(i2c_dev->dev, "i2c transfer failed: 0x%x | %d\n", i2c_dev->msg_err, i2c_dev->msg_err);
	return i2c_dev->msg_err;
}

static u32 bl808_i2c_func(struct i2c_adapter *adap)
{
        return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL | I2C_FUNC_10BIT_ADDR;
}

static const struct i2c_algorithm bl808_i2c_algo = {
        .master_xfer	= bl808_i2c_xfer,
        .functionality	= bl808_i2c_func,
};

static int bl808_i2c_probe(struct platform_device *pdev){
        struct bl808_i2c_dev *i2c_dev;
        struct resource *mem;
        int ret;
        struct i2c_adapter *adap;
        struct clk *mclk;
        u32 bus_clk_rate;

        i2c_dev = devm_kzalloc(&pdev->dev, sizeof(*i2c_dev), GFP_KERNEL);
        if (!i2c_dev)
                return -ENOMEM;
        platform_set_drvdata(pdev, i2c_dev);
        i2c_dev->dev = &pdev->dev;
        init_completion(&i2c_dev->completion);

        mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
        i2c_dev->regs = devm_ioremap_resource(&pdev->dev, mem);
        if (IS_ERR(i2c_dev->regs))
                return PTR_ERR(i2c_dev->regs);

        mclk = devm_clk_get(&pdev->dev, NULL);
        if (IS_ERR(mclk))
                return dev_err_probe(&pdev->dev, PTR_ERR(mclk),
                                     "Could not get clock\n");

        i2c_dev->bus_clk = bl808_i2c_register_div(&pdev->dev, mclk, i2c_dev);

        if (IS_ERR(i2c_dev->bus_clk)) {
		dev_err(&pdev->dev, "Could not register clock\n");
		return PTR_ERR(i2c_dev->bus_clk);
	}

        ret = of_property_read_u32(pdev->dev.of_node, "clock-frequency",
                                   &bus_clk_rate);
        if (ret < 0) {
                dev_warn(&pdev->dev,
                         "Could not read clock-frequency property\n");
                bus_clk_rate = I2C_MAX_STANDARD_MODE_FREQ;
        }

        ret = clk_set_rate_exclusive(i2c_dev->bus_clk, bus_clk_rate);
        if (ret < 0) {
                dev_err(&pdev->dev, "Could not set clock frequency\n");
                return ret;
        }

        ret = clk_prepare_enable(i2c_dev->bus_clk);
        if (ret) {
                dev_err(&pdev->dev, "Couldn't prepare clock");
                goto err_put_exclusive_rate;
        }

        i2c_dev->irq = platform_get_irq(pdev, 0);
        if (i2c_dev->irq < 0) {
                ret = i2c_dev->irq;
                goto err_disable_unprepare_clk;
        }

        ret = request_irq(i2c_dev->irq, bl808_i2c_isr, IRQF_SHARED,
                          dev_name(&pdev->dev), i2c_dev);
        if (ret) {
                dev_err(&pdev->dev, "Could not request IRQ\n");
                goto err_disable_unprepare_clk;
        }

        adap = &i2c_dev->adapter;
        i2c_set_adapdata(adap, i2c_dev);
        adap->owner = THIS_MODULE;
        adap->class = I2C_CLASS_DEPRECATED;
        snprintf(adap->name, sizeof(adap->name), "bl808 (%s)",
                 of_node_full_name(pdev->dev.of_node));
        adap->algo = &bl808_i2c_algo;
        adap->dev.parent = &pdev->dev;
        adap->dev.of_node = pdev->dev.of_node;
        adap->quirks = of_device_get_match_data(&pdev->dev);

        ret = i2c_add_adapter(adap);
        if (ret)
                goto err_free_irq;

        bl808_i2c_init(i2c_dev);

        return 0;

err_free_irq:
        free_irq(i2c_dev->irq, i2c_dev);
err_disable_unprepare_clk:
        clk_disable_unprepare(i2c_dev->bus_clk);
err_put_exclusive_rate:
        clk_rate_exclusive_put(i2c_dev->bus_clk);

        return ret;
}

static int bl808_i2c_remove(struct platform_device *pdev)
{
    struct bl808_i2c_dev *i2c_dev = platform_get_drvdata(pdev);

        clk_rate_exclusive_put(i2c_dev->bus_clk);
        clk_disable_unprepare(i2c_dev->bus_clk);

        free_irq(i2c_dev->irq, i2c_dev);
        i2c_del_adapter(&i2c_dev->adapter);

        return 0;
}

static const struct of_device_id bl808_i2c_of_match[] = {
        { .compatible = "bflb,bl808-i2c" },
        {},
};
MODULE_DEVICE_TABLE(of, bl808_i2c_of_match);

static struct platform_driver bl808_i2c_driver = {
        .probe		= bl808_i2c_probe,
        .remove		= bl808_i2c_remove,
        .driver		= {
                .name	= "i2c-bl808",
                .of_match_table = bl808_i2c_of_match,
        },
};

module_platform_driver(bl808_i2c_driver);

MODULE_AUTHOR("Alessandro Guttrof <hunter1753@gmail.com>");
MODULE_DESCRIPTION("bl808 I2C bus adapter");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:i2c-bl808");
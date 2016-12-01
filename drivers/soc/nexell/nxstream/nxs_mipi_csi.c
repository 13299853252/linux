/*
 * Copyright (C) 2016  Nexell Co., Ltd.
 * Author: Sungwoo, Park <swpark@nexell.co.kr>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/module.h>

#include <linux/of.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>

#include <linux/soc/nexell/nxs_function.h>
#include <linux/soc/nexell/nxs_dev.h>
#include <linux/soc/nexell/nxs_res_manager.h>

struct nxs_csi {
	struct nxs_dev nxs_dev;
};

static void mipi_csi_set_interrupt_enable(const struct nxs_dev *pthis, int type,
					  bool enable)
{
}

static u32 mipi_csi_get_interrupt_enable(const struct nxs_dev *pthis, int type)
{
	return 0;
}

static u32 mipi_csi_get_interrupt_pending(const struct nxs_dev *pthis, int type)
{
	return 0;
}

static void mipi_csi_clear_interrupt_pending(const struct nxs_dev *pthis,
					     int type)
{
}

static int mipi_csi_open(const struct nxs_dev *pthis)
{
	return 0;
}

static int mipi_csi_close(const struct nxs_dev *pthis)
{
	return 0;
}

static int mipi_csi_start(const struct nxs_dev *pthis)
{
	return 0;
}

static int mipi_csi_stop(const struct nxs_dev *pthis)
{
	return 0;
}

static int mipi_csi_set_dirty(const struct nxs_dev *pthis)
{
	return 0;
}

static int mipi_csi_set_tid(const struct nxs_dev *pthis, u32 tid1, u32 tid2)
{
	return 0;
}

static int mipi_csi_set_syncinfo(const struct nxs_dev *pthis,
				 const union nxs_control *pparam)
{
	return 0;
}

static int mipi_csi_get_syncinfo(const struct nxs_dev *pthis,
				 union nxs_control *pparam)
{
	return 0;
}

static int nxs_mipi_csi_probe(struct platform_device *pdev)
{
	int ret;
	struct nxs_csi *csi;
	struct nxs_dev *nxs_dev;

	csi = devm_kzalloc(&pdev->dev, sizeof(*csi), GFP_KERNEL);
	if (!csi)
		return -ENOMEM;

	nxs_dev = &csi->nxs_dev;

	ret = nxs_dev_parse_dt(pdev, nxs_dev);
	if (ret)
		return ret;

	nxs_dev->set_interrupt_enable = mipi_csi_set_interrupt_enable;
	nxs_dev->get_interrupt_enable = mipi_csi_get_interrupt_enable;
	nxs_dev->get_interrupt_pending = mipi_csi_get_interrupt_pending;
	nxs_dev->clear_interrupt_pending = mipi_csi_clear_interrupt_pending;
	nxs_dev->open = mipi_csi_open;
	nxs_dev->close = mipi_csi_close;
	nxs_dev->start = mipi_csi_start;
	nxs_dev->stop = mipi_csi_stop;
	nxs_dev->set_dirty = mipi_csi_set_dirty;
	nxs_dev->set_tid = mipi_csi_set_tid;
	nxs_dev->set_control = nxs_set_control;
	nxs_dev->get_control = nxs_get_control;
	nxs_dev->dev_services[0].type = NXS_CONTROL_SYNCINFO;
	nxs_dev->dev_services[0].set_control = mipi_csi_set_syncinfo;
	nxs_dev->dev_services[0].get_control = mipi_csi_get_syncinfo;

	nxs_dev->dev = &pdev->dev;

	ret = register_nxs_dev(nxs_dev);
	if (ret)
		return ret;

	dev_info(nxs_dev->dev, "%s: success\n", __func__);
	platform_set_drvdata(pdev, csi);

	return 0;
}

static int nxs_mipi_csi_remove(struct platform_device *pdev)
{
	struct nxs_csi *csi = platform_get_drvdata(pdev);

	if (csi)
		unregister_nxs_dev(&csi->nxs_dev);

	return 0;
}

static const struct of_device_id nxs_mipi_csi_match[] = {
	{ .compatible = "nexell,mipi_csi-nxs-1.0", },
	{},
};

static struct platform_driver nxs_mipi_csi_driver = {
	.probe	= nxs_mipi_csi_probe,
	.remove	= nxs_mipi_csi_remove,
	.driver	= {
		.name = "nxs-mipi_csi",
		.of_match_table = of_match_ptr(nxs_mipi_csi_match),
	},
};

static int __init nxs_mipi_csi_init(void)
{
	return platform_driver_register(&nxs_mipi_csi_driver);
}
/* subsys_initcall(nxs_mipi_csi_init); */
fs_initcall(nxs_mipi_csi_init);

static void __exit nxs_mipi_csi_exit(void)
{
	platform_driver_unregister(&nxs_mipi_csi_driver);
}
module_exit(nxs_mipi_csi_exit);

MODULE_DESCRIPTION("Nexell Stream MIPI_CSI driver");
MODULE_AUTHOR("Sungwoo Park, <swpark@nexell.co.kr>");
MODULE_LICENSE("GPL");

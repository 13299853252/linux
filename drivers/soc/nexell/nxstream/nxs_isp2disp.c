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

struct nxs_isp2disp {
	struct nxs_dev nxs_dev;
};

static void isp2disp_set_interrupt_enable(const struct nxs_dev *pthis, int type,
				     bool enable)
{
}

static u32 isp2disp_get_interrupt_enable(const struct nxs_dev *pthis, int type)
{
	return 0;
}

static u32 isp2disp_get_interrupt_pending(const struct nxs_dev *pthis, int type)
{
	return 0;
}

static void isp2disp_clear_interrupt_pending(const struct nxs_dev *pthis,
					     int type)
{
}

static int isp2disp_open(const struct nxs_dev *pthis)
{
	return 0;
}

static int isp2disp_close(const struct nxs_dev *pthis)
{
	return 0;
}

static int isp2disp_start(const struct nxs_dev *pthis)
{
	return 0;
}

static int isp2disp_stop(const struct nxs_dev *pthis)
{
	return 0;
}

static int isp2disp_set_syncinfo(const struct nxs_dev *pthis,
			    const union nxs_control *pparam)
{
	return 0;
}

static int isp2disp_get_syncinfo(const struct nxs_dev *pthis,
			    union nxs_control *pparam)
{
	return 0;
}

static int nxs_isp2disp_probe(struct platform_device *pdev)
{
	int ret;
	struct nxs_isp2disp *isp2disp;
	struct nxs_dev *nxs_dev;

	isp2disp = devm_kzalloc(&pdev->dev, sizeof(*isp2disp), GFP_KERNEL);
	if (!isp2disp)
		return -ENOMEM;

	nxs_dev = &isp2disp->nxs_dev;

	ret = nxs_dev_parse_dt(pdev, nxs_dev);
	if (ret)
		return ret;

	nxs_dev->set_interrupt_enable = isp2disp_set_interrupt_enable;
	nxs_dev->get_interrupt_enable = isp2disp_get_interrupt_enable;
	nxs_dev->get_interrupt_pending = isp2disp_get_interrupt_pending;
	nxs_dev->clear_interrupt_pending = isp2disp_clear_interrupt_pending;
	nxs_dev->open = isp2disp_open;
	nxs_dev->close = isp2disp_close;
	nxs_dev->start = isp2disp_start;
	nxs_dev->stop = isp2disp_stop;
	nxs_dev->set_control = nxs_set_control;
	nxs_dev->get_control = nxs_get_control;
	nxs_dev->dev_services[0].type = NXS_CONTROL_SYNCINFO;
	nxs_dev->dev_services[0].set_control = isp2disp_set_syncinfo;
	nxs_dev->dev_services[0].get_control = isp2disp_get_syncinfo;

	nxs_dev->dev = &pdev->dev;

	ret = register_nxs_dev(nxs_dev);
	if (ret)
		return ret;

	dev_info(nxs_dev->dev, "%s: success\n", __func__);
	platform_set_drvdata(pdev, isp2disp);

	return 0;
}

static int nxs_isp2disp_remove(struct platform_device *pdev)
{
	struct nxs_isp2disp *isp2disp = platform_get_drvdata(pdev);

	if (isp2disp)
		unregister_nxs_dev(&isp2disp->nxs_dev);

	return 0;
}

static const struct of_device_id nxs_isp2disp_match[] = {
	{ .compatible = "nexell,isp2disp-nxs-1.0", },
	{},
};

static struct platform_driver nxs_isp2disp_driver = {
	.probe	= nxs_isp2disp_probe,
	.remove	= nxs_isp2disp_remove,
	.driver	= {
		.name = "nxs-isp2disp",
		.of_match_table = of_match_ptr(nxs_isp2disp_match),
	},
};

static int __init nxs_isp2disp_init(void)
{
	return platform_driver_register(&nxs_isp2disp_driver);
}
/* subsys_initcall(nxs_isp2disp_init); */
fs_initcall(nxs_isp2disp_init);

static void __exit nxs_isp2disp_exit(void)
{
	platform_driver_unregister(&nxs_isp2disp_driver);
}
module_exit(nxs_isp2disp_exit);

MODULE_DESCRIPTION("Nexell Stream ISP2DISP driver");
MODULE_AUTHOR("Sungwoo Park, <swpark@nexell.co.kr>");
MODULE_LICENSE("GPL");


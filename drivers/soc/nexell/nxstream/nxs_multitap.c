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

struct nxs_multitap {
	struct nxs_dev nxs_dev;
};

static void multitap_set_interrupt_enable(const struct nxs_dev *pthis, int type,
					  bool enable)
{
}

static u32 multitap_get_interrupt_enable(const struct nxs_dev *pthis, int type)
{
	return 0;
}

static u32 multitap_get_interrupt_pending(const struct nxs_dev *pthis, int type)
{
	return 0;
}

static void multitap_clear_interrupt_pending(const struct nxs_dev *pthis,
					     int type)
{
}

static int multitap_open(const struct nxs_dev *pthis)
{
	return 0;
}

static int multitap_close(const struct nxs_dev *pthis)
{
	return 0;
}

static int multitap_start(const struct nxs_dev *pthis)
{
	return 0;
}

static int multitap_stop(const struct nxs_dev *pthis)
{
	return 0;
}

static int multitap_set_dirty(const struct nxs_dev *pthis)
{
	return 0;
}

static int multitap_set_tid(const struct nxs_dev *pthis)
{
	return 0;
}

static int multitap_set_syncinfo(const struct nxs_dev *pthis,
			    const union nxs_control *pparam)
{
	return 0;
}

static int multitap_get_syncinfo(const struct nxs_dev *pthis,
			    union nxs_control *pparam)
{
	return 0;
}

static int nxs_multitap_probe(struct platform_device *pdev)
{
	int ret;
	struct nxs_multitap *multitap;
	struct nxs_dev *nxs_dev;

	multitap = devm_kzalloc(&pdev->dev, sizeof(*multitap), GFP_KERNEL);
	if (!multitap)
		return -ENOMEM;

	nxs_dev = &multitap->nxs_dev;

	ret = nxs_dev_parse_dt(pdev, nxs_dev);
	if (ret)
		return ret;

	nxs_dev->set_interrupt_enable = multitap_set_interrupt_enable;
	nxs_dev->get_interrupt_enable = multitap_get_interrupt_enable;
	nxs_dev->get_interrupt_pending = multitap_get_interrupt_pending;
	nxs_dev->clear_interrupt_pending = multitap_clear_interrupt_pending;
	nxs_dev->open = multitap_open;
	nxs_dev->close = multitap_close;
	nxs_dev->start = multitap_start;
	nxs_dev->stop = multitap_stop;
	nxs_dev->set_dirty = multitap_set_dirty;
	nxs_dev->set_tid = multitap_set_tid;
	nxs_dev->set_control = nxs_set_control;
	nxs_dev->get_control = nxs_get_control;
	nxs_dev->dev_services[0].type = NXS_CONTROL_SYNCINFO;
	nxs_dev->dev_services[0].set_control = multitap_set_syncinfo;
	nxs_dev->dev_services[0].get_control = multitap_get_syncinfo;

	nxs_dev->dev = &pdev->dev;

	ret = register_nxs_dev(nxs_dev);
	if (ret)
		return ret;

	dev_info(nxs_dev->dev, "%s: success\n", __func__);
	platform_set_drvdata(pdev, multitap);

	return 0;
}

static int nxs_multitap_remove(struct platform_device *pdev)
{
	struct nxs_multitap *multitap = platform_get_drvdata(pdev);

	if (multitap)
		unregister_nxs_dev(&multitap->nxs_dev);

	return 0;
}

static const struct of_device_id nxs_multitap_match[] = {
	{ .compatible = "nexell,multitap-nxs-1.0", },
	{},
};

static struct platform_driver nxs_multitap_driver = {
	.probe	= nxs_multitap_probe,
	.remove	= nxs_multitap_remove,
	.driver	= {
		.name = "nxs-multitap",
		.of_match_table = of_match_ptr(nxs_multitap_match),
	},
};

static int __init nxs_multitap_init(void)
{
	return platform_driver_register(&nxs_multitap_driver);
}
/* subsys_initcall(nxs_multitap_init); */
fs_initcall(nxs_multitap_init);

static void __exit nxs_multitap_exit(void)
{
	platform_driver_unregister(&nxs_multitap_driver);
}
module_exit(nxs_multitap_exit);

MODULE_DESCRIPTION("Nexell Stream MULTITAP driver");
MODULE_AUTHOR("Sungwoo Park, <swpark@nexell.co.kr>");
MODULE_LICENSE("GPL");

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
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/v4l2-mediabus.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-async.h>
#include <media/v4l2-of.h>

#include <dt-bindings/media/nexell-vip.h>
#include <linux/soc/nexell/nxs_function.h>
#include <linux/soc/nexell/nxs_dev.h>
#include <linux/soc/nexell/nxs_res_manager.h>

static LIST_HEAD(func_inst_list);
static DEFINE_MUTEX(func_inst_lock);
static int func_inst_seq;

static const char *nxs_function_str[] = {
	"NONE",
	"DMAR",
	"DMAW",
	"VIP_CLIPPER",
	"VIP_DECIMATOR",
	"MIPI_CSI",
	"ISP2DISP",
	"CROPPER",
	"MULTI_TAP",
	"SCALER_4096",
	"SCALER_5376",
	"MLC_BOTTOM",
	"MLC_BLENDER",
	"HUE",
	"GAMMA",
	"FIFO",
	"MAPCONV",
	"TPGEN",
	"DISP2ISP",
	"CSC",
	"DPC",
	"LVDS",
	"MIPI_DSI",
	"HDMI",
	"INVALID",
};

inline const char *nxs_function_to_str(int function)
{
	if (function < NXS_FUNCTION_MAX)
		return nxs_function_str[function];
	return NULL;
}

static int apply_ep_to_capture(struct device *dev, struct v4l2_of_endpoint *sep,
			       struct nxs_capture_ctx *capture)
{
	capture->bus_type = sep->bus_type;

	if (sep->bus_type == V4L2_MBUS_PARALLEL ||
	    sep->bus_type == V4L2_MBUS_BT656) {
		struct nxs_capture_hw_vip_param *param = &capture->bus.parallel;
		struct v4l2_of_bus_parallel *sep_param = &sep->bus.parallel;

		param->bus_width = sep_param->bus_width;
		param->interlace = sep_param->flags &
			(V4L2_MBUS_FIELD_EVEN_HIGH | V4L2_MBUS_FIELD_EVEN_LOW) ?
			1 : 0;
		param->pclk_polarity = sep_param->flags &
			V4L2_MBUS_PCLK_SAMPLE_RISING ? 1 : 0;
		param->hsync_polarity = sep_param->flags &
			V4L2_MBUS_HSYNC_ACTIVE_HIGH ? 1 : 0;
		param->vsync_polarity = sep_param->flags &
			V4L2_MBUS_VSYNC_ACTIVE_HIGH ? 1 : 0;
		param->field_polarity = sep_param->flags &
			V4L2_MBUS_FIELD_EVEN_HIGH ? 1 : 0;
	} else if (sep->bus_type == V4L2_MBUS_CSI2) {
		struct nxs_capture_hw_csi_param *param = &capture->bus.csi;
		struct v4l2_of_bus_mipi_csi2 *sep_param = &sep->bus.mipi_csi2;
		int i;

		param->num_data_lanes = sep_param->num_data_lanes;
		for (i = 0; i < param->num_data_lanes; i++)
			param->data_lanes[i] = sep_param->data_lanes[i];
		param->clock_lane = sep_param->clock_lane;
		for (i = 0; i < 5; i++)
			param->lane_polarities[i] =
				sep_param->lane_polarities[i];
		if (sep->nr_of_link_frequencies)
			param->link_frequency = sep->link_frequencies[0];
	} else {
		dev_err(dev, "%s: invalid bus type %d\n", __func__,
			sep->bus_type);
		return -EINVAL;
	}

	return 0;
}

static int find_action_mark(u32 *p, int length, u32 mark)
{
	int i;

	for (i = 0; i < length; i++) {
		if (p[i] == mark)
			return i;
	}
	return -1;
}

static int find_action_start(u32 *p, int length)
{
	return find_action_mark(p, length, NX_ACTION_START);
}

static int find_action_end(u32 *p, int length)
{
	return find_action_mark(p, length, NX_ACTION_END);
}

static int get_num_of_action(struct device *dev, u32 *array, int count)
{
	u32 *p = array;
	int action_num = 0;
	int next_index = 0;
	int length = count;

	while (length > 0) {
		next_index = find_action_start(p, length);
		if (next_index < 0)
			break;
		p += next_index;
		length -= next_index;
		if (length <= 0)
			break;

		next_index = find_action_end(p, length);
		if (next_index <= 0) {
			dev_err(dev, "failed to find_action_end\n");
			return 0;
		}

		p += next_index;
		length -= next_index;
		action_num++;
	}

	return action_num;
}

static u32 *get_next_action(u32 *array, int count)
{
	u32 *p = array;
	int next_index = find_action_start(p, count);

	if (next_index >= 0)
		return p + next_index;
	return NULL;
}

static u32 get_action_type(u32 *array)
{
	return array[1];
}

static void free_action_seq(struct nxs_action_seq *seq)
{
	int i;
	struct nxs_action *action;

	for (i = 0; i < seq->count; i++) {
		action = &seq->actions[i];
		if (action->action) {
			if (action->type == NX_ACTION_TYPE_GPIO) {
				struct nxs_gpio_action *gpio_action =
					action->action;

				if (gpio_action->units)
					kfree(gpio_action->units);
			}
			kfree(action->action);
		}
	}

	kfree(seq->actions);
	seq->count = 0;
	seq->actions = NULL;
}

static int make_gpio_action(struct device *dev, u32 *start, u32 *end,
			    struct nxs_action *action)
{
	struct nxs_gpio_action *gpio_action;
	struct nxs_action_unit *unit;
	int i;
	u32 *p;
	/* start_marker, type, gpio num */
	int unit_count = end - start - 1 - 1 - 1;

	if ((unit_count <= 0) || (unit_count % 2)) {
		dev_err(dev, "%s: invalid unit_count %d\n", __func__,
			unit_count);
		return -EINVAL;
	}
	unit_count /= 2;

	gpio_action = kzalloc(sizeof(*gpio_action), GFP_KERNEL);
	if (!gpio_action) {
		WARN_ON(1);
		return -ENOMEM;
	}

	gpio_action->count = unit_count;
	gpio_action->units = kcalloc(unit_count, sizeof(*unit), GFP_KERNEL);
	if (!gpio_action->units) {
		WARN_ON(1);
		kfree(gpio_action);
		return -ENOMEM;
	}

	gpio_action->gpio_num = start[2];
	p = &start[3];
	for (i = 0; i < unit_count; i++) {
		unit = &gpio_action->units[i];
		unit->value = *p;
		p++;
		unit->delay_ms = *p;
		p++;
	}

	action->type = start[1];
	action->action = gpio_action;

	return 0;
}

static int make_generic_action(struct device *dev, u32 *start, u32 *end,
			       struct nxs_action *action)
{
	struct nxs_action_unit *unit;
	/* start_marker, type */
	int unit_count = end - start - 1 - 1;

	if ((unit_count <= 0) || (unit_count % 2)) {
		dev_err(dev, "%s: invalid unit_count %d\n", __func__,
			unit_count);
		return -EINVAL;
	}

	unit = kzalloc(sizeof(*unit), GFP_KERNEL);
	if (!unit) {
		WARN_ON(1);
		return -ENOMEM;
	}

	unit->value = start[2];
	unit->delay_ms = start[3];

	action->type = start[1];
	action->action = unit;

	return 0;
}

static int make_nxs_action(struct device *dev, u32 *array, int count,
			   struct nxs_action *action)
{
	u32 *p = array;
	int end_index = find_action_end(p, count);

	if (end_index <= 0) {
		dev_err(dev, "%s: can't find action end\n", __func__);
		return -EINVAL;
	}

	switch (get_action_type(p)) {
	case NX_ACTION_TYPE_GPIO:
		return make_gpio_action(dev, p, p + end_index, action);
	case NX_ACTION_TYPE_PMIC:
	case NX_ACTION_TYPE_CLOCK:
		return make_generic_action(dev, p, p + end_index, action);
	default:
		dev_err(dev, "%s: incalid type 0x%x\n", __func__,
			get_action_type(p));
		break;
	}

	return -EINVAL;
}

static int parse_power_seq(struct device *dev, struct device_node *node,
			   char *node_name, struct nxs_action_seq *seq)
{
	int count = of_property_count_elems_of_size(node, node_name, 4);

	if (count > 0) {
		u32 *p;
		int ret = 0;
		struct nxs_action *action;
		int action_nums;
		u32 *array = kcalloc(count, sizeof(u32), GFP_KERNEL);

		if (!array) {
			WARN_ON(1);
			return -ENOMEM;
		}

		of_property_read_u32_array(node, node_name, array, count);
		action_nums = get_num_of_action(dev, array, count);
		if (action_nums <= 0) {
			dev_err(dev, "%s: no actions in %s\n", __func__,
				node_name);
			return -ENOENT;
		}

		seq->actions = kcalloc(count, sizeof(*action), GFP_KERNEL);
		if (!seq->actions) {
			WARN_ON(1);
			return -ENOMEM;
		}
		seq->count = action_nums;

		p = array;
		action = seq->actions;
		while (action_nums--) {
			p = get_next_action(p, count - (p - array));
			if (!p) {
				dev_err(dev, "failed to get_next_action(%d/%d)\n",
					seq->count, action_nums);
				free_action_seq(seq);
				return -EINVAL;
			}

			ret = make_nxs_action(dev, p, count - (p - array),
					      action);
			if (ret != 0) {
				free_action_seq(seq);
				return ret;
			}

			p++;
			action++;
		}
	}

	return 0;
}

static int parse_capture_dts(struct device *dev,
			     struct nxs_capture_ctx *capture,
			     struct device_node *node)
{
	int ret;

	if (capture->bus_type == V4L2_MBUS_PARALLEL ||
	    capture->bus_type == V4L2_MBUS_BT656) {
		struct nxs_capture_hw_vip_param *param = &capture->bus.parallel;

		if (of_property_read_u32(node, "data_order",
					 &param->data_order)) {
			dev_err(dev, "failed to get dt data_order\n");
			return -EINVAL;
		}

		if (param->bus_width > 8) {
			/* bus_align must be exist: LSB 1, MSB 0 */
			if (of_property_read_u32(node, "bus_align",
						 &param->bus_align)) {
				dev_err(dev, "failed to get dt bus_align\n");
				return -EINVAL;
			}
		}

		if (capture->bus_type == V4L2_MBUS_PARALLEL) {
			if (of_property_read_u32(node, "h_backporch",
						 &param->h_backporch)) {
				dev_err(dev, "failed to get dt h_backporch\n");
				return -EINVAL;
			}

			if (of_property_read_u32(node, "v_backporch",
						 &param->v_backporch)) {
				dev_err(dev, "failed to get dt v_backporch\n");
				return -EINVAL;
			}
		}
	}

	/* regulator */
	capture->regulator_nr = of_property_count_strings(node, "regulator_names");
	if (capture->regulator_nr > 0) {
		int i;
		const char *name;

		capture->regulator_names = kcalloc(capture->regulator_nr,
						   sizeof(char *),
						   GFP_KERNEL);
		if (!capture->regulator_names) {
			WARN_ON(1);
			return -ENOMEM;
		}

		capture->regulator_voltages = kcalloc(capture->regulator_nr,
						      sizeof(u32),
						      GFP_KERNEL);
		if (!capture->regulator_voltages) {
			WARN_ON(1);
			return -ENOMEM;
		}

		for (i = 0; i < capture->regulator_nr; i++) {
			if (of_property_read_string_index(node, "regulator_names",
							  i, &name)) {
				dev_err(dev, "failed to read regulator %d name\n", i);
				return -EINVAL;
			}
			capture->regulator_names[i] = (char *)name;
		}

		of_property_read_u32_array(node, "regulator_voltages",
					   capture->regulator_voltages,
					   capture->regulator_nr);
	}

	/* clock */
	capture->clk = clk_get(dev, "vclkout");
	if (!IS_ERR(capture->clk)) {
		if (of_property_read_u32(node, "clock-frequency",
					 &capture->clock_freq)) {
			dev_err(dev, "failed to get clock-frequency\n");
			return -EINVAL;
		}
	}

	/* enable seq */
	if (of_find_property(node, "enable_seq", NULL)) {
		ret = parse_power_seq(dev, node, "enable_seq",
				      &capture->enable_seq);
		if (ret)
			return ret;
	}

	/* disable seq */
	if (of_find_property(node, "disable_seq", NULL)) {
		ret = parse_power_seq(dev, node, "disable_seq",
				      &capture->enable_seq);
		if (ret)
			return ret;
	}

	return 0;
}

static int apply_gpio_action(struct device *dev, int gpio_num,
			     struct nxs_action_unit *unit)
{
	int ret;
	char label[64] = {0, };
	struct device_node *node;
	int gpio;

	node = dev->of_node;
	gpio = of_get_named_gpio(node, "gpios", gpio_num);

	sprintf(label, "nxs-v4l2 camera #pwr gpio %d", gpio);
	if (!gpio_is_valid(gpio)) {
		dev_err(dev, "%s: invalid gpio %d\n", __func__, gpio);
		return -EINVAL;
	}

	ret = devm_gpio_request_one(dev, gpio, unit->value ?
				    GPIOF_OUT_INIT_HIGH : GPIOF_OUT_INIT_LOW,
				    label);
	if (ret < 0) {
		dev_err(dev, "%s: failed to set gpio %d to %d\n",
			__func__, gpio, unit->value);
		return ret;
	}

	if (unit->delay_ms > 0)
		mdelay(unit->delay_ms);

	devm_gpio_free(dev, gpio);

	return 0;
}

static int do_gpio_action(struct device *dev, struct nxs_capture_ctx *capture,
			  struct nxs_gpio_action *action)
{
	int ret;
	struct nxs_action_unit *unit;
	int i;

	for (i = 0; i < action->count; i++) {
		unit = &action->units[i];
		ret = apply_gpio_action(dev, action->gpio_num, unit);
		/* test comment out */
		/* if (ret) */
		/* 	return ret; */
	}

	return 0;
}

static int do_pmic_action(struct device *dev, struct nxs_capture_ctx *capture,
			  struct nxs_action_unit *unit)
{
	int ret;
	int i;
	struct regulator *power;

	for (i = 0; i < capture->regulator_nr; i++) {
		power = devm_regulator_get(dev, capture->regulator_names[i]);
		if (IS_ERR(power)) {
			dev_err(dev, "%s: failed to get power %s\n",
				__func__, capture->regulator_names[i]);
			return -ENODEV;
		}

		if (regulator_can_change_voltage(power)) {
			ret = regulator_set_voltage(power,
						capture->regulator_voltages[i],
						capture->regulator_voltages[i]);
			if (ret) {
				devm_regulator_put(power);
				dev_err(dev, "%s: failed to set voltages for %s\n",
					__func__, capture->regulator_names[i]);
			}
		}

		ret = 0;
		if (unit->value && !regulator_is_enabled(power))
			ret = regulator_enable(power);
		else if (!unit->value && regulator_is_enabled(power))
			ret = regulator_disable(power);

		devm_regulator_put(power);
		if (ret) {
			dev_err(dev, "%s: failed to power %s to %s\n",
				__func__, capture->regulator_names[i],
				unit->value ? "enable" : "disable");
			return ret;
		}

		if (unit->delay_ms > 0)
			mdelay(unit->delay_ms);
	}

	return 0;
}

static int do_clock_action(struct device *dev, struct nxs_capture_ctx *capture,
			   struct nxs_action_unit *unit)
{
	int ret = 0;

	if (capture->clk) {
		if (unit->value)
			ret = clk_prepare_enable(capture->clk);
		else
			clk_disable_unprepare(capture->clk);

		if (ret) {
			dev_err(dev, "%s: failed to clk control\n", __func__);
			return ret;
		}

		if (unit->delay_ms > 0)
			mdelay(unit->delay_ms);
	}

	return 0;
}

int nxs_capture_power(struct nxs_capture_ctx *capture, bool enable)
{
	struct nxs_action_seq *seq;
	int i;
	struct nxs_action *act;
	struct device *dev = capture->dev;
	int ret = 0;

	if (enable)
		seq = &capture->enable_seq;
	else
		seq = &capture->disable_seq;

	for (i = 0; i < seq->count; i++) {
		act = &seq->actions[i];
		switch (act->type) {
		case NX_ACTION_TYPE_GPIO:
			ret = do_gpio_action(dev, capture, act->action);
			if (ret)
				return ret;
			break;
		case NX_ACTION_TYPE_PMIC:
			ret = do_pmic_action(dev, capture, act->action);
			if (ret)
				return ret;
			break;
		case NX_ACTION_TYPE_CLOCK:
			ret = do_clock_action(dev, capture, act->action);
			if (ret)
				return ret;
			break;
		default:
			dev_err(dev, "unknown action type 0x%x\n", act->type);
			break;
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(nxs_capture_power);

int nxs_capture_bind_sensor(struct device *dev, struct nxs_dev *nxs_dev,
			    struct v4l2_async_subdev *asd,
			    struct nxs_capture_ctx *capture)
{
	struct device_node *node = NULL;
	struct device_node *remote_parent_node = NULL, *remote_node = NULL;
	struct v4l2_of_endpoint vep, sep;
	int ret;

	node = of_graph_get_next_endpoint(nxs_dev->dev->of_node, NULL);
	if (!node) {
		dev_err(dev, "%s: can't find endpoint node\n", __func__);
		ret = -ENOENT;
		goto err_out;
	}

	ret = v4l2_of_parse_endpoint(node, &vep);
	if (ret) {
		dev_err(dev, "%s: failed to v4l2_of_parse_endpoint\n",
			__func__);
		goto err_out;
	}

	remote_parent_node = of_graph_get_remote_port_parent(node);
	if (!remote_parent_node) {
		dev_err(dev, "%s: can't find remote parent node\n", __func__);
		ret = -ENOENT;
		goto err_out;
	}

	remote_node = of_graph_get_next_endpoint(remote_parent_node, NULL);
	if (!remote_node) {
		dev_err(dev, "%s: can't find remote node\n", __func__);
		ret = -ENOENT;
		goto err_out;
	}

	ret = v4l2_of_parse_endpoint(remote_node, &sep);
	if (ret) {
		dev_err(dev,
			"%s: failed to v4l2_of_parse_endpoint for remote\n",
			__func__);
		goto err_out;
	}

	ret = apply_ep_to_capture(dev, &sep, capture);
	if (ret) {
		dev_err(dev, "%s: failed to apply_ep_to_capture\n", __func__);
		goto err_out;
	}

	ret = parse_capture_dts(dev, capture, node);
	if (ret)
		dev_err(dev, "%s: failed to parse_capture_dts\n", __func__);

	capture->dev = nxs_dev->dev;

err_out:
	if (remote_node)
		of_node_put(remote_node);

	if (remote_parent_node)
		of_node_put(remote_parent_node);

	if (node)
		of_node_put(node);

	return ret;
}
EXPORT_SYMBOL_GPL(nxs_capture_bind_sensor);

void nxs_capture_free(struct nxs_capture_ctx *capture)
{
	free_action_seq(&capture->enable_seq);
	free_action_seq(&capture->disable_seq);
	kfree(capture);
}
EXPORT_SYMBOL_GPL(nxs_capture_free);

void nxs_free_function_request(struct nxs_function_request *req)
{
	if (req) {
		struct nxs_function *func;

		while (!list_empty(&req->head)) {
			func = list_first_entry(&req->head, struct nxs_function,
						list);
			list_del_init(&func->list);
			kfree(func);
		}
		kfree(req);
	}
}
EXPORT_SYMBOL_GPL(nxs_free_function_request);

struct nxs_function_instance *
nxs_function_build(struct nxs_function_request *req)
{
	struct nxs_function *func;
	struct nxs_dev *nxs_dev;
	struct nxs_function_instance *inst = NULL;

	inst = kzalloc(sizeof(*inst), GFP_KERNEL);
	if (!inst) {
		WARN_ON(1);
		return NULL;
	}

	INIT_LIST_HEAD(&inst->dev_list);
	INIT_LIST_HEAD(&inst->dev_sibling_list);

	list_for_each_entry(func, &req->head, list) {
		nxs_dev = get_nxs_dev(func->function, func->index, func->user,
				      func->multitap_follow);
		if (!nxs_dev) {
			pr_err("can't get nxs_dev for func %s, index 0x%x\n",
				nxs_function_to_str(func->function),
				func->index);
			goto error_out;
		}

		/* TODO: need refactoring */
#if 0
		if ((nxs_dev->dev_function == NXS_FUNCTION_MULTITAP &&
		     atomic_read(&nxs_dev->refcount) > 1) ||
		    (nxs_dev->multitap_connected &&
		     atomic_read(&nxs_dev->connect_count) > 1)) {
			pr_info("function %s, index %d added to sibling_list\n",
				nxs_function_to_str(nxs_dev->dev_function),
				nxs_dev->dev_inst_index);
			list_add_tail(&nxs_dev->sibling_list,
				      &inst->dev_sibling_list);
		}
		else
#endif
		list_add_tail(&nxs_dev->func_list, &inst->dev_list);
	}

	inst->req = req;

	if (req->flags & BLENDING_TO_MINE) {
		inst->top = get_nxs_dev(NXS_FUNCTION_MLC_BOTTOM,
					req->option.bottom_id,
					NXS_FUNCTION_USER_APP,
					0);
		if (!inst->top) {
			pr_err("%s: failed to bottom dev(inst %d)\n",
				__func__, req->option.bottom_id);
			goto error_out;
		}
	}

	/* TODO: BLENDING_TO_OTHER, MULTI_PATH */

	mutex_lock(&func_inst_lock);
	func_inst_seq++;
	inst->id = func_inst_seq;
	list_add_tail(&inst->sibling_list, &func_inst_list);
	mutex_unlock(&func_inst_lock);

	return inst;

error_out:
	if (inst)
		nxs_function_destroy(inst);

	return NULL;
}
EXPORT_SYMBOL_GPL(nxs_function_build);

void nxs_function_destroy(struct nxs_function_instance *inst)
{
	struct nxs_dev *nxs_dev;

	/* TODO: refactoring */
#if 0
	while (!list_empty(&inst->dev_sibling_list)) {
		nxs_dev = list_first_entry(&inst->dev_sibling_list,
					   struct nxs_dev, sibling_list);
		pr_info("sibling: put %p, function %s, index %d\n",
			nxs_dev, nxs_function_to_str(nxs_dev->dev_function),
			nxs_dev->dev_inst_index);
		list_del_init(&nxs_dev->sibling_list);
		put_nxs_dev(nxs_dev);
	}
#endif

	mutex_lock(&func_inst_lock);
	list_del_init(&inst->sibling_list);
	mutex_unlock(&func_inst_lock);

	while (!list_empty(&inst->dev_list)) {
		nxs_dev = list_first_entry(&inst->dev_list,
					   struct nxs_dev, func_list);
		/* pr_info("dev: put %p, function %s, index %d\n", */
		/* 	nxs_dev, nxs_function_to_str(nxs_dev->dev_function), */
		/* 	nxs_dev->dev_inst_index); */
		list_del_init(&nxs_dev->func_list);
		put_nxs_dev(nxs_dev);
	}
	/* pr_info("%s: free dev_list end\n", __func__); */

	if (inst->top)
		put_nxs_dev(inst->top);
	if (inst->cur_blender)
		put_nxs_dev(inst->cur_blender);
	if (inst->blender_next)
		put_nxs_dev(inst->blender_next);

	nxs_free_function_request(inst->req);

	kfree(inst);
}
EXPORT_SYMBOL_GPL(nxs_function_destroy);

struct nxs_function_instance *nxs_function_get(int handle)
{
	struct nxs_function_instance *inst;
	bool found = false;

	mutex_lock(&func_inst_lock);
	list_for_each_entry(inst, &func_inst_list, sibling_list) {
		if (inst->id == handle) {
			found = true;
			break;
		}
	}
	if (!found)
		inst = NULL;
	mutex_unlock(&func_inst_lock);

	return inst;
}
EXPORT_SYMBOL_GPL(nxs_function_get);

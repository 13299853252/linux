/*
 * Copyright (C) 2016  Nexell Co., Ltd.
 * Author: Jongkeun, Choi <jkchoi@nexell.co.kr>
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
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_address.h>
#include <linux/reset.h>
#include <linux/i2c.h>
#include <linux/pwm.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>

#include <dt-bindings/media/nexell-vip.h>

#include "../nx-v4l2.h"
#include "nx-vip-primitive.h"
#include "nx-vip.h"
#include "../../../../gpu/drm/nexell/soc/s5pxx18_dp_dev.h"
#include "../../../../gpu/drm/nexell/soc/s5pxx18_soc_mlc.h"
#include "../../../../gpu/drm/nexell/soc/s5pxx18_soc_dpc.h"
#include "../../../../pinctrl/nexell/s5pxx18-gpio.h"

#include "nx-rearcam-vendor.h"

#define NX_REARCAM_DEV_NAME "nx-rearcam"

#define TIME_LOG	0

#ifndef MLC_LAYER_RGB_OVERLAY
#define MLC_LAYER_RGB_OVERLAY 0
#endif

#ifndef MLC_LAYER_VIDEO
#define MLC_LAYER_VIDEO	3
#endif

#define YUV_STRIDE_ALIGN_FACTOR		64
#define YUV_VSTRIDE_ALIGN_FACTOR	16

#define YUV_STRIDE(w)	ALIGN(w, YUV_STRIDE_ALIGN_FACTOR)
#define YUV_YSTIRDE(w)	(ALIGN(w/2, YUV_STRIDE_ALIGN_FACTOR) * 2)
#define YUV_VSTRIDE(h)	ALIGN(h, YUV_VSTRIDE_ALIGN_FACTOR)

#define MAX_BUFFER_COUNT	13
#define MININUM_INQUEUE_COUNT	2

#define parse_read_prop(n, s, v) { \
	u32 _v; \
	if (!of_property_read_u32(n, s, &_v)) \
		v = _v; \
		}
struct measurement {
	u64 s_time;
	u64 e_time;
	u64 r_s_time;
	u64 r_e_time;
	u64 t_s_time;
	u64 t_e_time;

	u32 time_msec;
	u32 time_cnt;
	u32 t_time_msec;
	u32 lu_time;
	u32 cb_time;
	u32 cr_time;

	u32 dp_frame_cnt;
	u32 vip_frame_cnt;
	u32 rot_frame_cnt;
};

/**
 * common queue
 */
/******************************************************************************/
struct queue_entry {
	struct list_head head;
	void *data;
};

struct common_queue {
	struct list_head head;
	spinlock_t slock;
	unsigned long flags;

	int buf_count;

	void (*init)(struct common_queue *);
	void (*enqueue)(struct common_queue *, struct queue_entry *);
	struct queue_entry *(*dequeue)(struct common_queue *);
	struct queue_entry *(*peek)(struct common_queue *, int pos);
	int (*clear)(struct common_queue *);
	void (*lock)(struct common_queue *);
	void (*unlock)(struct common_queue *);
	int (*size)(struct common_queue *);
};

static void _init_queue(struct common_queue *q)
{
	INIT_LIST_HEAD(&q->head);
	spin_lock_init(&q->slock);
	q->buf_count = 0;
}

static void _enqueue(struct common_queue *q, struct queue_entry *buf)
{
	q->lock(q);
	list_add_tail(&buf->head, &q->head);
	q->buf_count++;
	q->unlock(q);
}

static struct queue_entry *_dequeue(struct common_queue *q)
{
	struct queue_entry *ent = NULL;
	struct nx_video_buf *buf = NULL;

	q->lock(q);
	if (!list_empty(&q->head)) {
		ent = list_first_entry(&q->head, struct queue_entry, head);
		buf = (struct nx_video_buf *)(ent->data);
		list_del_init(&ent->head);
		q->buf_count--;

		if (q->buf_count < 0)
			q->buf_count = 0;
	}
	q->unlock(q);

	return ent;
}

static struct queue_entry *_peekqueue(struct common_queue *q, int pos)
{
	struct queue_entry *ent = NULL;
	int idx = 0;

	q->lock(q);
	if ((q->buf_count > 0) && (q->buf_count >= (pos+1))) {
		list_for_each_entry(ent, &q->head, head) {
			if (idx == pos)
				break;

			idx++;
		}
	}
	q->unlock(q);

	if (idx > pos)
		ent = NULL;

	return ent;
}

static int _clearqueue(struct common_queue *q)
{
	while (q->dequeue(q))
		;
	return 0;
}

static int _sizequeue(struct common_queue *q)
{
	int count = 0;

	q->lock(q);
	count = q->buf_count;
	q->unlock(q);

	return count;
}

static void _lockqueue(struct common_queue *q)
{
	spin_lock_irqsave(&q->slock, q->flags);
}

static void _unlockqueue(struct common_queue *q)
{
	spin_unlock_irqrestore(&q->slock, q->flags);
}

static void register_queue_func(
	struct common_queue *q,
	void (*init)(struct common_queue *),
	void (*enqueue)(struct common_queue *, struct queue_entry *),
	struct queue_entry *(*dequeue)(struct common_queue *),
	struct queue_entry *(*peek)(struct common_queue *, int pos),
	int (*clear)(struct common_queue *),
	void (*lock)(struct common_queue *),
	void (*unlock)(struct common_queue *),
	int (*size)(struct common_queue *)

	)
{
	q->init = init;
	q->enqueue = enqueue;
	q->dequeue = dequeue;
	q->peek	= peek;
	q->clear = clear;
	q->lock = lock;
	q->unlock = unlock;
	q->size = size;
}
/*****************************************************************************/
enum FRAME_KIND {
	Y,
	CB,
	CR
};

struct nx_buf_meta {
	struct sg_table *sgt;
	size_t size;
	void *vaddr;
	dma_addr_t paddr;
	u32 direction;
};

struct nx_video_buf {
	struct nx_buf_meta buf_meta;

	unsigned long frame_num;

	dma_addr_t dma_buf;
	void *virt_video;

	u32 lu_addr;
	u32 cb_addr;
	u32 cr_addr;

	u32 lu_stride;
	u32 cb_stride;
	u32 cr_stride;

	u32 lu_size;
	u32 cb_size;
	u32 cr_size;

	u32 page_size;
};

struct nx_rgb_buf {
	dma_addr_t dma_buf;

	void *virt_rgb;
	u32 rgb_addr;

	u32 page_size;
};

struct nx_frame_set {
	u32 width;
	u32 height;

	int cur_mode;

	struct queue_entry *cur_entry_vip;

	struct queue_entry vip_queue_entry[MAX_BUFFER_COUNT];
	struct queue_entry rot_queue_entry[MAX_BUFFER_COUNT];

	struct nx_video_buf vip_bufs[MAX_BUFFER_COUNT];
	struct nx_video_buf rot_bufs[MAX_BUFFER_COUNT];

	struct nx_rgb_buf rgb_buf;
};

struct reg_val {
	uint8_t reg;
	uint8_t val;
};

#define MAX_RES_NUM             8

struct nx_rearcam_res {
	void *base;
	struct reset_control *reset;
	void *dev_bases[MAX_RES_NUM];
	int   num_devs;
	void *dev_clk_bases[MAX_RES_NUM];
	int   dev_clk_ids[MAX_RES_NUM];
	int   num_dev_clks;
	struct reset_control *dev_resets[MAX_RES_NUM];
	int   num_dev_resets;
};

struct gpio_action_unit {
	int value;
	int delay_ms;
};

struct nx_capture_enable_gpio_action {
	int gpio_num;
	int count;
	/* alloc dynamically by count */
	struct gpio_action_unit *units;
};

struct nx_capture_enable_pmic_action {
	int enable;
	int delay_ms;
};

struct nx_capture_enable_clock_action {
	int enable;
	int delay_ms;
};

struct nx_capture_power_action {
	int type;
	/**
	 * nx_enable_gpio_action or
	 * nx_enable_pmic_action or
	 * nx_enable_clock_action
	 */
	void *action;
};

struct nx_capture_power_seq {
	int count;
	/* alloc dynamically by count */
	struct nx_capture_power_action *actions;
};

struct nx_v4l2_i2c_board_info {
	int	i2c_adapter_id;
	struct i2c_board_info board_info;
};

struct nx_clipper_info {
	u32 module;
	u32 interface_type;
	u32 external_sync;
	u32 h_frontporch;
	u32 h_syncwidth;
	u32 h_backporch;
	u32 v_frontporch;
	u32 v_syncwidth;
	u32 v_backporch;
	u32 clock_invert;
	u32 port;
	u32 interlace;
	int regulator_nr;
	char **regulator_names;
	u32 *regulator_voltages;
	struct pwm_device *pwm;
	u32 clock_rate;
	struct nx_capture_power_seq enable_seq;
	struct nx_capture_power_seq disable_seq;
	struct nx_v4l2_i2c_board_info sensor_info;

	u32 bus_fmt; /* data_order */
	u32 width;
	u32 height;

	bool sensor_enabled;
};

struct nx_rearcam {
	struct measurement measure;

	u32 rotation;

	struct i2c_client *client;
	struct nx_clipper_info clipper_info;
	struct dp_ctrl_info dp_ctl;
	struct dp_sync_info dp_sync;

	struct delayed_work work;
	struct mutex decide_work_lock;

	int event_gpio;
	int active_high;
	int detect_delay;
	struct reg_val *init_data;

	int irq_event;
	int irq_vip;
	int irq_count;
	char irq_name_vip[30];

	int irq_dpc;
	char irq_name_dpc[30];

	u32 width;
	u32 height;

	/* display worker */
	struct work_struct work_display;
	struct workqueue_struct *wq_display;

	/* display rotation worker */
	struct work_struct work_lu_rot;
	struct workqueue_struct *wq_lu_rot;

	struct work_struct work_cb_rot;
	struct workqueue_struct *wq_cb_rot;

	struct work_struct work_cr_rot;
	struct workqueue_struct *wq_cr_rot;

	/* reset */
	struct reset_control *reset_display_top;
	/* mlc, dpc */
	struct reset_control *reset_dual_display;

	/* reg-names, reg parsing */
	void __iomem **base_addr;

	/* resourcr */
	struct nx_rearcam_res res;

	/* dpc */
	spinlock_t display_lock;
	u32 dpc_module;

	/* deinterlace */
	int use_deinterlace;
	int divide_frame;
	int is_odd_frist;

	/* mlc */
	u32 mlc_module;
	u32 rgb_format;
	bool mlc_on_first;
	int rot_width_rate_y_size;
	int rot_width_rate_c_size;

	struct platform_device *pdev;

	bool is_first;
	bool is_on;
	bool is_remove;
	bool removed;

	bool is_display_on;
	bool is_mlc_on;

	bool is_mlc_on_first;

	bool running;
	bool reargear_on;

	/* rotation */
	struct common_queue q_rot_empty;
	struct common_queue q_rot_done;

	struct mutex rot_lock;

	/* vip */
	struct common_queue q_vip_empty;
	struct common_queue q_vip_done;
	spinlock_t vip_lock;

	/* frame set */
	struct nx_frame_set frame_set;
	struct nx_video_buf *rot_src;
	struct nx_video_buf *rot_dst;

	/* vendor context */
	struct nx_vendor_context *vendor_context;
	struct nx_vendor_context *(*alloc_vendor_context)(void);
	void (*free_vendor_context)(void *);
	bool (*pre_turn_on)(void *);
	void (*post_turn_off)(void *);
	bool (*decide)(void *);
	void (*draw_rgb_overlay)(int, int, int, int, void *, void *);
};

extern struct nx_vendor_context nx_vendor_ctx;

static irqreturn_t _dpc_irq_handler(int, void *);
static int nx_rearcam_remove(struct platform_device *pdev);
static void _init_hw_vip(struct nx_rearcam *me);
static void _dpc_set_display(struct nx_rearcam *);

static struct device_node *_of_get_node_by_property(struct device *dev,
		struct device_node *np, char *prop_name)
{
	const __be32 *list;
	int size;
	struct device_node *node = NULL;
	phandle phandle;

	list = of_get_property(np, prop_name, &size);
	if (!list) {
		dev_err(dev, "%s: could not find list\n", np->full_name);
		return NULL;
	}

	phandle = be32_to_cpup(list++);
	if (phandle) {
		node = of_find_node_by_phandle(phandle);
		if (!node) {
			dev_err(dev, "%s: could not find phandle\n",
				np->full_name);
				return NULL;
		}
	}

	return node;
}

/**
 * parse device tree
 */
static int parse_sensor_i2c_board_info_dt(struct device_node *np,
			struct device *dev, struct nx_clipper_info *clip)
{
	const char *name;
	u32 adapter;
	u32 addr;
	struct nx_v4l2_i2c_board_info *info = &clip->sensor_info;

	if (of_property_read_string(np, "i2c_name", &name)) {
		dev_err(dev, "failed to get sensor i2c name\n");
		return -EINVAL;
	}

	if (of_property_read_u32(np, "i2c_adapter", &adapter)) {
		dev_err(dev, "failed to get sensor i2c adapter\n");
		return -EINVAL;
	}

	if (of_property_read_u32(np, "addr", &addr)) {
		dev_err(dev, "failed to get sensor i2c addr\n");
		return -EINVAL;
	}

	strcpy(info->board_info.type, name);
	info->board_info.addr = addr;
	info->i2c_adapter_id = adapter;

	return 0;
}

static int parse_sensor_dt(struct device_node *np, struct device *dev,
			   struct nx_clipper_info *clip)
{
	int ret;
	u32 type;

	if (of_property_read_u32(np, "type", &type)) {
		dev_err(dev, "failed to get dt sensor type\n");
		return -EINVAL;
	}

	if (type == NX_CAPTURE_SENSOR_I2C) {
		ret = parse_sensor_i2c_board_info_dt(np, dev, clip);
		if (ret)
			return ret;
	} else {
		dev_err(dev, "sensor type %d is not supported\n",
			type);
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

static int get_num_of_enable_action(u32 *array, int count)
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
			pr_err("failed to find_action_end,");
			pr_err(" check power node of dts\n");
			return 0;
		}
		p += next_index;
		length -= next_index;
		action_num++;
	}

	return action_num;
}

static u32 *get_next_action_unit(u32 *array, int count)
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

static int make_enable_gpio_action(u32 *start, u32 *end,
				   struct nx_capture_power_action *action)
{
	struct nx_capture_enable_gpio_action *gpio_action;
	struct gpio_action_unit *unit;
	int i;
	u32 *p;
	/* start_marker, type, gpio num */
	int unit_count = end - start - 1 - 1 - 1;

	if ((unit_count <= 0) || (unit_count % 2)) {
		pr_err("invalid unit_count %d of gpio action\n", unit_count);
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

	action->type = NX_ACTION_TYPE_GPIO;
	action->action = gpio_action;

	return 0;
}

static int make_enable_pmic_action(u32 *start, u32 *end,
				   struct nx_capture_power_action *action)
{
	struct nx_capture_enable_pmic_action *pmic_action;
	int entry_count = end - start - 1 - 1; /* start_marker, type */

	if ((entry_count <= 0) || (entry_count % 2)) {
		pr_err("invalid entry_count %d of pmic action\n", entry_count);
		return -EINVAL;
	}

	pmic_action = kzalloc(sizeof(*pmic_action), GFP_KERNEL);
	if (!pmic_action) {
		WARN_ON(1);
		return -ENOMEM;
	}

	pmic_action->enable = start[2];
	pmic_action->delay_ms = start[3];

	action->type = NX_ACTION_TYPE_PMIC;
	action->action = pmic_action;

	return 0;
}

static int make_enable_clock_action(u32 *start, u32 *end,
				    struct nx_capture_power_action *action)
{
	struct nx_capture_enable_clock_action *clock_action;
	int entry_count = end - start - 1 - 1; /* start_marker, type */

	if ((entry_count <= 0) || (entry_count % 2)) {
		pr_err("invalid entry_count %d of clock action\n", entry_count);
		return -EINVAL;
	}

	clock_action = kzalloc(sizeof(*clock_action), GFP_KERNEL);
	if (!clock_action) {
		WARN_ON(1);
		return -ENOMEM;
	}

	clock_action->enable = start[2];
	clock_action->delay_ms = start[3];

	action->type = NX_ACTION_TYPE_CLOCK;
	action->action = clock_action;

	return 0;
}

static int make_enable_action(u32 *array, int count,
			      struct nx_capture_power_action *action)
{
	u32 *p = array;
	int end_index = find_action_end(p, count);

	if (end_index <= 0) {
		pr_err("parse dt for v4l2 capture error:");
		pr_err(" can't find action end\n");
		return -EINVAL;
	}

	switch (get_action_type(p)) {
	case NX_ACTION_TYPE_GPIO:
		return make_enable_gpio_action(p, p + end_index, action);
	case NX_ACTION_TYPE_PMIC:
		return make_enable_pmic_action(p, p + end_index, action);
	case NX_ACTION_TYPE_CLOCK:
		return make_enable_clock_action(p, p + end_index, action);
	default:
		pr_err("parse dt v4l2 capture: invalid type 0x%x\n",
		       get_action_type(p));
		return -EINVAL;
	}
}

static void free_enable_seq_actions(struct nx_capture_power_seq *seq)
{
	int i;
	struct nx_capture_power_action *action;

	for (i = 0; i < seq->count; i++) {
		action = &seq->actions[i];
		if (action->action) {
			if (action->type == NX_ACTION_TYPE_GPIO) {
				struct nx_capture_enable_gpio_action *
					gpio_action = action->action;
				kfree(gpio_action->units);
			}
			kfree(action->action);
		}
	}

	kfree(seq->actions);
	seq->count = 0;
	seq->actions = NULL;
}

static int parse_capture_power_seq(struct device_node *np,
				   char *node_name,
				   struct nx_capture_power_seq *seq)
{
	int count = of_property_count_elems_of_size(np, node_name, 4);

	if (count > 0) {
		u32 *p;
		int ret = 0;
		struct nx_capture_power_action *action;
		int action_nums;
		u32 *array = kcalloc(count, sizeof(u32), GFP_KERNEL);

		if (!array) {
			WARN_ON(1);
			return -ENOMEM;
		}

		of_property_read_u32_array(np, node_name, array, count);
		action_nums = get_num_of_enable_action(array, count);
		if (action_nums <= 0) {
			pr_err("parse_dt v4l2 capture: no actions in %s\n",
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
			p = get_next_action_unit(p, count - (p - array));
			if (!p) {
				pr_err("failed to");
				pr_err(" get_next_action_unit(%d/%d)\n",
				       seq->count, action_nums);
				free_enable_seq_actions(seq);
				return -EINVAL;
			}

			ret = make_enable_action(p, count - (p - array),
						 action);
			if (ret != 0) {
				free_enable_seq_actions(seq);
				return ret;
			}

			p++;
			action++;
		}
	}

	return 0;
}

static int parse_power_dt(struct device_node *np, struct device *dev,
			  struct nx_clipper_info *clip)
{
	int ret = 0;

	if (of_find_property(np, "enable_seq", NULL))
		ret = parse_capture_power_seq(np, "enable_seq",
					      &clip->enable_seq);

	if (ret)
		return ret;

	if (of_find_property(np, "disable_seq", NULL))
		ret = parse_capture_power_seq(np, "disable_seq",
					      &clip->disable_seq);

	return ret;
}

static int parse_clock_dt(struct device_node *np, struct device *dev,
			  struct nx_clipper_info *clip)
{
	clip->pwm = devm_pwm_get(dev, NULL);
	if (!IS_ERR(clip->pwm)) {
		unsigned int period = pwm_get_period(clip->pwm);

		pwm_config(clip->pwm, period/2, period);
	} else {
		clip->pwm = NULL;
	}

	return 0;
}

static int nx_sensor_reg_parse_dt(struct device *dev, struct device_node *np,
	struct nx_rearcam *me)
{
	int size = 0;
	const __be32 *list;
	int i = 0;
	u8 addr, val;

	list = of_get_property(np, "data", &size);
	if (!list)
		return -ENOENT;

	size /= 8;
	if (me->init_data != NULL)
		kfree(me->init_data);

	me->init_data = kcalloc(size+1, sizeof(struct reg_val), GFP_KERNEL);

	for (i = 0; i < size; i++) {
		addr = be32_to_cpu(*list++);
		val = be32_to_cpu(*list++);

		(me->init_data+i)->reg = addr;
		(me->init_data+i)->val = val;

		pr_debug("%s - size : %d, addr : 0x%02x, val: 0x%02x\n",
			__func__, size, addr, val);
	}

	(me->init_data+size)->reg = 0xFF;
	(me->init_data+size)->val = 0xFF;

	return 0;
}

static int nx_gpio_parse_dt(struct device *dev, struct device_node *np,
	struct nx_rearcam *me)
{
	me->event_gpio = of_get_named_gpio(np, "event-gpio", 0);
	if (!gpio_is_valid(me->event_gpio)) {
		dev_err(dev, "invalid event-gpio\n");
		return -EINVAL;
	}

	if (of_property_read_u32(np, "active_high", &me->active_high)) {
		dev_err(dev, "failed to get dt active high\n");
		return -EINVAL;
	}

	if (of_property_read_u32(np, "detect_delay", &me->detect_delay)) {
		dev_err(dev, "failed to get dt interface_type\n");
		return -EINVAL;
	}

	return 0;
}

static int nx_dpc_parse_dt(struct device *dev, struct nx_rearcam *me)
{
	struct device_node *d_np;
	struct device_node *p_np;
	struct device_node *np = NULL;
	struct device_node *t_np = NULL;
	struct device_node *child_dpc_node = NULL;

	struct dp_ctrl_info *ctl = &me->dp_ctl;
	struct dp_sync_info *sync = &me->dp_sync;

	d_np = dev->of_node;

	child_dpc_node = of_find_node_by_name(d_np, "dpc");
	if (!child_dpc_node) {
		dev_err(dev, "failed to get dpc node\n");
		return -EINVAL;
	}

	if (of_property_read_u32(child_dpc_node, "module", &me->dpc_module)) {
		dev_err(dev, "failed to get dt dpc module!\n");
		return -EINVAL;
	}

	p_np = _of_get_node_by_property(dev, d_np, "dp_drm_dev");
	if (!p_np) {
		dev_err(dev, "failed to get dp_drm_dev node\n");
		return -EINVAL;
	}

	np = of_find_node_by_name(p_np, "dp_control");
	if (!np) {
		dev_err(dev, "failed to get display control node\n");
		return -EINVAL;
	}

	t_np = of_find_node_by_name(p_np, "display-timing");
	if (!np) {
		dev_err(dev, "failed to get display control node\n");
		return -EINVAL;
	}

	parse_read_prop(np, "clk_src_lv0", ctl->clk_src_lv0);
	parse_read_prop(np, "clk_div_lv0", ctl->clk_div_lv0);
	parse_read_prop(np, "clk_src_lv1", ctl->clk_src_lv1);
	parse_read_prop(np, "clk_div_lv1", ctl->clk_div_lv1);
	parse_read_prop(np, "out_format", ctl->out_format);
	parse_read_prop(np, "invert_field", ctl->invert_field);
	parse_read_prop(np, "swap_rb", ctl->swap_rb);
	parse_read_prop(np, "yc_order", ctl->yc_order);
	parse_read_prop(np, "delay_mask", ctl->delay_mask);
	parse_read_prop(np, "d_rgb_pvd", ctl->d_rgb_pvd);
	parse_read_prop(np, "d_hsync_cp1", ctl->d_hsync_cp1);
	parse_read_prop(np, "d_vsync_fram", ctl->d_vsync_fram);
	parse_read_prop(np, "d_de_cp2", ctl->d_de_cp2);
	parse_read_prop(np, "vs_start_offset", ctl->vs_start_offset);
	parse_read_prop(np, "vs_end_offset", ctl->vs_end_offset);
	parse_read_prop(np, "ev_start_offset", ctl->ev_start_offset);
	parse_read_prop(np, "ev_end_offset", ctl->ev_end_offset);
	parse_read_prop(np, "vck_select", ctl->vck_select);
	parse_read_prop(np, "clk_inv_lv0", ctl->clk_inv_lv0);
	parse_read_prop(np, "clk_delay_lv0", ctl->clk_delay_lv0);
	parse_read_prop(np, "clk_inv_lv1", ctl->clk_inv_lv1);
	parse_read_prop(np, "clk_delay_lv1", ctl->clk_delay_lv1);
	parse_read_prop(np, "clk_sel_div1", ctl->clk_sel_div1);

	sync->h_sync_invert = 0;
	sync->v_sync_invert = 0;

	parse_read_prop(t_np, "hactive", sync->h_active_len);
	parse_read_prop(t_np, "vactive", sync->v_active_len);
	parse_read_prop(t_np, "hfront-porch", sync->h_front_porch);
	parse_read_prop(t_np, "vfront-porch", sync->v_front_porch);
	parse_read_prop(t_np, "hback-porch", sync->h_back_porch);
	parse_read_prop(t_np, "vback-porch", sync->v_back_porch);
	parse_read_prop(t_np, "hsync-len", sync->h_sync_width);
	parse_read_prop(t_np, "vsync-len", sync->v_sync_width);

	pr_debug("%s - clk_src_lv0 : %d\n", __func__, ctl->clk_src_lv0);
	pr_debug("%s - clk_div_lv0 : %d\n", __func__, ctl->clk_div_lv0);
	pr_debug("%s - clk_src_lv1 : %d\n", __func__, ctl->clk_src_lv1);
	pr_debug("%s - clk_div_lv1 : %d\n", __func__, ctl->clk_div_lv1);
	pr_debug("%s -  out_format : %d\n", __func__, ctl->out_format);

	pr_debug("%s - hactive : %d\n", __func__, sync->h_active_len);
	pr_debug("%s - vactive : %d\n", __func__, sync->v_active_len);
	pr_debug("%s - hfront-porch : %d\n", __func__, sync->h_front_porch);
	pr_debug("%s - vfront-porch : %d\n", __func__, sync->v_front_porch);
	pr_debug("%s - hback-porch : %d\n", __func__, sync->h_back_porch);
	pr_debug("%s - vback-porch : %d\n", __func__, sync->v_back_porch);
	pr_debug("%s - hsync-len : %d\n", __func__, sync->h_sync_width);
	pr_debug("%s - vsync-len : %d\n", __func__, sync->v_sync_width);

	return 0;
}

static int nx_mlc_parse_dt(struct device *dev, struct device_node *np,
	struct nx_rearcam *me)
{
	if (of_property_read_u32(np, "format", &me->rgb_format)) {
		dev_err(dev, "failed to get dt rgb format!\n");
		return -EINVAL;
	}

	if (of_property_read_u32(np, "module", &me->mlc_module)) {
		dev_err(dev, "failed to get dt mlc module!\n");
		return -EINVAL;
	}

	return 0;
}

static int nx_clipper_parse_dt(struct device *dev, struct device_node *np,
	struct nx_clipper_info *clip)
{
	int ret;
	struct device_node *child_node = NULL;

	if (of_property_read_u32(np, "module", &clip->module)) {
		dev_err(dev, "failed to get dt module\n");
		return -EINVAL;
	}

	if (of_property_read_u32(np, "interface_type", &clip->interface_type)) {
		dev_err(dev, "failed to get dt interface_type\n");
		return -EINVAL;
	}

	if (clip->interface_type == NX_CAPTURE_INTERFACE_MIPI_CSI) {
		/* mipi use always saclip config, so ignore user config */
		if (clip->module != 0) {
			dev_err(dev, "module of mipi-csi must be 0 but,\n");
			dev_err(dev, " current %d\n", clip->module);
			return -EINVAL;
		}
		clip->port = 1;
#ifdef CONFIG_ARCH_S5P4418
		clip->h_frontporch = 8;
		clip->h_syncwidth = 7;
		clip->h_backporch = 7;
		clip->v_frontporch = 1;
		clip->v_syncwidth = 8;
		clip->v_backporch = 1;
		clip->clock_invert = 0;
#else
		clip->h_frontporch = 4;
		clip->h_syncwidth = 4;
		clip->h_backporch = 4;
		clip->v_frontporch = 1;
		clip->v_syncwidth = 1;
		clip->v_backporch = 1;
		clip->clock_invert = 0;
#endif
		clip->bus_fmt = NX_VIN_CBY0CRY1;
		clip->interlace = 0;
	} else {
		if (of_property_read_u32(np, "port", &clip->port)) {
			dev_err(dev, "failed to get dt port\n");
			return -EINVAL;
		}

		if (of_property_read_u32(np, "external_sync",
					 &clip->external_sync)) {
			dev_err(dev, "failed to get dt external_sync\n");
			return -EINVAL;
		}

		if (clip->external_sync == 0) {
			/* when 656, porch value is always saclip,
			 * so ignore user config
			 */
			clip->h_frontporch = 7;
			clip->h_syncwidth = 1;
			clip->h_backporch = 10;
			clip->v_frontporch = 0;
			clip->v_syncwidth = 2;
			clip->v_backporch = 3;
		} else {
			if (of_property_read_u32(np, "h_frontporch",
						 &clip->h_frontporch)) {
				dev_err(dev, "failed to get dt h_frontporch\n");
				return -EINVAL;
			}
			if (of_property_read_u32(np, "h_syncwidth",
						 &clip->h_syncwidth)) {
				dev_err(dev, "failed to get dt h_syncwidth\n");
				return -EINVAL;
			}
			if (of_property_read_u32(np, "h_backporch",
						 &clip->h_backporch)) {
				dev_err(dev, "failed to get dt h_backporch\n");
				return -EINVAL;
			}
			if (of_property_read_u32(np, "v_frontporch",
						 &clip->v_frontporch)) {
				dev_err(dev, "failed to get dt v_frontporch\n");
				return -EINVAL;
			}
			if (of_property_read_u32(np, "v_syncwidth",
						 &clip->v_syncwidth)) {
				dev_err(dev, "failed to get dt v_syncwidth\n");
				return -EINVAL;
			}
			if (of_property_read_u32(np, "v_backporch",
						 &clip->v_backporch)) {
				dev_err(dev, "failed to get dt v_backporch\n");
				return -EINVAL;
			}
		}

		/* optional */
		of_property_read_u32(np, "interlace", &clip->interlace);
	}

	/* common property */
	of_property_read_u32(np, "data_order", &clip->bus_fmt);

	clip->regulator_nr = of_property_count_strings(np, "regulator_names");
	if (clip->regulator_nr > 0) {
		int i;
		const char *name;

		clip->regulator_names = devm_kcalloc(dev,
						   clip->regulator_nr,
						   sizeof(char *),
						   GFP_KERNEL);
		if (!clip->regulator_names) {
			WARN_ON(1);
			return -ENOMEM;
		}

		clip->regulator_voltages = devm_kcalloc(dev,
						      clip->regulator_nr,
						      sizeof(u32),
						      GFP_KERNEL);
		if (!clip->regulator_voltages) {
			WARN_ON(1);
			return -ENOMEM;
		}


		for (i = 0; i < clip->regulator_nr; i++) {
			if (of_property_read_string_index(np,
							"regulator_names",
							i, &name)) {
				dev_err(dev, "failed to read\n");
				dev_err(dev, " regulator %d name\n", i);
				return -EINVAL;
			}
			clip->regulator_names[i] = (char *)name;
		}

		of_property_read_u32_array(np, "regulator_voltages",
					   clip->regulator_voltages,
					   clip->regulator_nr);
	}

	child_node = of_find_node_by_name(np, "sensor");
	if (!child_node) {
		dev_err(dev, "failed to get sensor node\n");
		return -EINVAL;
	}
	ret = parse_sensor_dt(child_node, dev, clip);
	if (ret)
		return ret;

	child_node = of_find_node_by_name(np, "power");
	if (!child_node) {
		dev_err(dev, "failed to get power node\n");
		return -EINVAL;
	}
	ret = parse_power_dt(child_node, dev, clip);
	if (ret)
		return ret;

	ret = parse_clock_dt(child_node, dev, clip);
	if (ret)
		return ret;

	return 0;
}

/**
 * sensor power enable
 */
static int apply_gpio_action(struct device *dev, int gpio_num,
		  struct gpio_action_unit *unit)
{
	int ret;
	char label[64] = {0, };
	struct device_node *d_np;
	struct device_node *np;
	int gpio;

	d_np = dev->of_node;

	np = _of_get_node_by_property(dev, d_np, "rear_cam_dev");
	if (!np) {
		dev_err(dev, "failed to get clipper node\n");
		return -EINVAL;
	}

	gpio = of_get_named_gpio(np, "gpios", gpio_num);

	sprintf(label, "rear-cam #pwr gpio %d", gpio);
	if (!gpio_is_valid(gpio)) {
		dev_err(dev, "invalid gpio %d set to %d\n", gpio, unit->value);
		return -EINVAL;
	}

	ret = devm_gpio_request_one(dev, gpio,
				    unit->value ?
				    GPIOF_OUT_INIT_HIGH : GPIOF_OUT_INIT_LOW,
				    label);
	if (ret < 0) {
		dev_err(dev, "failed to gpio %d set to %d\n",
			gpio, unit->value);
		return ret;
	}

	if (unit->delay_ms > 0)
		mdelay(unit->delay_ms);

	devm_gpio_free(dev, gpio);

	return 0;
}

static int do_gpio_action(struct device *dev, struct nx_clipper_info *clip,
				 struct nx_capture_enable_gpio_action *action)
{
	int ret;
	struct gpio_action_unit *unit;
	int i;

	for (i = 0; i < action->count; i++) {
		unit = &action->units[i];
		ret = apply_gpio_action(dev, action->gpio_num, unit);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int do_pmic_action(struct device *dev, struct nx_clipper_info *clip,
				 struct nx_capture_enable_pmic_action *action)
{
	int ret;
	int i;
	struct regulator *power;

	for (i = 0; i < clip->regulator_nr; i++) {
		power = devm_regulator_get(dev,
					   clip->regulator_names[i]);
		if (IS_ERR(power)) {
			dev_err(dev, "failed to get power %s\n",
				clip->regulator_names[i]);
			return -ENODEV;
		}

		if (regulator_can_change_voltage(power)) {
			ret = regulator_set_voltage(power,
					    clip->regulator_voltages[i],
					    clip->regulator_voltages[i]);
			if (ret) {
				devm_regulator_put(power);
				dev_err(dev,
					"can't set voltages(index: %d)\n", i);
				return ret;
			}
		}

		ret = 0;
		if (action->enable && !regulator_is_enabled(power))
			ret = regulator_enable(power);
		else if (!action->enable && regulator_is_enabled(power))
			ret = regulator_disable(power);

		devm_regulator_put(power);

		if (ret) {
			dev_err(dev, "failed to power %s %s\n",
				clip->regulator_names[i],
				action->enable ? "enable" : "disable");
			return ret;
		}
	}

	return 0;
}

static int do_clock_action(struct device *dev, struct nx_clipper_info *clip,
			  struct nx_capture_enable_clock_action *action)
{
	if (clip->pwm)
		pwm_enable(clip->pwm);

	if (action->delay_ms > 0)
		mdelay(action->delay_ms);

	return 0;
}

static int enable_sensor_power(struct device *dev,
		struct nx_clipper_info *clip, bool enable)
{
	struct nx_capture_power_seq *seq = NULL;

	if (enable && !clip->sensor_enabled)
		seq = &clip->enable_seq;
	else if (!enable && clip->sensor_enabled)
		seq = &clip->disable_seq;

	if (seq) {
		int i;
		struct nx_capture_power_action *action;

		for (i = 0; i < seq->count; i++) {
			action = &seq->actions[i];
			switch (action->type) {
			case NX_ACTION_TYPE_GPIO:
				do_gpio_action(dev, clip, action->action);
				break;
			case NX_ACTION_TYPE_PMIC:
				do_pmic_action(dev, clip, action->action);
				break;
			case NX_ACTION_TYPE_CLOCK:
				do_clock_action(dev, clip, action->action);
				break;
			default:
				pr_warn("unknown action type %d\n",
					action->type);
				break;
			}
		}
	}

	clip->sensor_enabled = enable;
	return 0;
}

static int nx_display_top_parse_dt(struct device *dev, struct device_node *node,
	struct nx_rearcam *me)
{
	struct device_node *np;
	const __be32 *list;
	const char *strings[8];
	u32 addr, length;
	int size, i, err;
	struct nx_rearcam_res *res = &me->res;

	res->base = of_iomap(node, 0);
	if (!res->base) {
		dev_err(dev, "fail : %s of_iomap\n", dev_name(dev));
		return -EINVAL;
	}

	pr_debug("top base  : 0x%lx\n",
		(unsigned long)res->base);

	err = of_property_read_string(node, "reset-names", &strings[0]);
	if (!err) {
		res->reset = of_reset_control_get(node, strings[0]);
		pr_debug("top reset : %s\n", strings[0]);
	}

	/*
	 * set sub device resources
	 */
	np = of_find_node_by_name(node, "dp-resource");
	if (!np)
		return 0;

	/* register base */
	list = of_get_property(np, "reg_base", &size);
	size /= 8;
	if (size > MAX_RES_NUM) {
		dev_err(dev, "error: over devs dt size %d (max %d)\n",
			size, MAX_RES_NUM);
		return -EINVAL;
	}

	for (i = 0; size > i; i++) {
		addr = be32_to_cpu(*list++);
		length = PAGE_ALIGN(be32_to_cpu(*list++));

		res->dev_bases[i] = ioremap(addr, length);
		if (!res->dev_bases[i])
			return -EINVAL;

		pr_debug("dev base  :  0x%x (0x%x) %p\n",
			addr, size, res->dev_bases[i]);
	}
	res->num_devs = size;

	/* clock gen base : 2 contents */
	list = of_get_property(np, "clk_base", &size);
	size /= 8;
	if (size > MAX_RES_NUM) {
		dev_err(dev, "error: over clks dt size %d (max %d)\n",
			size, MAX_RES_NUM);
		return -EINVAL;
	}

	for (i = 0; size > i; i++) {
		addr = be32_to_cpu(*list++);
		res->dev_clk_bases[i] = ioremap(addr, PAGE_SIZE);
		res->dev_clk_ids[i] = be32_to_cpu(*list++);
		pr_debug("dev clock : [%d] clk 0x%x, %d\n",
			i, addr, res->dev_clk_ids[i]);
	}
	res->num_dev_clks = size;

	/* resets */
	size = of_property_read_string_array(np,
			"reset-names", strings, MAX_RES_NUM);
	for (i = 0; size > i; i++) {
		res->dev_resets[i] = of_reset_control_get(np, strings[i]);
		pr_debug("dev reset : [%d] %s\n", i, strings[i]);
	}
	res->num_dev_resets = size;

	return 0;
}

static int nx_display_parse_dt(struct device *dev, struct device_node *np,
	struct nx_rearcam *me)
{
	const char *strings[10];
	int i, n, size = 0;

	size = of_property_read_string_array(np,
		"reg-names", strings, ARRAY_SIZE(strings));
	if (size > ARRAY_SIZE(strings))
		return -EINVAL;

	if (me->base_addr != NULL)
		kfree(me->base_addr);

	me->base_addr = kcalloc(size, sizeof(void __iomem **),
		GFP_KERNEL);
	for (n = 0, i = 0; size > i; i++) {
		pr_debug("%s - size : %d, addr : %s\n",
			__func__, size, strings[i]);
		*(me->base_addr + i) = of_iomap(np, i);
	}

	return 0;
}

static int nx_reset_apply(struct reset_control *reset_ctrl)
{
	bool reset = reset_control_status(reset_ctrl);

	if (reset)
		reset_control_assert(reset_ctrl);

	reset_control_deassert(reset_ctrl);

	return 0;
}

static int nx_reset_parse_dt(struct device *dev, struct nx_rearcam *me)
{
	const char *strings[10];
	int size = 0;
	struct device_node *np = dev->of_node;
	struct device_node *child_display_node = NULL;

	/* display top */
	child_display_node = _of_get_node_by_property(dev, np, "display");
	if (!child_display_node) {
		dev_err(dev, "failed to get display node\n");
		return -EINVAL;
	}

	size = of_property_read_string_array(child_display_node,
		"reset-names", strings, ARRAY_SIZE(strings));
	if (size > ARRAY_SIZE(strings))
		return -EINVAL;

	pr_debug("%s - size : %d, dual_display : %s, display_top : %s\n",
			__func__, size, strings[0], strings[1]);

	/* mlc, dpc */
	me->reset_dual_display = of_reset_control_get(child_display_node,
		strings[0]);
	/* display top */
	me->reset_display_top = of_reset_control_get(child_display_node,
		strings[1]);

	return 0;
}

static int nx_interrupt_parse_dt(struct device *dev, struct nx_rearcam *me)
{
	const char *strings[10];
	int int_num[10];
	int size = 0;
	struct device_node *np = dev->of_node;
	struct device_node *child_display_node = NULL;
	int module = me->dpc_module;

	child_display_node = _of_get_node_by_property(dev, np, "display");
	if (!child_display_node) {
		dev_err(dev, "failed to get display node\n");
		return -EINVAL;
	}

	size = of_property_read_string_array(child_display_node,
		"interrupts-names", strings, ARRAY_SIZE(strings));
	if (size > ARRAY_SIZE(strings))
		return -EINVAL;

	of_property_read_u32_array(child_display_node,
		"interrupts", int_num, size);

	me->irq_dpc = int_num[module];

	return 0;
}

static int nx_rearcam_parse_dt(struct device *dev, struct nx_rearcam *me)
{
	int ret;
	struct device_node *np = dev->of_node;
	struct nx_clipper_info *clipper_info = &me->clipper_info;
	struct device_node *child_clipper_node = NULL;
	struct device_node *child_gpio_node = NULL;
	struct device_node *child_sensor_reg_node = NULL;
	struct device_node *child_mlc_node = NULL;
	struct device_node *child_display_top_node = NULL;
	struct device_node *child_display_node = NULL;

	if (of_property_read_u32(np, "rotation", &me->rotation)) {
		dev_err(dev, "failed to get dt rotation\n");
		return -EINVAL;
	}

	if (of_property_read_u32(np, "width", &me->width)) {
		dev_err(dev, "failed to get dt width.\n");
		return -EINVAL;
	}
	me->clipper_info.width = me->width;

	if (of_property_read_u32(np, "height", &me->height)) {
		dev_err(dev, "failed to get dt height.\n");
		return -EINVAL;
	}
	me->clipper_info.height = me->height;

	child_sensor_reg_node = _of_get_node_by_property(dev, np, "sensor_reg");
	if (!child_sensor_reg_node) {
		dev_err(dev, "failed to get clipper node\n");
		return -EINVAL;
	}

	ret = nx_sensor_reg_parse_dt(dev, child_sensor_reg_node, me);
	if (ret) {
		dev_err(dev, "failed to parse sensor register dt\n");
		return ret;
	}

	child_gpio_node = of_find_node_by_name(np, "gpio");
	if (!child_gpio_node) {
		dev_err(dev, "failed to get gpio node\n");
		return -EINVAL;
	}

	ret = nx_gpio_parse_dt(dev, child_gpio_node, me);
	if (ret) {
		dev_err(dev, "failed to parse gpio dt\n");
		return ret;
	}

	child_clipper_node = _of_get_node_by_property(dev, np, "rear_cam_dev");
	if (!child_clipper_node) {
		dev_err(dev, "failed to get clipper node\n");
		return -EINVAL;
	}

	ret = nx_clipper_parse_dt(dev, child_clipper_node, clipper_info);
	if (ret) {
		dev_err(dev, "failed to parse dt\n");
		return ret;
	}

	child_display_top_node = _of_get_node_by_property(dev, dev->of_node,
		"dp_drm_dev");
	if (!child_display_top_node) {
		dev_err(dev, "failed to get dp_drm_dev node\n");
		return -EINVAL;
	}

	ret = nx_display_top_parse_dt(dev, child_display_top_node, me);
	if (ret) {
		dev_err(dev, "failed to display top parse dt\n");
		return ret;
	}

	child_display_node = _of_get_node_by_property(dev, np, "display");
	if (!child_display_node) {
		dev_err(dev, "failed to get display node\n");
		return -EINVAL;
	}

	ret = nx_display_parse_dt(dev, child_display_node, me);
	if (ret) {
		dev_err(dev, "failed to parse dt\n");
		return ret;
	}

	child_mlc_node = of_find_node_by_name(np, "mlc");
	if (!child_mlc_node) {
		dev_err(dev, "failed to get mlc node\n");
		return -EINVAL;
	}

	ret = nx_mlc_parse_dt(dev, child_mlc_node, me);
	if (ret) {
		dev_err(dev, "failed to parse gpio dt\n");
		return ret;
	}

	ret = nx_reset_parse_dt(dev, me);
	if (ret) {
		dev_err(dev, "failed to parse reset dt\n");
		return ret;
	}

	ret = nx_interrupt_parse_dt(dev, me);
	if (ret) {
		dev_err(dev, "failed to parse interrupt dt\n");
		return ret;
	}

	ret = nx_dpc_parse_dt(dev, me);
	if (ret) {
		dev_err(dev, "failed to parse dpc dt\n");
		return ret;
	}

	return 0;
}

static inline u32 _get_pixel_byte(u32 rgb_format)
{
	switch (rgb_format) {
	case nx_mlc_rgbfmt_r5g6b5:
	case nx_mlc_rgbfmt_b5g6r5:
	case nx_mlc_rgbfmt_x1r5g5b5:
	case nx_mlc_rgbfmt_x1b5g5r5:
	case nx_mlc_rgbfmt_x4r4g4b4:
	case nx_mlc_rgbfmt_x4b4g4r4:
	case nx_mlc_rgbfmt_x8r3g3b2:
	case nx_mlc_rgbfmt_x8b3g3r2:
	case nx_mlc_rgbfmt_a1r5g5b5:
	case nx_mlc_rgbfmt_a1b5g5r5:
	case nx_mlc_rgbfmt_a4r4g4b4:
	case nx_mlc_rgbfmt_a4b4g4r4:
	case nx_mlc_rgbfmt_a8r3g3b2:
	case nx_mlc_rgbfmt_a8b3g3r2:
		return 2;

	case nx_mlc_rgbfmt_r8g8b8:
	case nx_mlc_rgbfmt_b8g8r8:
		return 3;

	case nx_mlc_rgbfmt_a8r8g8b8:
	case nx_mlc_rgbfmt_a8b8g8r8:
		return 4;

	default:
		return -1;
	}

	return 0;
}

static int _stride_cal(struct nx_rearcam *me, enum FRAME_KIND type)
{
	int stride;
	int width = me->clipper_info.width;

	switch (type) {
	case Y:
		stride = YUV_STRIDE(width);
		break;
	case CB:
		stride = YUV_STRIDE(width/2);
		break;
	case CR:
		stride = YUV_STRIDE(width/2);
		break;

	}

	return stride;
}

static void set_vip(struct nx_clipper_info *clip)
{
	u32 module = clip->module;
	bool is_mipi = clip->interface_type == NX_CAPTURE_INTERFACE_MIPI_CSI;

	nx_vip_set_input_port(module, clip->port);
	nx_vip_set_field_mode(module, false, nx_vip_fieldsel_bypass,
			      clip->interlace, false);

	if (is_mipi) {
		nx_vip_set_data_mode(module, clip->bus_fmt, 16);
		nx_vip_set_dvalid_mode(module, true, true, true);
		nx_vip_set_hvsync_for_mipi(module,
					   clip->width * 2,
					   clip->height,
					   clip->h_syncwidth,
					   clip->h_frontporch,
					   clip->h_backporch,
					   clip->v_syncwidth,
					   clip->v_frontporch,
					   clip->v_backporch);
	} else {
		nx_vip_set_data_mode(module, clip->bus_fmt, 8);
		nx_vip_set_dvalid_mode(module, false, false, false);
		nx_vip_set_hvsync(module,
				  clip->external_sync,
				  clip->width * 2,
				  clip->interlace ?
				  clip->height >> 1 : clip->height,
				  clip->h_syncwidth,
				  clip->h_frontporch,
				  clip->h_backporch,
				  clip->v_syncwidth,
				  clip->v_frontporch,
				  clip->v_backporch);
	}

	nx_vip_set_fiforeset_mode(module, nx_vip_fiforeset_all);
	nx_vip_set_clip_region(module,
			       0,
			       0,
			       clip->width,
			       clip->interlace ?
			       clip->height >> 1 :
			       clip->height);
}

static void _reset_values(struct nx_rearcam *me)
{
	me->frame_set.cur_entry_vip = NULL;

	me->is_display_on = false;
	me->is_mlc_on = false;
}

static void _reset_queue(struct nx_rearcam *me)
{
	me->q_vip_empty.clear(&me->q_vip_empty);
	me->q_vip_done.clear(&me->q_vip_done);

	if (me->rotation) {
		me->q_rot_empty.clear(&me->q_rot_empty);
		me->q_rot_done.clear(&me->q_rot_done);
	}
}

static int _all_buffer_to_empty_queue(struct nx_rearcam *me)
{
	int i = 0;

	struct queue_entry *entry = NULL;
	struct nx_video_buf *buf = NULL;

	me->q_vip_empty.init(&me->q_vip_empty);
	me->q_vip_done.init(&me->q_vip_done);

	me->q_rot_empty.init(&me->q_rot_empty);
	me->q_rot_done.init(&me->q_rot_done);

	for (i = 0; i < MAX_BUFFER_COUNT; i++) {
		entry = &me->frame_set.vip_queue_entry[i];
		buf = &me->frame_set.vip_bufs[i];
		pr_debug("%s: vip lu 0x%x, cb 0x%x, cr 0x%x\n", __func__,
			buf->lu_addr, buf->cb_addr, buf->cr_addr);

		entry->data = buf;
		me->q_vip_empty.enqueue(&me->q_vip_empty, entry);
	}

	if (me->rotation) {
		for (i = 0; i < MAX_BUFFER_COUNT; i++) {
			entry = &me->frame_set.rot_queue_entry[i];
			buf = &me->frame_set.rot_bufs[i];
			pr_debug("%s: rotation lu 0x%x, cb 0x%x, cr 0x%x\n",
				__func__,
				buf->lu_addr,
				buf->cb_addr,
				buf->cr_addr);

			entry->data = buf;
			me->q_rot_empty.enqueue(&me->q_rot_empty, entry);
		}
	}

	return 0;
}

static void _set_display_top(struct nx_rearcam *me)
{
	int i;
	int clkid = 3;
	struct dp_ctrl_info *ctrl = &me->dp_ctl;

	for (i = 0; i < me->res.num_dev_resets; i++)
		nx_reset_apply(me->res.dev_resets[i]);

	for (i = 0; i < me->res.num_dev_resets; i++)
		reset_control_deassert(me->res.dev_resets[i]);

	for (i = 0; i < me->res.num_dev_clks; i++) {
		nx_disp_top_clkgen_set_base_address(me->res.dev_clk_ids[i],
			(void __iomem *)me->res.dev_clk_bases[i]);
		nx_disp_top_clkgen_set_clock_pclk_mode(me->res.dev_clk_ids[i],
			nx_pclkmode_always);
	}

	nx_disp_top_clkgen_set_clock_divisor_enable(clkid, 0);
	nx_disp_top_clkgen_set_clock_source(clkid, 0, ctrl->clk_src_lv0);
	nx_disp_top_clkgen_set_clock_divisor(clkid, 0, ctrl->clk_div_lv0);
	nx_disp_top_clkgen_set_clock_source(clkid, 1, ctrl->clk_src_lv1);
	nx_disp_top_clkgen_set_clock_divisor(clkid, 1, ctrl->clk_div_lv1);
	nx_disp_top_clkgen_set_clock_divisor_enable(clkid, 1);
}

static void _vip_hw_set_addr(int module, struct nx_rearcam *me,
	u32 lu_addr, u32 cb_addr, u32 cr_addr)
{
	u32 width = me->clipper_info.width;
	u32 height = me->clipper_info.height;
	u32 interlace = me->clipper_info.interlace;
	bool is_mipi = me->clipper_info.interface_type ==
			NX_CAPTURE_INTERFACE_MIPI_CSI;

	u32 lu_stride = _stride_cal(me, Y);
	u32 cb_stride = _stride_cal(me, CB);

	if (is_mipi) {
		nx_vip_set_clipper_addr(
			module,
			nx_vip_format_420,
			width,
			height,
			lu_addr,
			cb_addr,
			cr_addr,
			interlace ? ALIGN(width, 64) : lu_stride,
			interlace ? ALIGN(width/2, 64) : cb_stride
			);
	} else {
		nx_vip_set_clipper_addr(
			module,
			nx_vip_format_420,
			width,
			height,
			lu_addr,
			cb_addr,
			cr_addr,
			lu_stride,
			cb_stride
			);
	}
}

static inline bool is_reargear_on(struct nx_rearcam *me)
{
	bool is_on = gpio_get_value(me->event_gpio);

	if (!me->active_high)
		is_on ^= 1;

	return is_on;
}

static inline bool is_running(struct nx_rearcam *me)
{
	return nx_mlc_get_layer_enable(me->mlc_module, 3);
}

static void _set_vip_interrupt(struct nx_rearcam *me, bool enable)
{
	int module = me->clipper_info.module;

	nx_vip_clear_interrupt_pending_all(module);
	nx_vip_set_interrupt_enable_all(module, false);

	if (enable)
		nx_vip_set_interrupt_enable(module, VIP_OD_INT, true);
}

static void _vip_run(struct nx_rearcam *me)
{
	struct queue_entry *ent = NULL;
	struct nx_video_buf *buf = NULL;
	int module = me->clipper_info.module;

	set_vip(&me->clipper_info);

	ent = me->q_vip_empty.dequeue(&me->q_vip_empty);
	if (ent) {
		buf = (struct nx_video_buf *)(ent->data);
		_vip_hw_set_addr(module, me,
			buf->lu_addr, buf->cb_addr, buf->cr_addr);

		pr_debug("%s: vip lu 0x%x, cb 0x%x, cr 0x%x\n", __func__,
			buf->lu_addr, buf->cb_addr, buf->cr_addr);

		me->frame_set.cur_entry_vip = ent;
	} else
		WARN_ON(true);


	nx_vip_set_vipenable(module, true, true, true, false);
	/* nx_vip_dump_register(module); */
}

static void _vip_stop(struct nx_rearcam *me)
{
	int module = me->clipper_info.module;

	nx_vip_set_vipenable(module, false, false, false, false);
}

static void _mlc_video_run(struct nx_rearcam *me)
{
	int module = me->mlc_module;

	nx_mlc_set_top_dirty_flag(module);
	nx_mlc_set_video_layer_line_buffer_power_mode(module, 1);
	nx_mlc_set_video_layer_line_buffer_sleep_mode(module, 0);
	nx_mlc_set_layer_enable(module, MLC_LAYER_VIDEO, 1);
	nx_mlc_set_dirty_flag(module, MLC_LAYER_VIDEO);
}

static void _mlc_video_stop(struct nx_rearcam *me)
{
	int module = me->mlc_module;

	nx_mlc_set_top_dirty_flag(module);
	nx_mlc_set_video_layer_line_buffer_power_mode(module, 0);
	nx_mlc_set_video_layer_line_buffer_sleep_mode(module, 1);
	nx_mlc_set_layer_enable(module, MLC_LAYER_VIDEO, 0);
	nx_mlc_set_dirty_flag(module, MLC_LAYER_VIDEO);
}

static void _mlc_overlay_run(struct nx_rearcam *me)
{
	int module = me->mlc_module;
	u32 layer = MLC_LAYER_RGB_OVERLAY;

	nx_mlc_set_layer_enable(module, layer, 1);
	nx_mlc_set_dirty_flag(module, layer);
}

static void _mlc_overlay_stop(struct nx_rearcam *me)
{
	int module = me->mlc_module;
	u32 layer = MLC_LAYER_RGB_OVERLAY;

	nx_mlc_set_layer_enable(module, layer, 0);
	nx_mlc_set_dirty_flag(module, layer);
}

static void _set_mlc_video(struct nx_rearcam *me)
{
	int module = me->mlc_module;
	int src_width = me->clipper_info.width;
	int src_height = me->clipper_info.height;
	int dst_width;
	int dst_height;
	int hf, vf;

	if (me->rotation > 0) {
		src_width = me->rot_width_rate_y_size;
		src_height = me->width;
	}

	nx_mlc_get_screen_size(module, &dst_width, &dst_height);

	hf = 1, vf = 1;

	if (src_width == dst_width && src_height == dst_height)
		hf = 0, vf = 0;

	nx_mlc_set_format_yuv(module, nx_mlc_yuvfmt_420);
	nx_mlc_set_video_layer_scale(module, src_width, src_height, dst_width,
		dst_height, hf, hf, vf, vf);
	nx_mlc_set_position(module, MLC_LAYER_VIDEO, 8, 0, dst_width-1,
		dst_height-1);
	nx_mlc_set_layer_priority(module, 1);
	nx_mlc_set_dirty_flag(module, MLC_LAYER_VIDEO);
}

static void _mlc_video_set_addr(struct nx_rearcam *me,
	struct nx_video_buf *buf)
{
	int module = me->mlc_module;

	nx_mlc_set_video_layer_stride(module, buf->lu_stride, buf->cb_stride,
		buf->cr_stride);
	nx_mlc_set_video_layer_address(module, buf->lu_addr, buf->cb_addr,
		buf->cr_addr);

	nx_mlc_set_video_layer_line_buffer_power_mode(module, 1);
	nx_mlc_set_video_layer_line_buffer_sleep_mode(module, 0);
	nx_mlc_set_dirty_flag(module, MLC_LAYER_VIDEO);
}

static void _set_mlc_overlay(struct nx_rearcam *me)
{
	int module = me->mlc_module;
	u32 format = me->rgb_format;
	u32 pixelbyte = _get_pixel_byte(format);
	struct dp_sync_info *sync = &me->dp_sync;

	u32 stride = sync->h_active_len * pixelbyte;
	u32 layer = MLC_LAYER_RGB_OVERLAY;
	int en_alpha = 0;

	if (format == nx_mlc_rgbfmt_a1r5g5b5 ||
	    format == nx_mlc_rgbfmt_a1b5g5r5 ||
	    format == nx_mlc_rgbfmt_a4r4g4b4 ||
	    format == nx_mlc_rgbfmt_a4b4g4r4 ||
	    format == nx_mlc_rgbfmt_a8r3g3b2 ||
	    format == nx_mlc_rgbfmt_a8b3g3r2 ||
	    format == nx_mlc_rgbfmt_a8r8g8b8 ||
	    format == nx_mlc_rgbfmt_a8b8g8r8)
		en_alpha = 1;

	nx_mlc_set_color_inversion(module, layer, 0, 0);
	nx_mlc_set_alpha_blending(module, layer, en_alpha, 0);
	nx_mlc_set_format_rgb(module, layer, (enum nx_mlc_rgbfmt)format);
	nx_mlc_set_rgblayer_invalid_position(module, layer, 0, 0, 0, 0, 0, 0);
	nx_mlc_set_rgblayer_invalid_position(module, layer, 1, 0, 0, 0, 0, 0);
	nx_mlc_set_position(module, layer, 0, 0, sync->h_active_len-1,
				sync->v_active_len-1);

	nx_mlc_set_rgblayer_stride(module, layer, pixelbyte, stride);
	nx_mlc_set_rgblayer_address(module, layer,
		me->frame_set.rgb_buf.rgb_addr);
	nx_mlc_set_dirty_flag(module, layer);
}

static void _mlc_rgb_overlay_draw(struct nx_rearcam *me)
{
	struct nx_rgb_buf *rgb_buf = &me->frame_set.rgb_buf;
	struct dp_sync_info *sync = &me->dp_sync;
	u32 format = me->rgb_format;
	u32 pixelbyte = _get_pixel_byte(format);

	pr_debug("%s - rgb width : %d, rgb height : %d\n", __func__,
		sync->h_active_len, sync->v_active_len);

	if (me->draw_rgb_overlay)
		me->draw_rgb_overlay(sync->h_active_len, sync->v_active_len,
					pixelbyte, me->rotation,
					(void *)me->vendor_context,
					rgb_buf->virt_rgb);
}

static void set_enable_mlc(struct nx_rearcam *me)
{
	int module = me->mlc_module;
	struct dp_sync_info *sync = &me->dp_sync;

	nx_mlc_set_field_enable(module, sync->interlace);
	nx_mlc_set_rgblayer_gama_table_power_mode(module, 0, 0, 0);
	nx_mlc_set_rgblayer_gama_table_sleep_mode(module, 1, 1, 1);
	nx_mlc_set_rgblayer_gamma_enable(module, 0);
	nx_mlc_set_dither_enable_when_using_gamma(module, 0);
	nx_mlc_set_gamma_priority(module, 0);
	nx_mlc_set_top_power_mode(module, 1);
	nx_mlc_set_top_sleep_mode(module, 0);
	nx_mlc_set_mlc_enable(module, 1);

	nx_mlc_set_top_dirty_flag(module);
}

static void _set_disable_mlc(struct nx_rearcam *me)
{
	int module = me->mlc_module;

	nx_mlc_set_top_power_mode(module, 0);
	nx_mlc_set_top_sleep_mode(module, 1);
	nx_mlc_set_mlc_enable(module, 0);
	nx_mlc_set_top_dirty_flag(module);
}

static void _set_mlc(struct nx_rearcam *me)
{
	int module = me->mlc_module;
	unsigned int bgcolor = 0x0;
	struct dp_sync_info *sync = &me->dp_sync;

	nx_mlc_set_clock_pclk_mode(module, nx_pclkmode_always);
	nx_mlc_set_clock_bclk_mode(module, nx_bclkmode_always);

	nx_mlc_set_screen_size(module, sync->h_active_len, sync->v_active_len);
	nx_mlc_set_background(module, bgcolor & 0x00FFFFFF);
	nx_mlc_set_layer_priority(module, MLC_LAYER_VIDEO);
	nx_mlc_set_top_dirty_flag(module);

	_set_mlc_video(me);
	_set_mlc_overlay(me);
	_mlc_rgb_overlay_draw(me);
	set_enable_mlc(me);
}

static bool _is_enable_mlc(struct nx_rearcam *me)
{
	int module = me->mlc_module;

	return nx_mlc_get_mlc_enable(module) ? true : false;
}

static bool _is_enable_dpc(struct nx_rearcam *me)
{
	int module = me->dpc_module;

	return nx_dpc_get_dpc_enable(module) ? true : false;
}

static void _set_dpc_interrupt(struct nx_rearcam *me, bool enable)
{
	int module = me->dpc_module;

	nx_dpc_clear_interrupt_pending_all(module);
	nx_dpc_set_interrupt_enable_all(module, 0);

	if (enable)
		nx_dpc_set_interrupt_enable_all(module, 1);
}

static void _dpc_set_enable(struct nx_rearcam *me)
{
	int module = me->dpc_module;
	bool poweron;

	poweron = nx_dpc_get_dpc_enable(module) ? true : false;

	nx_dpc_clear_interrupt_pending_all(module);

	if (!poweron) {
		nx_dpc_set_reg_flush(module);

		nx_dpc_set_dpc_enable(module, 1);
		nx_dpc_set_clock_divisor_enable(module, 1);
	}
}

static void _dpc_set_disable(struct nx_rearcam *me)
{
	int module = me->dpc_module;

	nx_dpc_set_clock_divisor_enable(module, 0);
	nx_dpc_set_dpc_enable(module, 0);
}

static void _set_dpc(struct nx_rearcam *me)
{
	_dpc_set_disable(me);
	_dpc_set_display(me);
	_dpc_set_enable(me);
}

static void _dpc_set_display(struct nx_rearcam *me)
{
	int module = me->dpc_module;

	struct dp_sync_info *sync = &me->dp_sync;
	struct dp_ctrl_info *ctl = &me->dp_ctl;
	unsigned int out_format = ctl->out_format;
	unsigned int delay_mask = ctl->delay_mask;
	int rgb_pvd = 0, hsync_cp1 = 7, vsync_fram = 7, de_cp2 = 7;
	int v_vso = 1, v_veo = 1, e_vso = 1, e_veo = 1;

	int interlace = sync->interlace;
	int invert_field = ctl->invert_field;
	int swap_rb = ctl->swap_rb;
	unsigned int yc_order = ctl->yc_order;
	int vck_select = ctl->vck_select;
	int vclk_invert = ctl->clk_inv_lv0 | ctl->clk_inv_lv1;
	int emb_sync = (out_format == DPC_FORMAT_CCIR656 ? 1 : 0);

	enum nx_dpc_dither r_dither, g_dither, b_dither;
	int rgb_mode = 0;
	bool lcd_rgb = false;

	if (delay_mask & DP_SYNC_DELAY_RGB_PVD)
		rgb_pvd = ctl->d_rgb_pvd;
	if (delay_mask & DP_SYNC_DELAY_HSYNC_CP1)
		hsync_cp1 = ctl->d_hsync_cp1;
	if (delay_mask & DP_SYNC_DELAY_VSYNC_FRAM)
		vsync_fram = ctl->d_vsync_fram;
	if (delay_mask & DP_SYNC_DELAY_DE_CP)
		de_cp2 = ctl->d_de_cp2;

	if (ctl->vs_start_offset != 0 ||
	    ctl->vs_end_offset != 0 ||
	    ctl->ev_start_offset != 0 || ctl->ev_end_offset != 0) {
		v_vso = ctl->vs_start_offset;
		v_veo = ctl->vs_end_offset;
		e_vso = ctl->ev_start_offset;
		e_veo = ctl->ev_end_offset;
	}

	if ((nx_dpc_format_rgb555 == out_format) ||
	    (nx_dpc_format_mrgb555a == out_format) ||
	    (nx_dpc_format_mrgb555b == out_format)) {
		r_dither = g_dither = b_dither = nx_dpc_dither_5bit;
		rgb_mode = 1;
	} else if ((nx_dpc_format_rgb565 == out_format) ||
		   (nx_dpc_format_mrgb565 == out_format)) {
		r_dither = b_dither = nx_dpc_dither_5bit;
		g_dither = nx_dpc_dither_6bit, rgb_mode = 1;
	} else if ((nx_dpc_format_rgb666 == out_format) ||
		   (nx_dpc_format_mrgb666 == out_format)) {
		r_dither = g_dither = b_dither = nx_dpc_dither_6bit;
		rgb_mode = 1;
	} else {
		r_dither = g_dither = b_dither = nx_dpc_dither_bypass;
		rgb_mode = 1;
	}

	nx_dpc_set_clock_out_enb(module, 0, 0);
	nx_dpc_set_clock_out_enb(module, 1, 0);

	nx_dpc_set_clock_source(module, 0, ctl->clk_src_lv0 == 3 ?
		6 : ctl->clk_src_lv0);
	nx_dpc_set_clock_divisor(module, 0, ctl->clk_div_lv0);
	nx_dpc_set_clock_out_delay(module, 0, ctl->clk_delay_lv0);
	nx_dpc_set_clock_source(module, 1, ctl->clk_src_lv1);
	nx_dpc_set_clock_divisor(module, 1, ctl->clk_div_lv1);
	nx_dpc_set_clock_out_delay(module, 1, ctl->clk_delay_lv1);

	/* LCD out */
	if (lcd_rgb) {
		nx_dpc_set_mode(module, out_format, interlace, invert_field,
			rgb_mode, swap_rb, yc_order, emb_sync, emb_sync,
			vck_select, vclk_invert, 0);
		nx_dpc_set_hsync(module, sync->h_active_len,
				sync->h_sync_width, sync->h_front_porch,
				sync->h_back_porch, sync->h_sync_invert);
		nx_dpc_set_vsync(module, sync->v_active_len,
				sync->v_sync_width, sync->v_front_porch,
				sync->v_back_porch, sync->v_sync_invert,
				sync->v_active_len, sync->v_sync_width,
				sync->v_front_porch, sync->v_back_porch);
		nx_dpc_set_vsync_offset(module, v_vso, v_veo, e_vso, e_veo);
		nx_dpc_set_delay(module, rgb_pvd, hsync_cp1, vsync_fram,
				de_cp2);
		nx_dpc_set_dither(module, r_dither, g_dither, b_dither);
	} else {
		enum polarity fd_polarity = polarity_activehigh;
		enum polarity hs_polarity = sync->h_sync_invert ?
				polarity_activelow : polarity_activehigh;
		enum polarity vs_polarity = sync->v_sync_invert ?
				polarity_activelow : polarity_activehigh;

		nx_dpc_set_sync(module,
				progressive,
				sync->h_active_len,
				sync->v_active_len,
				sync->h_sync_width,
				sync->h_front_porch,
				sync->h_back_porch,
				sync->v_sync_width,
				sync->v_front_porch,
				sync->v_back_porch,
				fd_polarity, hs_polarity,
				vs_polarity, 0, 0, 0, 0, 0, 0, 0);

		nx_dpc_set_delay(module, rgb_pvd, hsync_cp1,
				vsync_fram, de_cp2);
		nx_dpc_set_output_format(module, out_format, 0);
		nx_dpc_set_dither(module, r_dither, g_dither, b_dither);
		nx_dpc_set_quantization_mode(module, qmode_256, qmode_256);
	}

	pr_debug("%s - module : %d\n", __func__, module);
	pr_debug("%s - hactive : %d\n", __func__, sync->h_active_len);
	pr_debug("%s - vactive : %d\n", __func__, sync->v_active_len);
	pr_debug("%s - hfront-porch : %d\n", __func__, sync->h_front_porch);
	pr_debug("%s - vfront-porch : %d\n", __func__, sync->v_front_porch);
	pr_debug("%s - hback-porch : %d\n", __func__, sync->h_back_porch);
	pr_debug("%s - vback-porch : %d\n", __func__, sync->v_back_porch);
	pr_debug("%s - hsync-len : %d\n", __func__, sync->h_sync_width);
	pr_debug("%s - vsync-len : %d\n", __func__, sync->v_sync_width);
}

static void _cancel_display_worker(struct nx_rearcam *me)
{
	cancel_work_sync(&me->work_display);
	flush_workqueue(me->wq_display);
}

static void _cancel_rot_worker(struct nx_rearcam *me)
{
	cancel_work_sync(&me->work_lu_rot);
	flush_workqueue(me->wq_lu_rot);

	cancel_work_sync(&me->work_cb_rot);
	flush_workqueue(me->wq_cb_rot);

	cancel_work_sync(&me->work_cr_rot);
	flush_workqueue(me->wq_cr_rot);
}

static void _cleanup_me(struct nx_rearcam *me)
{
	unsigned long flags;

	spin_lock_irqsave(&me->display_lock, flags);
	_set_dpc_interrupt(me, false);
	spin_unlock_irqrestore(&me->display_lock, flags);

	if (me->rotation) {
		mutex_lock(&me->rot_lock);
		mutex_unlock(&me->rot_lock);
	}

	_cancel_display_worker(me);

	if (me->rotation)
		_cancel_rot_worker(me);
}

static void _turn_on(struct nx_rearcam *me)
{
	struct device *dev = &me->pdev->dev;

	if (_is_enable_mlc(me) == false)
		_set_mlc(me);

	if (_is_enable_dpc(me) == false)
		_set_dpc(me);

	if (me->is_on)
		return;

	me->is_on = true;

	if (me->pre_turn_on) {
		if (!me->pre_turn_on(me->vendor_context)) {
			dev_err(dev, "%s: failed to pre_turn_on().\n",
				__func__);
			return;
		}
	}

	if (me->is_first == true)
		me->is_first = false;

	_reset_values(me);
	_all_buffer_to_empty_queue(me);
	_set_vip_interrupt(me, true);
	_vip_run(me);
}

static void _turn_off(struct nx_rearcam *me)
{
	_mlc_overlay_stop(me);
	_mlc_video_stop(me);

	_set_vip_interrupt(me, false);

	_cleanup_me(me);
	_reset_queue(me);

	me->is_display_on = false;
	me->is_on = false;

	if (me->post_turn_off)
		me->post_turn_off(me->vendor_context);
}

static void _decide(struct nx_rearcam *me)
{
	struct device *dev = &me->pdev->dev;

	if (me->decide) {
		if (!me->decide(me->vendor_context))
			dev_err(dev, "%s: failed to decide()\n", __func__);
	}

	me->running = is_running(me);
	me->reargear_on = is_reargear_on(me);
	pr_debug("%s: running %d, reargear on %d\n", __func__,
		me->running, me->reargear_on);
	if (me->reargear_on && !me->running) {
		_turn_on(me);
	} else if (me->running && !me->reargear_on) {
		_turn_off(me);
	} else if (!me->running && is_reargear_on(me)) {
		dev_err(&me->pdev->dev, "Recheck Rear Camera!!\n");
		schedule_delayed_work(&me->work,
			msecs_to_jiffies(me->detect_delay));
	}
}

static void _work_handler_reargear(struct work_struct *work)
{
	struct nx_rearcam *me = container_of(work,
				struct nx_rearcam, work.work);

	mutex_lock(&me->decide_work_lock);
	_decide(me);
	mutex_unlock(&me->decide_work_lock);
}

static irqreturn_t _irq_handler(int irq, void *devdata)
{
	struct nx_rearcam *me = devdata;

	nx_soc_gpio_clr_int_pend(PAD_GPIO_ALV + 3);

	mutex_lock(&me->decide_work_lock);
	cancel_delayed_work(&me->work);
	if (!is_reargear_on(me)) {
		schedule_delayed_work(&me->work, msecs_to_jiffies(100));
	} else {
		schedule_delayed_work(&me->work,
			msecs_to_jiffies(me->detect_delay));
	}
	mutex_unlock(&me->decide_work_lock);

	return IRQ_HANDLED;
}


static irqreturn_t _dpc_irq_handler(int irq, void *devdata)
{
	unsigned long flags;
	struct nx_rearcam *me = devdata;
	int module = me->dpc_module;

	nx_dpc_clear_interrupt_pending_all(module);

	spin_lock_irqsave(&me->display_lock, flags);
	queue_work(me->wq_display, &me->work_display);
	spin_unlock_irqrestore(&me->display_lock, flags);

	return IRQ_HANDLED;
}

static irqreturn_t _vip_irq_handler(int irq, void *devdata)
{
	unsigned long flags;
	bool interlace = false;
	bool do_process = true;

	struct nx_rearcam *me = devdata;
	int module = me->clipper_info.module;
	struct device *dev = &me->pdev->dev;

	struct queue_entry *entry = NULL;
	struct nx_video_buf *buf = NULL;

	nx_vip_clear_interrupt_pending_all(module);

	interlace = me->clipper_info.interlace;
	if (interlace) {
		bool is_odd = nx_vip_get_field_status(module);

		if (me->irq_count == 0) {
			if (!is_odd)
				me->irq_count++;
		} else {
			if (is_odd)
				me->irq_count++;
		}
	}

	if (me->irq_count == 2)
		me->irq_count = 0;
	else
		do_process = false;

	if (!do_process)
		return IRQ_HANDLED;

	if (me->frame_set.cur_entry_vip == NULL)
		return IRQ_HANDLED;

	spin_lock_irqsave(&me->vip_lock, flags);

#if TIME_LOG
	me->measure.vip_frame_cnt++;
#endif

	entry = me->frame_set.cur_entry_vip;
	buf = (struct nx_video_buf *)(entry->data);
	pr_debug("%s - lu_addr : 0x%x", __func__, buf->lu_addr);

	if (entry) {
		me->q_vip_done.enqueue(&me->q_vip_done, entry);
		me->frame_set.cur_entry_vip = NULL;
	}

	entry =  me->q_vip_empty.dequeue(&me->q_vip_empty);
	if (!entry) {
		dev_err(dev, "VIP empty buffer underrun!!\n");
		spin_unlock_irqrestore(&me->vip_lock, flags);
		return IRQ_HANDLED;
	}

	buf = (struct nx_video_buf *)(entry->data);
	_vip_hw_set_addr(module, me,
		buf->lu_addr, buf->cb_addr, buf->cr_addr);
	me->frame_set.cur_entry_vip = entry;

	if (me->rotation)
		queue_work(me->wq_lu_rot, &me->work_lu_rot);

	if (!me->is_display_on) {
		me->is_display_on = true;
		_set_dpc_interrupt(me, true);
	}
	spin_unlock_irqrestore(&me->vip_lock, flags);

	return IRQ_HANDLED;
}


static void _reset_hw_display(struct nx_rearcam *me)
{
	/* mlc, dpc reset */
	/* display top reset */
	nx_reset_apply(me->reset_display_top);
	nx_reset_apply(me->res.reset);
}

static void _init_hw_display_top(struct nx_rearcam *me)
{
	nx_disp_top_set_base_address(me->res.base);

	_set_display_top(me);
}

static void _init_hw_gpio(struct nx_rearcam *me)
{
	int ret;
	struct device *dev = &me->pdev->dev;

	ret = devm_gpio_request_one(dev, me->event_gpio,
					GPIOF_IN, dev_name(dev));
	if (ret)
		dev_err(dev, "unable to request GPIO %d\n", me->event_gpio);

	me->irq_event = gpio_to_irq(me->event_gpio);
	ret = devm_request_irq(dev, me->irq_event, _irq_handler,
			IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
			"rearcam-event-irq", me);
	if (ret) {
		dev_err(dev, "%s: failed to devm_request_irq(irqnum %d)\n",
			__func__, me->irq_event);
		return;
	}

	disable_irq(me->irq_event);
}

static void _init_hw_vip(struct nx_rearcam *me)
{
	int ret;
	int err;
	struct device *dev = &me->pdev->dev;
	int module = me->clipper_info.module;

	ret = platform_get_irq(me->pdev, module);
	if (ret < 0) {
		dev_err(dev, "failed to get module\n");
		return;
	}

	me->irq_vip = ret;
	sprintf(me->irq_name_vip, "nx-rearcam-vip%d", module);
	err = devm_request_irq(dev, me->irq_vip, &_vip_irq_handler,
		IRQF_SHARED, me->irq_name_vip, me);
	if (err) {
		dev_err(dev, "failed to devm_request_irq for vip %d\n",
			module);
		return;
	}
}

static void _init_hw_mlc(struct nx_rearcam *me)
{
	int module = me->mlc_module;

	nx_mlc_set_base_address(module, *(me->base_addr + 2));

	if (_is_enable_mlc(me) == false)
		_set_mlc(me);
	else
		_set_mlc_video(me);
}


static void _init_hw_dpc(struct nx_rearcam *me)
{
	struct device *dev = &me->pdev->dev;
	int module = me->dpc_module;
	int err;

	nx_dpc_set_base_address(module, *(me->base_addr + 0));

	if (_is_enable_dpc(me) == false)
		_set_dpc(me);

	nx_dpc_set_clock_pclk_mode(module, nx_pclkmode_always);

	me->wq_display = create_singlethread_workqueue("wq_display");
	if (!me->wq_display) {
		dev_err(dev, "create work queue error!\n");
		return;
	}

	me->irq_dpc += 16;
	sprintf(me->irq_name_dpc, "nx-rearcam-dpc%d", module);
	err = devm_request_irq(dev, me->irq_dpc, &_dpc_irq_handler,
		IRQF_SHARED, me->irq_name_dpc, me);
	if (err) {
		dev_err(dev, "failed to devm_request_irq for dpc %d\n",
			module);
		return;
	}
}

static int get_rotate_width_rate(struct nx_rearcam *me, enum FRAME_KIND type)
{
	int width = me->clipper_info.width;
	struct dp_sync_info *sync = &me->dp_sync;

	switch (type) {
	case Y:
		break;
	case CB:
	case CR:
		width /= 2;
	}

	return ((sync->h_active_len * width)/sync->v_active_len)-1;
}

static int _alloc_cacheable_buf(struct device *dev,
	struct nx_buf_meta *meta)
{
	char *data;
	dma_addr_t mapping;
	int size = PAGE_ALIGN(meta->size);

	data = kmalloc(size, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	mapping = dma_map_single(dev, data, size, meta->direction);
	if (unlikely(dma_mapping_error(dev, mapping)))
		goto err_out;

	meta->vaddr = data;
	meta->paddr = mapping;

	return 0;

err_out:
	kfree(data);

	return -1;

}

static void _free_cacheable_buf(struct device *dev, struct nx_buf_meta *meta)
{
	dma_unmap_single(dev, meta->paddr, meta->size, meta->direction);

	if (meta->vaddr != NULL) {
		kfree(meta->vaddr);
		meta->vaddr = NULL;
	}
}

static int _alloc_video_memory(struct nx_rearcam *me, struct nx_video_buf *buf)
{
	int size = 0;
	u32 lu_stride;
	u32 cb_stride;
	u32 cr_stride;
	struct device *dev = &me->pdev->dev;
	int width;
	int height;

	lu_stride = _stride_cal(me, Y);
	cb_stride = _stride_cal(me, CB);
	cr_stride = _stride_cal(me, CR);

	width = me->width;
	height = me->height;

	size = lu_stride * height +
		cb_stride * (height / 2) +
		cr_stride * (height / 2);
	size = PAGE_ALIGN(size);
	buf->page_size = size;

	buf->buf_meta.size = buf->page_size;
	buf->buf_meta.direction = DMA_TO_DEVICE;
	_alloc_cacheable_buf(dev, &buf->buf_meta);

	buf->virt_video = buf->buf_meta.vaddr;
	buf->dma_buf = buf->buf_meta.paddr;

	buf->lu_addr = buf->dma_buf;
	buf->cb_addr = buf->lu_addr + (lu_stride * height);
	buf->cr_addr = buf->cb_addr + (cb_stride * (height / 2));
	buf->lu_stride = lu_stride;
	buf->cb_stride = cb_stride;
	buf->cr_stride = cr_stride;
	buf->lu_size = lu_stride * height;
	buf->cb_size = cb_stride * (height / 2);
	buf->cr_size = cr_stride * (height / 2);



	pr_debug("%s - [addr] lu 0x%x, cb 0x%x, cr 0x%x, virt %p\n", __func__,
		buf->lu_addr, buf->cb_addr, buf->cr_addr, buf->virt_video);

	pr_debug("%s - [stride] lu %d, cb %d, cr %d\n", __func__,
		buf->lu_stride, buf->cb_stride, buf->cr_stride);

	pr_debug("%s - [size] lu %d, cb %d, cr %d\n", __func__,
		buf->lu_size, buf->cb_size, buf->cr_size);

	return 0;
}

static int _alloc_video_rot_memory(struct nx_rearcam *me,
	struct nx_video_buf *buf)
{
	int size = 0;
	u32 lu_stride;
	u32 cb_stride;
	u32 cr_stride;
	struct device *dev = &me->pdev->dev;

	int width = me->rot_width_rate_y_size;
	int height = me->width;

	lu_stride = YUV_STRIDE(width);
	cb_stride = YUV_STRIDE(width/2);
	cr_stride = YUV_STRIDE(width/2);

	size = lu_stride * height +
		cb_stride * (height / 2) +
		cr_stride * (height / 2);
	size = PAGE_ALIGN(size);
	buf->page_size = size;

	buf->buf_meta.size = buf->page_size;
	buf->buf_meta.direction = DMA_FROM_DEVICE;
	_alloc_cacheable_buf(dev, &buf->buf_meta);

	buf->virt_video = buf->buf_meta.vaddr;
	buf->dma_buf = buf->buf_meta.paddr;

	buf->lu_addr = buf->dma_buf;
	buf->cb_addr = buf->lu_addr + (lu_stride * height);
	buf->cr_addr = buf->cb_addr + (cb_stride * (height / 2));
	buf->lu_stride = lu_stride;
	buf->cb_stride = cb_stride;
	buf->cr_stride = cr_stride;
	buf->lu_size = lu_stride * height;
	buf->cb_size = cb_stride * (height / 2);
	buf->cr_size = cr_stride * (height / 2);

	if (me->rotation) {
		memset(buf->virt_video, 0x10, buf->lu_size);
		memset(buf->virt_video + (lu_stride * height), 0x80,
			buf->cb_size);
		memset(buf->virt_video + (lu_stride * height) +
			(cb_stride * (height / 2)), 0x80, buf->cr_size);
	}

	pr_debug("%s - [addr] lu 0x%x, cb 0x%x, cr 0x%x, virt %p\n", __func__,
		buf->lu_addr, buf->cb_addr, buf->cr_addr, buf->virt_video);

	pr_debug("%s - [stride] lu %d, cb %d, cr %d\n", __func__,
		buf->lu_stride, buf->cb_stride, buf->cr_stride);

	pr_debug("%s - [size] lu %d, cb %d, cr %d, total size : %d\n", __func__,
		buf->lu_size, buf->cb_size, buf->cr_size, size);

	return 0;
}

static int _alloc_rgb_memory(struct nx_rearcam *me)
{
	int size = 0;
	struct device *dev = &me->pdev->dev;
	int width;
	int height;
	struct nx_rgb_buf *buf = NULL;
	struct dp_sync_info *sync = &me->dp_sync;

	u32 format = me->rgb_format;
	u32 pixelbyte = _get_pixel_byte(format);

	width = sync->h_active_len;
	height = sync->v_active_len;

	size = width * height * pixelbyte;
	size = PAGE_ALIGN(size);

	buf = &me->frame_set.rgb_buf;
	buf->page_size = size;
	buf->virt_rgb = dma_alloc_coherent(NULL, size, &buf->dma_buf,
				GFP_KERNEL);
	if (!buf->virt_rgb) {
		dev_err(dev, "Not able to allocate the DMA buffer.\n");
		return -ENOMEM;
	};

	buf->rgb_addr = buf->dma_buf;

	return 0;
}

static int _get_i2c_client(struct nx_rearcam *me)
{
	struct device *dev = &me->pdev->dev;
	struct i2c_client *client;
	struct nx_v4l2_i2c_board_info *info = &me->clipper_info.sensor_info;
	struct i2c_adapter *adapter = i2c_get_adapter(info->i2c_adapter_id);

	if (!adapter) {
		dev_err(dev, "unable to get i2c adapter %d\n",
			info->i2c_adapter_id);
		return -EINVAL;
	}

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;

	client->adapter = adapter;
	client->addr = info->board_info.addr;
	client->flags = 0;

	me->client = client;

	return 0;
}

static int _camera_sensor_run(struct nx_rearcam *me)
{
	int ret;
	struct reg_val *reg_val;
	struct device *dev = &me->pdev->dev;
	int i = 0;

	_get_i2c_client(me);
	ret = enable_sensor_power(dev, &me->clipper_info, true);
	if (ret) {
		dev_err(&me->pdev->dev, "unable to enable sensor power!\n");
		return -1;
	}

	reg_val = me->init_data;
	while (reg_val->reg != 0xFF && reg_val->val != 0xFF) {
		i2c_smbus_write_byte_data(me->client, reg_val->reg,
			reg_val->val);
		pr_debug("%s - index : %d, addr : 0x%02x, val: 0x%02x\n",
			__func__, i++, reg_val->reg, reg_val->val);
		reg_val++;
	}

	return 0;
}

static void _display_worker(struct work_struct *work)
{
	struct nx_rearcam *me = container_of(work, struct nx_rearcam,
				work_display);

	struct nx_video_buf *buf = NULL;
	struct queue_entry *entry = NULL;

	unsigned long flags;
	int q_size = 0;
	int i = 0;

	struct common_queue *q_display_empty = NULL;
	struct common_queue *q_display_done = NULL;

	if (me->rotation) {
		q_display_empty = &me->q_rot_empty;
		q_display_done	= &me->q_rot_done;
	} else {
		q_display_empty = &me->q_vip_empty;
		q_display_done	= &me->q_vip_done;
	}

	spin_lock_irqsave(&me->display_lock, flags);

#if TIME_LOG
	me->measure.dp_frame_cnt++;
#endif

	if (q_display_done->size(q_display_done) < 1) {
		spin_unlock_irqrestore(&me->display_lock, flags);
		return;
	}

	q_size = q_display_done->size(q_display_done);
	entry = q_display_done->peek(q_display_done, q_size-1);
	if (!entry) {
		spin_unlock_irqrestore(&me->display_lock, flags);
		return;
	}

	buf = (struct nx_video_buf *)(entry->data);
	pr_debug("%s - lu_addr : 0x%x\n", __func__, buf->lu_addr);

	_mlc_video_set_addr(me, buf);

	if (!me->mlc_on_first) {
		_set_mlc_overlay(me);
		_mlc_rgb_overlay_draw(me);
		me->mlc_on_first = true;
	}

	if (!me->is_mlc_on) {
		_mlc_video_run(me);
		_mlc_overlay_run(me);
		me->is_mlc_on = true;
	}

	for (i = 0 ; i < q_size-1 ; i++) {
		entry = q_display_done->dequeue(q_display_done);
		q_display_empty->enqueue(q_display_empty, entry);
	}

	spin_unlock_irqrestore(&me->display_lock, flags);

#if TIME_LOG
	me->measure.e_time = get_jiffies_64();
	me->measure.time_msec = me->measure.e_time - me->measure.s_time;

	if (me->measure.time_msec >= 1000) {
		pr_info("## [%s():%s:%d\t] Time: %3d Sec,", __func__,
			strrchr(__FILE__, '/') + 1, __LINE__,
			me->measure.time_cnt);
		pr_info(" vip: %2d rot: %2d display: %2d total: %3d",
			me->measure.vip_frame_cnt, me->measure.rot_frame_cnt,
			me->measure.dp_frame_cnt, me->measure.t_time_msec);
		pr_info(" lu: %3d cb: %3d cr:%3d\n",
			me->measure.lu_time,
			me->measure.cb_time,
			me->measure.cr_time);

		me->measure.time_cnt++;
		me->measure.s_time = get_jiffies_64();
		me->measure.dp_frame_cnt = 0;
		me->measure.vip_frame_cnt = 0;
		me->measure.rot_frame_cnt = 0;
		me->measure.time_msec = 0;
		me->measure.t_time_msec = 0;
	}
#endif
}

static void _lu_rot_worker(struct work_struct *work)
{
	struct nx_rearcam *me = container_of(work, struct nx_rearcam,
				work_lu_rot);
	struct device *dev = &me->pdev->dev;

	struct queue_entry *entry = NULL;

	struct queue_entry *dst_ent = NULL;
	struct queue_entry *src_ent = NULL;

	struct nx_video_buf *buf = NULL;

	int q_size = 0;
	int i;

	struct common_queue *q_vip_empty = &me->q_vip_empty;
	struct common_queue *q_vip_done = &me->q_vip_done;
	struct common_queue *q_rot_empty = &me->q_rot_empty;
	struct common_queue *q_rot_done	= &me->q_rot_done;

	int width = me->clipper_info.width;
	int height = me->clipper_info.height;

	u8 *t_src;
	u8 *t_dst;
	int x, y;

#if TIME_LOG
	me->measure.t_s_time = get_jiffies_64();
#endif
	mutex_lock(&me->rot_lock);

	if (q_vip_done->size(q_vip_done) < 1) {
		mutex_unlock(&me->rot_lock);
		return;
	}

	q_size = q_vip_done->size(q_vip_done);
	src_ent = q_vip_done->peek(q_vip_done, q_size-1);
	if (!src_ent) {
		dev_err(dev, "q_vip_done UNDERRUN --> !!!\n");

		mutex_unlock(&me->rot_lock);
		return;
	}

	me->rot_src = (struct nx_video_buf *)(src_ent->data);

	dma_sync_single_for_device(&me->pdev->dev, me->rot_src->dma_buf,
		me->rot_src->page_size, DMA_FROM_DEVICE);

	dst_ent = q_rot_empty->dequeue(q_rot_empty);
	if (dst_ent == NULL) {
		dev_err(dev, "q rotation empty UNDERRUN--> !!!\n");
		mutex_unlock(&me->rot_lock);
		return;
	}

	me->rot_dst = (struct nx_video_buf *)(dst_ent->data);

#if TIME_LOG
	me->measure.r_s_time = get_jiffies_64();
#endif

	queue_work(me->wq_cb_rot, &me->work_cb_rot);
	queue_work(me->wq_cr_rot, &me->work_cr_rot);

	t_src = (u8 *)me->rot_src->virt_video;
	t_dst = (u8 *)me->rot_dst->virt_video;

	t_dst += (me->rot_dst->lu_stride * (width - 1)) +
		((me->rot_width_rate_y_size/2) - height/2);

	for (y = 0 ; y < height; y++) {
		for (x = 0; x < width; x++)
			*(t_dst - (me->rot_dst->lu_stride * x)) = *(t_src + x);

		t_src += me->rot_src->lu_stride;
		t_dst += 1;
	}

	dma_sync_single_for_device(&me->pdev->dev, me->rot_dst->dma_buf,
		me->rot_dst->page_size, DMA_TO_DEVICE);

#if TIME_LOG
	me->measure.r_e_time = get_jiffies_64();
	me->measure.lu_time = me->measure.r_e_time - me->measure.r_s_time;
#endif

	if (!me->is_display_on) {
		me->is_display_on = true;
		_set_dpc_interrupt(me, true);
	}

	q_rot_done->enqueue(q_rot_done, dst_ent);

	for (i = 0; i < q_size-1 ; i++) {
		entry = q_vip_done->dequeue(q_vip_done);
		buf = (struct nx_video_buf *)(entry->data);
		q_vip_empty->enqueue(q_vip_empty, entry);
	}

#if TIME_LOG
	me->measure.rot_frame_cnt++;
#endif
	mutex_unlock(&me->rot_lock);

#if TIME_LOG
	me->measure.t_e_time = get_jiffies_64();
	me->measure.t_time_msec = me->measure.t_e_time - me->measure.t_s_time;
#endif
}

static void _cb_rot_worker(struct work_struct *work)
{
	struct nx_rearcam *me = container_of(work, struct nx_rearcam,
				work_cb_rot);
	u8 *t_src;
	u8 *t_dst;
	int x, y;
	int width = me->clipper_info.width/2;
	int height = me->clipper_info.height/2;

#if TIME_LOG
	u64 s_time, e_time;

	s_time = get_jiffies_64();
#endif
	t_src = (u8 *)(me->rot_src->virt_video + me->rot_src->lu_size);
	t_dst = (u8 *)(me->rot_dst->virt_video + me->rot_dst->lu_size);

	t_dst += (me->rot_dst->cb_stride * (width - 1)) +
		((me->rot_width_rate_c_size/2) - height/2);

	for (y = 0 ; y < height ; y++) {
		for (x = 0; x < width; x++)
			*(t_dst - (me->rot_dst->cb_stride * x)) =
				*(t_src + x);

		t_src += me->rot_src->cb_stride;
		t_dst += 1;
	}

#if TIME_LOG
	e_time = get_jiffies_64();
	me->measure.cb_time = e_time - s_time;
#endif
}

static void _cr_rot_worker(struct work_struct *work)
{
	struct nx_rearcam *me = container_of(work, struct nx_rearcam,
				work_cr_rot);
	u8 *t_src;
	u8 *t_dst;
	int x, y;

	int width = me->clipper_info.width/2;
	int height = me->clipper_info.height/2;

#if TIME_LOG
	u64 s_time, e_time;

	s_time = get_jiffies_64();
#endif

	t_src = (u8 *)(me->rot_src->virt_video + me->rot_src->lu_size
		+ me->rot_src->cb_size);
	t_dst = (u8 *)(me->rot_dst->virt_video + me->rot_dst->lu_size
		+ me->rot_dst->cb_size);

	t_dst += (me->rot_dst->cr_stride * (width - 1)) +
		((me->rot_width_rate_c_size/2) - height/2);

	for (y = 0 ; y < height ; y++) {
		for (x = 0; x < width; x++)
			*(t_dst - (me->rot_dst->cr_stride * x)) =
				*(t_src + x);

		t_src += me->rot_src->cr_stride;
		t_dst += 1;
	}

#if TIME_LOG
	e_time = get_jiffies_64();
	me->measure.cr_time = e_time - s_time;
#endif
}

static void _init_context(struct nx_rearcam *me)
{
	spin_lock_init(&me->vip_lock);
	spin_lock_init(&me->display_lock);

	if (me->rotation)
		mutex_init(&me->rot_lock);

	mutex_init(&me->decide_work_lock);

	INIT_DELAYED_WORK(&me->work, _work_handler_reargear);
	INIT_WORK(&me->work_display, _display_worker);

	me->rot_src = NULL;
	me->rot_dst = NULL;

	me->irq_count = 0;
	me->is_first = true;
	me->mlc_on_first = false;
	me->removed = false;
	me->is_remove = false;

	me->rot_width_rate_y_size = get_rotate_width_rate(me, Y);
	me->rot_width_rate_c_size = get_rotate_width_rate(me, CB);
}

static int _init_rotation_worker(struct nx_rearcam *me)
{
	struct device *dev = &me->pdev->dev;

	INIT_WORK(&me->work_lu_rot, _lu_rot_worker);
	INIT_WORK(&me->work_cb_rot, _cb_rot_worker);
	INIT_WORK(&me->work_cr_rot, _cr_rot_worker);

	me->wq_lu_rot = create_singlethread_workqueue("wq_lu_rot");
	if (!me->wq_lu_rot) {
		dev_err(dev, "create wq_lu_rot error.\n");
		return -1;
	}

	me->wq_cb_rot = create_singlethread_workqueue("wq_cb_rot");
	if (!me->wq_cb_rot) {
		dev_err(dev, "create wq_cb_rot error.\n");
		return -1;
	}

	me->wq_cr_rot = create_singlethread_workqueue("wq_cr_rot");
	if (!me->wq_cr_rot) {
		dev_err(dev, "create wq_cr_rot error.\n");
		return -1;
	}

	return 0;
}

static void _init_queue_context(struct nx_rearcam *me)
{
	/* vip empty queue */
	register_queue_func(
				&me->q_vip_empty,
				_init_queue,
				_enqueue,
				_dequeue,
				_peekqueue,
				_clearqueue,
				_lockqueue,
				_unlockqueue,
				_sizequeue
			);

	/* vip done queue */
	register_queue_func(
				&me->q_vip_done,
				_init_queue,
				_enqueue,
				_dequeue,
				_peekqueue,
				_clearqueue,
				_lockqueue,
				_unlockqueue,
				_sizequeue
			);

	/* rotation empty queue */
	register_queue_func(
				&me->q_rot_empty,
				_init_queue,
				_enqueue,
				_dequeue,
				_peekqueue,
				_clearqueue,
				_lockqueue,
				_unlockqueue,
				_sizequeue
			);

	/* rotation done queue */
	register_queue_func(
				&me->q_rot_done,
				_init_queue,
				_enqueue,
				_dequeue,
				_peekqueue,
				_clearqueue,
				_lockqueue,
				_unlockqueue,
				_sizequeue
			);
}

static int _init_buffer(struct nx_rearcam *me)
{
	int i = 0;

	me->frame_set.width = me->width;
	me->frame_set.height = me->height;

	for (i = 0; i < MAX_BUFFER_COUNT; i++) {
		struct nx_video_buf *buf = &me->frame_set.vip_bufs[i];
		struct queue_entry *entry = &me->frame_set.vip_queue_entry[i];

		_alloc_video_memory(me, buf);

		entry->data = buf;
	}

	if (me->rotation) {
		for (i = 0 ; i < MAX_BUFFER_COUNT; i++) {
			struct nx_video_buf *buf = &me->frame_set.rot_bufs[i];
			struct queue_entry *entry =
				&me->frame_set.rot_queue_entry[i];

			_alloc_video_rot_memory(me, buf);

			entry->data = buf;
		}
	}

	_alloc_rgb_memory(me);
	_all_buffer_to_empty_queue(me);

	return 0;
}

static void _free_buffer(struct nx_rearcam *me)
{
	int i;
	struct nx_video_buf *video_buf;
	struct nx_rgb_buf *rgb_buf;
	struct device *dev = &me->pdev->dev;

	for (i = 0; i < MAX_BUFFER_COUNT; i++) {
		video_buf = &me->frame_set.vip_bufs[i];
		if (video_buf) {
			if (video_buf->virt_video != NULL) {
				_free_cacheable_buf(dev, &video_buf->buf_meta);
				video_buf->virt_video = NULL;
				video_buf->dma_buf = 0;
			}
		}
	}

	for (i = 0; i < MAX_BUFFER_COUNT; i++) {
		video_buf = &me->frame_set.rot_bufs[i];
		if (video_buf) {
			if (video_buf->virt_video != NULL) {
				_free_cacheable_buf(dev, &video_buf->buf_meta);
				video_buf->virt_video = NULL;
				video_buf->dma_buf = 0;
			}
		}
	}

	rgb_buf = &me->frame_set.rgb_buf;
	if (rgb_buf) {
		if (rgb_buf->virt_rgb != NULL) {
			dma_free_coherent(NULL, rgb_buf->page_size,
				rgb_buf->virt_rgb, rgb_buf->dma_buf);
			rgb_buf->virt_rgb = NULL;
			rgb_buf->dma_buf = 0;
		}
	}
}

static void _init_vendor(struct nx_rearcam *me)
{
	me->vendor_context		= NULL;
	me->alloc_vendor_context	= nx_rearcam_alloc_vendor_context;
	me->free_vendor_context		= nx_rearcam_free_vendor_context;
	me->pre_turn_on			= nx_rearcam_pre_turn_on;
	me->post_turn_off		= nx_rearcam_post_turn_off;
	me->decide			= nx_rearcam_decide;
	me->draw_rgb_overlay		= nx_rearcam_draw_rgb_overlay;
}

static int init_me(struct nx_rearcam *me)
{
	_init_vendor(me);
	_init_context(me);

	if (me->rotation)
		_init_rotation_worker(me);

	_init_queue_context(me);
	_init_buffer(me);

	_reset_hw_display(me);

	_init_hw_display_top(me);


	nx_reset_apply(me->reset_dual_display);

	_init_hw_mlc(me);
	_init_hw_dpc(me);
	_init_hw_vip(me);
	_init_hw_gpio(me);

	return 0;
}

static int deinit_me(struct nx_rearcam *me)
{
	struct device *dev = &me->pdev->dev;

	if (me->init_data != NULL)
		kfree(me->init_data);

	if (me->base_addr != NULL)
		kfree(me->base_addr);

	if (me->removed) {
		_mlc_overlay_stop(me);
		_mlc_video_stop(me);

		_set_vip_interrupt(me, false);
		_vip_stop(me);
		_cleanup_me(me);
		_reset_queue(me);

		me->is_display_on = false;
		me->mlc_on_first = false;
	}

	devm_free_irq(dev, me->irq_dpc, me);
	devm_free_irq(dev, me->irq_vip, me);
	devm_gpio_free(dev, me->event_gpio);
	devm_free_irq(dev, me->irq_event, me);

	_free_buffer(me);
	me->removed = true;

	return 0;
}

static bool is_rearcam_on(struct nx_rearcam *me)
{
	return me->is_on;
}

static ssize_t _stop_rearcam(struct kobject *kobj,
			     struct kobj_attribute *attr,
			     const char *buf, size_t nbytes)
{
	struct device *dev = container_of(kobj->parent, struct device, kobj);
	struct nx_rearcam *me = (struct nx_rearcam *)dev_get_drvdata(dev);

	while (is_rearcam_on(me)) {
		pr_debug("wait rearcam stopping...\n");
		schedule_timeout_interruptible(HZ/5);
	}
	nx_rearcam_remove(me->pdev);

	pr_debug("end of nx_rearcam_remove()\n");

	me->is_remove = true;

	return nbytes;
}

static ssize_t _status_rearcam(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int ret = 0;

	struct device *dev = container_of(kobj->parent, struct device, kobj);
	struct nx_rearcam *me = (struct nx_rearcam *)dev_get_drvdata(dev);

	if (me->is_remove)
		ret = 1;
	else
		ret = 0;

	return sprintf(buf, "%d", ret);
}

static struct kobj_attribute rearcam_attr = __ATTR(stop, 0644,
					_status_rearcam, _stop_rearcam);

static struct attribute *attrs[] = {
	&rearcam_attr.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs	= (struct attribute **)attrs,
};

static int _create_sysfs(struct nx_rearcam *me)
{
	struct kobject *kobj = NULL;
	int ret = 0;
	struct device *dev = &me->pdev->dev;

	dev_set_drvdata(&platform_bus, me);
	kobj = kobject_create_and_add("nx-rearcam", &platform_bus.kobj);
	if (!kobj) {
		dev_err(dev, "%s: failed create kobject.\n", __func__);
		kobject_del(kobj);
		return -ret;
	}

	ret = sysfs_create_group(kobj, &attr_group);
	if (ret) {
		dev_err(dev, "%s: failed create sysfs group.\n", __func__);
		kobject_del(kobj);
		return -ret;
	}

	return 0;
}

static int nx_rearcam_probe(struct platform_device *pdev)
{
	int ret;
	struct nx_rearcam *me;
	struct device *dev = &pdev->dev;

	me = devm_kzalloc(dev, sizeof(*me), GFP_KERNEL);
	if (!me) {
		WARN_ON(1);
		return -ENOMEM;
	}

	dev_set_drvdata(dev, me);
	me->pdev = pdev;
	ret = nx_rearcam_parse_dt(dev, me);
	if (ret) {
		dev_err(dev, "failed to parse dt\n");
		goto ERROR;
	}

	init_me(me);

	ret = _camera_sensor_run(me);
	if (ret < 0)
		goto ERROR;

	/* TODO : MIPI Routine */
	platform_set_drvdata(pdev, me);

	if (_create_sysfs(me)) {
		dev_err(dev, "%s: failed to create sysfs.\n", __func__);
		return -1;
	}

	if (is_reargear_on(me))
		schedule_delayed_work(&me->work,
			msecs_to_jiffies(me->detect_delay));

	if (me->alloc_vendor_context) {
		me->vendor_context = me->alloc_vendor_context();
		if (!me->vendor_context) {
			dev_err(dev, "%s: failed to allcate vendor context.\n",
				__func__);
			return -ENOMEM;
		}
	}


	/* reargear gpio enable */
	enable_irq(me->irq_event);

	return 0;

ERROR:
	devm_kfree(&me->pdev->dev, me);

	return -1;
}

static int nx_rearcam_remove(struct platform_device *pdev)
{
	struct nx_rearcam *me = platform_get_drvdata(pdev);

	if (unlikely(!me))
		return 0;

	deinit_me(me);
	if (me->vendor_context != NULL)
		kfree(me->vendor_context);
	devm_kfree(&me->pdev->dev, me);

	if (me->free_vendor_context)
		me->free_vendor_context(me->vendor_context);

	return 0;
}

static struct platform_device_id nx_rearcam_id_table[] = {
	{ NX_REARCAM_DEV_NAME, 0 },
	{ },
};

static const struct of_device_id nx_rearcam_dt_match[] = {
	{ .compatible = "nexell,nx-rearcam" },
	{ },
};
MODULE_DEVICE_TABLE(of, nx_rearcam_dt_match);

static struct platform_driver nx_rearcam_driver = {
	.probe		= nx_rearcam_probe,
	.remove		= nx_rearcam_remove,
	.id_table	= nx_rearcam_id_table,
	.driver		= {
		.name	= NX_REARCAM_DEV_NAME,
		.owner	= THIS_MODULE,
		.of_match_table	= of_match_ptr(nx_rearcam_dt_match),
	},
};

static int __init nx_rearcam_init(void)
{
	return platform_driver_register(&nx_rearcam_driver);
}

static void __exit nx_rearcam_exit(void)
{
	platform_driver_unregister(&nx_rearcam_driver);
}

subsys_initcall(nx_rearcam_init);
module_exit(nx_rearcam_exit);

MODULE_AUTHOR("jkchoi <jkchoi@nexell.co.kr>");
MODULE_DESCRIPTION("Rear Camera Driver for Nexell");
MODULE_LICENSE("GPL");

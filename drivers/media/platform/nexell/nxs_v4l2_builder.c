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
#include <linux/of_gpio.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/videodev2.h>
#include <linux/v4l2-mediabus.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/regulator/consumer.h>

#include <media/media-device.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ctrls.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-async.h>
#include <media/v4l2-of.h>

#include <dt-bindings/media/nexell-vip.h>
#include <linux/nxs_ioctl.h>
#include <linux/nxs_v4l2.h>
#include <linux/soc/nexell/nxs_function.h>
#include <linux/soc/nexell/nxs_dev.h>
#include <linux/soc/nexell/nxs_res_manager.h>

#define NXS_VIDEO_MAX_NAME_SIZE	32
#define NXS_VIDEO_CAPTURE_DEF_FRAMERATE	30
#define NXS_VIDEO_RENDER_DEF_FRAMERATE	60
#define NXS_VIDEO_MAX_PLANES	3

#define NXS_VIDEO_JOB_TIMEOUT	(2*HZ)

enum nxs_video_type {
	NXS_VIDEO_TYPE_NONE = 0,
	NXS_VIDEO_TYPE_CAPTURE,
	NXS_VIDEO_TYPE_RENDER,
	NXS_VIDEO_TYPE_M2M,
	NXS_VIDEO_TYPE_SUBDEV,
	NXS_VIDEO_TYPE_INVALID
};

struct nxs_subdev_ctx {
	struct v4l2_mbus_framefmt format; /* source format */
	struct v4l2_mbus_framefmt dst_format; /* dest format */
	struct v4l2_crop crop;
};

/* below block must move to nxs_function.h */
struct nxs_action_unit {
	u32 value;
	u32 delay_ms;
};

struct nxs_gpio_action {
	u32 gpio_num;
	u32 count;
	struct nxs_action_unit *units;
};

struct nxs_action {
	int type;
	void *action;
};

struct nxs_action_seq {
	int count;
	struct nxs_action *actions;
};

struct nxs_capture_hw_vip_param {
	u32 bus_width;
	u32 bus_align;
	u32 interlace;
	u32 h_backporch;
	u32 v_backporch;
	u32 pclk_polarity;
	u32 hsync_polarity;
	u32 vsync_polarity;
	u32 field_polarity;
	u32 data_order;
};

struct nxs_capture_hw_csi_param {
	u8 data_lanes[4];
	u8 clock_lane;
	u8 num_data_lanes;
	u8 lane_polarities[5];
	u64 link_frequency;
};

struct nxs_capture_ctx {
	u32 bus_type;
	union {
		struct nxs_capture_hw_vip_param parallel;
		struct nxs_capture_hw_csi_param csi;
	} bus;
	struct nxs_action_seq enable_seq;
	struct nxs_action_seq disable_seq;
	struct clk *clk;
	u32 clock_freq;
	u32 regulator_nr;
	char **regulator_names;
	u32 *regulator_voltages;
	struct v4l2_subdev *sensor;
	struct device *dev;
};

struct nxs_video {
	char name[NXS_VIDEO_MAX_NAME_SIZE];
	u32 type; /* enum nxs_video_type */
	int open_count;

	struct mutex queue_lock;
	struct mutex stream_lock;

	struct v4l2_device *v4l2_dev;
	struct media_device *media_dev;
	struct vb2_queue *vbq;
	void *vb2_alloc_ctx;

	struct video_device vdev;
	struct v4l2_subdev subdev;
	struct nxs_subdev_ctx *subdev_ctx;
	struct media_pad pad;

	struct nxs_function_instance *nxs_function;

	struct v4l2_async_notifier notifier;
	struct nxs_capture_ctx *capture;
};

#define vdev_to_nxs_video(vdev) container_of(vdev, struct nxs_video, video)
#define vbq_to_nxs_video(vbq)	container_of(vbq, struct nxs_video, vbq)
#define is_m2m(video)		video->type == NXS_VIDEO_TYPE_M2M
#define is_capture(video)	video->type == NXS_VIDEO_TYPE_CAPTURE
#define is_render(video)	video->type == NXS_VIDEO_TYPE_RENDER
#define is_subdev(video)	video->type == NXS_VIDEO_TYPE_SUBDEV

struct nxs_video_fh {
	struct v4l2_fh vfh;
	struct nxs_video *video;
	struct vb2_queue queue;
	struct v4l2_format format; /* m2m: source format */
	struct v4l2_crop crop;
	struct v4l2_selection selection;
	struct v4l2_streamparm parm;
	unsigned int num_planes;
	unsigned int strides[NXS_VIDEO_MAX_PLANES];
	unsigned int sizes[NXS_VIDEO_MAX_PLANES];
	unsigned int width;
	unsigned int height;
	unsigned int pixelformat;
	struct list_head bufq;
	spinlock_t bufq_lock;
	atomic_t underflow;
	u32 type; /* enum v4l2_buf_type */
	/* m2m */
	struct v4l2_m2m_dev *m2m_dev;
	struct v4l2_m2m_ctx *m2m_ctx;
	struct v4l2_format dst_format;
	u32 dst_type;
	unsigned int dst_num_planes;
	unsigned int dst_strides[NXS_VIDEO_MAX_PLANES];
	unsigned int dst_sizes[NXS_VIDEO_MAX_PLANES];
	unsigned int dst_width;
	unsigned int dst_height;
	unsigned int dst_pixelformat;
	atomic_t running;
	struct completion stop_done;
};

#define to_nxs_video_fh(fh)	container_of(fh, struct nxs_video_fh, vfh)
#define queue_to_nxs_video_fh(q) \
				container_of(q, struct nxs_video_fh, queue)

/* nxs_video_buffer is same to v4l2_m2m_buffer */
struct nxs_video_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
	/* dma_addr_t dma_addr[NXS_VIDEO_MAX_PLANES]; */
	/* unsigned int strides[NXS_VIDEO_MAX_PLANES]; */
};

struct nxs_address_info {
	u32 num_planes;
	dma_addr_t dma_addr[NXS_VIDEO_MAX_PLANES];
	unsigned int strides[NXS_VIDEO_MAX_PLANES];
};

#define to_nxs_video_buffer(buf) container_of(buf, struct nxs_video_buffer, vb)

struct nxs_v4l2_builder {
	struct nxs_function_builder *builder;

	struct device *dev;

	struct mutex lock; /* protect function_list */
	struct list_head function_list;

	struct media_device media_dev;
	struct v4l2_device v4l2_dev;
	void  *vb2_alloc_ctx;

	int seq;
};

struct nxs_video_format {
	u32   format;
	u32   bpp;
	char *name;
};

/* name field is same to videodev2.h format comments */
static struct nxs_video_format supported_formats[] = {
	{
		.format		= V4L2_PIX_FMT_ARGB555,
		.bpp		= 16,
		.name		= "ARGB-1-5-5-5 16bit",
	},
	{
		.format		= V4L2_PIX_FMT_XRGB555,
		.bpp		= 16,
		.name		= "XRGB-1-5-5-5",
	},
	{
		.format		= V4L2_PIX_FMT_RGB565,
		.bpp		= 16,
		.name		= "RGB-5-6-5",
	},
	{
		.format		= V4L2_PIX_FMT_BGR24,
		.bpp		= 24,
		.name		= "BGR-8-8-8",
	},
	{
		.format		= V4L2_PIX_FMT_RGB24,
		.bpp		= 24,
		.name		= "RGB-8-8-8",
	},
	{
		.format		= V4L2_PIX_FMT_BGR32,
		.bpp		= 32,
		.name		= "BGR-8-8-8-8",
	},
	{
		.format		= V4L2_PIX_FMT_ABGR32,
		.bpp		= 32,
		.name		= "BGRA-8-8-8-8",
	},
	{
		.format		= V4L2_PIX_FMT_XBGR32,
		.bpp		= 32,
		.name		= "BGRX-8-8-8-8",
	},
	{
		.format		= V4L2_PIX_FMT_RGB32,
		.bpp		= 32,
		.name		= "RGB-8-8-8-8",
	},
	{
		.format		= V4L2_PIX_FMT_ARGB32,
		.bpp		= 32,
		.name		= "ARGB-8-8-8-8",
	},
	{
		.format		= V4L2_PIX_FMT_XRGB32,
		.bpp		= 32,
		.name		= "XRGB-8-8-8-8",
	},
	{
		.format		= V4L2_PIX_FMT_YUYV,
		.bpp		= 16,
		.name		= "YUV 4:2:2",
	},
	{
		.format		= V4L2_PIX_FMT_YYUV,
		.bpp		= 16,
		.name		= "YUV 4:2:2",
	},
	{
		.format		= V4L2_PIX_FMT_YVYU,
		.bpp		= 16,
		.name		= "YVU 4:2:2",
	},
	{
		.format		= V4L2_PIX_FMT_UYVY,
		.bpp		= 16,
		.name		= "YUV 4:2:2",
	},
	{
		.format		= V4L2_PIX_FMT_VYUY,
		.bpp		= 16,
		.name		= "YUV 4:2:2",
	},
};

static struct nxs_video_format supported_mplane_formats[] = {
	{
		.format		= V4L2_PIX_FMT_YVU420,
		.bpp		= 12,
		.name		= "YVU 4:2:0",
	},
	{
		.format		= V4L2_PIX_FMT_YUV422P,
		.bpp		= 16,
		.name		= "YVU422 planar",
	},
	{
		.format		= V4L2_PIX_FMT_YUV420,
		.bpp		= 12,
		.name		= "YUV 4:2:0",
	},
	{
		.format		= V4L2_PIX_FMT_NV12,
		.bpp		= 12,
		.name		= "Y/CbCr 4:2:0",
	},
	{
		.format		= V4L2_PIX_FMT_NV21,
		.bpp		= 12,
		.name		= "Y/CrCb 4:2:0",
	},
	{
		.format		= V4L2_PIX_FMT_NV16,
		.bpp		= 16,
		.name		= "Y/CbCr 4:2:2",
	},
	{
		.format		= V4L2_PIX_FMT_NV61,
		.bpp		= 16,
		.name		= "Y/CrCb 4:2:2",
	},
	{
		.format		= V4L2_PIX_FMT_NV24,
		.bpp		= 24,
		.name		= "Y/CbCr 4:4:4",
	},
	{
		.format		= V4L2_PIX_FMT_NV42,
		.bpp		= 24,
		.name		= "Y/CrCb 4:4:4",
	},
	{
		.format		= V4L2_PIX_FMT_NV12M,
		.bpp		= 12,
		.name		= "Y/CbCr 4:2:0",
	},
	{
		.format		= V4L2_PIX_FMT_NV21M,
		.bpp		= 12,
		.name		= "Y/CrCb 4:2:0",
	},
	{
		.format		= V4L2_PIX_FMT_NV16M,
		.bpp		= 16,
		.name		= "Y/CbCr 4:2:2",
	},
	{
		.format		= V4L2_PIX_FMT_NV61M,
		.bpp		= 16,
		.name		= "Y/CrCb 4:2:2",
	},
	{
		.format		= V4L2_PIX_FMT_YUV420M,
		.bpp		= 12,
		.name		= "YUV420 planar",
	},
	{
		.format		= V4L2_PIX_FMT_YVU420M,
		.bpp		= 12,
		.name		= "YVU420 planar",
	},
};

/*
 * util functions
 */
static int nxs_vbq_init(struct nxs_video_fh *vfh, u32 type);
static int nxs_m2m_queue_init(void *priv, struct vb2_queue *src_q,
			      struct vb2_queue *dst_q);

static inline bool is_multiplanar(u32 buf_type)
{
	switch (buf_type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		return false;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		return true;
	}

	return false;
}

static inline bool is_output(u32 buf_type)
{
	switch (buf_type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		return true;
	}

	return false;
}

static void get_info_of_format(struct v4l2_format *f,
			       unsigned int *num_planes,
			       unsigned int strides[],
			       unsigned int sizes[],
			       unsigned int *width,
			       unsigned int *height,
			       unsigned int *pixelformat)
{
	/* TODO: check app source set_format routine
	 * bytesperline, sizeimage, num_planes, v4l2_plane_pix_format
	 */
	if (is_multiplanar(f->type)) {
		int i;

		*num_planes = f->fmt.pix_mp.num_planes;
		*width = f->fmt.pix_mp.width;
		*height = f->fmt.pix_mp.height;
		*pixelformat = f->fmt.pix_mp.pixelformat;

		for (i = 0; i < *num_planes; i++) {
			strides[i] = f->fmt.pix_mp.plane_fmt[i].bytesperline;
			sizes[i] = f->fmt.pix_mp.plane_fmt[i].sizeimage;
		}
	} else {
		*num_planes = 1;
		*width = f->fmt.pix.width;
		*height = f->fmt.pix.height;
		*pixelformat = f->fmt.pix.pixelformat;

		strides[0] = f->fmt.pix.bytesperline;
		sizes[0] = f->fmt.pix.sizeimage;
	}
}

static struct nxs_video_buffer *get_next_video_buffer(struct nxs_video_fh *vfh,
						      bool remove)
{
	struct nxs_video_buffer *buffer;
	unsigned long flags;

	spin_lock_irqsave(&vfh->bufq_lock, flags);
	if (list_empty(&vfh->bufq)) {
		spin_unlock_irqrestore(&vfh->bufq_lock, flags);
		return NULL;
	}

	buffer = list_first_entry(&vfh->bufq, struct nxs_video_buffer, list);
	if (remove)
		list_del(&buffer->list);
	spin_unlock_irqrestore(&vfh->bufq_lock, flags);

	return buffer;
}

/* chain functions */
static int nxs_chain_register_irqcallback(struct nxs_function_instance *inst,
					  struct nxs_video_fh *vfh,
					  void (*handler)(struct nxs_dev *,
							  void*)
					  )
{
	struct nxs_dev *last;
	struct nxs_irq_callback *callback;

	last = list_last_entry(&inst->dev_list, struct nxs_dev, func_list);
	if (!last)
		BUG();

	callback = kzalloc(sizeof(*callback), GFP_KERNEL);
	if (!callback)
		return -ENOMEM;

	callback->handler = handler;
	callback->data = vfh;

	return nxs_dev_register_irq_callback(last, NXS_DEV_IRQCALLBACK_TYPE_IRQ,
					     callback);
}

static int nxs_chain_unregister_irqcallback(struct nxs_function_instance *inst)
{
	struct nxs_dev *last;

	last = list_last_entry(&inst->dev_list, struct nxs_dev, func_list);
	if (!last)
		BUG();

	return nxs_dev_unregister_irq_callback(last,
					       NXS_DEV_IRQCALLBACK_TYPE_IRQ);
}

static int nxs_chain_config(struct nxs_function_instance *inst,
			    struct nxs_video_fh *vfh)
{
	struct nxs_dev *nxs_dev;
	union nxs_control f;
	union nxs_control c;
	union nxs_control s;
	/* union nxs_control t; */
	int ret;

	/* format */
	f.format.width = vfh->width;
	f.format.height = vfh->height;
	f.format.pixelformat = vfh->pixelformat;

	/* crop */
	c.crop.l = vfh->crop.c.left;
	c.crop.t = vfh->crop.c.top;
	c.crop.w = vfh->crop.c.width;
	c.crop.h = vfh->crop.c.height;

	/* selection */
	s.sel.l = vfh->selection.r.left;
	s.sel.t = vfh->selection.r.top;
	s.sel.w = vfh->selection.r.width;
	s.sel.h = vfh->selection.r.height;

	/* timeperframe */
	/* TODO */
	/* if (vfh->video->type == NXS_VIDEO_TYPE_CAPTURE) { */
	/* } else if (vfh->video->type == NXS_VIDEO_TYPE_RENDER) { */
	/* } */

	list_for_each_entry_reverse(nxs_dev, &inst->dev_list, func_list) {
		ret = nxs_set_control(nxs_dev, NXS_CONTROL_FORMAT, &f);
		if (ret)
			return ret;

		ret = nxs_set_control(nxs_dev, NXS_CONTROL_CROP, &c);
		if (ret)
			return ret;

		ret = nxs_set_control(nxs_dev, NXS_CONTROL_SELECTION, &s);
		if (ret)
			return ret;
	}

	return 0;
}

static int nxs_m2m_chain_config(struct nxs_function_instance *inst,
				struct nxs_video_fh *vfh)
{
	struct nxs_dev *nxs_dev;
	union nxs_control src_f, dst_f;
	int ret;

	/* format */
	src_f.format.width = vfh->width;
	src_f.format.height = vfh->height;
	src_f.format.pixelformat = vfh->pixelformat;

	dst_f.format.width = vfh->dst_width;
	dst_f.format.height = vfh->dst_height;
	dst_f.format.pixelformat = vfh->dst_pixelformat;

	list_for_each_entry_reverse(nxs_dev, &inst->dev_list, func_list) {
		ret = nxs_set_control(nxs_dev, NXS_CONTROL_FORMAT, &src_f);
		if (ret)
			return ret;

		ret = nxs_set_control(nxs_dev, NXS_CONTROL_DST_FORMAT, &dst_f);
		if (ret)
			return ret;
	}

	return 0;
}

static struct nxs_dev *
find_nxs_dev_by_function(struct nxs_function_instance *inst, u32 function)
{
	struct nxs_dev *nxs_dev;

	list_for_each_entry(nxs_dev, &inst->dev_list, func_list)
		if (nxs_dev->dev_function == function)
			return nxs_dev;

	return NULL;
}

static bool is_end_node(struct nxs_dev *dev)
{
	switch (dev->dev_function) {
	case NXS_FUNCTION_LVDS:
	case NXS_FUNCTION_MIPI_DSI:
	case NXS_FUNCTION_HDMI:
	case NXS_FUNCTION_DPC:
		return true;
	}

	return false;
}

static int nxs_subdev_chain_config(struct nxs_function_instance *inst,
				   struct nxs_subdev_ctx *ctx)
{
	struct nxs_dev *nxs_dev_from, *nxs_dev_to;
	struct nxs_dev *nxs_dev_tmp;
	struct list_head *head;
	union nxs_control f;
	union nxs_control df;
	union nxs_control c;
	int ret;

	f.format.width = ctx->format.width;
	f.format.height = ctx->format.height;
	f.format.pixelformat = ctx->format.code;

	if (ctx->dst_format.width > 0) {
		df.format.width = ctx->dst_format.width;
		df.format.height = ctx->dst_format.height;
		df.format.pixelformat = ctx->dst_format.code;
	} else {
		df.format.width = f.format.width;
		df.format.height = f.format.height;
		df.format.pixelformat = f.format.pixelformat;
	}

	/* set cropper */
	nxs_dev_to = NULL;
	nxs_dev_to = find_nxs_dev_by_function(inst, NXS_FUNCTION_CROPPER);
	/* pr_info("FOR CROPPER ====> \n"); */
	if (nxs_dev_to) {
		if (ctx->crop.c.width > 0) {
			c.crop.l = ctx->crop.c.left;
			c.crop.t = ctx->crop.c.top;
			c.crop.w = ctx->crop.c.width;
			c.crop.h = ctx->crop.c.height;
		} else {
			/* set default */
			c.crop.l = 0;
			c.crop.t = 0;
			c.crop.w = ctx->format.width;
			c.crop.h = ctx->format.height;
		}

		head = &inst->dev_list;
		list_for_each_entry(nxs_dev_tmp, head, func_list) {
			/* pr_info("function %s\n", */
			/* 	nxs_function_to_str(nxs_dev_tmp->dev_function)); */
			ret = nxs_set_control(nxs_dev_tmp, NXS_CONTROL_FORMAT,
					      &f);
			if (ret)
				return ret;

			ret = nxs_set_control(nxs_dev_tmp, NXS_CONTROL_CROP,
					      &c);
			if (ret)
				return ret;

			if (nxs_dev_tmp == nxs_dev_to)
				break;
		}

		f.format.width = c.crop.w;
		f.format.height = c.crop.h;
	}

	/* set csc */
	/* nxs_dev_from = get_next_nxs_dev(nxs_dev_to); */
	nxs_dev_from = nxs_dev_to;
	nxs_dev_to = find_nxs_dev_by_function(inst, NXS_FUNCTION_CSC);
	/* pr_info("FOR CSC ====> \n"); */
	if (nxs_dev_to) {
		if (nxs_dev_from)
			head = &nxs_dev_from->func_list;
		else
			head = &inst->dev_list;

		list_for_each_entry(nxs_dev_tmp, head, func_list) {
			/* pr_info("function %s\n", */
			/* 	nxs_function_to_str(nxs_dev_tmp->dev_function)); */
			ret = nxs_set_control(nxs_dev_tmp, NXS_CONTROL_FORMAT,
					      &f);
			if (ret)
				return ret;

			ret = nxs_set_control(nxs_dev_tmp,
					      NXS_CONTROL_DST_FORMAT, &df);
			if (ret)
				return ret;

			if (nxs_dev_tmp == nxs_dev_to)
				break;
		}

		f.format.pixelformat = df.format.pixelformat;
	}

	/* set scaler */
	nxs_dev_from = nxs_dev_to;
	nxs_dev_to = find_nxs_dev_by_function(inst, NXS_FUNCTION_SCALER_4096);
	if (!nxs_dev_to)
		nxs_dev_to = find_nxs_dev_by_function(inst,
						      NXS_FUNCTION_SCALER_5376);
	/* pr_info("FOR SCALER ====> \n"); */
	if (nxs_dev_to) {
		if (nxs_dev_from)
			head = &nxs_dev_from->func_list;
		else
			head = &inst->dev_list;

		list_for_each_entry(nxs_dev_tmp, head, func_list) {
			/* pr_info("function %s\n", */
			/* 	nxs_function_to_str(nxs_dev_tmp->dev_function)); */
			ret = nxs_set_control(nxs_dev_tmp, NXS_CONTROL_FORMAT,
					      &f);
			if (ret)
				return ret;

			ret = nxs_set_control(nxs_dev_tmp,
					      NXS_CONTROL_DST_FORMAT, &df);
			if (ret)
				return ret;

			if (nxs_dev_tmp == nxs_dev_to)
				break;
		}

		f.format.width = df.format.width;
		f.format.height = df.format.height;
	}

	/* others */
	nxs_dev_from = nxs_dev_to;
	if (nxs_dev_from)
		head = &nxs_dev_from->func_list;
	else
		head = &inst->dev_list;

	/* pr_info("FOR OTHERS ====> \n"); */
	nxs_dev_tmp = NULL;
	list_for_each_entry(nxs_dev_tmp, head, func_list) {
		if (!nxs_dev_tmp || !nxs_dev_tmp->set_control)
			break;
		/* pr_info("function %s\n", */
		/* 	nxs_function_to_str(nxs_dev_tmp->dev_function)); */
		ret = nxs_set_control(nxs_dev_tmp, NXS_CONTROL_FORMAT, &f);
		if (ret)
			return ret;
		/**
		 * TODO
		 * Sometimes kernel panic occured, I assume nxs_dev_tmp is
		 * invalid, Currently workaround this by next code
		 */
		if (is_end_node(nxs_dev_tmp))
			goto out;
	}

out:
	return 0;
}

static int nxs_dev_set_buffer(struct nxs_dev *nxs_dev,
			      struct nxs_address_info *info)
{
	union nxs_control control;
	int i;

	control.buffer.num_planes = info->num_planes;
	for (i = 0; i < info->num_planes; i++) {
		control.buffer.address[i] = info->dma_addr[i];
		control.buffer.strides[i] = info->strides[i];
	}

	return nxs_set_control(nxs_dev, NXS_CONTROL_BUFFER, &control);
}

static int nxs_get_address_info(struct vb2_v4l2_buffer *vb,
				unsigned int *strides,
				struct nxs_address_info *info)
{
	int i;
	struct vb2_buffer *buf = &vb->vb2_buf;

	info->num_planes = buf->num_planes;
	memcpy(info->strides, strides, buf->num_planes * sizeof(unsigned int));

	for (i = 0; i < buf->num_planes; i++) {
		info->dma_addr[i] = vb2_dma_contig_plane_dma_addr(buf, i);
		if (!info->dma_addr[i]) {
			WARN(1, "failed to get dma_addr for index %d\n", i);
			return -ENOMEM;
		}
	}

	return 0;
}

static int nxs_chain_set_buffer(struct nxs_function_instance *inst,
				struct nxs_video_fh *vfh,
				struct nxs_video_buffer *buffer)
{
	struct nxs_dev *nxs_dev = NULL;
	struct nxs_video *video = vfh->video;

	switch (video->type) {
	case NXS_VIDEO_TYPE_CAPTURE:
		nxs_dev = list_last_entry(&inst->dev_list, struct nxs_dev,
					  func_list);
		break;
	case NXS_VIDEO_TYPE_RENDER:
		nxs_dev = list_first_entry(&inst->dev_list, struct nxs_dev,
					   func_list);
		break;
	default:
		dev_err(&video->vdev.dev, "%s: Not supported type(0x%x)\n",
			__func__, video->type);
		return -EINVAL;
	}

	if (nxs_dev) {
		struct nxs_address_info info;

		nxs_get_address_info(&buffer->vb,
				     vfh->strides,
				     &info);
		return nxs_dev_set_buffer(nxs_dev, &info);
	}

	return 0;
}

static int nxs_m2m_chain_set_buffer(struct nxs_function_instance *inst,
				    struct nxs_video_fh *vfh,
				    struct nxs_address_info *src_addr_info,
				    struct nxs_address_info *dst_addr_info)
{
	struct nxs_dev *src_nxs_dev = NULL, *dst_nxs_dev = NULL;
	int ret;

	src_nxs_dev = list_first_entry(&inst->dev_list, struct nxs_dev,
				       func_list);
	if (WARN(!src_nxs_dev, "src nxs_dev non exist\n"))
		return -ENODEV;

	dst_nxs_dev = list_first_entry(&inst->dev_list, struct nxs_dev,
				       func_list);
	if (WARN(!dst_nxs_dev, "dst nxs_dev non exist\n"))
		return -ENODEV;

	ret = nxs_dev_set_buffer(src_nxs_dev, src_addr_info);
	if (WARN(ret, "failed to nxs_dev_set_buffer for src\n"))
		return ret;

	ret = nxs_dev_set_buffer(dst_nxs_dev, dst_addr_info);
	if (WARN(ret, "failed to nxs_dev_set_buffer for dst\n"))
		return ret;

	return 0;
}

static int nxs_chain_set_tid(struct nxs_function_instance *inst)
{
	struct nxs_dev *prev, *cur;
	int ret;

	prev = NULL;
	list_for_each_entry(cur, &inst->dev_list, func_list) {
		if (prev) {
			if (WARN(!prev->set_tid,
				 "no set_tid func for [%s:%d]\n",
				 nxs_function_to_str(prev->dev_function),
				 prev->dev_inst_index))
				return -EINVAL;

			/* TODO: handle multitap */
			ret = prev->set_tid(prev, cur->tid, 0);
			if (ret)
				return ret;
		}

		/* BLENDING_TO_MINE */
		if ((inst->req->flags & BLENDING_TO_MINE) &&
		    (cur->dev_function == NXS_FUNCTION_MLC_BLENDER)) {
			ret = inst->top->set_tid(inst->top, cur->tid, 0);
			if (ret)
				return ret;
		}

		prev = cur;
	}

	return 0;
}

static int nxs_chain_run(struct nxs_function_instance *inst)
{
	struct nxs_dev *nxs_dev, *first;
	int ret;

	ret = nxs_chain_set_tid(inst);
	if (ret)
		return ret;

	first = list_first_entry(&inst->dev_list, struct nxs_dev, func_list);
	list_for_each_entry_reverse(nxs_dev, &inst->dev_list, func_list) {
		if (nxs_dev->set_dirty) {
			ret = nxs_dev->set_dirty(nxs_dev);
			if (ret)
				return ret;
		}

		if (nxs_dev == first && inst->top) {
			ret = inst->top->set_dirty(inst->top);
			if (ret)
				return ret;
		}

		if (nxs_dev->start) {
			ret = nxs_dev->start(nxs_dev);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static void nxs_chain_stop(struct nxs_function_instance *inst)
{
	struct nxs_dev *nxs_dev;

	list_for_each_entry_reverse(nxs_dev, &inst->dev_list, func_list) {
		if (nxs_dev->stop)
			nxs_dev->stop(nxs_dev);
	}
}

/* irq callback */
/* capture, output */
static void nxs_video_irqcallback(struct nxs_dev *nxs_dev, void *data)
{
	struct nxs_video_fh *vfh;
	struct nxs_video *video;
	struct nxs_function_instance *inst;
	struct nxs_dev *first;
	struct nxs_video_buffer *done_buffer;
	struct nxs_video_buffer *next_buffer;

	vfh = data;
	video = vfh->video;
	inst = video->nxs_function;
	first = list_first_entry(&inst->dev_list, struct nxs_dev, func_list);

	done_buffer = get_next_video_buffer(vfh, true);
	if (!done_buffer)
		BUG();

	next_buffer = get_next_video_buffer(vfh, false);
	if (!next_buffer) {
		atomic_set(&vfh->underflow, 1);
		nxs_chain_stop(video->nxs_function);
	} else {
		struct nxs_address_info info;

		nxs_get_address_info(&next_buffer->vb,
				     vfh->strides,
				     &info);
		nxs_dev_set_buffer(nxs_dev, &info);

		if (nxs_dev->set_dirty)
			nxs_dev->set_dirty(nxs_dev);

		/**
		 * HACK
		 * dirty of first nxs_dev must be set to apply change
		 */
		if (nxs_dev != first)
			first->set_dirty(first);
	}

	v4l2_get_timestamp(&done_buffer->vb.timestamp);
	vb2_buffer_done(&done_buffer->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

/* m2m */
static void nxs_m2m_job_finish(struct nxs_video_fh *vfh, int vb_state)
{
	struct vb2_v4l2_buffer *src_vb, *dst_vb;

	src_vb = v4l2_m2m_src_buf_remove(vfh->m2m_ctx);
	if (WARN(!src_vb, "src_vb is NULL\n"))
		return;

	dst_vb = v4l2_m2m_dst_buf_remove(vfh->m2m_ctx);
	if (WARN(!dst_vb, "dst_vb is NULL\n"))
		return;

	v4l2_get_timestamp(&dst_vb->timestamp);

	v4l2_m2m_buf_done(src_vb, vb_state);
	v4l2_m2m_buf_done(dst_vb, vb_state);

	v4l2_m2m_job_finish(vfh->m2m_dev, vfh->m2m_ctx);
}

static void nxs_video_m2m_irqcallback(struct nxs_dev *nxs_dev, void *data)
{
	struct nxs_video_fh *vfh = data;

	nxs_m2m_job_finish(vfh, VB2_BUF_STATE_DONE);
	atomic_set(&vfh->running, 0);
	complete(&vfh->stop_done);
}

/*
 * v4l2_file_operations
 */
static int nxs_video_open(struct file *file)
{
	struct nxs_video *video = video_drvdata(file);
	struct nxs_video_fh *vfh;

	vfh = kzalloc(sizeof(*vfh), GFP_KERNEL);
	if (!vfh)
		return -ENOMEM;

	v4l2_fh_init(&vfh->vfh, &video->vdev);
	v4l2_fh_add(&vfh->vfh);

	if (is_m2m(video)) {
		atomic_set(&vfh->running, 0);
		init_completion(&vfh->stop_done);
	} else {
		INIT_LIST_HEAD(&vfh->bufq);
		spin_lock_init(&vfh->bufq_lock);
		atomic_set(&vfh->underflow, 0);
	}

	vfh->video = video;
	file->private_data = &vfh->vfh;

	video->open_count++;

	return 0;
}

static int nxs_video_release(struct file *file)
{
	struct nxs_video *video = video_drvdata(file);
	struct v4l2_fh *fh = file->private_data;
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);

	mutex_lock(&video->queue_lock);
	if (is_m2m(video)) {
		v4l2_m2m_ctx_release(vfh->m2m_ctx);
		v4l2_m2m_release(vfh->m2m_dev);
	} else {
		vb2_queue_release(&vfh->queue);
	}
	mutex_unlock(&video->queue_lock);

	v4l2_fh_del(fh);
	v4l2_fh_exit(fh);
	kfree(vfh);
	file->private_data = NULL;

	video->open_count--;
	/* TODO: if (nxs_video->open_count == 0) */

	return 0;
}

static int nxs_video_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(file->private_data);

	return vb2_mmap(&vfh->queue, vma);
}

static unsigned int nxs_video_poll(struct file *file,
				   struct poll_table_struct *tbl)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(file->private_data);
	struct nxs_video *video = video_drvdata(file);
	int ret;

	mutex_lock(&video->queue_lock);
	ret = vb2_poll(&vfh->queue, file, tbl);
	mutex_unlock(&video->queue_lock);

	return ret;
}

static struct v4l2_file_operations nxs_video_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl = video_ioctl2,
	.open		= nxs_video_open,
	.release	= nxs_video_release,
	.poll		= nxs_video_poll,
	.mmap		= nxs_video_mmap,
};

/* v4l2_m2m_ops */
static void nxs_m2m_device_run(void *priv)
{
	struct nxs_video_fh *vfh = priv;
	struct vb2_v4l2_buffer *src_vb, *dst_vb;
	struct nxs_address_info src_addr_info, dst_addr_info;
	int ret;

	src_vb = v4l2_m2m_next_src_buf(vfh->m2m_ctx);
	if (WARN(!src_vb, "failed to src_vb\n"))
		return;
	ret = nxs_get_address_info(src_vb, vfh->strides, &src_addr_info);
	if (WARN(ret, "failed to nxs_get_address_info for src_vb\n"))
		return;

	dst_vb = v4l2_m2m_next_dst_buf(vfh->m2m_ctx);
	if (WARN(!src_vb, "failed to src_vb\n"))
		return;
	ret = nxs_get_address_info(src_vb, vfh->dst_strides, &dst_addr_info);
	if (WARN(ret, "failed to nxs_get_address_info for dst_vb\n"))
		return;

	ret = nxs_m2m_chain_set_buffer(vfh->video->nxs_function, vfh,
				       &src_addr_info, &dst_addr_info);
	if (ret)
		return;

	atomic_set(&vfh->running, 1);

	nxs_chain_run(vfh->video->nxs_function);
}

/* This callback may not be needed */
/* static int nxs_m2m_job_ready(void *priv) */
/* { */
/* 	#<{(| TODO |)}># */
/* 	pr_info("%s: entered\n", __func__); */
/* 	return 0; */
/* } */

static void nxs_m2m_job_abort(void *priv)
{
	struct nxs_video_fh *vfh = priv;

	if (atomic_read(&vfh->running)) {
		if (!wait_for_completion_timeout(&vfh->stop_done,
						 NXS_VIDEO_JOB_TIMEOUT))
			dev_err(&vfh->video->vdev.dev, "timeout for waiting stop\n");
	} else
		nxs_m2m_job_finish(vfh, VB2_BUF_STATE_ERROR);
}

static struct v4l2_m2m_ops nxs_m2m_ops = {
	.device_run = nxs_m2m_device_run,
	/* .job_ready  = nxs_m2m_job_ready, */
	.job_abort  = nxs_m2m_job_abort,
};

/* v4l2_ioctl_ops */

/* VIDIOC_QUERYCAP handler */
static int nxs_vidioc_querycap(struct file *file, void *fh,
			       struct v4l2_capability *cap)
{
	struct nxs_video *nxs_video = video_drvdata(file);

	strlcpy(cap->driver, nxs_video->name, sizeof(cap->driver));
	strlcpy(cap->card, nxs_video->vdev.name, sizeof(cap->card));
	strlcpy(cap->bus_info, "media", sizeof(cap->bus_info));
	cap->version = KERNEL_VERSION(1, 0, 0);

	switch (nxs_video->type) {
	case NXS_VIDEO_TYPE_CAPTURE:
		cap->device_caps = V4L2_CAP_VIDEO_CAPTURE |
			V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING;
		break;
	case NXS_VIDEO_TYPE_RENDER:
		cap->device_caps = V4L2_CAP_VIDEO_OUTPUT |
			V4L2_CAP_VIDEO_OUTPUT_MPLANE | V4L2_CAP_STREAMING;
		break;
	case NXS_VIDEO_TYPE_M2M:
		cap->device_caps = V4L2_CAP_VIDEO_M2M |
			V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
		break;
	}

	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

/* VIDIOC_ENUM_FMT handlers */
static int generic_enum_fmt(struct v4l2_fmtdesc *f)
{
	struct nxs_video_format *array;
	int array_size;

	if (is_multiplanar(f->type)) {
		array = supported_mplane_formats;
		array_size = ARRAY_SIZE(supported_mplane_formats);
	} else {
		array = supported_formats;
		array_size = ARRAY_SIZE(supported_formats);
	}

	if (f->index >= array_size)
		return -EINVAL;

	f->flags = 0;
	strlcpy(f->description, array[f->index].name, sizeof(f->description));
	f->pixelformat = array[f->index].format;

	return 0;
}

static int nxs_vidioc_enum_fmt_vid_cap(struct file *file, void *fh,
				       struct v4l2_fmtdesc *f)
{
	return generic_enum_fmt(f);
}

static int nxs_vidioc_enum_fmt_vid_out(struct file *file, void *fh,
				       struct v4l2_fmtdesc *f)
{
	return generic_enum_fmt(f);
}

static int nxs_vidioc_enum_fmt_vid_cap_mplane(struct file *file, void *fh,
					      struct v4l2_fmtdesc *f)
{
	return generic_enum_fmt(f);
}

static int nxs_vidioc_enum_fmt_vid_out_mplane(struct file *file, void *fh,
					      struct v4l2_fmtdesc *f)
{
	return generic_enum_fmt(f);
}

/* VIDIOC_G_FMT handlers */
static int generic_g_fmt(void *fh, struct v4l2_format *f)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	struct nxs_video *video = vfh->video;
	struct v4l2_format *cur_format = NULL;

	if (video->type == NXS_VIDEO_TYPE_CAPTURE ||
	    video->type == NXS_VIDEO_TYPE_RENDER)
		cur_format = &vfh->format;
	else if (video->type == NXS_VIDEO_TYPE_M2M)
		cur_format = is_output(f->type) ? &vfh->format :
			&vfh->dst_format;

	if (cur_format) {
		if (cur_format->type == 0) {
			struct nxs_video *video = vfh->video;
			dev_err(&video->vdev.dev,
				"G_FMT: S_FMT is not called\n");
			return -EINVAL;
		}

		memcpy(f, cur_format, sizeof(*f));
		return 0;
	}

	return -EINVAL;
}

static int nxs_vidioc_g_fmt_vid_cap(struct file *file, void *fh,
				    struct v4l2_format *f)
{
	return generic_g_fmt(fh, f);
}

static int nxs_vidioc_g_fmt_vid_out(struct file *file, void *fh,
				    struct v4l2_format *f)
{
	return generic_g_fmt(fh, f);
}

static int nxs_vidioc_g_fmt_vid_cap_mplane(struct file *file, void *fh,
					   struct v4l2_format *f)
{
	return generic_g_fmt(fh, f);
}

static int nxs_vidioc_g_fmt_vid_out_mplane(struct file *file, void *fh,
					   struct v4l2_format *f)
{
	return generic_g_fmt(fh, f);
}

/* VIDIOC_S_FMT handlers */
static bool is_supported_format(struct nxs_video_fh *vfh, struct v4l2_format *f)
{
	struct nxs_video_format *array;
	int array_size;
	int i;
	bool supported;
	u32 pixelformat;

	if (is_multiplanar(f->type)) {
		array = supported_mplane_formats;
		array_size = ARRAY_SIZE(supported_mplane_formats);
		pixelformat = f->fmt.pix_mp.pixelformat;
	} else {
		array = supported_formats;
		array_size = ARRAY_SIZE(supported_formats);
		pixelformat = f->fmt.pix.pixelformat;
	}

	supported = false;
	for (i = 0; i < array_size; i++) {
		if (pixelformat == array[i].format) {
			supported = true;
			break;
		}
	}

	return supported;
}

static int m2m_s_fmt(void *fh, struct v4l2_format *f)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	struct nxs_video *video = vfh->video;

	if (is_supported_format(vfh, f)) {
		if (is_output(f->type)) {
			/* source path */
			memcpy(&vfh->format, f, sizeof(*f));
			vfh->type = f->type;
			get_info_of_format(f, &vfh->num_planes,
					   vfh->strides, vfh->sizes,
					   &vfh->width, &vfh->height,
					   &vfh->pixelformat);
		} else {
			/* dst path */
			memcpy(&vfh->dst_format, f, sizeof(*f));
			vfh->dst_type = f->type;
			get_info_of_format(f, &vfh->dst_num_planes,
					   vfh->dst_strides, vfh->dst_sizes,
					   &vfh->dst_width, &vfh->dst_height,
					   &vfh->dst_pixelformat);
		}

		if (!vfh->m2m_ctx) {
			vfh->m2m_dev = v4l2_m2m_init(&nxs_m2m_ops);
			if (IS_ERR(vfh->m2m_dev)) {
				dev_err(&video->vdev.dev,
					"%s: failed to init m2m device\n",
					__func__);
				return PTR_ERR(vfh->m2m_dev);
			}

			vfh->m2m_ctx = v4l2_m2m_ctx_init(vfh->m2m_dev, vfh,
							 nxs_m2m_queue_init);
			if (IS_ERR(vfh->m2m_ctx)) {
				dev_err(&video->vdev.dev,
					"%s: failed to init m2m context\n",
					__func__);
				return PTR_ERR(vfh->m2m_ctx);
			}
		}
	}

	return 0;
}

static int generic_s_fmt(void *fh, struct v4l2_format *f)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	struct nxs_video *video = vfh->video;

	if (is_supported_format(vfh, f)) {
		memcpy(&vfh->format, f, sizeof(*f));
		vfh->type = f->type;
		/* init crop, selection by format */
		if (vfh->crop.type == 0) {
			vfh->crop.type = f->type;
			vfh->crop.c.left = 0;
			vfh->crop.c.top = 0;
			vfh->crop.c.width = is_multiplanar(f->type) ?
				f->fmt.pix_mp.width : f->fmt.pix.width;
			vfh->crop.c.height = is_multiplanar(f->type) ?
				f->fmt.pix_mp.height : f->fmt.pix.height;
		}

		if (vfh->selection.type == 0) {
			vfh->selection.type = f->type;
			vfh->selection.r.left = 0;
			vfh->selection.r.top = 0;
			vfh->selection.r.width = is_multiplanar(f->type) ?
				f->fmt.pix_mp.width : f->fmt.pix.width;
			vfh->selection.r.height = is_multiplanar(f->type) ?
				f->fmt.pix_mp.height : f->fmt.pix.height;
		}

		if (vfh->parm.type == 0) {
			vfh->parm.type = f->type;
			if (video->type == NXS_VIDEO_TYPE_CAPTURE) {
				vfh->parm.parm.capture.capability =
					V4L2_CAP_TIMEPERFRAME;
				vfh->parm.parm.capture.timeperframe.denominator
					= 1;
				vfh->parm.parm.capture.timeperframe.numerator =
					NXS_VIDEO_CAPTURE_DEF_FRAMERATE;
				vfh->parm.parm.capture.readbuffers = 1;
			} else if (video->type == NXS_VIDEO_TYPE_RENDER) {
				vfh->parm.parm.output.capability =
					V4L2_CAP_TIMEPERFRAME;
				vfh->parm.parm.output.timeperframe.denominator =
					1;
				vfh->parm.parm.output.timeperframe.numerator =
					NXS_VIDEO_RENDER_DEF_FRAMERATE;
				vfh->parm.parm.output.writebuffers = 1;
			}
		}

		get_info_of_format(f, &vfh->num_planes, vfh->strides,
				   vfh->sizes, &vfh->width, &vfh->height,
				   &vfh->pixelformat);

		return nxs_vbq_init(vfh, f->type);
	}

	return -EINVAL;
}

static int nxs_vidioc_s_fmt_vid_cap(struct file *file, void *fh,
				    struct v4l2_format *f)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	struct nxs_video *video = vfh->video;

	if (is_m2m(video))
		return m2m_s_fmt(fh, f);

	return generic_s_fmt(fh, f);
}

static int nxs_vidioc_s_fmt_vid_out(struct file *file, void *fh,
				    struct v4l2_format *f)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	struct nxs_video *video = vfh->video;

	if (is_m2m(video))
		return m2m_s_fmt(fh, f);

	return generic_s_fmt(fh, f);
}

static int nxs_vidioc_s_fmt_vid_cap_mplane(struct file *file, void *fh,
					   struct v4l2_format *f)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	struct nxs_video *video = vfh->video;

	if (is_m2m(video))
		return m2m_s_fmt(fh, f);

	return generic_s_fmt(fh, f);
}

static int nxs_vidioc_s_fmt_vid_out_mplane(struct file *file, void *fh,
					   struct v4l2_format *f)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	struct nxs_video *video = vfh->video;

	if (is_m2m(video))
		return m2m_s_fmt(fh, f);

	return generic_s_fmt(fh, f);
}

/* VIDIOC_TRY_FMT handlers */
static int generic_try_fmt(void *fh, struct v4l2_format *f)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);

	if (is_supported_format(vfh, f))
		return 0;

	return -EINVAL;
}

static int nxs_vidioc_try_fmt_vid_cap(struct file *file, void *fh,
				      struct v4l2_format *f)
{
	return generic_try_fmt(fh, f);
}

static int nxs_vidioc_try_fmt_vid_out(struct file *file, void *fh,
				      struct v4l2_format *f)
{
	return generic_try_fmt(fh, f);
}

static int nxs_vidioc_try_fmt_vid_cap_mplane(struct file *file, void *fh,
					     struct v4l2_format *f)
{
	return generic_try_fmt(fh, f);
}

static int nxs_vidioc_try_fmt_vid_out_mplane(struct file *file, void *fh,
					     struct v4l2_format *f)
{
	return generic_try_fmt(fh, f);
}

/* Buffer handlers */
static int nxs_vidioc_reqbufs(struct file *file, void *fh,
			      struct v4l2_requestbuffers *b)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	struct nxs_video *video = vfh->video;
	int ret;

	mutex_lock(&video->queue_lock);
	ret = is_m2m(video) ? v4l2_m2m_reqbufs(file, vfh->m2m_ctx, b) :
		vb2_reqbufs(&vfh->queue, b);
	mutex_unlock(&video->queue_lock);

	return ret;
}

static int nxs_vidioc_querybuf(struct file *file, void *fh,
			       struct v4l2_buffer *b)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	struct nxs_video *video = vfh->video;
	int ret;

	mutex_lock(&video->queue_lock);
	ret = is_m2m(video) ? v4l2_m2m_querybuf(file, vfh->m2m_ctx, b) :
		vb2_querybuf(&vfh->queue, b);
	mutex_unlock(&video->queue_lock);

	return ret;
}

static int nxs_vidioc_qbuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	struct nxs_video *video = vfh->video;
	int ret;

	mutex_lock(&video->queue_lock);
	ret = is_m2m(video) ? v4l2_m2m_qbuf(file, vfh->m2m_ctx, b) :
		vb2_qbuf(&vfh->queue, b);
	mutex_unlock(&video->queue_lock);

	return ret;
}

static int nxs_vidioc_expbuf(struct file *file, void *fh,
			     struct v4l2_exportbuffer *e)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	struct nxs_video *video = vfh->video;
	int ret;

	mutex_lock(&video->queue_lock);
	ret = is_m2m(video) ? v4l2_m2m_expbuf(file, vfh->m2m_ctx, e) :
		vb2_expbuf(&vfh->queue, e);
	mutex_unlock(&video->queue_lock);

	return ret;
}

static int nxs_vidioc_dqbuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	struct nxs_video *video = vfh->video;
	int ret;

	mutex_lock(&video->queue_lock);
	ret = is_m2m(video) ? v4l2_m2m_dqbuf(file, vfh->m2m_ctx, b) :
		vb2_dqbuf(&vfh->queue, b, file->f_flags & O_NONBLOCK);
	mutex_unlock(&video->queue_lock);

	return ret;
}

static int nxs_vidioc_create_bufs(struct file *file, void *fh,
				  struct v4l2_create_buffers *b)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	struct nxs_video *video = vfh->video;
	int ret;

	mutex_lock(&video->queue_lock);
	ret = is_m2m(video) ? v4l2_m2m_create_bufs(file, vfh->m2m_ctx, b) :
		vb2_create_bufs(&vfh->queue, b);
	mutex_unlock(&video->queue_lock);

	return ret;
}

static int nxs_vidioc_prepare_buf(struct file *file, void *fh,
				  struct v4l2_buffer *b)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	struct nxs_video *video = vfh->video;
	int ret;

	mutex_lock(&video->queue_lock);
	ret = is_m2m(video) ? v4l2_m2m_prepare_buf(file, vfh->m2m_ctx, b):
		vb2_prepare_buf(&vfh->queue, b);
	mutex_unlock(&video->queue_lock);

	return ret;
}

/* Stream on/off */
static int nxs_vidioc_streamon(struct file *file, void *fh,
			       enum v4l2_buf_type type)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	struct nxs_video *video = vfh->video;
	int ret;

	if (!(is_m2m(video)) && type != vfh->queue.type)
		return -EINVAL;

	mutex_lock(&video->stream_lock);
	mutex_lock(&video->queue_lock);
	ret = is_m2m(video) ? v4l2_m2m_streamon(file, vfh->m2m_ctx, type) :
		vb2_streamon(&vfh->queue, type);
	mutex_unlock(&video->queue_lock);
	mutex_unlock(&video->stream_lock);

	return ret;
}

static int nxs_vidioc_streamoff(struct file *file, void *fh,
				enum v4l2_buf_type type)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	struct nxs_video *video = vfh->video;
	int ret;

	if (!(is_m2m(video)) && type != vfh->queue.type)
		return -EINVAL;

	mutex_lock(&video->stream_lock);
	mutex_lock(&video->queue_lock);
	ret = is_m2m(video) ? v4l2_m2m_streamoff(file, vfh->m2m_ctx, type) :
		vb2_streamoff(&vfh->queue, type);
	mutex_unlock(&video->queue_lock);
	mutex_unlock(&video->stream_lock);

	return ret;
}

/* Control handling */
static int nxs_vidioc_queryctrl(struct file *file, void *fh,
				struct v4l2_queryctrl *ctrl)
{
	/* struct nxs_video_fh *vfh = to_nxs_video_fh(fh); */
	/* struct nxs_video *video = vfh->video; */
	/* int ret; */

	/* TODO: see CIDS include/uapi/linux/v4l2-controls.h */
	/* use like below */
	/* switch (ctrl->id) { */
	/* case V4L2_CID_ROTATE: */
	/* 	ret = v4l2_ctrl_query_fill(ctrl, min, max, step, def); */
	/* 	break; */
	/* } */

	return 0;
}

static int nxs_vidioc_g_ctrl(struct file *file, void *fh,
			     struct v4l2_control *ctrl)
{
	/* struct nxs_video_fh *vfh = to_nxs_video_fh(fh); */
	/* struct nxs_video *video = vfh->video; */
	/* int ret; */

	/* TODO: use like below */
	/* switch (ctrl->id) { */
	/* case V4L2_CID_ROTATE: */
	/* 	ctrl->value = xxx; */
	/* 	break; */
	/* } */

	return 0;
}

static int nxs_vidioc_s_ctrl(struct file *file, void *fh,
			     struct v4l2_control *ctrl)
{
	/* struct nxs_video_fh *vfh = to_nxs_video_fh(fh); */
	/* struct nxs_video *video = vfh->video; */
	/* int ret; */

	/* TODO: use like below */
	/* switch (ctrl->id) { */
	/* case V4L2_CID_ROTATE: */
	/* 	vfh->xx = ctrl->value; */
	/* 	break; */
	/* } */

	return 0;
}

/* Ext control handling */
static int nxs_vidioc_query_ext_ctrl(struct file *file, void *fh,
				     struct v4l2_query_ext_ctrl *ctrl)
{
	/* struct nxs_video_fh *vfh = to_nxs_video_fh(fh); */
	/* struct nxs_video *video = vfh->video; */
	/* int ret; */

	/* TODO */
	return 0;
}

static int nxs_vidioc_g_ext_ctrls(struct file *file, void *fh,
				  struct v4l2_ext_controls *ctrls)
{
	/* struct nxs_video_fh *vfh = to_nxs_video_fh(fh); */
	/* struct nxs_video *video = vfh->video; */
	/* int ret; */

	/* TODO */
	return 0;
}

static int nxs_vidioc_s_ext_ctrls(struct file *file, void *fh,
				  struct v4l2_ext_controls *ctrls)
{
	/* struct nxs_video_fh *vfh = to_nxs_video_fh(fh); */
	/* struct nxs_video *video = vfh->video; */
	/* int ret; */

	/* TODO */
	return 0;
}

static int nxs_vidioc_try_ext_ctrls(struct file *file, void *fh,
				    struct v4l2_ext_controls *ctrls)
{
	/* struct nxs_video_fh *vfh = to_nxs_video_fh(fh); */
	/* struct nxs_video *video = vfh->video; */
	/* int ret; */

	/* TODO */
	return 0;
}

/* Crop ioctls */
/* crop is for input image cropping */
/* generally cropping will be used in vip clipper */
static int nxs_vidioc_cropcap(struct file *file, void *fh,
			      struct v4l2_cropcap *a)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);
	/* int ret; */

	/* m2m device does not support crop */
	if (is_m2m(vfh->video))
		return -EINVAL;

	/* TODO */
	return 0;
}

static int nxs_vidioc_g_crop(struct file *file, void *fh, struct v4l2_crop *a)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);

	/* m2m device does not support crop */
	if (is_m2m(vfh->video))
		return -EINVAL;

	if (vfh->crop.type == 0)
		return -EINVAL;

	memcpy(a, &vfh->crop, sizeof(*a));

	return 0;
}

static int nxs_vidioc_s_crop(struct file *file, void *fh,
			     const struct v4l2_crop *a)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);

	/* m2m device does not support crop */
	if (is_m2m(vfh->video))
		return -EINVAL;

	/* TODO: check crop area */
	memcpy(&vfh->crop, a, sizeof(*a));

	return 0;
}

static int nxs_vidioc_g_selection(struct file *file, void *fh,
				  struct v4l2_selection *s)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);

	/* m2m device does not support selection */
	if (is_m2m(vfh->video))
		return -EINVAL;

	if (vfh->selection.type == 0)
		return -EINVAL;

	memcpy(s, &vfh->selection, sizeof(*s));

	return 0;
}

static int nxs_vidioc_s_selection(struct file *file, void *fh,
				  struct v4l2_selection *s)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);

	/* m2m device does not support selection */
	if (is_m2m(vfh->video))
		return -EINVAL;

	/* TODO: check selection area */
	memcpy(&vfh->selection, s, sizeof(*s));

	return 0;
}

/* Stream type-dependent parameter ioctls */
static int nxs_vidioc_g_parm(struct file *file, void *fh,
			     struct v4l2_streamparm *a)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);

	/* m2m device does not support parm */
	if (is_m2m(vfh->video))
		return -EINVAL;

	if (vfh->parm.type == 0)
		return -EINVAL;

	memcpy(a, &vfh->parm, sizeof(*a));

	return 0;
}

static int nxs_vidioc_s_parm(struct file *file, void *fh,
			     struct v4l2_streamparm *a)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);

	/* m2m device does not support parm */
	if (is_m2m(vfh->video))
		return -EINVAL;

	/* TODO: check parm parameter */
	memcpy(&vfh->parm, a, sizeof(*a));

	return 0;
}

/* Debugging ioctls */
#ifdef CONFIG_VIDEO_ADV_DEBUG
static int nxs_vidioc_g_register(struct file *file, void *fh,
				 struct v4l2_dbg_register *reg)
{
	/* struct nxs_video_fh *vfh = to_nxs_video_fh(fh); */
	/* struct nxs_video *video = vfh->video; */
	/* int ret; */

	/* TODO */
	return 0;
}

static int nxs_vidioc_s_register(struct file *file, void *fh,
				 struct v4l2_dbg_register *reg)
{
	/* struct nxs_video_fh *vfh = to_nxs_video_fh(fh); */
	/* struct nxs_video *video = vfh->video; */
	/* int ret; */

	/* TODO */
	return 0;
}
#endif

static int nxs_vidioc_enum_framesizes(struct file *file, void *fh,
				      struct v4l2_frmsizeenum *fsize)
{
	/* struct nxs_video_fh *vfh = to_nxs_video_fh(fh); */
	/* struct nxs_video *video = vfh->video; */
	/* int ret; */

	/* TODO: implement for only capture type */
	/* ask for sensor subdev */

	return 0;
}

static int nxs_vidioc_enum_frameintervals(struct file *file, void *fh,
					  struct v4l2_frmivalenum *fival)
{
	/* struct nxs_video_fh *vfh = to_nxs_video_fh(fh); */
	/* struct nxs_video *video = vfh->video; */
	/* int ret; */

	/* TODO: implement for only capture type */
	/* ask for sensor subdev */

	return 0;
}

/* DV Timings IOCTLs */
/* TODO this handler is only for render device */
static int nxs_vidioc_s_dv_timings(struct file *file, void *fh,
				   struct v4l2_dv_timings *timings)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);

	if (is_render(vfh->video))
		return 0;

	return -EINVAL;
}

static int nxs_vidioc_g_dv_timings(struct file *file, void *fh,
				   struct v4l2_dv_timings *timings)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);

	if (is_render(vfh->video))
		return 0;

	return -EINVAL;
}

static int nxs_vidioc_query_dv_timings(struct file *file, void *fh,
				       struct v4l2_dv_timings *timings)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);

	if (is_render(vfh->video))
		return 0;

	return -EINVAL;
}

static int nxs_vidioc_enum_dv_timings(struct file *file, void *fh,
				      struct v4l2_enum_dv_timings *timings)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);

	if (is_render(vfh->video))
		return 0;

	return -EINVAL;
}

static int nxs_vidioc_dv_timings_cap(struct file *file, void *fh,
				     struct v4l2_dv_timings_cap *cap)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);

	if (is_render(vfh->video))
		return 0;

	return -EINVAL;
}

/* EDID IOCTLs */
/* TODO this handler is only for render device */
static int nxs_vidioc_g_edid(struct file *file, void *fh,
			     struct v4l2_edid *edid)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);

	if (is_render(vfh->video))
		return 0;

	return -EINVAL;
}

static int nxs_vidioc_s_edid(struct file *file, void *fh,
			     struct v4l2_edid *edid)
{
	struct nxs_video_fh *vfh = to_nxs_video_fh(fh);

	if (is_render(vfh->video))
		return 0;

	return -EINVAL;
}

/* v4l2_event IOCTLs */
/* App can subscribe event by VIDIOC_SUBSCRIBE_EVENT cmd and
 * can receive by VIDIOC_DQEVENT
 * Driver can emit event by v4l2_event_queue()
 * When driver calls v4l2_event_queue, process that called VIDIOC_DQEVENT
 * is awaken.
 * See videodev2.h, struct v4l2_event
 * event type: vsync, eos, ctrl, frame_sync, source_change, motion_det, private
 */
static int nxs_vidioc_subscribe_event(struct v4l2_fh *fh,
				      const struct v4l2_event_subscription *sub)
{
	if (sub->type == V4L2_EVENT_CTRL)
		return v4l2_ctrl_subscribe_event(fh, sub);
	/* TODO */
	/* else */

	return 0;
}

/* For other private ioctls */
static long nxs_vidioc_default(struct file *file, void *fh, bool valid_prio,
			       unsigned int cmd, void *arg)
{
	/* struct nxs_video_fh *vfh = to_nxs_video_fh(fh); */
	/* struct nxs_video *video = vfh->video; */
	/* int ret; */

	/* TODO */
	return 0;
}

static struct v4l2_ioctl_ops nxs_video_ioctl_ops = {
	.vidioc_querycap		= nxs_vidioc_querycap,
	.vidioc_enum_fmt_vid_cap	= nxs_vidioc_enum_fmt_vid_cap,
	.vidioc_enum_fmt_vid_out	= nxs_vidioc_enum_fmt_vid_out,
	.vidioc_enum_fmt_vid_cap_mplane	= nxs_vidioc_enum_fmt_vid_cap_mplane,
	.vidioc_enum_fmt_vid_out_mplane	= nxs_vidioc_enum_fmt_vid_out_mplane,
	.vidioc_g_fmt_vid_cap		= nxs_vidioc_g_fmt_vid_cap,
	.vidioc_g_fmt_vid_out		= nxs_vidioc_g_fmt_vid_out,
	.vidioc_g_fmt_vid_cap_mplane	= nxs_vidioc_g_fmt_vid_cap_mplane,
	.vidioc_g_fmt_vid_out_mplane	= nxs_vidioc_g_fmt_vid_out_mplane,
	.vidioc_s_fmt_vid_cap		= nxs_vidioc_s_fmt_vid_cap,
	.vidioc_s_fmt_vid_out		= nxs_vidioc_s_fmt_vid_out,
	.vidioc_s_fmt_vid_cap_mplane	= nxs_vidioc_s_fmt_vid_cap_mplane,
	.vidioc_s_fmt_vid_out_mplane	= nxs_vidioc_s_fmt_vid_out_mplane,
	.vidioc_try_fmt_vid_cap		= nxs_vidioc_try_fmt_vid_cap,
	.vidioc_try_fmt_vid_out		= nxs_vidioc_try_fmt_vid_out,
	.vidioc_try_fmt_vid_cap_mplane	= nxs_vidioc_try_fmt_vid_cap_mplane,
	.vidioc_try_fmt_vid_out_mplane	= nxs_vidioc_try_fmt_vid_out_mplane,
	.vidioc_reqbufs			= nxs_vidioc_reqbufs,
	.vidioc_querybuf		= nxs_vidioc_querybuf,
	.vidioc_qbuf			= nxs_vidioc_qbuf,
	.vidioc_expbuf			= nxs_vidioc_expbuf,
	.vidioc_dqbuf			= nxs_vidioc_dqbuf,
	.vidioc_create_bufs		= nxs_vidioc_create_bufs,
	.vidioc_prepare_buf		= nxs_vidioc_prepare_buf,
	.vidioc_streamon		= nxs_vidioc_streamon,
	.vidioc_streamoff		= nxs_vidioc_streamoff,
	.vidioc_queryctrl		= nxs_vidioc_queryctrl,
	.vidioc_g_ctrl			= nxs_vidioc_g_ctrl,
	.vidioc_s_ctrl			= nxs_vidioc_s_ctrl,
	.vidioc_query_ext_ctrl		= nxs_vidioc_query_ext_ctrl,
	.vidioc_g_ext_ctrls		= nxs_vidioc_g_ext_ctrls,
	.vidioc_s_ext_ctrls		= nxs_vidioc_s_ext_ctrls,
	.vidioc_try_ext_ctrls		= nxs_vidioc_try_ext_ctrls,
	.vidioc_cropcap			= nxs_vidioc_cropcap,
	.vidioc_g_crop			= nxs_vidioc_g_crop,
	.vidioc_s_crop			= nxs_vidioc_s_crop,
	.vidioc_g_selection		= nxs_vidioc_g_selection,
	.vidioc_s_selection		= nxs_vidioc_s_selection,
	.vidioc_g_parm			= nxs_vidioc_g_parm,
	.vidioc_s_parm			= nxs_vidioc_s_parm,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.vidioc_g_register		= nxs_vidioc_g_register,
	.vidioc_s_register		= nxs_vidioc_s_register,
#endif
	.vidioc_enum_framesizes		= nxs_vidioc_enum_framesizes,
	.vidioc_enum_frameintervals	= nxs_vidioc_enum_frameintervals,
	.vidioc_s_dv_timings		= nxs_vidioc_s_dv_timings,
	.vidioc_g_dv_timings		= nxs_vidioc_g_dv_timings,
	.vidioc_query_dv_timings	= nxs_vidioc_query_dv_timings,
	.vidioc_enum_dv_timings		= nxs_vidioc_enum_dv_timings,
	.vidioc_dv_timings_cap		= nxs_vidioc_dv_timings_cap,
	.vidioc_g_edid			= nxs_vidioc_g_edid,
	.vidioc_s_edid			= nxs_vidioc_s_edid,
	.vidioc_subscribe_event		= nxs_vidioc_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
	.vidioc_default			= nxs_vidioc_default,
};

/* struct vb2_ops */

/* reqbuf call this callback */
static int nxs_vb2_queue_setup(struct vb2_queue *q,
			       const void *parg,
			       unsigned int *num_buffers,
			       unsigned int *num_planes,
			       unsigned int sizes[],
			       void *alloc_ctxs[])
{
	struct nxs_video_fh *vfh = vb2_get_drv_priv(q);
	struct nxs_video *video = vfh->video;
	int i;

	if (vfh->format.type == 0) {
		dev_err(&video->vdev.dev, "%s: format is not set\n", __func__);
		return -EINVAL;
	}

	*num_planes = vfh->num_planes;
	memcpy(sizes, vfh->sizes, vfh->num_planes * sizeof(unsigned int));

	for (i = 0; i < *num_planes; i++)
		alloc_ctxs[i] = video->vb2_alloc_ctx;

	/* do not need to set num_buffers
	 * use user value
	 */

	return 0;
}

/* TODO: check wait_prepare, wait_finish overriding */
/* static void nxs_vb2_wait_prepare(struct vb2_queue *q) */
/* { */
/* } */
/*  */
/* static void nxs_vb2_wait_finish(struct vb2_queue *q) */
/* { */
/* } */

/* TODO: check vb2_buf_init, vb2_buf_finish, vb2_buf_cleanup overriding */
/* static int nxs_vb2_buf_init(struct vb2_buffer *vb) */
/* { */
/* 	return 0; */
/* } */
/*  */
/* static void nxs_vb2_buf_finish(struct vb2_buffer *vb) */
/* { */
/* } */
/*  */
/* static void nxs_vb2_buf_cleanup(struct vb2_buffer *vb) */
/* { */
/* } */

static int nxs_vb2_buf_prepare(struct vb2_buffer *buf)
{
	struct nxs_video_fh *vfh = vb2_get_drv_priv(buf->vb2_queue);
	int i;

	for (i = 0; i < buf->num_planes; i++)
		vb2_set_plane_payload(buf, i, vfh->sizes[i]);

	return 0;
}

static void nxs_vb2_buf_queue(struct vb2_buffer *buf)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(buf);
	struct nxs_video_fh *vfh = vb2_get_drv_priv(buf->vb2_queue);
	struct nxs_video_buffer *buffer = to_nxs_video_buffer(vbuf);
	unsigned long flags;

	spin_lock_irqsave(&vfh->bufq_lock, flags);
	list_add_tail(&buffer->list, &vfh->bufq);
	spin_unlock_irqrestore(&vfh->bufq_lock, flags);
}

static bool need_camera_sensor(struct nxs_video *video);
static int capture_on(struct nxs_video *video);
static int capture_off(struct nxs_video *video);

static int nxs_vb2_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct nxs_video_fh *vfh = vb2_get_drv_priv(q);
	struct nxs_video *video = vfh->video;
	struct nxs_video_buffer *buffer;
	int ret;

	buffer = get_next_video_buffer(vfh, false);
	if (!buffer) {
		dev_err(&video->vdev.dev, "%s: can't get buffer\n", __func__);
		return -ENOMEM;
	}

	ret = nxs_chain_register_irqcallback(video->nxs_function, vfh,
					     nxs_video_irqcallback);
	if (ret)
		return ret;

	ret = nxs_chain_config(video->nxs_function, vfh);
	if (ret)
		return ret;

	ret = nxs_chain_set_buffer(video->nxs_function, vfh, buffer);
	if (ret)
		return ret;

	ret = nxs_chain_run(video->nxs_function);
	if (ret)
		return ret;

	if (need_camera_sensor(video))
		return capture_on(video);

	return 0;
}

static void nxs_vb2_stop_streaming(struct vb2_queue *q)
{
	struct nxs_video_fh *vfh = vb2_get_drv_priv(q);
	struct nxs_video *video = vfh->video;

	nxs_chain_stop(video->nxs_function);
	nxs_chain_unregister_irqcallback(video->nxs_function);

	if (need_camera_sensor(video))
		capture_off(video);
}

static struct vb2_ops nxs_vb2_ops = {
	.queue_setup		= nxs_vb2_queue_setup,
	/* .wait_prepare		= vb2_ops_wait_prepare, */
	/* .wait_finish		= vb2_ops_wait_finish, */
	/* .buf_init		= nxs_vb2_buf_init, */
	/* .buf_finish		= nxs_vb2_buf_finish, */
	/* .buf_cleanup		= nxs_vb2_buf_cleanup, */
	.buf_prepare		= nxs_vb2_buf_prepare,
	.buf_queue		= nxs_vb2_buf_queue,
	.start_streaming	= nxs_vb2_start_streaming,
	.stop_streaming		= nxs_vb2_stop_streaming,
};

static int nxs_vbq_init(struct nxs_video_fh *vfh, u32 type)
{
	struct vb2_queue *vbq;

	vbq = &vfh->queue;

	vbq->type	= type;
	vbq->io_modes	= VB2_MMAP
			| VB2_USERPTR
			| VB2_READ
			| VB2_WRITE
			| VB2_DMABUF;
	vbq->drv_priv	= vfh;
	vbq->ops	= &nxs_vb2_ops;
	vbq->mem_ops	= &vb2_dma_contig_memops;
	vbq->buf_struct_size = sizeof(struct nxs_video_buffer);
	vbq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;

	return vb2_queue_init(vbq);
}

/* m2m vb2 ops */
/* static int nxs_m2m_vb2_queue_setup(struct vb2_queue *q, */
/* 				   const void *parg, */
/* 				   unsigned int *num_buffers, */
/* 				   unsigned int *num_planes, */
/* 				   unsigned int sizes[], */
/* 				   void *alloc_ctxs[]) */
/* { */
/* 	#<{(| TODO |)}># */
/* 	return 0; */
/* } */

static int nxs_m2m_vb2_buf_prepare(struct vb2_buffer *buf)
{
	struct nxs_video_fh *vfh = vb2_get_drv_priv(buf->vb2_queue);
	int i;

	if (!V4L2_TYPE_IS_OUTPUT(buf->vb2_queue->type))
		for (i = 0; i < vfh->dst_num_planes; i++)
			vb2_set_plane_payload(buf, 0, vfh->sizes[i]);

	return 0;
}

static void nxs_m2m_vb2_buf_queue(struct vb2_buffer *buf)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(buf);
	struct nxs_video_fh *vfh = vb2_get_drv_priv(buf->vb2_queue);

	if (vfh->m2m_ctx)
		v4l2_m2m_buf_queue(vfh->m2m_ctx, vbuf);
}

static int nxs_m2m_vb2_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct nxs_video_fh *vfh = vb2_get_drv_priv(q);
	struct nxs_video *video = vfh->video;
	int ret;

	/* start streaming called twice
	 * first: source
	 * second: dest
	 * when called firstly, do nothing */
	if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
	    q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return 0;

	ret = nxs_chain_register_irqcallback(video->nxs_function, vfh,
					     nxs_video_m2m_irqcallback);
	if (ret)
		return ret;

	ret = nxs_m2m_chain_config(video->nxs_function, vfh);
	if (ret)
		return ret;

	return nxs_chain_run(video->nxs_function);
}

static void nxs_m2m_vb2_stop_streaming(struct vb2_queue *q)
{
	struct nxs_video_fh *vfh = vb2_get_drv_priv(q);
	struct nxs_video *video = vfh->video;

	if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
	    q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return;

	nxs_m2m_job_abort(vfh);

	nxs_chain_stop(video->nxs_function);
	nxs_chain_unregister_irqcallback(video->nxs_function);
}

static struct vb2_ops nxs_m2m_vb2_ops = {
	/* .queue_setup		= nxs_m2m_vb2_queue_setup, */
	.queue_setup		= nxs_vb2_queue_setup,
	.buf_prepare		= nxs_m2m_vb2_buf_prepare,
	.buf_queue		= nxs_m2m_vb2_buf_queue,
	.start_streaming	= nxs_m2m_vb2_start_streaming,
	.stop_streaming		= nxs_m2m_vb2_stop_streaming,
};

static int nxs_m2m_queue_init(void *priv, struct vb2_queue *src_q,
			      struct vb2_queue *dst_q)
{
	struct nxs_video_fh *vfh = priv;
	int ret;

	memset(src_q, 0, sizeof(*src_q));
	src_q->type = is_multiplanar(vfh->type) ?
		V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE :
		V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	src_q->drv_priv = vfh;
	src_q->ops = &nxs_m2m_vb2_ops;
	src_q->mem_ops = &vb2_dma_contig_memops;
	src_q->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	/* HACK: check below type */
	/* src_q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY; */
	/* HACK: check lock like below */
	/* src_q->lock = &vfh->lock; */

	ret = vb2_queue_init(src_q);
	if (ret)
		return ret;

	memset(dst_q, 0, sizeof(*dst_q));
	dst_q->type = is_multiplanar(vfh->type) ?
		V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
		V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	dst_q->drv_priv = vfh;
	dst_q->ops = &nxs_m2m_vb2_ops;
	dst_q->mem_ops = &vb2_dma_contig_memops;
	dst_q->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	/* HACK: check below type */
	/* dst_q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY; */
	/* HACK: check lock like below */
	/* dst_q->lock = &vfh->lock; */

	return vb2_queue_init(dst_q);
}

/* subdev ops */
static int nxs_subdev_get_selection(struct v4l2_subdev *sd,
				    struct v4l2_subdev_pad_config *cfg,
				    struct v4l2_subdev_selection *sel)
{
	struct nxs_video *video = v4l2_get_subdevdata(sd);
	struct nxs_subdev_ctx *ctx = video->subdev_ctx;

	memcpy(&sel->r, &ctx->crop, sizeof(struct v4l2_rect));

	return 0;
}

static int nxs_subdev_set_selection(struct v4l2_subdev *sd,
				    struct v4l2_subdev_pad_config *cfg,
				    struct v4l2_subdev_selection *sel)
{
	struct nxs_video *video = v4l2_get_subdevdata(sd);
	struct nxs_subdev_ctx *ctx = video->subdev_ctx;

	if (sel->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		memcpy(&sel->r, &ctx->crop, sizeof(struct v4l2_rect));

	return 0;
}

static int nxs_subdev_get_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_pad_config *cfg,
			      struct v4l2_subdev_format *format)
{
	struct nxs_video *video = v4l2_get_subdevdata(sd);
	struct nxs_subdev_ctx *ctx = video->subdev_ctx;

	memcpy(&format->format, &ctx->format,
	       sizeof(struct v4l2_mbus_framefmt));

	return 0;
}

static int nxs_subdev_set_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_pad_config *cfg,
			      struct v4l2_subdev_format *format)
{
	struct nxs_video *video = v4l2_get_subdevdata(sd);
	struct nxs_subdev_ctx *ctx = video->subdev_ctx;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		memcpy(&ctx->format, &format->format,
		       sizeof(struct v4l2_mbus_framefmt));

	return 0;
}

static int nxs_subdev_get_dstfmt(struct v4l2_subdev *sd,
				 struct v4l2_subdev_format *format)
{
	struct nxs_video *video = v4l2_get_subdevdata(sd);
	struct nxs_subdev_ctx *ctx = video->subdev_ctx;

	memcpy(&format->format, &ctx->dst_format,
	       sizeof(struct v4l2_mbus_framefmt));

	return 0;
}

static int nxs_subdev_set_dstfmt(struct v4l2_subdev *sd,
				 struct v4l2_subdev_format *format)
{
	struct nxs_video *video = v4l2_get_subdevdata(sd);
	struct nxs_subdev_ctx *ctx = video->subdev_ctx;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		memcpy(&ctx->dst_format, &format->format,
		       sizeof(struct v4l2_mbus_framefmt));

	return 0;
}

static int nxs_subdev_start(struct v4l2_subdev *sd)
{
	struct nxs_video *video = v4l2_get_subdevdata(sd);
	struct nxs_subdev_ctx *ctx = video->subdev_ctx;
	int ret;

	ret = nxs_subdev_chain_config(video->nxs_function, ctx);
	if (ret)
		return ret;

	ret = nxs_chain_run(video->nxs_function);
	if (ret)
		return ret;

	if (need_camera_sensor(video))
		ret = capture_on(video);

	return ret;
}

static int nxs_subdev_stop(struct v4l2_subdev *sd)
{
	struct nxs_video *video = v4l2_get_subdevdata(sd);

	nxs_chain_stop(video->nxs_function);
	if (need_camera_sensor(video))
		return capture_off(video);

	return 0;
}

static long nxs_subdev_ioctl(struct v4l2_subdev *sd, unsigned int cmd,
			     void *arg)
{
	int ret;

	/* no need to copy_xx_user() here */
	switch (cmd) {
	case NXSIOC_S_DSTFMT:
		ret = nxs_subdev_set_dstfmt(sd, arg);
		break;
	case NXSIOC_G_DSTFMT:
		ret = nxs_subdev_get_dstfmt(sd, arg);
		break;
	case NXSIOC_START:
		ret = nxs_subdev_start(sd);
		break;
	case NXSIOC_STOP:
		ret = nxs_subdev_stop(sd);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct v4l2_subdev_pad_ops nxs_subdev_pad_ops = {
	.get_selection = nxs_subdev_get_selection,
	.set_selection = nxs_subdev_set_selection,
	.get_fmt = nxs_subdev_get_fmt,
	.set_fmt = nxs_subdev_set_fmt,
};

static const struct v4l2_subdev_core_ops nxs_subdev_core_ops = {
	.ioctl = nxs_subdev_ioctl,
};

static const struct v4l2_subdev_ops nxs_subdev_ops = {
	.pad  = &nxs_subdev_pad_ops,
	.core = &nxs_subdev_core_ops,
};

static u32 get_nxs_video_type(struct nxs_function_instance *inst)
{
	struct nxs_dev *first, *last;

	first = list_first_entry(&inst->dev_list, struct nxs_dev, func_list);
	last = list_last_entry(&inst->dev_list, struct nxs_dev, func_list);

	if (!first || !last)
		return NXS_VIDEO_TYPE_INVALID;

	if (first->dev_function == NXS_FUNCTION_DMAR) {
		if (last->dev_function == NXS_FUNCTION_DMAW)
			return NXS_VIDEO_TYPE_M2M;
		else
			return NXS_VIDEO_TYPE_RENDER;
	} else {
		if (last->dev_function == NXS_FUNCTION_DMAW)
			return NXS_VIDEO_TYPE_CAPTURE;
		else
			return NXS_VIDEO_TYPE_SUBDEV;
	}
}

static void dump_nxs_dev(struct nxs_dev *dev)
{
	pr_info("\tdev %p: [%s:%d] user %d, multitap_connected %d, connect_count %d\n",
		dev, nxs_function_to_str(dev->dev_function),
		dev->dev_inst_index,
		dev->user,
		dev->multitap_connected,
		atomic_read(&dev->connect_count));
	pr_info("\t\tcan_multitap_follow %d, refcount %d, max_refcount %d\n",
		dev->can_multitap_follow, atomic_read(&dev->refcount),
		dev->max_refcount);
}

static void dump_nxs_function_inst(struct nxs_function_instance *inst)
{
	struct nxs_dev *nxs_dev;

	pr_info("dump inst %p ==================> \n", inst);
	pr_info("\tsibling list\n");
	list_for_each_entry(nxs_dev, &inst->dev_sibling_list, sibling_list) {
		dump_nxs_dev(nxs_dev);
	}
	pr_info("\tdev list\n");
	list_for_each_entry(nxs_dev, &inst->dev_list, func_list) {
		dump_nxs_dev(nxs_dev);
	}
}

static int init_nxs_vdev(struct nxs_v4l2_builder *builder,
			 struct nxs_video *nxs_video)
{
	int ret;
	struct media_pad *pad = &nxs_video->pad;
	struct media_entity *entity = &nxs_video->vdev.entity;

	pad->flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_init(entity, 1, pad, 0);
	if (ret) {
		dev_err(builder->dev, "failed to media_entity_init(name: %s)\n",
			nxs_video->name);
		return ret;
	}

	/* snprintf(nxs_video->vdev.name, sizeof(nxs_video->vdev.name), */
	/* 	 "%s", nxs_video->name); */

	mutex_init(&nxs_video->queue_lock);
	mutex_init(&nxs_video->stream_lock);

	nxs_video->vdev.fops		= &nxs_video_fops;
	nxs_video->vdev.ioctl_ops	= &nxs_video_ioctl_ops;
	nxs_video->vdev.v4l2_dev	= &builder->v4l2_dev;
	nxs_video->vdev.minor		= -1;
	nxs_video->vdev.vfl_type	= VFL_TYPE_GRABBER;
	nxs_video->vdev.release		= video_device_release;
	/* nxs_video->vdev.lock		= &nxs_video->queue_lock; */

	switch (nxs_video->type) {
	case NXS_VIDEO_TYPE_CAPTURE:
		nxs_video->vdev.vfl_dir = VFL_DIR_RX;
		break;
	case NXS_VIDEO_TYPE_RENDER:
		nxs_video->vdev.vfl_dir = VFL_DIR_TX;
		break;
	case NXS_VIDEO_TYPE_M2M:
		nxs_video->vdev.vfl_dir = VFL_DIR_M2M;
		break;
	}

	nxs_video->vb2_alloc_ctx	= builder->vb2_alloc_ctx;

	ret = video_register_device(&nxs_video->vdev, VFL_TYPE_GRABBER, -1);
	if (ret < 0) {
		dev_err(builder->dev,
			"failed to video_register_device()\n");
		return ret;
	}

	video_set_drvdata(&nxs_video->vdev, nxs_video);
	return 0;
}

static int init_nxs_subdev(struct nxs_v4l2_builder *builder,
			   struct nxs_video *nxs_video)
{
	int ret;
	struct v4l2_device *v4l2_dev = nxs_video->v4l2_dev;
	struct v4l2_subdev *sd = &nxs_video->subdev;
	struct video_device *vdev = &nxs_video->vdev;
	struct media_pad *pad = &nxs_video->pad;
	struct media_entity *entity = &sd->entity;

	v4l2_subdev_init(sd, &nxs_subdev_ops);
	snprintf(sd->name, sizeof(sd->name), "%s", nxs_video->name);
	v4l2_set_subdevdata(sd, nxs_video);
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	pad->flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_init(entity, 1, pad, 0);
	if (ret) {
		dev_err(builder->dev, "failed to media_entity_init(name: %s)\n",
			nxs_video->name);
		return ret;
	}

	ret = v4l2_device_register_subdev(v4l2_dev, sd);
	if (ret) {
		dev_err(builder->dev, "failed to v4l2_device_register_subdev(name: %s)\n",
			nxs_video->name);
		return ret;
	}

	video_set_drvdata(vdev, sd);
	strlcpy(vdev->name, sd->name, sizeof(vdev->name));
	vdev->v4l2_dev = v4l2_dev;
	vdev->fops = &v4l2_subdev_fops;
	vdev->ctrl_handler = sd->ctrl_handler;
	vdev->release = video_device_release;
	ret = __video_register_device(vdev, VFL_TYPE_SUBDEV, -1, 1, sd->owner);
	if (ret) {
		dev_err(builder->dev, "failed to __video_register_device(name: %s)\n",
			nxs_video->name);
		return ret;
	}

	nxs_video->subdev_ctx = kzalloc(sizeof(*nxs_video->subdev_ctx),
					GFP_KERNEL);
	if (WARN(!nxs_video->subdev_ctx, "failed to alloc subdev_ctx\n"))
		return -ENOMEM;

	return 0;
}

static bool need_camera_sensor(struct nxs_video *video)
{
	struct nxs_function_instance *inst = video->nxs_function;
	struct nxs_dev *nxs_dev;

	nxs_dev = list_first_entry(&inst->dev_list, struct nxs_dev, func_list);

	if (nxs_dev->dev_function == NXS_FUNCTION_VIP_CLIPPER ||
	    nxs_dev->dev_function == NXS_FUNCTION_VIP_DECIMATOR ||
	    nxs_dev->dev_function == NXS_FUNCTION_MIPI_CSI)
		return true;

	return false;
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
		if (ret)
			return ret;
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

static int capture_on(struct nxs_video *video)
{
	int ret;

	if (!video->capture) {
		dev_err(&video->vdev.dev,
			"%s: capture context is NULL\n", __func__);
		return -ENODEV;
	}
	ret = nxs_capture_power(video->capture, true);
	if (ret)
		return ret;

	ret = v4l2_subdev_call(video->capture->sensor, video, s_stream, 1);
	if (ret)
		dev_err(&video->vdev.dev,
			"%s: failed to s_stream of sensor\n", __func__);

	return ret;
}

static int capture_off(struct nxs_video *video)
{
	int ret;

	if (!video->capture) {
		dev_err(&video->vdev.dev,
			"%s: capture context is NULL\n", __func__);
		return -ENODEV;
	}

	ret = v4l2_subdev_call(video->capture->sensor, video, s_stream, 0);
	if (ret) {
		dev_err(&video->vdev.dev,
			"%s: failed to s_stream of sensor\n", __func__);
		return ret;
	}

	return nxs_capture_power(video->capture, false);
}

static int capture_sensor_notifier_bound(struct v4l2_async_notifier *async,
					 struct v4l2_subdev *sd,
					 struct v4l2_async_subdev *asd)
{
	struct nxs_video *video =
		container_of(async, struct nxs_video, notifier);

	dev_info(&video->vdev.dev, "%s: sd %p\n", __func__, sd);
	video->capture->sensor = sd;
	return 0;
}

static int capture_sensor_notifier_complete(struct v4l2_async_notifier *async)
{
	struct nxs_video *video =
		container_of(async, struct nxs_video, notifier);

	return v4l2_device_register_subdev(video->v4l2_dev,
					   video->capture->sensor);
}

static int bind_sensor(struct nxs_video *video)
{
	struct nxs_function_instance *inst = video->nxs_function;
	struct nxs_capture_ctx *capture = NULL;
	struct v4l2_async_notifier *notifier = &video->notifier;
	struct v4l2_async_subdev *asd = NULL;
	struct nxs_dev *nxs_dev;
	int ret;

	capture = kzalloc(sizeof(*capture), GFP_KERNEL);
	if (!capture) {
		ret = -ENOMEM;
		goto err_out;
	}

	notifier->num_subdevs = 1;
	notifier->subdevs = kzalloc(sizeof(*notifier->subdevs), GFP_KERNEL);
	if (!notifier->subdevs) {
		ret = -ENOMEM;
		goto err_out;
	}

	asd = kzalloc(sizeof(*asd), GFP_KERNEL);
	if (!asd) {
		ret = -ENOMEM;
		goto err_out;
	}
	*notifier->subdevs = asd;

	nxs_dev = list_first_entry(&inst->dev_list, struct nxs_dev, func_list);
	ret = nxs_capture_bind_sensor(&video->vdev.dev, nxs_dev, asd, capture);
	if (ret)
		goto err_out;

	video->capture = capture;

	notifier->bound = capture_sensor_notifier_bound;
	notifier->complete = capture_sensor_notifier_complete;

	return v4l2_async_notifier_register(video->v4l2_dev, notifier);

err_out:
	if (asd)
		kfree(asd);
	if (notifier->num_subdevs) {
		if (notifier->subdevs)
			kfree(notifier->subdevs);
	}
	if (capture) {
		free_action_seq(&capture->enable_seq);
		free_action_seq(&capture->disable_seq);
		kfree(capture);
	}

	return ret;
}

static struct nxs_video *build_nxs_video(struct nxs_v4l2_builder *builder,
					 const char *name,
					 struct nxs_function_instance *inst)
{
	int ret;
	struct nxs_video *nxs_video;

	nxs_video = kzalloc(sizeof(*nxs_video), GFP_KERNEL);
	if (!nxs_video) {
		WARN_ON(1);
		return ERR_PTR(-ENOMEM);
	}

	dev_info(builder->dev, "%s: builder %p, name %s, inst %p\n",
		 __func__, builder, name, inst);

	nxs_video->v4l2_dev = &builder->v4l2_dev;
	snprintf(nxs_video->name, sizeof(nxs_video->name), "%s", name);

	nxs_video->type = get_nxs_video_type(inst);
	if (nxs_video->type == NXS_VIDEO_TYPE_INVALID) {
		dev_err(builder->dev, "invalid type 0x%x\n", nxs_video->type);
		kfree(nxs_video);
		return NULL;
	}

	if (is_subdev(nxs_video))
		ret = init_nxs_subdev(builder, nxs_video);
	else
		ret = init_nxs_vdev(builder, nxs_video);

	if (ret < 0)
		goto free_nxs_video;

	nxs_video->nxs_function = inst;
	if (need_camera_sensor(nxs_video)) {
		ret = bind_sensor(nxs_video);
		if (ret)
			goto free_nxs_video;
	}
	dump_nxs_function_inst(inst);

	return nxs_video;

free_nxs_video:
	kfree(nxs_video);
	return ERR_PTR(ret);
}

static int cleanup_nxs_video(struct nxs_video *video)
{
	video_unregister_device(&video->vdev);
	if (video->capture) {
		if (video->capture->sensor)
			v4l2_device_unregister_subdev(video->capture->sensor);
		free_action_seq(&video->capture->enable_seq);
		free_action_seq(&video->capture->disable_seq);
		kfree(video->capture);
	}
	if (video->notifier.num_subdevs) {
		v4l2_async_notifier_unregister(&video->notifier);
		kfree(video->notifier.subdevs[0]);
		kfree(video->notifier.subdevs);
	}
	/* vb2_queue_release(video->vbq); */
	/* kfree(video->vbq); */
	mutex_destroy(&video->queue_lock);
	mutex_destroy(&video->stream_lock);
	/* below code is source of kobject_uevent_env kernel panic */
	/* kfree(video); */

	return 0;
}

static void free_function_instance(struct nxs_function_instance *inst)
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

	free_function_request(inst->req);

	kfree(inst);
}

static struct nxs_function_instance *
nxs_v4l2_build(struct nxs_function_builder *pthis,
	       const char *name,
	       struct nxs_function_request *req)
{
	struct nxs_function *func;
	struct nxs_dev *nxs_dev;
	struct nxs_function_instance *inst = NULL;
	struct nxs_v4l2_builder *me;
	struct nxs_video *nxs_video;

	me = pthis->priv;

	inst = kzalloc(sizeof(*inst), GFP_KERNEL);
	if (!inst) {
		WARN_ON(1);
		return NULL;
	}

	INIT_LIST_HEAD(&inst->dev_list);
	INIT_LIST_HEAD(&inst->dev_sibling_list);

	list_for_each_entry(func, &req->head, list) {
		if (func)
			dev_info(me->dev, "REQ: function %s, index %d, user %d\n",
				nxs_function_to_str(func->function),
				func->index, func->user);
		nxs_dev = get_nxs_dev(func->function, func->index, func->user,
				      func->multitap_follow);
		if (!nxs_dev) {
			dev_err(me->dev, "can't get nxs_dev for func %s, index 0x%x\n",
				nxs_function_to_str(func->function),
				func->index);
			goto error_out;
		}

		dev_info(me->dev, "GET: fuction %s, index %d, user %d, multitap_connected %d\n",
			 nxs_function_to_str(nxs_dev->dev_function),
			 nxs_dev->dev_inst_index,
			 nxs_dev->user,
			 nxs_dev->multitap_connected);

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
			dev_err(me->dev, "%s: failed to bottom dev(inst %d)\n",
				__func__, req->option.bottom_id);
			goto error_out;
		}
	}

	/* TODO: BLENDING_TO_OTHER, MULTI_PATH */

	nxs_video = build_nxs_video(me, name, inst);
	if (!nxs_video) {
		dev_err(me->dev, "failed to build nxs v4l2 driver\n");
		goto error_out;
	}

	inst->priv = nxs_video;

	mutex_lock(&me->lock);
	me->seq++;
	inst->id = me->seq;
	list_add_tail(&inst->sibling_list, &me->function_list);
	mutex_unlock(&me->lock);

	return inst;

error_out:
	if (inst)
		free_function_instance(inst);

	return NULL;
}

static int nxs_v4l2_free(struct nxs_function_builder *pthis,
			 struct nxs_function_instance *inst)
{
	struct nxs_v4l2_builder *me;
	struct nxs_video *nxs_video;
	struct nxs_function_instance *entry;
	int ret;

	me = pthis->priv;
	nxs_video = inst->priv;

	ret = cleanup_nxs_video(nxs_video);
	if (ret) {
		dev_err(me->dev,
			"%s: failed to cleanup_nxs_video\n", __func__);
		return ret;
	}

	mutex_lock(&me->lock);
	list_for_each_entry(entry, &me->function_list, sibling_list) {
		if (entry == inst) {
			list_del_init(&inst->sibling_list);
			break;
		}
	}
	mutex_unlock(&me->lock);

	free_function_instance(inst);

	return 0;
}

static struct nxs_function_instance *
nxs_v4l2_get(struct nxs_function_builder *pthis, int handle)
{
	struct nxs_v4l2_builder *me;
	struct nxs_function_instance *entry;
	bool found = false;

	me = pthis->priv;

	mutex_lock(&me->lock);
	list_for_each_entry(entry, &me->function_list, sibling_list) {
		if (entry->id == handle) {
			found = true;
			break;
		}
	}

	if (!found)
		entry = NULL;

	mutex_unlock(&me->lock);

	return entry;
}

static inline char *get_vdev_name_base(struct video_device *vdev)
{
	switch (vdev->vfl_type) {
	case VFL_TYPE_GRABBER:
		return "video";
	case VFL_TYPE_SUBDEV:
		return "v4l-subdev";
	default:
		BUG();
	}

	return NULL;
}

static inline int get_vdev_nr(struct video_device *vdev)
{
	return vdev->num;
}

static int nxs_v4l2_query(struct nxs_function_builder *pthis,
			  struct nxs_query_function *query)
{
	struct nxs_v4l2_builder *me;
	struct nxs_function_instance *inst;
	struct nxs_video *nxs_video;

	me = pthis->priv;
	inst = nxs_v4l2_get(pthis, query->handle);

	if (!inst) {
		dev_err(me->dev,
			"%s: can't find function instance for handle %d\n",
			__func__, query->handle);
		return -ENOENT;
	}

	nxs_video = (struct nxs_video *)inst->priv;

	switch (query->query) {
	case NXS_FUNCTION_QUERY_DEVINFO:
		sprintf(query->devinfo.dev_name, "%s%d",
			get_vdev_name_base(&nxs_video->vdev),
			get_vdev_nr(&nxs_video->vdev));
		break;
	default:
		dev_err(me->dev,
			"%s: unknown query 0x%x\n", __func__, query->query);
		return -EINVAL;
	}

	return 0;
}

static struct nxs_function_builder v4l2_builder = {
	.build = nxs_v4l2_build,
	.free  = nxs_v4l2_free,
	.get   = nxs_v4l2_get,
	.query = nxs_v4l2_query,
};

static int init_v4l2_context(struct nxs_v4l2_builder *builder)
{
	int ret;
	struct v4l2_device *v4l2_dev;

	builder->media_dev.dev = builder->dev;
	snprintf(builder->media_dev.model, sizeof(builder->media_dev.model),
		 "%s", dev_name(builder->dev));

	v4l2_dev	= &builder->v4l2_dev;
	v4l2_dev->mdev	= &builder->media_dev;
	snprintf(v4l2_dev->name, sizeof(v4l2_dev->name),
		 "%s", dev_name(builder->dev));

	builder->vb2_alloc_ctx = vb2_dma_contig_init_ctx(builder->dev);
	if (!builder->vb2_alloc_ctx) {
		WARN_ON(1);
		return -ENOMEM;
	}

	ret = v4l2_device_register(builder->dev, &builder->v4l2_dev);
	if (ret < 0) {
		dev_err(builder->dev, "failed to register nxs v4l2_device\n");
		goto cleanup_alloc_ctx;
	}

	ret = media_device_register(&builder->media_dev);
	if (ret < 0) {
		dev_err(builder->dev, "failed to register nxs media_device\n");
		goto unregister_v4l2;
	}

	dev_info(builder->dev, "%s success\n", __func__);

	return 0;

unregister_v4l2:
	v4l2_device_unregister(&builder->v4l2_dev);

cleanup_alloc_ctx:
	vb2_dma_contig_cleanup_ctx(&builder->vb2_alloc_ctx);

	return ret;
}

static int nxs_v4l2_builder_probe(struct platform_device *pdev)
{
	int ret;
	struct nxs_v4l2_builder *builder;

	builder = devm_kzalloc(&pdev->dev, sizeof(*builder), GFP_KERNEL);
	if (!builder) {
		WARN_ON(1);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&builder->function_list);
	mutex_init(&builder->lock);

	builder->builder = &v4l2_builder;
	v4l2_builder.priv = builder;
	builder->dev = &pdev->dev;

	ret = register_nxs_function_builder(builder->builder);
	if (ret) {
		dev_err(builder->dev,
			"failed to register_nxs_function_builder\n");
		return ret;
	}

	ret = init_v4l2_context(builder);
	if (ret) {
		unregister_nxs_function_builder(builder->builder);
		return ret;
	}

	platform_set_drvdata(pdev, builder);

	dev_info(builder->dev, "%s success\n", __func__);

	return 0;
}

static int nxs_v4l2_builder_remove(struct platform_device *pdev)
{
	struct nxs_v4l2_builder *builder;

	builder = platform_get_drvdata(pdev);
	/* TODO */
	/* returns all functions */
	unregister_nxs_function_builder(builder->builder);

	return 0;
}

static const struct of_device_id nxs_v4l2_builder_match[] = {
	{ .compatible = "nexell,nxs-v4l2-builder-1.0", },
	{},
};

static struct platform_driver nxs_v4l2_builder_driver = {
	.probe  = nxs_v4l2_builder_probe,
	.remove = nxs_v4l2_builder_remove,
	.driver = {
		.name = "nxs-v4l2-builer",
		.of_match_table = of_match_ptr(nxs_v4l2_builder_match),
	},
};

module_platform_driver(nxs_v4l2_builder_driver);

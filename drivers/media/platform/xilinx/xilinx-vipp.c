// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Video IP Composite Device
 *
 * Copyright (C) 2013-2015 Ideas on Board
 * Copyright (C) 2013-2015 Xilinx, Inc.
 *
 * Contacts: Hyun Kwon <hyun.kwon@xilinx.com>
 *           Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 */

#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/of_reserved_mem.h>

#include <media/v4l2-async.h>
#include <media/v4l2-common.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#include "xilinx-dma.h"
#include "xilinx-vipp.h"

#define XVIPP_DMA_S2MM				0
#define XVIPP_DMA_MM2S				1

/*
 * This is for backward compatibility for existing applications,
 * and planned to be deprecated
 */
static bool xvip_is_mplane = true;
MODULE_PARM_DESC(is_mplane,
		 "v4l2 device capability to handle multi planar formats");
module_param_named(is_mplane, xvip_is_mplane, bool, 0444);

/**
 * struct xvip_graph_entity - Entity in the video graph
 * @asd: subdev asynchronous registration information
 * @entity: media entity, from the corresponding V4L2 subdev
 * @subdev: V4L2 subdev
 * @streaming: status of the V4L2 subdev if streaming or not
 */
struct xvip_graph_entity {
	struct v4l2_async_connection asd; /* must be first */
	struct media_entity *entity;
	struct v4l2_subdev *subdev;
	bool streaming;
};

static inline struct xvip_graph_entity *
to_xvip_entity(struct v4l2_async_connection *asd)
{
	return container_of(asd, struct xvip_graph_entity, asd);
}

/* -----------------------------------------------------------------------------
 * Graph Management
 */

static struct xvip_graph_entity *
xvip_graph_find_entity(struct xvip_composite_device *xdev,
		       const struct fwnode_handle *fwnode)
{
	struct xvip_graph_entity *entity;
	struct v4l2_async_connection *asd;
	struct list_head *lists[] = {
		&xdev->notifier.done_list,
		&xdev->notifier.waiting_list
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(lists); i++) {
		list_for_each_entry(asd, lists[i], asc_entry) {
			entity = to_xvip_entity(asd);
			if (entity->asd.match.fwnode == fwnode)
				return entity;
		}
	}

	return NULL;
}

static struct xvip_graph_entity *
xvip_graph_find_entity_from_media(struct xvip_composite_device *xdev,
				  struct media_entity *entity)
{
	struct xvip_graph_entity *xvip_entity;
	struct v4l2_async_connection *asd;

	list_for_each_entry(asd, &xdev->notifier.waiting_list, asc_entry) {
		xvip_entity = to_xvip_entity(asd);
		if (xvip_entity->entity == entity)
			return xvip_entity;
	}

	return NULL;
}

static int xvip_graph_build_one(struct xvip_composite_device *xdev,
				struct xvip_graph_entity *entity)
{
	u32 link_flags = MEDIA_LNK_FL_ENABLED;
	struct media_entity *local = entity->entity;
	struct media_entity *remote;
	struct media_pad *local_pad;
	struct media_pad *remote_pad;
	struct xvip_graph_entity *ent;
	struct v4l2_fwnode_link link;
	struct fwnode_handle *ep = NULL;
	int ret = 0;

	dev_dbg(xdev->dev, "creating links for entity %s\n", local->name);

	while (1) {
		/* Get the next endpoint and parse its link. */
		ep = fwnode_graph_get_next_endpoint(entity->asd.match.fwnode,
						    ep);
		if (ep == NULL)
			break;

		dev_dbg(xdev->dev, "processing endpoint %p\n", ep);

		ret = v4l2_fwnode_parse_link(ep, &link);
		if (ret < 0) {
			dev_err(xdev->dev, "failed to parse link for %p\n",
				ep);
			continue;
		}

		/* Skip sink ports, they will be processed from the other end of
		 * the link.
		 */
		if (link.local_port >= local->num_pads) {
			dev_err(xdev->dev, "invalid port number %u for %p\n",
				link.local_port, link.local_node);
			v4l2_fwnode_put_link(&link);
			ret = -EINVAL;
			break;
		}

		local_pad = &local->pads[link.local_port];

		if (local_pad->flags & MEDIA_PAD_FL_SINK) {
			dev_dbg(xdev->dev, "skipping sink port %p:%u\n",
				link.local_node, link.local_port);
			v4l2_fwnode_put_link(&link);
			continue;
		}

		/* Skip DMA engines, they will be processed separately. */
		if (link.remote_node == of_fwnode_handle(xdev->dev->of_node)) {
			dev_dbg(xdev->dev, "skipping DMA port %p:%u\n",
				link.local_node, link.local_port);
			v4l2_fwnode_put_link(&link);
			continue;
		}

		/* Find the remote entity. */
		ent = xvip_graph_find_entity(xdev, link.remote_node);
		if (ent == NULL) {
			dev_err(xdev->dev, "no entity found for %p\n",
				link.remote_node);
			v4l2_fwnode_put_link(&link);
			ret = -ENODEV;
			break;
		}

		remote = ent->entity;

		if (link.remote_port >= remote->num_pads) {
			dev_err(xdev->dev, "invalid port number %u on %p\n",
				link.remote_port, link.remote_node);
			v4l2_fwnode_put_link(&link);
			ret = -EINVAL;
			break;
		}

		remote_pad = &remote->pads[link.remote_port];

		v4l2_fwnode_put_link(&link);

		/* Create the media link. */
		dev_dbg(xdev->dev, "creating %s:%u -> %s:%u link\n",
			local->name, local_pad->index,
			remote->name, remote_pad->index);

		ret = media_create_pad_link(local, local_pad->index,
					       remote, remote_pad->index,
					       link_flags);
		if (ret < 0) {
			dev_err(xdev->dev,
				"failed to create %s:%u -> %s:%u link\n",
				local->name, local_pad->index,
				remote->name, remote_pad->index);
			break;
		}
	}

	return ret;
}

static struct xvip_dma *
xvip_graph_find_dma(struct xvip_composite_device *xdev, unsigned int port)
{
	struct xvip_dma *dma;

	list_for_each_entry(dma, &xdev->dmas, list) {
		if (dma->port == port)
			return dma;
	}

	return NULL;
}

/**
 * xvip_graph_entity_set_streaming - Update the streaming status
 * @xdev: Composite video device
 * @entity: graph entity to update
 * @enable: enable/disable streaming status
 *
 * Update the streaming status of given entity.
 *
 * Return: previous streaming status (true or false)
 */
static bool xvip_graph_entity_set_streaming(struct xvip_composite_device *xdev,
					    struct xvip_graph_entity *entity,
					    bool enable)
{
	bool status = entity->streaming;

	entity->streaming = enable;
	return status;
}

static int
xvip_graph_entity_start_stop_subdev(struct xvip_composite_device *xdev,
				    struct xvip_graph_entity *entity, bool on)
{
	struct v4l2_subdev *subdev;
	int ret = 0;

	dev_dbg(xdev->dev, "%s entity %s\n",
		on ? "Starting" : "Stopping", entity->entity->name);
	subdev = media_entity_to_v4l2_subdev(entity->entity);

	/*
	 * start or stop the subdev only once in case if they are
	 * shared between sub-graphs
	 */
	if (on) {
		/* power-on subdevice */
		ret = v4l2_subdev_call(subdev, core, s_power, 1);
		if (ret < 0 && ret != -ENOIOCTLCMD) {
			dev_err(xdev->dev,
				"s_power on failed on subdev\n");
			xvip_graph_entity_set_streaming(xdev, entity, 0);
			return ret;
		}

		/* stream-on subdevice */
		ret = v4l2_subdev_call(subdev, video, s_stream, 1);
		if (ret < 0 && ret != -ENOIOCTLCMD) {
			dev_err(xdev->dev,
				"s_stream on failed on subdev\n");
			v4l2_subdev_call(subdev, core, s_power, 0);
			xvip_graph_entity_set_streaming(xdev, entity, 0);
		}
	} else {
		/* stream-off subdevice */
		ret = v4l2_subdev_call(subdev, video, s_stream, 0);
		if (ret < 0 && ret != -ENOIOCTLCMD) {
			dev_err(xdev->dev,
				"s_stream off failed on subdev\n");
			xvip_graph_entity_set_streaming(xdev, entity, 1);
		}

		/* power-off subdevice */
		ret = v4l2_subdev_call(subdev, core, s_power, 0);
		if (ret < 0 && ret != -ENOIOCTLCMD)
			dev_err(xdev->dev,
				"s_power off failed on subdev\n");
	}

	if (ret == -ENOIOCTLCMD)
		ret = 0;

	return ret;
}

/**
 * xvip_graph_entity_start_stop - start / stop the graph entity
 * @xdev: composite device
 * @entity: entity to check
 * @on: boolean flag. true for enable and false for disable
 *
 * Check if all immediate dependencies are ready dependeing on 'on' flag.
 * If enabling, check all source pads. Sink pads for disabling. Once all
 * dependencies are ready, set the streaming state on the entity. If the state
 * is already set, optimize it by skipping checks.
 *
 * Return: true if the state is successfully or already set. false otherwise.
 */
static bool xvip_graph_entity_start_stop(struct xvip_composite_device *xdev,
					 struct xvip_graph_entity *entity,
					 bool on)
{
	unsigned long pad_flag = on ? MEDIA_PAD_FL_SOURCE : MEDIA_PAD_FL_SINK;
	unsigned int i;
	bool state;
	int ret;

	if (entity->streaming == on)
		return true;

	for (i = 0; i < entity->entity->num_pads; i++) {
		struct xvip_graph_entity *remote;
		struct media_pad *pad;

		if (!(entity->entity->pads[i].flags & pad_flag))
			continue;

		/* skipping not connected pads */
		pad = media_pad_remote_pad_first(&entity->entity->pads[i]);
		if (!pad || !pad->entity)
			continue;

		/*
		 * Skip if there is no remote. This entity is at the end,
		 * such as DMA, sensor, or other type.
		 */
		remote = xvip_graph_find_entity_from_media(xdev, pad->entity);
		if (!remote)
			continue;

		/* the dependency state doesn't meet */
		if (remote->streaming != on) {
			state = xvip_graph_entity_start_stop(xdev, remote, on);
			if (!state)
				return state;
		}
	}

	/* set state and report if state is changed or not */
	state = xvip_graph_entity_set_streaming(xdev, entity, on);
	/* This shouldn't happen as check is already above */
	if (state == on) {
		WARN(1, "Should never get here\n");
		return true;
	}

	ret = xvip_graph_entity_start_stop_subdev(xdev, entity, on);
	if (ret < 0) {
		dev_err(xdev->dev, "ret = %d for entity %s\n",
			ret, entity->entity->name);
		return false;
	}

	return true;
}

/**
 * xvip_graph_pipeline_start_stop - start or stop the pipe in the graph
 * @xdev: composite device
 * @pipe: pipeline to start / stop
 * @on: boolean flag. true for enable and false for disable
 *
 * Enable or disable the pipe in the graph by iterating the asd list.
 * The pipe is a sub-graph, and the check ensures the given entity
 * is part of the pipe before doing start or stop. This function
 * or any subsequent functions don't and shouldn't change the asd list,
 * so that there's no race if the caller holds the pipeline lock.
 * xvip_graph_entity_start_stop() takes care of dependencies,
 * or state-checking.
 *
 * Return: 0 for success, otherwise error code
 */
int xvip_graph_pipeline_start_stop(struct xvip_composite_device *xdev,
				   struct xvip_pipeline *pipe, bool on)
{
	struct v4l2_async_connection *asd;

	list_for_each_entry(asd, &xdev->notifier.done_list, asc_entry) {
		struct xvip_graph_entity *entity;
		bool state;

		entity = to_xvip_entity(asd);
		/* skip an entity not belongng to the given pipe */
		if (&pipe->pipe != media_entity_pipeline(entity->entity))
			continue;

		state = xvip_graph_entity_start_stop(xdev, entity, on);
		if (!state)
			return -EPIPE;
	}

	return 0;
}

static int xvip_graph_build_dma(struct xvip_composite_device *xdev)
{
	u32 link_flags = MEDIA_LNK_FL_ENABLED;
	struct device_node *node = xdev->dev->of_node;
	struct media_entity *source;
	struct media_entity *sink;
	struct media_pad *source_pad;
	struct media_pad *sink_pad;
	struct xvip_graph_entity *ent;
	struct v4l2_fwnode_link link;
	struct device_node *ep;
	struct xvip_dma *dma;
	int ret = 0;

	dev_dbg(xdev->dev, "creating links for DMA engines\n");

	for_each_endpoint_of_node(node, ep) {
		dev_dbg(xdev->dev, "processing endpoint %pOF\n", ep);

		ret = v4l2_fwnode_parse_link(of_fwnode_handle(ep), &link);
		if (ret < 0) {
			dev_err(xdev->dev, "failed to parse link for %pOF\n",
				ep);
			continue;
		}

		/* Find the DMA engine. */
		dma = xvip_graph_find_dma(xdev, link.local_port);
		if (dma == NULL) {
			dev_err(xdev->dev, "no DMA engine found for port %u\n",
				link.local_port);
			v4l2_fwnode_put_link(&link);
			ret = -EINVAL;
			break;
		}

		dev_dbg(xdev->dev, "creating link for DMA engine %s\n",
			dma->video.name);

		/* Find the remote entity. */
		ent = xvip_graph_find_entity(xdev, link.remote_node);
		if (ent == NULL) {
			dev_err(xdev->dev, "no entity found for %pOF\n",
				to_of_node(link.remote_node));
			v4l2_fwnode_put_link(&link);
			ret = -ENODEV;
			break;
		}

		if (link.remote_port >= ent->entity->num_pads) {
			dev_err(xdev->dev, "invalid port number %u on %pOF\n",
				link.remote_port,
				to_of_node(link.remote_node));
			v4l2_fwnode_put_link(&link);
			ret = -EINVAL;
			break;
		}

		if (dma->pad.flags & MEDIA_PAD_FL_SOURCE) {
			source = &dma->video.entity;
			source_pad = &dma->pad;
			sink = ent->entity;
			sink_pad = &sink->pads[link.remote_port];
		} else {
			source = ent->entity;
			source_pad = &source->pads[link.remote_port];
			sink = &dma->video.entity;
			sink_pad = &dma->pad;
		}

		v4l2_fwnode_put_link(&link);

		/* Create the media link. */
		dev_dbg(xdev->dev, "creating %s:%u -> %s:%u link\n",
			source->name, source_pad->index,
			sink->name, sink_pad->index);

		ret = media_create_pad_link(source, source_pad->index,
					       sink, sink_pad->index,
					       link_flags);
		if (ret < 0) {
			dev_err(xdev->dev,
				"failed to create %s:%u -> %s:%u link\n",
				source->name, source_pad->index,
				sink->name, sink_pad->index);
			break;
		}
	}

	return ret;
}

static int xvip_graph_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct xvip_composite_device *xdev =
		container_of(notifier, struct xvip_composite_device, notifier);
	struct xvip_graph_entity *entity;
	struct v4l2_async_connection *asd;
	int ret;

	dev_dbg(xdev->dev, "notify complete, all subdevs registered\n");

	/* Create links for every entity. */
	list_for_each_entry(asd, &xdev->notifier.done_list, asc_entry) {
		entity = to_xvip_entity(asd);
		ret = xvip_graph_build_one(xdev, entity);
		if (ret < 0)
			return ret;
	}

	/* Create links for DMA channels. */
	ret = xvip_graph_build_dma(xdev);
	if (ret < 0)
		return ret;

	ret = v4l2_device_register_subdev_nodes(&xdev->v4l2_dev);
	if (ret < 0)
		dev_err(xdev->dev, "failed to register subdev nodes\n");

	return media_device_register(&xdev->media_dev);
}

static int xvip_graph_notify_bound(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *subdev,
				   struct v4l2_async_connection *asc)
{
	struct xvip_graph_entity *entity = to_xvip_entity(asc);

	entity->entity = &subdev->entity;
	entity->subdev = subdev;

	return 0;
}

static const struct v4l2_async_notifier_operations xvip_graph_notify_ops = {
	.bound = xvip_graph_notify_bound,
	.complete = xvip_graph_notify_complete,
};

static int xvip_graph_parse_one(struct xvip_composite_device *xdev,
				struct fwnode_handle *fwnode)
{
	struct fwnode_handle *remote;
	struct fwnode_handle *ep = NULL;
	int ret = 0;

	dev_dbg(xdev->dev, "parsing node %p\n", fwnode);

	while (1) {
		struct xvip_graph_entity *xge;

		ep = fwnode_graph_get_next_endpoint(fwnode, ep);
		if (ep == NULL)
			break;

		dev_dbg(xdev->dev, "handling endpoint %p\n", ep);

		remote = fwnode_graph_get_remote_port_parent(ep);
		if (remote == NULL) {
			ret = -EINVAL;
			goto err_notifier_cleanup;
		}

		fwnode_handle_put(ep);

		/* Skip entities that we have already processed. */
		if (remote == of_fwnode_handle(xdev->dev->of_node) ||
		    xvip_graph_find_entity(xdev, remote)) {
			fwnode_handle_put(remote);
			continue;
		}

		xge = v4l2_async_nf_add_fwnode(&xdev->notifier, remote,
					       struct xvip_graph_entity);
		fwnode_handle_put(remote);
		if (IS_ERR(xge)) {
			ret = PTR_ERR(xge);
			goto err_notifier_cleanup;
		}
	}

	return 0;

err_notifier_cleanup:
	v4l2_async_nf_cleanup(&xdev->notifier);
	fwnode_handle_put(ep);
	return ret;
}

static int xvip_graph_parse(struct xvip_composite_device *xdev)
{
	struct xvip_graph_entity *entity;
	struct v4l2_async_connection *asd;
	int ret;

	/*
	 * Walk the links to parse the full graph. Start by parsing the
	 * composite node and then parse entities in turn. The list_for_each
	 * loop will handle entities added at the end of the list while walking
	 * the links.
	 */
	ret = xvip_graph_parse_one(xdev, of_fwnode_handle(xdev->dev->of_node));
	if (ret < 0)
		return 0;

	list_for_each_entry(asd, &xdev->notifier.waiting_list, asc_entry) {
		entity = to_xvip_entity(asd);
		ret = xvip_graph_parse_one(xdev, entity->asd.match.fwnode);
		if (ret < 0) {
			v4l2_async_nf_cleanup(&xdev->notifier);
			break;
		}
	}

	return ret;
}

static int xvip_graph_dma_init_one(struct xvip_composite_device *xdev,
				   struct device_node *node)
{
	struct xvip_dma *dma;
	enum v4l2_buf_type type;
	const char *direction;
	unsigned int index;
	int ret;

	ret = of_property_read_string(node, "direction", &direction);
	if (ret < 0)
		return ret;

	if (strcmp(direction, "input") == 0)
		type = xvip_is_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
						V4L2_BUF_TYPE_VIDEO_CAPTURE;
	else if (strcmp(direction, "output") == 0)
		type = xvip_is_mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE :
					V4L2_BUF_TYPE_VIDEO_OUTPUT;
	else
		return -EINVAL;

	of_property_read_u32(node, "reg", &index);

	dma = devm_kzalloc(xdev->dev, sizeof(*dma), GFP_KERNEL);
	if (dma == NULL)
		return -ENOMEM;

	ret = xvip_dma_init(xdev, dma, type, index);
	if (ret < 0) {
		dev_err(xdev->dev, "%pOF initialization failed\n", node);
		return ret;
	}

	list_add_tail(&dma->list, &xdev->dmas);

	if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		xdev->v4l2_caps |= V4L2_CAP_VIDEO_CAPTURE_MPLANE;
	else if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE)
		xdev->v4l2_caps |= V4L2_CAP_VIDEO_CAPTURE;
	else if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
		xdev->v4l2_caps |= V4L2_CAP_VIDEO_OUTPUT;
	else if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		xdev->v4l2_caps |= V4L2_CAP_VIDEO_OUTPUT_MPLANE;

	return 0;
}

static int xvip_graph_dma_init(struct xvip_composite_device *xdev)
{
	struct device_node *ports;
	struct device_node *port;
	int ret = 0;

	ports = of_get_child_by_name(xdev->dev->of_node, "ports");
	if (ports == NULL) {
		dev_err(xdev->dev, "ports node not present\n");
		return -EINVAL;
	}

	for_each_child_of_node(ports, port) {
		ret = xvip_graph_dma_init_one(xdev, port);
		if (ret) {
			of_node_put(port);
			break;
		}
	}

	of_node_put(ports);
	return ret;
}

static void xvip_graph_cleanup(struct xvip_composite_device *xdev)
{
	struct xvip_dma *dmap;
	struct xvip_dma *dma;

	v4l2_async_nf_unregister(&xdev->notifier);
	v4l2_async_nf_cleanup(&xdev->notifier);

	list_for_each_entry_safe(dma, dmap, &xdev->dmas, list) {
		xvip_dma_cleanup(dma);
		list_del(&dma->list);
	}
}

static int xvip_graph_init(struct xvip_composite_device *xdev)
{
	int ret;

	/* Init the DMA channels. */
	ret = xvip_graph_dma_init(xdev);
	if (ret < 0) {
		dev_err(xdev->dev, "DMA initialization failed\n");
		goto done;
	}

	v4l2_async_nf_init(&xdev->notifier, &xdev->v4l2_dev);

	/* Parse the graph to extract a list of subdevice DT nodes. */
	ret = xvip_graph_parse(xdev);
	if (ret < 0) {
		dev_err(xdev->dev, "graph parsing failed\n");
		goto done;
	}

	if (list_empty(&xdev->notifier.waiting_list)) {
		dev_err(xdev->dev, "no subdev found in graph\n");
		ret = -ENOENT;
		goto done;
	}

	/* Register the subdevices notifier. */
	xdev->notifier.ops = &xvip_graph_notify_ops;

	ret = v4l2_async_nf_register(&xdev->notifier);
	if (ret < 0) {
		dev_err(xdev->dev, "notifier registration failed\n");
		goto done;
	}

	ret = 0;

done:
	if (ret < 0)
		xvip_graph_cleanup(xdev);

	return ret;
}

/* -----------------------------------------------------------------------------
 * Media Controller and V4L2
 */

static void xvip_composite_v4l2_cleanup(struct xvip_composite_device *xdev)
{
	v4l2_device_unregister(&xdev->v4l2_dev);
	media_device_unregister(&xdev->media_dev);
	media_device_cleanup(&xdev->media_dev);
}

static int xvip_composite_v4l2_init(struct xvip_composite_device *xdev)
{
	int ret;

	xdev->media_dev.dev = xdev->dev;
	strscpy(xdev->media_dev.model, "Xilinx Video Composite Device",
		sizeof(xdev->media_dev.model));
	xdev->media_dev.hw_revision = 0;

	media_device_init(&xdev->media_dev);

	xdev->v4l2_dev.mdev = &xdev->media_dev;
	ret = v4l2_device_register(xdev->dev, &xdev->v4l2_dev);
	if (ret < 0) {
		dev_err(xdev->dev, "V4L2 device registration failed (%d)\n",
			ret);
		media_device_cleanup(&xdev->media_dev);
		return ret;
	}

	return 0;
}

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static int xvip_composite_probe(struct platform_device *pdev)
{
	struct xvip_composite_device *xdev;
	int ret;

	xdev = devm_kzalloc(&pdev->dev, sizeof(*xdev), GFP_KERNEL);
	if (!xdev)
		return -ENOMEM;

	xdev->dev = &pdev->dev;
	mutex_init(&xdev->lock);
	INIT_LIST_HEAD(&xdev->dmas);

	ret = xvip_composite_v4l2_init(xdev);
	if (ret < 0)
		return ret;

	ret = xvip_graph_init(xdev);
	if (ret < 0)
		goto error;

	xdev->atomic_streamon = of_property_read_bool(xdev->dev->of_node, "xlnx,atomic_streamon");

	ret = of_reserved_mem_device_init(&pdev->dev);
	if (ret)
		dev_dbg(&pdev->dev, "of_reserved_mem_device_init: %d\n", ret);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_err(&pdev->dev, "dma_set_mask_and_coherent: %d\n", ret);
		goto error;
	}

	platform_set_drvdata(pdev, xdev);

	dev_info(xdev->dev, "device registered\n");

	return 0;

error:
	xvip_composite_v4l2_cleanup(xdev);
	return ret;
}

static void xvip_composite_remove(struct platform_device *pdev)
{
	struct xvip_composite_device *xdev = platform_get_drvdata(pdev);

	mutex_destroy(&xdev->lock);
	xvip_graph_cleanup(xdev);
	xvip_composite_v4l2_cleanup(xdev);
}

static const struct of_device_id xvip_composite_of_id_table[] = {
	{ .compatible = "xlnx,video" },
	{ }
};
MODULE_DEVICE_TABLE(of, xvip_composite_of_id_table);

static struct platform_driver xvip_composite_driver = {
	.driver = {
		.name = "xilinx-video",
		.of_match_table = xvip_composite_of_id_table,
	},
	.probe = xvip_composite_probe,
	.remove_new = xvip_composite_remove,
};

module_platform_driver(xvip_composite_driver);

MODULE_AUTHOR("Laurent Pinchart <laurent.pinchart@ideasonboard.com>");
MODULE_DESCRIPTION("Xilinx Video IP Composite Driver");
MODULE_LICENSE("GPL v2");

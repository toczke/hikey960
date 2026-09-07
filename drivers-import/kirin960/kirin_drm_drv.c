#include <drm/clients/drm_client_setup.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_print.h>
#include <drm/drm_drv.h>
#include <drm/drm_vblank.h>
/*
 * Hisilicon Kirin SoCs drm master driver
 *
 * Copyright (c) 2016 Linaro Limited.
 * Copyright (c) 2014-2016 Hisilicon Limited.
 *
 * Author:
 *	<cailiwei@hisilicon.com>
 *	<zhengwanchun@hisilicon.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/of_platform.h>
#include <linux/component.h>
#include <linux/of_graph.h>

#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>

#include "kirin_drm_drv.h"


#ifdef CONFIG_DRM_FBDEV_EMULATION
static bool fbdev = true;
MODULE_PARM_DESC(fbdev, "Enable fbdev compat layer");
module_param(fbdev, bool, 0600);
#endif


static struct kirin_dc_ops *dc_ops;

static int kirin_drm_kms_cleanup(struct drm_device *dev)
{
	struct kirin_drm_private *priv = dev->dev_private;

	if (priv->fbdev) {
		priv->fbdev = NULL;
	}

	dc_ops->cleanup(dev);
	drm_mode_config_cleanup(dev);
	devm_kfree(dev->dev, priv);
	dev->dev_private = NULL;

	return 0;
}

static const struct drm_mode_config_funcs kirin_drm_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static void kirin_drm_mode_config_init(struct drm_device *dev)
{
	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;

	dev->mode_config.max_width = 2048;
	dev->mode_config.max_height = 2048;

	dev->mode_config.funcs = &kirin_drm_mode_config_funcs;
}

static int kirin_drm_kms_init(struct drm_device *dev)
{
	struct kirin_drm_private *priv;
	int ret;

	priv = devm_kzalloc(dev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	dev->dev_private = priv;
	dev_set_drvdata(dev->dev, dev);

	/* dev->mode_config initialization */
	drm_mode_config_init(dev);
	kirin_drm_mode_config_init(dev);

	/* display controller init */
	ret = dc_ops->init(dev);
	if (ret)
		goto err_mode_config_cleanup;

	/* bind and init sub drivers */
	ret = component_bind_all(dev->dev, dev);
	if (ret) {
		DRM_ERROR("failed to bind all component.\n");
		goto err_dc_cleanup;
	}

	/* vblank init */
	ret = drm_vblank_init(dev, dev->mode_config.num_crtc);
	if (ret) {
		DRM_ERROR("failed to initialize vblank.\n");
		goto err_unbind_all;
	}
	/* with irq_enabled = true, we can use the vblank feature. */

	/* reset all the states of crtc/plane/encoder/connector */
	drm_mode_config_reset(dev);

	//if (fbdev)
	//	priv->fbdev = kirin_drm_fbdev_init(dev);

	/* init kms poll for handling hpd */
	drm_kms_helper_poll_init(dev);

	/* force detection after connectors init */
	(void)drm_helper_hpd_irq_event(dev);

	return 0;

err_unbind_all:
	component_unbind_all(dev->dev, dev);
err_dc_cleanup:
	dc_ops->cleanup(dev);
err_mode_config_cleanup:
	drm_mode_config_cleanup(dev);
	devm_kfree(dev->dev, priv);
	dev->dev_private = NULL;

	return ret;
}

static const struct file_operations kirin_drm_fops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.release	= drm_release,
	.unlocked_ioctl	= drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= drm_compat_ioctl,
#endif
	.poll		= drm_poll,
	.read		= drm_read,
	.llseek		= noop_llseek,
	.mmap		= drm_gem_mmap,
	.fop_flags	= FOP_UNSIGNED_OFFSET,
};

static struct drm_driver kirin_drm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops				= &kirin_drm_fops,

	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,

	.name			= "kirin",
	.desc			= "Hisilicon Kirin SoCs' DRM Driver",
	.major			= 1,
	.minor			= 0,
};

/* NOTE: the CONFIG_OF case duplicates the same code as exynos or imx
 * (or probably any other).. so probably some room for some helpers
 */
static int compare_of(struct device *dev, void *data)
{
	return dev->of_node == data;
}

static int kirin_drm_bind(struct device *dev)
{
	struct drm_driver *driver = &kirin_drm_driver;
	struct drm_device *drm_dev;
	int ret;

	//drm_platform_init(&kirin_drm_driver, to_platform_device(dev));

	drm_dev = drm_dev_alloc(driver, dev);
	if (!drm_dev)
		return -ENOMEM;


	ret = kirin_drm_kms_init(drm_dev);
	if (ret)
		goto err_drm_dev_put;

	ret = drm_dev_register(drm_dev, 0);
	if (ret)
		goto err_kms_cleanup;

	DRM_INFO("Initialized %s %d.%d.%d on minor %d\n",
		 driver->name, driver->major, driver->minor, driver->patchlevel,
		 drm_dev->primary->index);
	
	drm_client_setup(drm_dev, NULL);
	dsi_set_output_client(drm_dev);

	return 0;

err_drm_dev_unregister:
	drm_dev_unregister(drm_dev);
err_kms_cleanup:
	kirin_drm_kms_cleanup(drm_dev);
err_drm_dev_put:
	drm_dev_put(drm_dev);

	return ret;
}

static void kirin_drm_unbind(struct device *dev)
{
	drm_put_dev(dev_get_drvdata(dev));
}

static const struct component_master_ops kirin_drm_ops = {
	.bind = kirin_drm_bind,
	.unbind = kirin_drm_unbind,
};

static struct device_node *kirin_get_remote_node(struct device_node *np)
{
	struct device_node *endpoint, *remote;

	/* get the first endpoint, in our case only one remote node
	 * is connected to display controller.
	 */
	endpoint = of_graph_get_next_endpoint(np, NULL);
	if (!endpoint) {
		DRM_ERROR("no valid endpoint node\n");
		return ERR_PTR(-ENODEV);
	}
	of_node_put(endpoint);

	remote = of_graph_get_remote_port_parent(endpoint);
	if (!remote) {
		DRM_ERROR("no valid remote node\n");
		return ERR_PTR(-ENODEV);
	}
	of_node_put(remote);

	if (!of_device_is_available(remote)) {
		DRM_ERROR("not available for remote node\n");
		return ERR_PTR(-ENODEV);
	}

	return remote;
}

static int kirin_drm_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct component_match *match = NULL;
	struct device_node *remote;

	dc_ops = (struct kirin_dc_ops *)of_device_get_match_data(dev);
	if (!dc_ops) {
		DRM_ERROR("failed to get dt id data\n");
		return -EINVAL;
	}

	remote = kirin_get_remote_node(np);
	if (IS_ERR(remote))
		return PTR_ERR(remote);

	component_match_add(dev, &match, compare_of, remote);

	return component_master_add_with_match(dev, &kirin_drm_ops, match);

	return 0;
}

static void kirin_drm_platform_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &kirin_drm_ops);
	dc_ops = NULL;
}

static const struct of_device_id kirin_drm_dt_ids[] = {
	{ .compatible = "hisilicon,hi3660-dpe",
	  .data = &dss_dc_ops,
	},
	{ /* end node */ },
};
MODULE_DEVICE_TABLE(of, kirin_drm_dt_ids);

static struct platform_driver kirin_drm_platform_driver = {
	.probe = kirin_drm_platform_probe,
	.remove = kirin_drm_platform_remove,
	.driver = {
		.name = "kirin960-drm",
		.of_match_table = kirin_drm_dt_ids,
	},
};

module_platform_driver(kirin_drm_platform_driver);

MODULE_AUTHOR("cailiwei <cailiwei@hisilicon.com>");
MODULE_AUTHOR("zhengwanchun <zhengwanchun@hisilicon.com>");
MODULE_DESCRIPTION("hisilicon Kirin SoCs' DRM master driver");
MODULE_LICENSE("GPL v2");

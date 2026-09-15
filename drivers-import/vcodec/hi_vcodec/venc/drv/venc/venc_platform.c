/*
 * venc_platform.c - Kirin 960 Hardware Video Encoder (VENC) Platform Driver & Char Device
 * Modern Linux 7.x replacement for legacy drv_venc_intf.S
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/compat.h>

#include "hi_type.h"
#include "hi_drv_venc.h"
#include "hi_drv_mem.h"
#include "drv_venc.h"
#include "drv_omxvenc.h"
#include "drv_venc_ioctl.h"
#include "venc_regulator.h"

#define VENC_DEV_NAME "hi_venc"

struct device *g_venc_dev = NULL;
static dev_t g_venc_devno;
static struct cdev g_venc_cdev;
static struct class *g_venc_class = NULL;
static atomic_t g_venc_open_count = ATOMIC_INIT(0);
static DEFINE_MUTEX(g_venc_ioctl_mutex);

extern OPTM_VENC_CHN_S g_stVencChn[VENC_MAX_CHN_NUM];

static int venc_open(struct inode *inode, struct file *file)
{
	int ret = 0;

	mutex_lock(&g_venc_ioctl_mutex);
	if (atomic_inc_return(&g_venc_open_count) == 1) {
		ret = Venc_Regulator_Enable();
		if (ret == HI_SUCCESS) {
			VENC_DRV_BoardInit();
			pr_info("[VENC] First open: board and hardware power initialized\n");
		} else {
			pr_err("[VENC] Venc_Regulator_Enable failed during open\n");
			atomic_dec(&g_venc_open_count);
			mutex_unlock(&g_venc_ioctl_mutex);
			return -EIO;
		}
	}
	mutex_unlock(&g_venc_ioctl_mutex);

	file->private_data = NULL;
	return 0;
}

static int venc_release(struct inode *inode, struct file *file)
{
	int i;

	mutex_lock(&g_venc_ioctl_mutex);

	/* Clean up any active channels created by this file instance */
	for (i = 0; i < VENC_MAX_CHN_NUM; i++) {
		if (g_stVencChn[i].pWhichFile == file) {
			pr_info("[VENC] Auto-destroying orphaned channel %d (handle=0x%llx) on file close\n",
				i, (unsigned long long)g_stVencChn[i].hVEncHandle);
			VENC_DRV_StopReceivePic(g_stVencChn[i].hVEncHandle);
			VENC_DRV_DestroyChn(g_stVencChn[i].hVEncHandle);
			g_stVencChn[i].pWhichFile = NULL;
		}
	}

	if (atomic_dec_and_test(&g_venc_open_count)) {
		VENC_DRV_BoardDeinit();
		Venc_Regulator_Disable(HI_TRUE);
		pr_info("[VENC] Last close: board and hardware power powered down\n");
	}

	mutex_unlock(&g_venc_ioctl_mutex);
	return 0;
}

static long venc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret = 0;

	switch (cmd) {
	case CMD_VENC_CREATE_CHN: {
		VENC_INFO_CREATE_S stCreate;
		if (copy_from_user(&stCreate, (void __user *)arg, sizeof(stCreate)))
			return -EFAULT;

		mutex_lock(&g_venc_ioctl_mutex);
		ret = VENC_DRV_CreateChn(&stCreate.hVencChn, &stCreate.stAttr, &stCreate.stVeInfo, file);
		if (ret == HI_SUCCESS) {
			int ch = stCreate.hVencChn & 0x7;
			if (ch < VENC_MAX_CHN_NUM)
				g_stVencChn[ch].pWhichFile = file;
		}
		mutex_unlock(&g_venc_ioctl_mutex);

		if (ret == HI_SUCCESS) {
			if (copy_to_user((void __user *)arg, &stCreate, sizeof(stCreate)))
				return -EFAULT;
		}
		break;
	}

	case CMD_VENC_DESTROY_CHN: {
		VENC_INFO_CREATE_S stDestroy;
		HI_HANDLE handle;

		if (copy_from_user(&stDestroy, (void __user *)arg, sizeof(stDestroy)))
			return -EFAULT;
		handle = stDestroy.hVencChn;

		mutex_lock(&g_venc_ioctl_mutex);
		ret = VENC_DRV_DestroyChn(handle);
		if (ret == HI_SUCCESS) {
			int ch = handle & 0x7;
			if (ch < VENC_MAX_CHN_NUM && g_stVencChn[ch].pWhichFile == file)
				g_stVencChn[ch].pWhichFile = NULL;
		}
		mutex_unlock(&g_venc_ioctl_mutex);
		break;
	}

	case CMD_VENC_SET_CHN_ATTR: {
		VENC_INFO_CREATE_S stAttr;
		if (copy_from_user(&stAttr, (void __user *)arg, sizeof(stAttr)))
			return -EFAULT;

		mutex_lock(&g_venc_ioctl_mutex);
		ret = VENC_DRV_SetAttr(stAttr.hVencChn, &stAttr.stAttr, &stAttr.stVeInfo);
		mutex_unlock(&g_venc_ioctl_mutex);
		break;
	}

	case CMD_VENC_GET_CHN_ATTR: {
		VENC_INFO_CREATE_S stAttr;
		if (copy_from_user(&stAttr, (void __user *)arg, sizeof(stAttr)))
			return -EFAULT;

		mutex_lock(&g_venc_ioctl_mutex);
		ret = VENC_DRV_GetAttr(stAttr.hVencChn, &stAttr.stAttr);
		mutex_unlock(&g_venc_ioctl_mutex);

		if (ret == HI_SUCCESS) {
			if (copy_to_user((void __user *)arg, &stAttr, sizeof(stAttr)))
				return -EFAULT;
		}
		break;
	}

	case CMD_VENC_START_RECV_PIC: {
		HI_HANDLE handle;
		if (copy_from_user(&handle, (void __user *)arg, sizeof(handle)))
			return -EFAULT;

		mutex_lock(&g_venc_ioctl_mutex);
		ret = VENC_DRV_StartReceivePic(handle);
		mutex_unlock(&g_venc_ioctl_mutex);
		break;
	}

	case CMD_VENC_STOP_RECV_PIC: {
		HI_HANDLE handle;
		if (copy_from_user(&handle, (void __user *)arg, sizeof(handle)))
			return -EFAULT;

		mutex_lock(&g_venc_ioctl_mutex);
		ret = VENC_DRV_StopReceivePic(handle);
		mutex_unlock(&g_venc_ioctl_mutex);
		break;
	}

	case CMD_VENC_REQUEST_I_FRAME: {
		HI_HANDLE handle;
		if (copy_from_user(&handle, (void __user *)arg, sizeof(handle)))
			return -EFAULT;

		mutex_lock(&g_venc_ioctl_mutex);
		ret = VENC_DRV_RequestIFrame(handle);
		mutex_unlock(&g_venc_ioctl_mutex);
		break;
	}

	case CMD_VENC_QUEUE_FRAME: {
		VENC_INFO_QUEUE_FRAME_S qframe;
		if (copy_from_user(&qframe, (void __user *)arg, sizeof(qframe)))
			return -EFAULT;

		ret = VENC_DRV_QueueFrame_OMX(qframe.hVencChn, &qframe.stVencFrame_OMX);
		if (ret == HI_SUCCESS) {
			if (copy_to_user((void __user *)arg, &qframe, sizeof(qframe)))
				return -EFAULT;
		}
		break;
	}

	case CMD_VENC_QUEUE_STREAM: {
		VENC_INFO_QUEUE_FRAME_S qstream;
		if (copy_from_user(&qstream, (void __user *)arg, sizeof(qstream)))
			return -EFAULT;

		ret = VENC_DRV_QueueStream_OMX(qstream.hVencChn, &qstream.stVencFrame_OMX);
		if (ret == HI_SUCCESS) {
			if (copy_to_user((void __user *)arg, &qstream, sizeof(qstream)))
				return -EFAULT;
		}
		break;
	}

	case CMD_VENC_GET_MSG: {
		VENC_INFO_GET_MSG_S getmsg;
		if (copy_from_user(&getmsg, (void __user *)arg, sizeof(getmsg)))
			return -EFAULT;

		ret = VENC_DRV_GetMessage_OMX(getmsg.hVencChn, &getmsg.msg_info_omx);
		if (ret == HI_SUCCESS) {
			if (copy_to_user((void __user *)arg, &getmsg, sizeof(getmsg)))
				return -EFAULT;
		}
		break;
	}

	case CMD_VENC_FLUSH_PORT: {
		VENC_INFO_FLUSH_PORT_S flush;
		if (copy_from_user(&flush, (void __user *)arg, sizeof(flush)))
			return -EFAULT;

		ret = VENC_DRV_FlushPort_OMX(flush.hVencChn, flush.u32PortIndex);
		break;
	}

	case CMD_VENC_KEN_MAP:
	case CMD_VENC_MMZ_MAP: {
		VENC_INFO_MAP_S stMap;
		venc_user_buf ubuf;

		if (copy_from_user(&stMap, (void __user *)arg, sizeof(stMap)))
			return -EFAULT;
		if (!stMap.VencMapBuffer)
			return -EINVAL;
		if (copy_from_user(&ubuf, stMap.VencMapBuffer, sizeof(ubuf)))
			return -EFAULT;

		ret = VENC_DRV_Map(&ubuf);
		if (ret == HI_SUCCESS) {
			if (copy_to_user(stMap.VencMapBuffer, &ubuf, sizeof(ubuf)))
				return -EFAULT;
		}
		break;
	}

	case CMD_VENC_KEN_UMMAP:
	case CMD_VENC_MMZ_UMMAP: {
		VENC_INFO_MAP_S stMap;
		venc_user_buf ubuf;

		if (copy_from_user(&stMap, (void __user *)arg, sizeof(stMap)))
			return -EFAULT;
		if (!stMap.VencMapBuffer)
			return -EINVAL;
		if (copy_from_user(&ubuf, stMap.VencMapBuffer, sizeof(ubuf)))
			return -EFAULT;

		ret = VENC_DRV_Umap(&ubuf);
		if (copy_to_user(stMap.VencMapBuffer, &ubuf, sizeof(ubuf)))
			return -EFAULT;
		break;
	}

	case CMD_VENC_MMZ_ALLOC: {
		VENC_INFO_VENC_MMZ_PHY_S mmz;
		MEM_BUFFER_S sMBuf;

		if (copy_from_user(&mmz, (void __user *)arg, sizeof(mmz)))
			return -EFAULT;

		memset(&sMBuf, 0, sizeof(sMBuf));
		sMBuf.u32Size = mmz.bufsize;
		ret = DRV_MMU_MEM_AllocAndMap("venc_mmz", "venc", mmz.bufsize, 4096, &sMBuf, 0);
		if (ret == HI_SUCCESS) {
			mmz.phyaddr = sMBuf.u32StartPhyAddr;
			if (copy_to_user((void __user *)arg, &mmz, sizeof(mmz)))
				return -EFAULT;
		}
		break;
	}

	default:
		pr_warn("[VENC] Unknown ioctl cmd: 0x%x\n", cmd);
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long venc_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return venc_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations venc_fops = {
	.owner          = THIS_MODULE,
	.open           = venc_open,
	.release        = venc_release,
	.unlocked_ioctl = venc_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = venc_compat_ioctl,
#endif
};

static int venc_platform_probe(struct platform_device *pdev)
{
	int ret;

	if (!pdev || !pdev->dev.of_node) {
		pr_info("[VENC] venc_platform_probe: skipping non-DT platform device\n");
		return -ENODEV;
	}

	g_venc_dev = &pdev->dev;

	/* Initialize memory manager */
	ret = DRV_MEM_INIT();
	if (ret != HI_SUCCESS) {
		pr_err("[VENC] DRV_MEM_INIT failed\n");
		return -ENOMEM;
	}

	/* Initialize regulator, clocks, and DTS hardware config */
	ret = Venc_Regulator_Init(pdev);
	if (ret != HI_SUCCESS) {
		pr_err("[VENC] Venc_Regulator_Init failed\n");
		DRV_MEM_EXIT();
		return -ENODEV;
	}

	/* Register character device /dev/hi_venc */
	ret = alloc_chrdev_region(&g_venc_devno, 0, 1, VENC_DEV_NAME);
	if (ret < 0) {
		pr_err("[VENC] alloc_chrdev_region failed: %d\n", ret);
		goto err_reg;
	}

	cdev_init(&g_venc_cdev, &venc_fops);
	g_venc_cdev.owner = THIS_MODULE;
	ret = cdev_add(&g_venc_cdev, g_venc_devno, 1);
	if (ret < 0) {
		pr_err("[VENC] cdev_add failed: %d\n", ret);
		goto err_chrdev;
	}

	/* class_create in modern Linux takes 1 argument */
	g_venc_class = class_create(VENC_DEV_NAME);
	if (IS_ERR(g_venc_class)) {
		ret = PTR_ERR(g_venc_class);
		pr_err("[VENC] class_create failed: %d\n", ret);
		goto err_cdev;
	}

	device_create(g_venc_class, &pdev->dev, g_venc_devno, NULL, VENC_DEV_NAME);

	platform_set_drvdata(pdev, g_venc_dev);

	pr_info("[VENC] Kirin 960 Hardware Video Encoder (VENC) probed successfully! /dev/%s created (major=%d)\n",
		VENC_DEV_NAME, MAJOR(g_venc_devno));

	return 0;

err_cdev:
	cdev_del(&g_venc_cdev);
err_chrdev:
	unregister_chrdev_region(g_venc_devno, 1);
err_reg:
	Venc_Regulator_Deinit(pdev);
	DRV_MEM_EXIT();
	g_venc_dev = NULL;
	return ret;
}

static void venc_platform_remove(struct platform_device *pdev)
{
	device_destroy(g_venc_class, g_venc_devno);
	class_destroy(g_venc_class);
	cdev_del(&g_venc_cdev);
	unregister_chrdev_region(g_venc_devno, 1);
	Venc_Regulator_Deinit(pdev);
	DRV_MEM_EXIT();
	g_venc_dev = NULL;
	pr_info("[VENC] Kirin 960 Hardware Video Encoder (VENC) removed\n");
}

static const struct of_device_id hisi_venc_match[] = {
	{ .compatible = "hisi,kirin960-venc" },
	{ .compatible = "hisilicon,hi3660-venc" },
	{ },
};
MODULE_DEVICE_TABLE(of, hisi_venc_match);

static struct platform_driver venc_platform_driver = {
	.probe  = venc_platform_probe,
	.remove = venc_platform_remove,
	.driver = {
		.name           = VENC_DEV_NAME,
		.owner          = THIS_MODULE,
		.of_match_table = hisi_venc_match,
	},
};

static int __init venc_driver_init(void)
{
	return platform_driver_register(&venc_platform_driver);
}

static void __exit venc_driver_exit(void)
{
	platform_driver_unregister(&venc_platform_driver);
}

module_init(venc_driver_init);
module_exit(venc_driver_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HiKey960 Kirin 960 Hardware Video Encoder (VENC) Driver");
MODULE_AUTHOR("Huawei / HiSilicon / Modernized for Linux 7.x");

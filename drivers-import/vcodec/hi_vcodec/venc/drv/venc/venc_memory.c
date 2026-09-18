/*
 * venc_memory.c - Kirin 960 Hardware Video Encoder (VENC) Memory Manager
 * Modern Linux 7.x replacement for legacy hi_drv_mem.S (FLATMEM and ION removal)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/timekeeping.h>
#include <linux/ktime.h>
#include <linux/fs.h>
#include <linux/semaphore.h>
#include <linux/stdarg.h>

#include "hi_type.h"
#include "hi_drv_venc.h"
#include "hi_drv_mem.h"
#include "drv_venc_ioctl.h"

#define MAX_VENC_MEM_NODE 200
#define MAX_VENC_NAME_LEN 16

/* Matches exact 56-byte layout expected by drv_venc_proc.S */
typedef struct {
	HI_CHAR node_name[MAX_VENC_NAME_LEN];  /* 0..15 */
	HI_CHAR zone_name[MAX_VENC_NAME_LEN];  /* 16..31 */
	HI_VOID *virt_addr;                    /* 32..39 */
	HI_U32  phys_addr;                     /* 40..43 */
	HI_U32  size;                          /* 44..47 */
	void   *priv;                          /* 48..55 */
} VENC_MEM_NODE_S;

/* Global variables referenced by assembly objects */
VENC_MEM_NODE_S gVencMemNode[MAX_VENC_MEM_NODE];
HI_U32 gVencNodeNum = 0;
HI_U32 g_VencPrintEnable = 0x1f;
HI_CHAR g_VencPrintMsg[1024];
char *pszMsg = g_VencPrintMsg;
DDR_MEM_ALLOC DdrMem;
struct semaphore g_VencMemSem;
void *g_ion_client = NULL;

extern struct device *g_venc_dev;

static DEFINE_MUTEX(g_venc_mem_mutex);

/* DMA-BUF mapping tracking for user buffer import */
#define MAX_DMABUF_MAPS 64
struct venc_dmabuf_entry {
	int fd;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct iosys_map map;
	dma_addr_t dma_addr;
	void *vaddr;
	bool is_active;
};

static struct venc_dmabuf_entry g_dmabuf_maps[MAX_DMABUF_MAPS];
static DEFINE_MUTEX(g_dmabuf_mutex);

/* Debug logging implementation */
HI_VOID HI_PRINT(HI_U32 type, char *file, int line, char *function, HI_CHAR *msg, ...)
{
	va_list args;
	char buf[512];

	if (!(g_VencPrintEnable & (1 << type)) && type != VENC_ALW)
		return;

	va_start(args, msg);
	vsnprintf(buf, sizeof(buf), msg, args);
	va_end(args);

	if (type == VENC_FATAL || type == VENC_ERR)
		printk(KERN_ERR "[VENC] %s:%d %s(): %s", file, line, function, buf);
	else if (type == VENC_WARN)
		printk(KERN_WARNING "[VENC] %s:%d %s(): %s", file, line, function, buf);
	else
		printk(KERN_INFO "[VENC] %s:%d %s(): %s", file, line, function, buf);
}
EXPORT_SYMBOL(HI_PRINT);

HI_U32 HI_GetTS(HI_VOID)
{
	return (HI_U32)ktime_to_ms(ktime_get());
}
EXPORT_SYMBOL(HI_GetTS);

HI_S32 DRV_Venc_GetTimeStampMs(HI_U32 *pu32TimeMs)
{
	if (!pu32TimeMs)
		return HI_FAILURE;
	*pu32TimeMs = (HI_U32)ktime_to_ms(ktime_get());
	return HI_SUCCESS;
}
EXPORT_SYMBOL(DRV_Venc_GetTimeStampMs);

/* Legacy do_gettimeofday shim */
void do_gettimeofday(void *tv);
void do_gettimeofday(void *tv)
{
	struct timespec64 ts;
	long *p = (long *)tv;

	ktime_get_real_ts64(&ts);
	if (p) {
		p[0] = (long)ts.tv_sec;
		p[1] = (long)(ts.tv_nsec / 1000);
	}
}
EXPORT_SYMBOL(do_gettimeofday);

HI_S32 DRV_MEM_INIT(HI_VOID)
{
	mutex_lock(&g_venc_mem_mutex);
	memset(gVencMemNode, 0, sizeof(gVencMemNode));
	gVencNodeNum = 0;
	sema_init(&g_VencMemSem, 1);
	mutex_unlock(&g_venc_mem_mutex);

	mutex_lock(&g_dmabuf_mutex);
	memset(g_dmabuf_maps, 0, sizeof(g_dmabuf_maps));
	mutex_unlock(&g_dmabuf_mutex);

	return HI_SUCCESS;
}
EXPORT_SYMBOL(DRV_MEM_INIT);

HI_S32 DRV_MEM_EXIT(HI_VOID)
{
	int i;
	struct device *dev = g_venc_dev;

	mutex_lock(&g_venc_mem_mutex);
	for (i = 0; i < MAX_VENC_MEM_NODE; i++) {
		if (gVencMemNode[i].virt_addr) {
			if (dev) {
				dma_free_coherent(dev, gVencMemNode[i].size,
						  gVencMemNode[i].virt_addr,
						  (dma_addr_t)gVencMemNode[i].phys_addr);
			} else {
				kfree(gVencMemNode[i].virt_addr);
			}
			gVencMemNode[i].virt_addr = NULL;
			gVencMemNode[i].phys_addr = 0;
			gVencMemNode[i].size = 0;
		}
	}
	gVencNodeNum = 0;
	mutex_unlock(&g_venc_mem_mutex);

	return HI_SUCCESS;
}
EXPORT_SYMBOL(DRV_MEM_EXIT);

HI_S32 DRV_MMU_MEM_AllocAndMap(const HI_CHAR *bufname, HI_CHAR *zone_name,
			       HI_U32 size, HI_S32 align,
			       MEM_BUFFER_S *psMBuf, HI_U32 mmu_bypass_flag)
{
	int i;
	dma_addr_t dma_addr = 0;
	void *virt_addr = NULL;
	struct device *dev = g_venc_dev;

	if (!psMBuf || size == 0)
		return HI_FAILURE;

	if (align > 0)
		size = ALIGN_UP(size, align);

	mutex_lock(&g_venc_mem_mutex);
	for (i = 0; i < MAX_VENC_MEM_NODE; i++) {
		if (gVencMemNode[i].virt_addr == NULL && gVencMemNode[i].size == 0)
			break;
	}
	if (i >= MAX_VENC_MEM_NODE) {
		mutex_unlock(&g_venc_mem_mutex);
		pr_err("[VENC] DRV_MMU_MEM_AllocAndMap: out of mem nodes\n");
		return HI_FAILURE;
	}

	if (dev) {
		virt_addr = dma_alloc_coherent(dev, size, &dma_addr, GFP_KERNEL);
	}
	if (!virt_addr) {
		virt_addr = kzalloc(size, GFP_KERNEL | GFP_DMA);
		if (virt_addr)
			dma_addr = (dma_addr_t)virt_to_phys(virt_addr);
	}

	if (!virt_addr) {
		mutex_unlock(&g_venc_mem_mutex);
		pr_err("[VENC] DRV_MMU_MEM_AllocAndMap: failed to allocate %u bytes\n", size);
		return HI_FAILURE;
	}

	/* Zero buffer to prevent uninitialized memory leakage */
	memset(virt_addr, 0, size);

	if (bufname)
		strscpy(gVencMemNode[i].node_name, bufname, sizeof(gVencMemNode[i].node_name));
	if (zone_name)
		strscpy(gVencMemNode[i].zone_name, zone_name, sizeof(gVencMemNode[i].zone_name));

	gVencMemNode[i].virt_addr = virt_addr;
	gVencMemNode[i].phys_addr = (HI_U32)dma_addr;
	gVencMemNode[i].size = size;
	gVencNodeNum++;

	psMBuf->pStartVirAddr = virt_addr;
	psMBuf->u32StartPhyAddr = (HI_U32)dma_addr;
	psMBuf->u32Size = size;
	mutex_unlock(&g_venc_mem_mutex);

	return HI_SUCCESS;
}
EXPORT_SYMBOL(DRV_MMU_MEM_AllocAndMap);

HI_S32 DRV_MMU_MEM_UnmapAndRelease(MEM_BUFFER_S *psMBuf, HI_U32 mmu_bypass_flag)
{
	int i;
	struct device *dev = g_venc_dev;
	MEM_BUFFER_S local_buf;

	if (!psMBuf || !psMBuf->pStartVirAddr)
		return HI_FAILURE;

	/* Safe stack copy to prevent any teardown UAF */
	local_buf = *psMBuf;

	mutex_lock(&g_venc_mem_mutex);
	for (i = 0; i < MAX_VENC_MEM_NODE; i++) {
		if (gVencMemNode[i].virt_addr == local_buf.pStartVirAddr ||
		    (local_buf.u32StartPhyAddr && gVencMemNode[i].phys_addr == local_buf.u32StartPhyAddr)) {
			if (dev) {
				dma_free_coherent(dev, gVencMemNode[i].size,
						  gVencMemNode[i].virt_addr,
						  (dma_addr_t)gVencMemNode[i].phys_addr);
			} else {
				kfree(gVencMemNode[i].virt_addr);
			}
			memset(&gVencMemNode[i], 0, sizeof(VENC_MEM_NODE_S));
			if (gVencNodeNum > 0)
				gVencNodeNum--;
			break;
		}
	}
	mutex_unlock(&g_venc_mem_mutex);

	psMBuf->pStartVirAddr = NULL;
	psMBuf->u32StartPhyAddr = 0;
	psMBuf->u32Size = 0;

	return HI_SUCCESS;
}
EXPORT_SYMBOL(DRV_MMU_MEM_UnmapAndRelease);

HI_S32 DRV_MEM_KAlloc(const HI_CHAR* bufName, const HI_CHAR *zone_name, MEM_BUFFER_S *psMBuf)
{
	if (!psMBuf)
		return HI_FAILURE;
	return DRV_MMU_MEM_AllocAndMap(bufName, (HI_CHAR*)zone_name, psMBuf->u32Size, 0, psMBuf, 0);
}
EXPORT_SYMBOL(DRV_MEM_KAlloc);

HI_S32 DRV_MEM_KFree(const MEM_BUFFER_S *psMBuf)
{
	if (!psMBuf)
		return HI_FAILURE;
	return DRV_MMU_MEM_UnmapAndRelease((MEM_BUFFER_S *)psMBuf, 0);
}
EXPORT_SYMBOL(DRV_MEM_KFree);

HI_S32 DRV_MMU_MapKernel(venc_user_buf* pstFrameBuf)
{
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct iosys_map map;
	int ret, i;
	struct device *dev = g_venc_dev;

	if (!pstFrameBuf)
		return HI_FAILURE;

	/* If userspace provided a dma-buf file descriptor */
	if (pstFrameBuf->pmem_fd >= 0) {
		if (!dev) {
			pr_err("[VENC] DRV_MMU_MapKernel: device not initialized\n");
			return HI_FAILURE;
		}

		dmabuf = dma_buf_get(pstFrameBuf->pmem_fd);
		if (IS_ERR(dmabuf)) {
			pr_err("[VENC] DRV_MMU_MapKernel: dma_buf_get failed fd=%d\n", pstFrameBuf->pmem_fd);
			return HI_FAILURE;
		}

		attach = dma_buf_attach(dmabuf, dev);
		if (IS_ERR(attach)) {
			dma_buf_put(dmabuf);
			pr_err("[VENC] DRV_MMU_MapKernel: dma_buf_attach failed\n");
			return HI_FAILURE;
		}

		sgt = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
		if (IS_ERR(sgt)) {
			dma_buf_detach(dmabuf, attach);
			dma_buf_put(dmabuf);
			pr_err("[VENC] DRV_MMU_MapKernel: map_attachment failed\n");
			return HI_FAILURE;
		}

		ret = dma_buf_vmap_unlocked(dmabuf, &map);
		if (ret) {
			dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_BIDIRECTIONAL);
			dma_buf_detach(dmabuf, attach);
			dma_buf_put(dmabuf);
			pr_err("[VENC] DRV_MMU_MapKernel: vmap failed: %d\n", ret);
			return HI_FAILURE;
		}

		mutex_lock(&g_dmabuf_mutex);
		for (i = 0; i < MAX_DMABUF_MAPS; i++) {
			if (!g_dmabuf_maps[i].is_active)
				break;
		}
		if (i >= MAX_DMABUF_MAPS) {
			mutex_unlock(&g_dmabuf_mutex);
			dma_buf_vunmap_unlocked(dmabuf, &map);
			dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_BIDIRECTIONAL);
			dma_buf_detach(dmabuf, attach);
			dma_buf_put(dmabuf);
			pr_err("[VENC] DRV_MMU_MapKernel: out of dmabuf tracking slots\n");
			return HI_FAILURE;
		}

		g_dmabuf_maps[i].fd = pstFrameBuf->pmem_fd;
		g_dmabuf_maps[i].dmabuf = dmabuf;
		g_dmabuf_maps[i].attach = attach;
		g_dmabuf_maps[i].sgt = sgt;
		g_dmabuf_maps[i].map = map;
		g_dmabuf_maps[i].dma_addr = sg_dma_address(sgt->sgl);
		g_dmabuf_maps[i].vaddr = map.vaddr;
		g_dmabuf_maps[i].is_active = true;
		mutex_unlock(&g_dmabuf_mutex);

		pstFrameBuf->kernelbufferaddr = (HI_U64)(uintptr_t)map.vaddr;
		pstFrameBuf->bufferaddr_Phy = (HI_U64)g_dmabuf_maps[i].dma_addr;
		return HI_SUCCESS;
	}

	/* If buffer already has physical address mapped */
	if (pstFrameBuf->bufferaddr_Phy != 0) {
		return HI_SUCCESS;
	}

	return HI_FAILURE;
}
EXPORT_SYMBOL(DRV_MMU_MapKernel);

HI_S32 DRV_MMU_UmapKernel(venc_user_buf* pstFrameBuf)
{
	int i;

	if (!pstFrameBuf)
		return HI_FAILURE;

	if (pstFrameBuf->pmem_fd >= 0) {
		mutex_lock(&g_dmabuf_mutex);
		for (i = 0; i < MAX_DMABUF_MAPS; i++) {
			if (g_dmabuf_maps[i].is_active && g_dmabuf_maps[i].fd == pstFrameBuf->pmem_fd) {
				dma_buf_vunmap_unlocked(g_dmabuf_maps[i].dmabuf, &g_dmabuf_maps[i].map);
				dma_buf_unmap_attachment_unlocked(g_dmabuf_maps[i].attach,
								  g_dmabuf_maps[i].sgt,
								  DMA_BIDIRECTIONAL);
				dma_buf_detach(g_dmabuf_maps[i].dmabuf, g_dmabuf_maps[i].attach);
				dma_buf_put(g_dmabuf_maps[i].dmabuf);
				memset(&g_dmabuf_maps[i], 0, sizeof(struct venc_dmabuf_entry));
				break;
			}
		}
		mutex_unlock(&g_dmabuf_mutex);
	}

	pstFrameBuf->kernelbufferaddr = 0;
	return HI_SUCCESS;
}
EXPORT_SYMBOL(DRV_MMU_UmapKernel);

HI_S32 DRV_MEM_CheckBuffer(venc_user_buf* pstFrameBuf, HI_BOOL cmdMapOrUnmap)
{
	if (!pstFrameBuf)
		return HI_FAILURE;

	if (cmdMapOrUnmap)
		return DRV_MMU_MapKernel(pstFrameBuf);
	else
		return DRV_MMU_UmapKernel(pstFrameBuf);
}
EXPORT_SYMBOL(DRV_MEM_CheckBuffer);

HI_S32 HI_DRV_UserCopy(struct file *file,
		       HI_U32 cmd, unsigned long arg,
		       long (*func)(struct file *file,
				   HI_U32 cmd, unsigned long uarg))
{
	if (!func)
		return -EINVAL;
	return func(file, cmd, arg);
}
EXPORT_SYMBOL(HI_DRV_UserCopy);

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/mutex.h>
#include <linux/io.h>

#include "omxvdec.h"
#include "platform.h"
#include "memory.h"
#include "vfmw.h"

#define MAX_MEM_NODE 1024
#define MAX_NAME_LEN 32

typedef struct {
    HI_U32   phys_addr;
    HI_VOID *virt_addr;
    HI_U32   size;
    HI_CHAR  name[MAX_NAME_LEN];
    struct dma_buf *dmabuf;
    struct dma_buf_attachment *attach;
    struct sg_table *sgt;
    HI_BOOL  is_coherent;
} VDEC_MEM_NODE_S;

static VDEC_MEM_NODE_S g_MemNodes[MAX_MEM_NODE];
static DEFINE_MUTEX(g_MemMutex);
extern OMXVDEC_ENTRY *g_pOmxVdec;

HI_S32 VDEC_MEM_Probe(HI_VOID)
{
    return HI_SUCCESS;
}

HI_S32 VDEC_MEM_Init(HI_VOID)
{
    mutex_lock(&g_MemMutex);
    memset(g_MemNodes, 0, sizeof(g_MemNodes));
    mutex_unlock(&g_MemMutex);
    return HI_SUCCESS;
}

HI_S32 VDEC_MEM_Exit(HI_VOID)
{
    int i;
    struct device *dev = g_pOmxVdec ? g_pOmxVdec->device : NULL;

    mutex_lock(&g_MemMutex);
    for (i = 0; i < MAX_MEM_NODE; i++) {
        if (g_MemNodes[i].virt_addr) {
            if (g_MemNodes[i].is_coherent && dev) {
                dma_free_coherent(dev, g_MemNodes[i].size,
                                  g_MemNodes[i].virt_addr,
                                  (dma_addr_t)g_MemNodes[i].phys_addr);
            } else if (!g_MemNodes[i].dmabuf) {
                kfree(g_MemNodes[i].virt_addr);
            }
            g_MemNodes[i].virt_addr = NULL;
            g_MemNodes[i].phys_addr = 0;
        }
    }
    mutex_unlock(&g_MemMutex);
    return HI_SUCCESS;
}

HI_S32 VDEC_MEM_AllocAndMap(const char *bufname, char *zone_name, MEM_BUFFER_S *psMBuf)
{
    int i;
    dma_addr_t dma_addr = 0;
    void *virt_addr = NULL;
    struct device *dev = g_pOmxVdec ? g_pOmxVdec->device : NULL;

    if (!psMBuf || psMBuf->u32Size == 0)
        return HI_FAILURE;

    mutex_lock(&g_MemMutex);
    for (i = 0; i < MAX_MEM_NODE; i++) {
        if (g_MemNodes[i].virt_addr == NULL && g_MemNodes[i].phys_addr == 0)
            break;
    }
    if (i >= MAX_MEM_NODE) {
        mutex_unlock(&g_MemMutex);
        pr_err("VDEC_MEM_AllocAndMap: out of mem nodes\n");
        return HI_FAILURE;
    }

    if (dev) {
        virt_addr = dma_alloc_coherent(dev, psMBuf->u32Size, &dma_addr, GFP_KERNEL);
    }
    if (!virt_addr) {
        virt_addr = kzalloc(psMBuf->u32Size, GFP_KERNEL | GFP_DMA);
        if (virt_addr)
            dma_addr = (dma_addr_t)virt_to_phys(virt_addr);
    }

    if (!virt_addr) {
        mutex_unlock(&g_MemMutex);
        pr_err("VDEC_MEM_AllocAndMap: failed to allocate %u bytes\n", psMBuf->u32Size);
        return HI_FAILURE;
    }

    g_MemNodes[i].virt_addr = virt_addr;
    g_MemNodes[i].phys_addr = (HI_U32)dma_addr;
    g_MemNodes[i].size = psMBuf->u32Size;
    g_MemNodes[i].is_coherent = (dev != NULL);
    if (bufname)
        strscpy(g_MemNodes[i].name, bufname, sizeof(g_MemNodes[i].name));

    psMBuf->pStartVirAddr = virt_addr;
    psMBuf->u32StartPhyAddr = (HI_U32)dma_addr;
    mutex_unlock(&g_MemMutex);

    return HI_SUCCESS;
}

HI_S32 VDEC_MEM_UnmapAndRelease(MEM_BUFFER_S *psMBuf)
{
    int i;
    struct device *dev = g_pOmxVdec ? g_pOmxVdec->device : NULL;

    if (!psMBuf || !psMBuf->pStartVirAddr)
        return HI_FAILURE;

    mutex_lock(&g_MemMutex);
    for (i = 0; i < MAX_MEM_NODE; i++) {
        if (g_MemNodes[i].virt_addr == psMBuf->pStartVirAddr ||
            (psMBuf->u32StartPhyAddr && g_MemNodes[i].phys_addr == psMBuf->u32StartPhyAddr)) {
            if (g_MemNodes[i].is_coherent && dev) {
                dma_free_coherent(dev, g_MemNodes[i].size,
                                  g_MemNodes[i].virt_addr,
                                  (dma_addr_t)g_MemNodes[i].phys_addr);
            } else {
                kfree(g_MemNodes[i].virt_addr);
            }
            memset(&g_MemNodes[i], 0, sizeof(VDEC_MEM_NODE_S));
            break;
        }
    }
    mutex_unlock(&g_MemMutex);

    psMBuf->pStartVirAddr = NULL;
    psMBuf->u32StartPhyAddr = 0;
    return HI_SUCCESS;
}

HI_S32 VDEC_MEM_MapKernel(HI_S32 share_fd, MEM_BUFFER_S *psMBuf)
{
    struct dma_buf *dmabuf;
    struct dma_buf_attachment *attach;
    struct sg_table *sgt;
    struct iosys_map map;
    int ret, i;
    struct device *dev = g_pOmxVdec ? g_pOmxVdec->device : NULL;

    if (!psMBuf || share_fd < 0 || !dev)
        return HI_FAILURE;

    dmabuf = dma_buf_get(share_fd);
    if (IS_ERR(dmabuf))
        return HI_FAILURE;

    attach = dma_buf_attach(dmabuf, dev);
    if (IS_ERR(attach)) {
        dma_buf_put(dmabuf);
        return HI_FAILURE;
    }

    sgt = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
    if (IS_ERR(sgt)) {
        dma_buf_detach(dmabuf, attach);
        dma_buf_put(dmabuf);
        return HI_FAILURE;
    }

    ret = dma_buf_vmap_unlocked(dmabuf, &map);
    if (ret) {
        dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_BIDIRECTIONAL);
        dma_buf_detach(dmabuf, attach);
        dma_buf_put(dmabuf);
        return HI_FAILURE;
    }

    mutex_lock(&g_MemMutex);
    for (i = 0; i < MAX_MEM_NODE; i++) {
        if (!g_MemNodes[i].virt_addr && !g_MemNodes[i].phys_addr)
            break;
    }
    if (i < MAX_MEM_NODE) {
        g_MemNodes[i].virt_addr = map.vaddr;
        g_MemNodes[i].phys_addr = (HI_U32)sg_dma_address(sgt->sgl);
        g_MemNodes[i].size = psMBuf->u32Size;
        g_MemNodes[i].dmabuf = dmabuf;
        g_MemNodes[i].attach = attach;
        g_MemNodes[i].sgt = sgt;
    }
    mutex_unlock(&g_MemMutex);

    psMBuf->pStartVirAddr = map.vaddr;
    psMBuf->u32StartPhyAddr = (HI_U32)sg_dma_address(sgt->sgl);
    return HI_SUCCESS;
}

HI_S32 VDEC_MEM_UnmapKernel(MEM_BUFFER_S *psMBuf)
{
    int i;
    struct iosys_map map;

    if (!psMBuf || !psMBuf->pStartVirAddr)
        return HI_FAILURE;

    mutex_lock(&g_MemMutex);
    for (i = 0; i < MAX_MEM_NODE; i++) {
        if (g_MemNodes[i].virt_addr == psMBuf->pStartVirAddr) {
            if (g_MemNodes[i].dmabuf) {
                map.vaddr = g_MemNodes[i].virt_addr;
                map.is_iomem = false;
                dma_buf_vunmap_unlocked(g_MemNodes[i].dmabuf, &map);
                dma_buf_unmap_attachment_unlocked(g_MemNodes[i].attach, g_MemNodes[i].sgt, DMA_BIDIRECTIONAL);
                dma_buf_detach(g_MemNodes[i].dmabuf, g_MemNodes[i].attach);
                dma_buf_put(g_MemNodes[i].dmabuf);
            }
            memset(&g_MemNodes[i], 0, sizeof(VDEC_MEM_NODE_S));
            break;
        }
    }
    mutex_unlock(&g_MemMutex);

    psMBuf->pStartVirAddr = NULL;
    psMBuf->u32StartPhyAddr = 0;
    return HI_SUCCESS;
}

HI_S32 VDEC_MEM_AllocAndShare(const HI_CHAR *bufname, HI_CHAR *zone_name, MEM_BUFFER_S *psMBuf, HI_S32 *pShareFd)
{
    HI_S32 ret = VDEC_MEM_AllocAndMap(bufname, zone_name, psMBuf);
    if (pShareFd)
        *pShareFd = -1;
    return ret;
}

HI_S32 VDEC_MEM_CloseAndFree(MEM_BUFFER_S *psMBuf, HI_S32 *pShareFd)
{
    if (pShareFd && *pShareFd >= 0)
        *pShareFd = -1;
    return VDEC_MEM_UnmapAndRelease(psMBuf);
}

HI_S32 VDEC_MEM_GetVirAddr_FromPhyAddr(HI_U8 **ppVirAddr, HI_U32 PhyAddr, HI_U32 Size)
{
    int i;
    if (!ppVirAddr || !PhyAddr)
        return HI_FAILURE;

    mutex_lock(&g_MemMutex);
    for (i = 0; i < MAX_MEM_NODE; i++) {
        if (g_MemNodes[i].phys_addr &&
            PhyAddr >= g_MemNodes[i].phys_addr &&
            PhyAddr < g_MemNodes[i].phys_addr + g_MemNodes[i].size) {
            *ppVirAddr = (HI_U8 *)g_MemNodes[i].virt_addr + (PhyAddr - g_MemNodes[i].phys_addr);
            mutex_unlock(&g_MemMutex);
            return HI_SUCCESS;
        }
    }
    mutex_unlock(&g_MemMutex);
    return HI_FAILURE;
}

HI_S32 VDEC_MEM_KAlloc(const HI_CHAR *bufname, HI_CHAR *zone_name, MEM_BUFFER_S *psMBuf, HI_U8 NeedZero)
{
    return VDEC_MEM_AllocAndMap(bufname, zone_name, psMBuf);
}

HI_S32 VDEC_MEM_KFree(MEM_BUFFER_S *psMBuf)
{
    return VDEC_MEM_UnmapAndRelease(psMBuf);
}

HI_VOID VDEC_MEM_Read_Proc(HI_VOID *p, HI_VOID *v)
{
    int i, count = 0;
    mutex_lock(&g_MemMutex);
    for (i = 0; i < MAX_MEM_NODE; i++) {
        if (g_MemNodes[i].virt_addr)
            count++;
    }
    mutex_unlock(&g_MemMutex);
}

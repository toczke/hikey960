#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/time.h>
#include <linux/time64.h>
#include <linux/ktime.h>
#include <linux/dma-buf.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/fdtable.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/vmalloc.h>

void *__ioremap(phys_addr_t offset, size_t size, pgprot_t prot)
{
	return ioremap(offset, size);
}
EXPORT_SYMBOL(__ioremap);

void __iounmap(volatile void __iomem *addr)
{
	iounmap(addr);
}
EXPORT_SYMBOL(__iounmap);

#undef vmalloc
void *vmalloc(unsigned long size)
{
	return __vmalloc_noprof(size, GFP_KERNEL);
}
EXPORT_SYMBOL(vmalloc);

/* Provide __stack_chk_guard for precompiled assembly objects that lack per-task canary */
#ifndef __stack_chk_guard
unsigned long __stack_chk_guard = 0xdeadbeef12345678UL;
EXPORT_SYMBOL(__stack_chk_guard);
#endif

/* Compatibility for do_gettimeofday (removed in 64-bit Linux) */
struct compat_timeval {
	long tv_sec;
	long tv_usec;
};

void do_gettimeofday(void *tv)
{
	struct timespec64 ts;
	struct compat_timeval *ctv = (struct compat_timeval *)tv;
	ktime_get_real_ts64(&ts);
	ctv->tv_sec = ts.tv_sec;
	ctv->tv_usec = ts.tv_nsec / 1000;
}
EXPORT_SYMBOL(do_gettimeofday);

/* Procfs PDE_DATA compatibility */
void *PDE_DATA(const struct inode *inode)
{
	return pde_data(inode);
}
EXPORT_SYMBOL(PDE_DATA);

/* Syscall sys_close compatibility */
int sys_close(unsigned int fd)
{
	return close_fd(fd);
}
EXPORT_SYMBOL(sys_close);

/* Flatmem compatibility symbols */
struct page *mem_map = NULL;
EXPORT_SYMBOL(mem_map);

unsigned long phystart_addr = 0;
EXPORT_SYMBOL(phystart_addr);

/* Stub/compatibility implementations for Android ION allocator APIs */
void *hisi_ion_client_create(const char *name)
{
	return NULL;
}
EXPORT_SYMBOL(hisi_ion_client_create);

void ion_client_destroy(void *client)
{
}
EXPORT_SYMBOL(ion_client_destroy);

void ion_free(void *client, void *handle)
{
}
EXPORT_SYMBOL(ion_free);

void *ion_alloc(void *client, size_t len, size_t align, unsigned int heap_id_mask, unsigned int flags)
{
	return NULL;
}
EXPORT_SYMBOL(ion_alloc);

void *ion_import_dma_buf_fd(void *client, int fd)
{
	return NULL;
}
EXPORT_SYMBOL(ion_import_dma_buf_fd);

int ion_share_dma_buf_fd(void *client, void *handle)
{
	return -EINVAL;
}
EXPORT_SYMBOL(ion_share_dma_buf_fd);

void *ion_map_kernel(void *client, void *handle)
{
	return NULL;
}
EXPORT_SYMBOL(ion_map_kernel);

void ion_unmap_kernel(void *client, void *handle)
{
}
EXPORT_SYMBOL(ion_unmap_kernel);

int ion_map_iommu(void *dev, void *client, void *handle, unsigned long *iova, unsigned long *size)
{
	return 0;
}
EXPORT_SYMBOL(ion_map_iommu);

int ion_unmap_iommu(void *dev, void *client, void *handle)
{
	return 0;
}
EXPORT_SYMBOL(ion_unmap_iommu);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HiKey960 Kirin 960 VPU ION/Legacy Compatibility Layer");

/* TEE/TVP stubs (TrustZone secure video decode not used in open Linux) */
int TEEK_InitializeContext(void *name, void *context) { return -EOPNOTSUPP; }
EXPORT_SYMBOL(TEEK_InitializeContext);

void TEEK_FinalizeContext(void *context) {}
EXPORT_SYMBOL(TEEK_FinalizeContext);

int TEEK_OpenSession(void *context, void *session, void *uuid, unsigned int login_type, void *connection_data, void *operation, void *return_origin) { return -EOPNOTSUPP; }
EXPORT_SYMBOL(TEEK_OpenSession);

void TEEK_CloseSession(void *session) {}
EXPORT_SYMBOL(TEEK_CloseSession);

int TEEK_InvokeCommand(void *session, unsigned int command_id, void *operation, void *return_origin) { return -EOPNOTSUPP; }
EXPORT_SYMBOL(TEEK_InvokeCommand);

void SMMU_SetMasterReg(void) {}
EXPORT_SYMBOL(SMMU_SetMasterReg);

#include <linux/spinlock.h>
#include <linux/stdarg.h>

#undef printk
asmlinkage __visible int printk(const char *fmt, ...)
{
	va_list args;
	int r;

	va_start(args, fmt);
	r = vprintk(fmt, args);
	va_end(args);

	return r;
}
EXPORT_SYMBOL(printk);

void __raw_spin_lock_init(raw_spinlock_t *lock, const char *name,
			  struct lock_class_key *key, short flags)
{
	*lock = (raw_spinlock_t)__RAW_SPIN_LOCK_UNLOCKED(*lock);
}
EXPORT_SYMBOL(__raw_spin_lock_init);

/* SMMU stubs */
void SMMU_IntServProc(void) {}
EXPORT_SYMBOL(SMMU_IntServProc);

void SMMU_DeInit(void) {}
EXPORT_SYMBOL(SMMU_DeInit);

void SMMU_Init(void) {}
EXPORT_SYMBOL(SMMU_Init);

void SMMU_InitGlobalReg(void) {}
EXPORT_SYMBOL(SMMU_InitGlobalReg);

/* Additional stubs for VENC and modern kernel compatibility */
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/slab.h>

int hisi_ion_enable_iommu(void *client)
{
	return 0;
}
EXPORT_SYMBOL(hisi_ion_enable_iommu);

void out_of_line_mutex_init(struct mutex *lock)
{
	mutex_init(lock);
}

asm(
	".globl __mutex_init\n"
	".type __mutex_init, %function\n"
	"__mutex_init:\n"
	"	b out_of_line_mutex_init\n"
);

struct class *__class_create(struct module *owner, const char *name, struct lock_class_key *key)
{
	return class_create(name);
}
EXPORT_SYMBOL(__class_create);

#undef kmalloc
void *__kmalloc(size_t size, gfp_t flags)
{
	return __kmalloc_noprof(size, flags);
}
EXPORT_SYMBOL(__kmalloc);

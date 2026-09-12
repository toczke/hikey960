#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>

/*
 * HiKey960 Kirin 960 VPU (vdec) Legacy Linkage Shims
 * Provides compatibility wrappers strictly for symbols required by
 * precompiled vdec assembly firmware without overriding core kernel symbols.
 */

/* ioremap / iounmap compatibility */
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

/* vmalloc wrapper for modern vmalloc_noprof */
#undef vmalloc
void *vmalloc(unsigned long size)
{
	return __vmalloc_noprof(size, GFP_KERNEL);
}
EXPORT_SYMBOL(vmalloc);

/* Stack protector canary for assembly objects referencing global canary */
#ifndef __stack_chk_guard
unsigned long __stack_chk_guard = 0xdeadbeef12345678UL;
EXPORT_SYMBOL(__stack_chk_guard);
#endif

/* Procfs PDE_DATA compatibility */
void *PDE_DATA(const struct inode *inode)
{
	return pde_data(inode);
}
EXPORT_SYMBOL(PDE_DATA);

/* Spinlock initialization for legacy assembly */
void __raw_spin_lock_init(raw_spinlock_t *lock, const char *name,
			  struct lock_class_key *key, short flags)
{
	*lock = (raw_spinlock_t)__RAW_SPIN_LOCK_UNLOCKED(*lock);
}
EXPORT_SYMBOL(__raw_spin_lock_init);

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

/* SMMU stubs for HiSMMUV100 */
void SMMU_SetMasterReg(void) {}
EXPORT_SYMBOL(SMMU_SetMasterReg);

void SMMU_IntServProc(void) {}
EXPORT_SYMBOL(SMMU_IntServProc);

void SMMU_DeInit(void) {}
EXPORT_SYMBOL(SMMU_DeInit);

void SMMU_Init(void) {}
EXPORT_SYMBOL(SMMU_Init);

void SMMU_InitGlobalReg(void) {}
EXPORT_SYMBOL(SMMU_InitGlobalReg);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HiKey960 Kirin 960 VPU Decoder Compatibility Layer");

# VPU Source Code & Binary Blob Audit (Linux 7.1)

> **Document Purpose:** Ground-truth audit of the VPU (`hi_vcodec`) decoder (`vdec`) and encoder (`venc`) codebase on branch `feature/kirin960-vpu-1080p`.
> Generated pursuant to Phase 1 of the VPU bring-up work order.

---

## 1. Executive Summary

| Subsystem | Source Form | Kernel Interface | Layout Risk Verdict | Bring-Up Action |
|---|---|---|---|---|
| **Decoder (`vdec`) Modern Glue** | 9 editable `.c` files (`omxvdec/`) | Linux 7.1 native DMA (`dma_alloc_coherent`), dma_buf, modern platform driver | **LOW RISK** (Real modern C) | Safe foundation for bring-up |
| **Decoder (`vdec`) Firmware HAL** | 38 compiled `.S` assembly files (`vfmw/`) | Internal `vfmw_osal` and `MEM_Phy2Vir`/`Vir2Phy` abstraction | **LINKAGE-ONLY RISK** (Safe to shim) | Safe to load once platform device probes |
| **Encoder (`venc`) Driver & HAL** | 12 compiled `.S` assembly files (0 `.c` files) | Direct calls to 4.9 ION, FLATMEM `mem_map`, 4.9 `struct platform_driver` | **CRITICAL STRUCT-LAYOUT RISK** | **DO NOT LOAD** without C glue rewrite |
| **ION Shim (`ion_compat.c`)** | Modern C linkage shim | Stubs returning `NULL` / no-op | **BROKEN FOR RUNTIME** | Must NOT be called by real memory paths |

### Safety Directive
> **CRITICAL SAFETY PROTOCOL:** No `.S` file marked `STRUCT-LAYOUT RISK` may be loaded on real hardware without a named human sign-off. Loading the un-shimmed `venc` assembly against Linux 7.1 will cause severe kernel memory corruption or AXI interconnect lockup due to FLATMEM address corruption and struct layout changes.

---

## 2. Architecture & Source Breakdown

### 2.1 Decoder (`hi_vcodec/vdec`) Architecture
- **Modern C Glue Layer (`vdec/omxvdec/`):** 9 C source files (`channel.c`, `decoder_vfmw.c`, `message.c`, `omxvdec.c`, `processor_bpp.c`, `task.c`, and platform files `platform.c`, `memory.c`, `regulator.c`).
  - `memory.c` has been fully ported to use standard Linux DMA coherent allocation (`dma_alloc_coherent`) and standard Linux DMA-BUF mapping (`dma_buf_attach`, `dma_buf_vmap`). It does **not** rely on Android ION.
  - `platform.c` implements a standard modern Linux `platform_driver` registering probe/remove routines.
- **Firmware / Syntax Parser Assembly (`vdec/vfmw/`):** 38 compiler-emitted `.S` assembly files.
  - All codec HALs (`vdm_hal_h264.S`, `vdm_hal_hevc.S`, `vdm_hal_mpeg2.S`, `vdm_hal_mpeg4.S`, `vdm_hal_vc1.S`, `vdm_hal_vp6.S`, `vdm_hal_vp8.S`, etc.) operate purely on physical/virtual addresses via `MEM_Phy2Vir`, `MEM_Vir2Phy`, `MEM_ReadPhyWord`, and `MEM_WritePhyWord`.
  - None of the 38 decoder `.S` files access kernel structures (`struct device`, `struct file`, `struct scatterlist`) directly.
  - **Verdict for vdec .S files:** All 38 files are **LINKAGE-ONLY RISK**.

### 2.2 Encoder (`hi_vcodec/venc`) Architecture
- **No Modern C Glue Layer:** There are **zero** `.c` files in `drivers-import/vcodec/hi_vcodec/venc/`. The entire subsystem consists of 12 compiled `.S` assembly files.
- **Landmines Identified:**
  1. **FLATMEM `mem_map` indexing (`hi_drv_mem.S` lines 811-846):** Directly references obsolete global symbols `mem_map` and `phystart_addr`. It calculates physical addresses using `(page - mem_map) << 12`, assuming FLATMEM and `sizeof(struct page) == 64`. On modern 64-bit Linux 7.1 with SPARSEMEM_VMEMMAP, `mem_map` is NULL, leading to completely invalid physical addresses fed directly to hardware DMA!
  2. **Frozen Linux 4.9 Platform Driver Registration (`drv_venc_intf.S` lines 3328, 3350):** Directly passes static assembly structures to `platform_device_register` and `__platform_driver_register`. `struct platform_driver` and `struct device_driver` have completely different field offsets in Linux 7.1 compared to 4.9.
  3. **Embedded Mutex Offset Mismatch (`drv_venc_efl.S` line 12944):** Hardcoded offset `+112` for `mutex_lock` into vendor static structs.
  4. **Legacy ION Reliance (`hi_drv_mem.S`):** Calls `ion_alloc`, `hisi_ion_client_create`, `ion_import_dma_buf_fd`, `ion_map_kernel`, and `ion_map_iommu`. Because `ion_compat.c` stubs these to return `NULL`, any runtime code path through `hi_drv_mem.S` causes an immediate NULL pointer dereference kernel panic.

---

## 3. Decoder (`vdec`) Source Audit (38 `.S` Files)

| File | Category | External Kernel Symbols Called | Immediate Offset / ABI Hazards | Verdict |
|---|---|---|---|---|
| `bitstream.S` | Decoder Firmware | None (internal only) | None detected | **LINKAGE-ONLY RISK** |
| `fsp.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `mem_manage.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `postprocess.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `public.S` | Decoder Firmware | `__stack_chk_fail`, `vsnprintf` | None detected | **LINKAGE-ONLY RISK** |
| `divx3.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `h264.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `hevc.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy`, `memset`, `snprintf` | None detected | **LINKAGE-ONLY RISK** |
| `mpeg2.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `mpeg4.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy`, `memset`, `strncmp` | None detected | **LINKAGE-ONLY RISK** |
| `real8.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `real9.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `syntax.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy` | None detected | **LINKAGE-ONLY RISK** |
| `vc1.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `vp6.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `vp8.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `vdm_drv.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel` | None detected | **LINKAGE-ONLY RISK** |
| `vfmw_ctrl.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy`, `memset`, `snprintf`, `strlen`, `strncpy` | None detected | **LINKAGE-ONLY RISK** |
| `scd_drv.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `smmu.S` | Decoder Firmware | `dprint_linux_kernel` | None detected | **LINKAGE-ONLY RISK** |
| `bitplane.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel` | None detected | **LINKAGE-ONLY RISK** |
| `vdm_hal.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `vdm_hal_divx3.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel` | None detected | **LINKAGE-ONLY RISK** |
| `vdm_hal_h264.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel` | None detected | **LINKAGE-ONLY RISK** |
| `vdm_hal_hevc.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `vdm_hal_mpeg2.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel` | None detected | **LINKAGE-ONLY RISK** |
| `vdm_hal_mpeg4.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel` | None detected | **LINKAGE-ONLY RISK** |
| `vdm_hal_real8.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel` | None detected | **LINKAGE-ONLY RISK** |
| `vdm_hal_real9.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel` | None detected | **LINKAGE-ONLY RISK** |
| `vdm_hal_vc1.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy` | None detected | **LINKAGE-ONLY RISK** |
| `vdm_hal_vp6.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `vdm_hal_vp8.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy` | None detected | **LINKAGE-ONLY RISK** |
| `linux_kernel_osal.S` | Decoder Firmware | `__init_waitqueue_head`, `__ioremap`, `__iounmap`, `__msecs_to_jiffies`, `__raw_spin_lock_init`, `__stack_chk_fail`, `__udelay`, `__wake_up`, `_raw_spin_lock_irqsave`, `_raw_spin_unlock_irqrestore`, `down_interruptible`, `dprint_linux_kernel`, `filp_close`, `filp_open`, `finish_wait`, `free_irq`, `init_wait_entry`, `kthread_create_on_node`, `msleep`, `prepare_to_wait_event`, `request_threaded_irq`, `sched_clock`, `schedule_timeout`, `up`, `vfree`, `vmalloc`, `wake_up_process` | None detected | **LINKAGE-ONLY RISK** |
| `linux_kernel_proc.S` | Decoder Firmware | `PDE_DATA`, `__arch_copy_from_user`, `__check_object_size`, `__stack_chk_fail`, `dprint_linux_kernel`, `memset`, `proc_create_data`, `remove_proc_entry`, `seq_printf`, `single_open`, `snprintf`, `strncmp`, `strncpy` | None detected | **LINKAGE-ONLY RISK** |
| `vfmw.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel` | None detected | **LINKAGE-ONLY RISK** |
| `vfmw_dts.S` | Decoder Firmware | `dprint_linux_kernel` | None detected | **LINKAGE-ONLY RISK** |
| `vfmw_intf.S` | Decoder Firmware | None (internal only) | None detected | **LINKAGE-ONLY RISK** |
| `tvp_adapter.S` | Decoder Firmware | `__stack_chk_fail`, `dprint_linux_kernel`, `memcpy`, `memset`, `msleep`, `strlen` | None detected | **LINKAGE-ONLY RISK** |

---

## 4. Encoder (`venc`) Source Audit (12 `.S` Files)

| File | Category | External Kernel Symbols Called | Immediate Offset / ABI Hazards | Verdict |
|---|---|---|---|---|
| `drv_omxvenc.S` | Encoder Driver | `HI_PRINT`, `__stack_chk_fail`, `memcpy`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `drv_omxvenc_efl.S` | Encoder Driver | `HI_PRINT`, `__stack_chk_fail`, `memcpy`, `memset` | None detected | **LINKAGE-ONLY RISK** |
| `drv_venc.S` | Encoder Driver | `HI_PRINT`, `__raw_spin_lock_init`, `__stack_chk_fail`, `_raw_spin_lock_irqsave`, `_raw_spin_unlock_irqrestore`, `do_gettimeofday`, `memcpy`, `printk`, `snprintf` | None detected | **LINKAGE-ONLY RISK** |
| `drv_venc_buf_mng.S` | Encoder Driver | None (internal only) | None detected | **LINKAGE-ONLY RISK** |
| `drv_venc_efl.S` | Encoder Driver | `HI_PRINT`, `__ioremap`, `__iounmap`, `__mutex_init`, `__stack_chk_fail`, `get_random_bytes`, `memcpy`, `memset`, `msleep`, `mutex_lock`, `mutex_unlock`, `printk`, `vfree`, `vmalloc` | Line 12944: Hardcoded immediate offset to `struct mutex` in vendor structure (`add	x0, x0, 112`) | **STRUCT-LAYOUT RISK** |
| `drv_venc_intf.S` | Encoder Driver | `HI_PRINT`, `__arch_copy_from_user`, `__arch_copy_to_user`, `__class_create`, `__ioremap`, `__iounmap`, `__mutex_init`, `__platform_driver_register`, `__stack_chk_fail`, `alloc_chrdev_region`, `cdev_add`, `cdev_del`, `cdev_init`, `class_destroy`, `device_create`, `device_destroy`, `memset`, `mutex_lock`, `mutex_unlock`, `platform_device_register`, `platform_device_unregister`, `platform_driver_unregister`, `unregister_chrdev_region`, `vfree`, `vmalloc` | Line 3328: Registration of frozen 4.9 `struct platform_driver`/`platform_device` (`bl	platform_device_register`)<br>Line 3350: Registration of frozen 4.9 `struct platform_driver`/`platform_device` (`bl	__platform_driver_register`)<br>Line 4578: Registration of frozen 4.9 `struct platform_driver`/`platform_device` (`.string	"%s call platform_device_register failed!\n"`)<br>Line 4581: Registration of frozen 4.9 `struct platform_driver`/`platform_device` (`.string	"%s call platform_driver_register failed!\n"`) | **STRUCT-LAYOUT RISK** |
| `drv_venc_osal.S` | Encoder Driver | `HI_PRINT`, `__init_waitqueue_head`, `__msecs_to_jiffies`, `__raw_spin_lock_init`, `__stack_chk_fail`, `__wake_up`, `_raw_spin_lock_irqsave`, `_raw_spin_unlock_irqrestore`, `filp_close`, `filp_open`, `finish_wait`, `free_irq`, `init_wait_entry`, `kthread_create_on_node`, `prepare_to_wait_event`, `request_threaded_irq`, `schedule`, `schedule_timeout`, `vfree`, `vfs_write`, `vmalloc`, `wake_up_process` | None detected | **LINKAGE-ONLY RISK** |
| `drv_venc_proc.S` | Encoder Driver | `HI_PRINT`, `PDE_DATA`, `__arch_copy_from_user`, `__check_object_size`, `__stack_chk_fail`, `memcpy`, `memset`, `printk`, `seq_printf`, `single_open`, `strncmp` | None detected | **LINKAGE-ONLY RISK** |
| `drv_venc_queue_mng.S` | Encoder Driver | `HI_PRINT`, `__init_waitqueue_head`, `__raw_spin_lock_init`, `__stack_chk_fail`, `__wake_up`, `_raw_spin_lock_irqsave`, `_raw_spin_unlock_irqrestore`, `finish_wait`, `init_wait_entry`, `memcpy`, `memset`, `msleep`, `prepare_to_wait_event`, `schedule_timeout`, `vfree`, `vmalloc` | None detected | **LINKAGE-ONLY RISK** |
| `hal_venc.S` | Encoder Driver | `HI_PRINT`, `__stack_chk_fail`, `filp_close`, `filp_open`, `get_random_bytes`, `memcpy`, `msleep`, `vfs_write` | None detected | **LINKAGE-ONLY RISK** |
| `hi_drv_mem.S` | Encoder Driver | `__arch_copy_from_user`, `__arch_copy_to_user`, `__check_object_size`, `__kmalloc`, `__stack_chk_fail`, `dma_buf_attach`, `dma_buf_detach`, `dma_buf_get`, `dma_buf_map_attachment`, `dma_buf_put`, `dma_buf_unmap_attachment`, `do_gettimeofday`, `down_interruptible`, `hisi_ion_client_create`, `ion_alloc`, `ion_client_destroy`, `ion_free`, `ion_import_dma_buf_fd`, `ion_map_iommu`, `ion_map_kernel`, `ion_share_dma_buf_fd`, `ion_unmap_iommu`, `ion_unmap_kernel`, `kfree`, `memset`, `printk`, `rtc_time64_to_tm`, `sched_clock`, `snprintf`, `sys_close`, `up`, `vsnprintf` | Line 811: Access to obsolete FLATMEM global symbol (`adrp	x3, mem_map`)<br>Line 824: Access to obsolete FLATMEM global symbol (`ldr	x6, [x3,#:lo12:mem_map]`)<br>Line 825: Access to obsolete FLATMEM global symbol (`adrp	x3, phystart_addr`)<br>Line 834: Access to obsolete FLATMEM global symbol (`ldr	x3, [x3,#:lo12:phystart_addr]`)<br>Calls deprecated/stubbed Android ION APIs: hisi_ion_client_create, ion_alloc, ion_client_destroy, ion_free, ion_import_dma_buf_fd, ion_map_iommu, ion_map_kernel, ion_share_dma_buf_fd, ion_unmap_iommu, ion_unmap_kernel | **STRUCT-LAYOUT RISK** |
| `venc_regulator.S` | Encoder Driver | `HI_PRINT`, `__ioremap`, `__iounmap`, `__stack_chk_fail`, `clk_disable`, `clk_enable`, `clk_prepare`, `clk_set_rate`, `clk_unprepare`, `devm_clk_get`, `devm_regulator_get`, `hisi_ion_enable_iommu`, `irq_of_parse_and_map`, `of_address_to_resource`, `of_property_read_u32_index`, `of_property_read_variable_u32_array`, `regulator_disable`, `regulator_enable` | Calls deprecated/stubbed Android ION APIs: hisi_ion_enable_iommu | **STRUCT-LAYOUT RISK** |

---

## 5. ION Compatibility Layer Audit (`ion_compat.c`)

The file `drivers-import/vcodec/ion_compat.c` provides compilation shims for legacy Android ION APIs removed in upstream Linux:
- `hisi_ion_client_create()` -> returns `NULL`
- `ion_alloc()` -> returns `NULL`
- `ion_import_dma_buf_fd()` -> returns `NULL`
- `ion_map_kernel()` -> returns `NULL`
- `ion_map_iommu()` -> returns `0` (success stub without mapping)
- `mem_map` -> exported as `NULL` pointer
- `phystart_addr` -> exported as `0`

### Impact Analysis
1. **Decoder (`vdec`):** **SAFE.** The decoder modernized glue layer in `omxvdec/platform/kirin/memory.c` bypasses `ion_compat.c` entirely, allocating memory via `dma_alloc_coherent()` and importing buffers via `dma_buf_vmap()`. No decoder playback path dereferences `ion_alloc()` or `mem_map`.
2. **Encoder (`venc`):** **BROKEN.** The encoder memory allocator (`hi_drv_mem.S`) exclusively calls `ion_alloc` and `mem_map`. Loading the encoder will cause immediate crashes when dereferencing the stubbed `NULL` handles.

---

## 6. Action Plan & Gates for Subsequent Phases

1. **Phase 2 (Device Tree Integration):**
   - Implement `vdec@e8800000` node in `patches/0007-hikey960-vpu-node.patch`.
   - Probe decoder driver on hardware and verify in `dmesg`.
2. **Phase 3 (Decoder Bring-Up):**
   - Since all 38 `vdec` `.S` files are **LINKAGE-ONLY RISK**, decoder bring-up can proceed safely.
   - Begin with H.264 Baseline 320x240 elementary stream.
3. **Phase 4 (Encoder Modernization & Bring-Up Gate):**
   - **BLOCKING GATE:** The encoder has 4 files marked **STRUCT-LAYOUT RISK** (`hi_drv_mem.S`, `drv_venc_intf.S`, `drv_venc_efl.S`, `venc_regulator.S`).
   - Under the work order ground rules, encoder assembly must NOT be loaded on physical hardware without human review or until a modern C glue layer (replacing `hi_drv_mem.S` with `dma_alloc_coherent` and providing a modern platform device) is implemented.

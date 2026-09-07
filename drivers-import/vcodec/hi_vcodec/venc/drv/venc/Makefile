# Add your debugging flag (or not) to CFLAGS
include drivers/vcodec/hi_vcodec/venc/drv/venc/drv_venc_make.cfg

subdir-ccflags-y += $(VENC_CFLAGS) -fno-pic
subdir-ccflags-y += -Idrivers/vcodec/hi_vcodec/venc/drv/include
subdir-ccflags-y += -Idrivers/vcodec/hi_vcodec/venc/include
subdir-ccflags-y += -Idrivers/vcodec/hi_vcodec/venc/drv/venc

ifneq ($(TARGET_BUILD_VARIANT),user)
subdir-ccflags-y += -DUSER_DISABLE_VENC_PROC
endif

subdir-ccflags-y += -DPLATFORM_HI3660

obj-$(CONFIG_HI_VCODEC_VENC_HI3660) += hi_omxvenc.o
hi_omxvenc-objs :=  venc_regulator.o     \
                    drv_venc_intf.o      \
                    drv_venc_efl.o       \
                    drv_venc_osal.o      \
                    drv_venc.o           \
                    drv_omxvenc.o        \
                    drv_omxvenc_efl.o    \
                    drv_venc_buf_mng.o   \
                    drv_venc_queue_mng.o \
                    drv_venc_proc.o      \
                    hal_venc.o           \
                    hi_drv_mem.o


#obj-m += venc.o
#venc-y :=           venc_regulator.o     \
                    drv_venc_intf.o      \
                    drv_venc_efl.o       \
                    drv_venc_osal.o      \
                    drv_venc.o           \
                    drv_omxvenc.o        \
                    drv_omxvenc_efl.o    \
                    drv_venc_buf_mng.o   \
                    drv_venc_queue_mng.o \
                    drv_venc_proc.o      \
                    hal_venc.o           \
                    hi_drv_mem.o


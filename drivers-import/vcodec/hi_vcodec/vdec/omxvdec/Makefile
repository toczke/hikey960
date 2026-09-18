
TOP := drivers/../..

PLATFORM_DIR := platform/kirin

#subdir-ccflags-y += -DHIVDEC_SMMU_SUPPORT

subdir-ccflags-y += -Idrivers/vcodec/hi_vcodec/vdec/include
subdir-ccflags-y += -Idrivers/vcodec/hi_vcodec/vdec/omxvdec
subdir-ccflags-y += -Idrivers/vcodec/hi_vcodec/vdec/omxvdec/$(PLATFORM_DIR)
subdir-ccflags-y += -Idrivers/vcodec/hi_vcodec/vdec/vfmw/vfmw_v4.0
subdir-ccflags-y += -Idrivers/vcodec/hi_vcodec/vdec/vfmw/vfmw_v4.0/scene/kirin
subdir-ccflags-y += -fno-pic
subdir-ccflags-y += -DOMXVDEC_TVP_CONFLICT

ifneq ($(TARGET_BUILD_VARIANT), user)
subdir-ccflags-y += -DUSER_DISABLE_VDEC_PROC
endif

subdir-ccflags-y += -DPLATFORM_HI3660

#build in
obj-$(CONFIG_HI_VCODEC_VDEC_HI3660) += hi_omxvdec.o
hi_omxvdec-objs := $(PLATFORM_DIR)/platform.o $(PLATFORM_DIR)/regulator.o $(PLATFORM_DIR)/memory.o
hi_omxvdec-objs += omxvdec.o channel.o task.o message.o processor_bpp.o decoder_vfmw.o

#module
#obj-m += hi_omxvdec.o
#hi_omxvdec-y := $(PLATFORM_DIR)/platform.o $(PLATFORM_DIR)/regulator.o $(PLATFORM_DIR)/memory.o
#hi_omxvdec-y += omxvdec.o channel.o task.o message.o processor_bpp.o decoder_vfmw.o

# ================================================================
# 模块基名 —— 单一来源，决定 .ko 的名字
#   MODULE_NAME
# ================================================================
MODULE_NAME := pci_skel

# 实际的源文件（与模块名不同名，所以要用 <模块名>-objs 指定）
SRC_OBJ     := pci_skel.o 

# ================================================================
# 调试开关
#   DEBUG=y（默认）→ 定义 SCULL_DEBUG，把 #ifdef SCULL_DEBUG 的调试代码编进去
#   DEBUG=n        → 不定义，调试代码被预处理排除
#   用法：make            （默认开启）
#         make DEBUG=n    （关闭）
#
# 注意：外部模块构建是 kbuild 驱动的，LDD3 那套 "CFLAGS += $(DEBFLAGS)"
#       在这里完全无效，必须用 kbuild 的 ccflags-y（见文件末尾）。
# ================================================================
# DEBUG ?= y

ifeq ($(KERNELRELEASE),)

# ==================== 外部模块编译（你直接 make 时走这支）====================
KERNELDIR     ?= ../../kernel/linux-6.12.10
KODIR         := ../mydriver/
PWD           := $(shell pwd)

# 必须与内核编译时使用的工具链一致（内核用 aarch64-linux-gnu-gcc 13.3.0）
# 注意：必须用 := 而非 ?=，因为环境变量里已导出了 CROSS_COMPILE，
#       用 ?= 的话环境变量的值会优先，导致用到错误的工具链。
ARCH          := arm64
CROSS_COMPILE := aarch64-linux-gnu-

modules:
	$(MAKE) -C $(KERNELDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		M=$(PWD) DEBUG=$(DEBUG) modules

install: modules
	@mkdir -p $(KODIR)
	@cp -f $(MODULE_NAME).ko $(KODIR)
	@echo "[install] $(MODULE_NAME).ko -> $(KODIR)"





clean:
	$(MAKE) -C $(KERNELDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) clean

.PHONY: modules install clean

else

# ==================== kbuild 调用时走这支 ====================
# 1) obj-m 的基名决定最终 .ko 名字：hellop.o -> hellop.ko
obj-m += $(MODULE_NAME).o

# 2) 用 <模块名>-objs 指定真正的中间对象（与模块名不同名时必须写）
#    scull.o 由 scull.c 编译，access.o 由 access.c 编译，再链接成 scull.ko
# $(MODULE_NAME)-objs := $(SRC_OBJ)

# 3) 调试宏：kbuild 只认 ccflags-y（CFLAGS 会被忽略）
ifeq ($(DEBUG),y)
ccflags-y += -DSCULL_DEBUG
endif

endif

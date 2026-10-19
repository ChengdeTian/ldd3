# ================================================================
# 模块基名 —— 单一来源，决定 .ko 的名字
#   MODULE_NAME
# ================================================================
MODULE_NAME := scull

# 实际的源文件（与模块名不同名，所以要用 <模块名>-objs 指定）
SRC_OBJ     := main.o pipe.o access.o

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

# 交叉编译用户态测试程序（静态链接，guest 里无需额外库）
TEST_SRC := scull_test.c
TEST_BIN := scull_test

test:
	$(CROSS_COMPILE)gcc -static -O2 -Wall -o $(TEST_BIN) $(TEST_SRC)
	@mkdir -p $(KODIR)
	@cp -f $(TEST_BIN) $(KODIR)
	@echo "[install] $(TEST_BIN) -> $(KODIR)"

# 访问控制设备（scullsingle/sculluid/scullwuid/sullpriv）测试
TEST1_SRC := scull_test1.c
TEST1_BIN := scull_test1

test1:
	$(CROSS_COMPILE)gcc -static -O2 -Wall -o $(TEST1_BIN) $(TEST1_SRC)
	@mkdir -p $(KODIR)
	@cp -f $(TEST1_BIN) $(KODIR)
	@echo "[install] $(TEST1_BIN) -> $(KODIR)"

# scullpipe 设备测试
TEST2_SRC := scull_test2.c
TEST2_BIN := scull_test2

test2:
	$(CROSS_COMPILE)gcc -static -O2 -Wall -o $(TEST2_BIN) $(TEST2_SRC)
	@mkdir -p $(KODIR)
	@cp -f $(TEST2_BIN) $(KODIR)
	@echo "[install] $(TEST2_BIN) -> $(KODIR)"

tests: test test1 test2

clean:
	$(MAKE) -C $(KERNELDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) clean
	rm -f $(TEST_BIN) $(TEST1_BIN) $(TEST2_BIN)

.PHONY: modules install test test1 test2 tests clean

else

# ==================== kbuild 调用时走这支 ====================
# 1) obj-m 的基名决定最终 .ko 名字：hellop.o -> hellop.ko
obj-m += $(MODULE_NAME).o

# 2) 用 <模块名>-objs 指定真正的中间对象（与模块名不同名时必须写）
#    scull.o 由 scull.c 编译，access.o 由 access.c 编译，再链接成 scull.ko
$(MODULE_NAME)-objs := $(SRC_OBJ)

# 3) 调试宏：kbuild 只认 ccflags-y（CFLAGS 会被忽略）
ifeq ($(DEBUG),y)
ccflags-y += -DSCULL_DEBUG
endif

endif

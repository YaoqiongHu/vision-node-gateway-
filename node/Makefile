# =============================================================================
#  vision-node — STM32F407 视觉节点固件
#
#  分层构建：每一层只拿到"自己 + 下层"的 -I，跨层 include 在编译期就编不过。
#  依赖方向（箭头 = 可以 include）：
#
#      app  →  proto / lib  →  drv  →  bsp  →  arch
#                    ↑
#                    os（旁路：被 app 使用，drv 通过回调把事件交出去）
#
#  技术选型：CMSIS + 裸寄存器，不用 ST HAL / LL —— 工程里没有任何第三方 .c。
#  F4 的 LL 根本没有 CAN 和 DCMI 的封装，所以"全 LL"本来就不成立。
#
#  用法：
#     make                编固件（arch/…/app 下的 .c 自动收集）
#     make size           看 flash/ram 占用
#     make clean
#     make test           在 PC 上编译并运行 lib/ + proto/ 的单元测试
#     make check-layers   检查有没有人把 RTOS 头塞进 drv、把外设头塞进 lib
#     make toolchain      打印实际用的是哪个 gcc（两套工具链并存时很有用）
# =============================================================================

TARGET    := vision-node
BUILD     := build
SHARED    := ../shared

# toolchain 目标写在 all 前面，显式钉住默认目标，别让 make 的"第一个目标即默认"猜错
.DEFAULT_GOAL := all

# ---------- 工具链 ----------
# 优先用 ARM 官方 GNU Toolchain（自带 arm-none-eabi-gdb，newlib 完整）；
# 找不到再退回 PATH 里的 arm-none-eabi-*（例如 Ubuntu 的 gcc-arm-none-eabi 包，
# 那个包**不含 gdb**，调试还得另装）。要强指定就 make PREFIX=/your/bin/arm-none-eabi-
ARM_GNU_HOME ?= $(lastword $(sort $(wildcard $(HOME)/opt/arm-gnu-toolchain-*)))
ifneq ($(ARM_GNU_HOME),)
PREFIX    ?= $(ARM_GNU_HOME)/bin/arm-none-eabi-
else
PREFIX    ?= arm-none-eabi-
endif
CC        := $(PREFIX)gcc
OBJCOPY   := $(PREFIX)objcopy
SIZE      := $(PREFIX)size
GDB       := $(PREFIX)gdb
HOST_CC   ?= gcc

# ---------- MCU ----------
CPU       := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
DEFS      := -DSTM32F407xx -DHSE_VALUE=8000000
WARN      := -Wall -Wextra -Wshadow -Wundef -Wdouble-promotion -Wno-unused-parameter
OPT       ?= -Og -g3
CFLAGS    := $(CPU) $(DEFS) $(OPT) $(WARN) -std=c11 \
             -ffunction-sections -fdata-sections -MMD -MP
LDFLAGS   := $(CPU) -Tarch/stm32f407.ld --specs=nano.specs --specs=nosys.specs \
             -Wl,--gc-sections -Wl,--print-memory-usage -Wl,--no-warn-rwx-segments \
             -Wl,-Map=$(BUILD)/$(TARGET).map
LIBS      := -lc -lm

# ---------- 分层与 include 可见性 ----------
# 新增一层改这里 + 下面的 _incs 变量，别在 recipe 里到处加 -I。
LAYERS    := arch bsp drv os lib proto app

# CMSIS 的三个头（core_cm4.h / stm32f4xx.h / stm32f407xx.h）放在 arch/cmsis/，
# 只有碰硬件的层拿得到这个 -I；lib/ 与 proto/ 连 arch 都看不见。
cmsis_incs := -Iarch/cmsis

arch_incs  := -Iarch $(cmsis_incs)
bsp_incs   := $(arch_incs) -Ibsp -Iconfig
drv_incs   := $(bsp_incs) -Idrv -Ilib
os_incs    := $(arch_incs) -Ios -Ios/freertos/include \
              -Ios/freertos/portable/GCC/ARM_CM4F -Iconfig
lib_incs   := -Ilib
proto_incs := -Ilib -Iproto -I$(SHARED)/proto
app_incs   := $(drv_incs) $(os_incs) $(proto_incs) -Iapp -Iconfig

# ---------- 源文件收集 ----------
C_SRCS   := $(shell find $(LAYERS) -name '*.c' 2>/dev/null | sort)
S_SRCS   := $(shell find arch \( -name '*.s' -o -name '*.S' \) 2>/dev/null | sort)
OBJS     := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS)) \
            $(patsubst %.s,$(BUILD)/%.o,$(filter %.s,$(S_SRCS))) \
            $(patsubst %.S,$(BUILD)/%.o,$(filter %.S,$(S_SRCS)))
LDSCRIPT := arch/stm32f407.ld

.PHONY: all clean size test check-layers toolchain help

# ---------- 工具链自检 ----------
toolchain:
	@command -v $(CC) >/dev/null 2>&1 || { \
		echo "✗ 找不到 $(CC)"; \
		echo "  装 ARM GNU Toolchain 到 ~/opt/，或 make PREFIX=/path/to/bin/arm-none-eabi-"; \
		exit 1; \
	}
	@printf '[toolchain] %s\n' "$$($(CC) --version | head -1)"
	@printf '[prefix]    %s\n' "$(PREFIX)"
	@command -v $(GDB) >/dev/null 2>&1 || \
		echo "[warn] 没有 $(GDB)：烧写/单步能用，但 Cortex-Debug 需要 gdb"

# ---------- 每层独立的编译规则 ----------
# $(1)_incs 只包含该层允许看见的路径 —— 这就是"越界编不过"的机械保证。
define LAYER_RULE
$(BUILD)/$(1)/%.o: $(1)/%.c
	@mkdir -p $$(dir $$@)
	$(CC) -c $(CFLAGS) $($(1)_incs) $$< -o $$@
endef
$(foreach L,$(LAYERS),$(eval $(call LAYER_RULE,$(L))))

$(BUILD)/arch/%.o: arch/%.s
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -Iarch $< -o $@

-include $(OBJS:.o=.d)

# ---------- 目标 ----------
# 什么时候"还不能链接"：要么一个源文件都没有，要么有 arch/ 的启动文件但缺
# app/main.c（固件入口）。后一种现在还成立，等 S1 写出 main.c 就自动消失。
ifeq ($(strip $(C_SRCS)$(S_SRCS)),)
STAGE_MSG := 骨架已就绪，各层还没有 .c/.s 文件
else ifeq ($(wildcard app/main.c),)
STAGE_MSG := arch/ 的启动文件与时钟已经就位，但还没有 app/main.c（固件入口），暂不链接
endif

ifeq ($(STAGE_MSG),)
all: toolchain $(TARGET).elf

$(TARGET).elf: $(OBJS) $(LDSCRIPT)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $@
	$(SIZE) $@
	$(OBJCOPY) -O ihex   $@ $(BUILD)/$(TARGET).hex
	$(OBJCOPY) -O binary $@ $(BUILD)/$(TARGET).bin
else
all: toolchain
	@echo "[node] $(STAGE_MSG)"
	@echo "       分层规则见 node/README.md。"
endif

ifeq ($(STAGE_MSG),)
size: $(TARGET).elf
	$(SIZE) -A $<
else
size:
	@echo "[node] $(STAGE_MSG)"
endif

# ---------- 主机侧单测：lib/ 与 proto/ 必须能脱离硬件编译 ----------
HOST_LIB_SRCS  := $(shell find lib proto -name '*.c' 2>/dev/null | sort)
HOST_TEST_SRCS := $(shell find test -name 'test_*.c' 2>/dev/null | sort)

ifeq ($(strip $(HOST_TEST_SRCS)),)
test:
	@echo "[test] 还没有 test/test_*.c，跳过。"
else
test:
	@mkdir -p $(BUILD)
	$(HOST_CC) -std=c11 -O1 -g $(WARN) -DHOST_TEST=1 \
		-Ilib -Iproto -Itest -I$(SHARED)/proto \
		$(HOST_TEST_SRCS) $(HOST_LIB_SRCS) -o $(BUILD)/test_host
	@./$(BUILD)/test_host
endif

# ---------- 分层红线检查 ----------
check-layers:
	@echo "[check] drv/bsp/arch 不得依赖 RTOS 与 app"
	@if grep -rnE '#include[[:space:]]*"(FreeRTOS\.h|task\.h|queue\.h|semphr\.h|timers\.h|event_groups\.h|stream_buffer\.h|app_[a-z_]*\.h)"' \
		drv bsp arch 2>/dev/null; then \
		echo "  ✗ 越界"; exit 1; else echo "  ✓"; fi
	@echo "[check] lib/proto 不得依赖外设、板级与 OS"
	@if grep -rnE '#include[[:space:]]*"(stm32f4[0-9a-z_]*\.h|cmsis[^"]*|board\.h|bsp_[a-z_]*\.h|FreeRTOS\.h|task\.h|os_[a-z_]*\.h)"' \
		lib proto 2>/dev/null; then \
		echo "  ✗ 越界"; exit 1; else echo "  ✓"; fi

clean:
	rm -rf $(BUILD) $(TARGET).elf

help:
	@sed -n '2,20p' Makefile

# ================================================================
# NxCourier · Copyright (C) 2026 granule256
#
# 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
#（GPL-2.0-only）发布，不带任何担保。
# 完整条款见仓库根目录的 LICENSE。
# ================================================================

# NxCourier 平台 · 设备侧构建
#
# 与官方 sysmodule 模板的差异只有三处，其余保持一致：
#   1. TARGET 固定为 nxc（不取目录名）；
#   2. SOURCES 增加 source/core 与 source/tools 两个子目录；
#   3. CXXFLAGS 显式 -std=gnu++17（SDK 里用了 inline constexpr 变量）。
#
# ★ 因为工程里存在 config.json，Makefile 会自动产出 ExeFS PFS0（.nsp）而不是 .nro；
#   部署时把这个 .nsp 改名为 exefs.nsp，放进 /atmosphere/contents/<TID>/。

.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      :=  nxc
BUILD       :=  build
SOURCES     :=  source source/core source/tools
DATA        :=  data
INCLUDES    :=  include source/core

# ---- 版本号：单一来源是仓库根目录的 VERSION ----
#   ★ 路径层级：Makefile 在 `device/platform/`，VERSION 在仓库根 ⇒ 相对路径是
#     **`../../VERSION`**。`../VERSION` 只是 `device/VERSION`，那儿没这个文件。
#   ★ 本文件会被递归调用两次（先在 device/platform 里、再在 build/ 里），子调用的 CURDIR
#     已经变成 build/，`../../` 就指偏了 —— 所以**只在父调用里读一次、然后导出**，
#     子调用直接用环境里的值（父调用跑完整个文件后才发起子 make，导出一定已经生效）。
#   ★ 走 docker 时**必须把仓库根目录挂进容器**
#     （`-v <仓库根>:/work -w /work/device/platform`）：只挂 device/platform 的话，
#     仓库根在容器里根本不存在，读不到 VERSION。
#   ★ 读不到就不注入 —— 头文件里的兜底值带 `-unpinned` 后缀，一眼能看出没接上，
#     不会悄悄报一个看起来正常的错版本。
NXC_VERSION_FILE ?= $(CURDIR)/../../VERSION
ifeq ($(NXC_VERSION),)
    NXC_VERSION := $(shell cat $(NXC_VERSION_FILE) 2>/dev/null)
    export NXC_VERSION
endif
ifneq ($(strip $(NXC_VERSION)),)
    DEFINES += -DNXC_VERSION='"$(NXC_VERSION)"'
endif

ARCH    :=  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  :=  -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS  +=  $(INCLUDE) -D__SWITCH__

CXXFLAGS := $(CFLAGS) -std=gnu++17 -fno-rtti -fno-exceptions

ASFLAGS :=  -g $(ARCH)
LDFLAGS  =  -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    :=  -lnx

LIBDIRS :=  $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)

export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                    $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES    :=  $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES_BIN   :=  $(addsuffix .o,$(BINFILES))
export OFILES_SRC   :=  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES       :=  $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN   :=  $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD)

export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(CONFIG_JSON)),)
    jsons := $(wildcard *.json)
    ifneq (,$(findstring $(TARGET).json,$(jsons)))
        export APP_JSON := $(TOPDIR)/$(TARGET).json
    else
        ifneq (,$(findstring config.json,$(jsons)))
            export APP_JSON := $(TOPDIR)/config.json
        endif
    endif
else
    export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

ifeq ($(strip $(ICON)),)
    icons := $(wildcard *.jpg)
    ifneq (,$(findstring $(TARGET).jpg,$(icons)))
        export APP_ICON := $(TOPDIR)/$(TARGET).jpg
    else
        ifneq (,$(findstring icon.jpg,$(icons)))
            export APP_ICON := $(TOPDIR)/icon.jpg
        endif
    endif
else
    export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
    export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
    export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
    export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
    export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
ifeq ($(strip $(APP_JSON)),)
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
	@rm -fr $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf
endif

else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

ifeq ($(strip $(APP_JSON)),)

all : $(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro : $(OUTPUT).elf
endif

else

all : $(OUTPUT).nsp

$(OUTPUT).nsp : $(OUTPUT).nso $(OUTPUT).npdm
$(OUTPUT).nso : $(OUTPUT).elf

endif

$(OUTPUT).elf : $(OFILES)

$(OFILES_SRC) : $(HFILES_BIN)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif

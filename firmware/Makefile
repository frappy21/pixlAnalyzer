PROJECT_NAME     := secure_bootloader_ble_s112_pca10040
TARGETS          := PixlAnalyzerOLED PixlAnalyzerLCD
OUTPUT_DIRECTORY := build

# Toolchain: the SDK defaults to a fixed Linux path, so pick up whatever
# arm-none-eabi-gcc is in PATH instead. Override on the command line if needed.
ARM_GCC := $(shell command -v arm-none-eabi-gcc 2>/dev/null)
ifneq ($(ARM_GCC),)
GNU_INSTALL_ROOT ?= $(dir $(ARM_GCC))
GNU_VERSION ?= $(shell $(ARM_GCC) -dumpversion)
GNU_PREFIX ?= arm-none-eabi
endif

SDK_ROOT := sdk

# Default variant for flashing (can be overridden via cmd line: make flash VARIANT=PixlAnalyzerLCD)
VARIANT ?= PixlAnalyzerOLED

# Apply the linker script to all defined targets
$(foreach target, $(TARGETS), $(eval $(target): LINKER_SCRIPT := ld/loader.ld))

# SDK startup / system files
SRC_FILES += \
  $(SDK_ROOT)/modules/nrfx/mdk/gcc_startup_nrf52.S \
  $(SDK_ROOT)/modules/nrfx/mdk/system_nrf52.c \
  $(SDK_ROOT)/components/boards/boards.c \

# Application
SRC_FILES += \
  src/main.c \
  src/app/app.c \
  src/app/channels.c \
  src/app/classify.c \
  src/app/scr_ble.c \
  src/app/scr_ble_sensors.c \
  src/app/scr_esb.c \
  src/app/scr_identify.c \
  src/app/scr_info.c \
  src/app/scr_menu.c \
  src/app/scr_scanner.c \
  src/app/scr_settings.c \
  src/app/scr_tx.c \
  src/app/screens.c \
  src/app/settings.c \
  src/app/spectrum.c \
  src/app/ui.c \
  src/app/ui_ble.c \
  src/app/ui_esb.c \
  src/app/ui_info.c \
  src/app/ui_tx.c \
  src/drivers/battery.c \
  src/drivers/buttons.c \
  src/drivers/display.c \
  src/drivers/flash_ext.c \
  src/drivers/flash_int.c \
  src/drivers/led.c \
  src/drivers/power.c \
  src/drivers/spi_bus.c \
  src/drivers/systime.c \
  src/gfx/font3x5.c \
  src/gfx/font5x7.c \
  src/gfx/gfx.c \
  src/radio/ble_adv.c \
  src/radio/ble_devtab.c \
  src/radio/ble_scan.c \
  src/radio/ble_sensor.c \
  src/radio/esb_scan.c \
  src/radio/scanner.c \
  src/radio/sweep_order.c \
  src/radio/tx_test.c \

# Application headers
INC_FOLDERS += \
  config \
  src \
  src/app \
  src/drivers \
  src/gfx \
  src/radio \

# SDK headers
INC_FOLDERS += \
  $(SDK_ROOT)/components/softdevice/s112/headers \
  $(SDK_ROOT)/components/libraries/util \
  $(SDK_ROOT)/modules/nrfx \
  $(SDK_ROOT)/integration/nrfx \
  $(SDK_ROOT)/components/libraries/delay \
  $(SDK_ROOT)/components/boards \
  $(SDK_ROOT)/modules/nrfx/hal \
  $(SDK_ROOT)/components/toolchain/cmsis/include \
  $(SDK_ROOT)/modules/nrfx/mdk \

OPT = -Os -g3

CFLAGS += $(OPT)
CFLAGS += -DCONFIG_GPIO_AS_PINRESET
CFLAGS += -DFLOAT_ABI_HARD
CFLAGS += -DNRF52
CFLAGS += -DNRF52832_XXAA
CFLAGS += -DNRF52_PAN_74
CFLAGS += -mcpu=cortex-m4
CFLAGS += -mthumb -mabi=aapcs
CFLAGS += -mfloat-abi=hard -mfpu=fpv4-sp-d16
CFLAGS += -ffunction-sections -fdata-sections -fno-strict-aliasing
CFLAGS += -fno-builtin -fshort-enums
CFLAGS += -DBOARD_CUSTOM
CFLAGS += -Wall -Wextra -Wno-unused-parameter

CXXFLAGS += $(OPT)
ASMFLAGS += -g3
ASMFLAGS += -mcpu=cortex-m4
ASMFLAGS += -mthumb -mabi=aapcs
ASMFLAGS += -mfloat-abi=hard -mfpu=fpv4-sp-d16
ASMFLAGS += -DCONFIG_GPIO_AS_PINRESET
ASMFLAGS += -DFLOAT_ABI_HARD
ASMFLAGS += -DNRF52
ASMFLAGS += -DNRF52832_XXAA
ASMFLAGS += -DNRF52_PAN_74
ASMFLAGS += -DBOARD_CUSTOM

LDFLAGS += $(OPT)
LDFLAGS += -mthumb -mabi=aapcs -L$(SDK_ROOT)/modules/nrfx/mdk -T$(LINKER_SCRIPT)
LDFLAGS += -mcpu=cortex-m4
LDFLAGS += -mfloat-abi=hard -mfpu=fpv4-sp-d16
LDFLAGS += -Wl,--gc-sections
LDFLAGS += --specs=nano.specs

# Target Specific Flags
# ---------------------
$(TARGETS): ASMFLAGS += -D__HEAP_SIZE=0

# OLED Specifics
PixlAnalyzerOLED: CFLAGS += -DOLED_TYPE_SH1106

# LCD Specifics
PixlAnalyzerLCD: CFLAGS += -DLCD_TYPE_DEFAULT 

LIB_FILES += -lc -lnosys -lm

# -------------------------------------------------------------------------
# Default target
# 1. Builds all targets (OLED and LCD)
# 2. Generates OTA .zip files for all targets
# -------------------------------------------------------------------------
default: $(TARGETS)
	@echo "------------------------------------------------"
	@echo "Generating OTA Packages (.zip) for all targets..."
	@echo "------------------------------------------------"
	$(foreach target, $(TARGETS), \
		echo "Creating $(target).zip ..."; \
		nrfutil pkg generate --application $(OUTPUT_DIRECTORY)/$(target).hex --application-version 99 --hw-version 52 --sd-req 0x0103 --key-file priv.pem $(target).zip; \
	)
	@echo "Done."

# -------------------------------------------------------------------------
# OTA packages without nrfutil, needs only python3 and the cryptography module
# Output goes to build/ so the released .zip files are never overwritten
# -------------------------------------------------------------------------
packages: $(TARGETS)
	@echo "------------------------------------------------"
	@echo "Generating OTA Packages (.zip) with tools/mkdfu.py ..."
	@echo "------------------------------------------------"
	$(foreach target, $(TARGETS), \
		python3 tools/mkdfu.py $(OUTPUT_DIRECTORY)/$(target).bin $(OUTPUT_DIRECTORY)/$(target).zip; )

# -------------------------------------------------------------------------
# Host side tests: compile a driver against stubs and run it on this machine
# -------------------------------------------------------------------------
test:
	@mkdir -p $(OUTPUT_DIRECTORY)
	$(CC_HOST) -std=c11 -Wall -Wextra -Itest/stubs -Isrc/drivers -Iconfig \
		test/test_buttons.c src/drivers/buttons.c -o $(OUTPUT_DIRECTORY)/test_buttons
	@$(OUTPUT_DIRECTORY)/test_buttons
	$(CC_HOST) -std=c11 -Wall -Wextra -DCLASSIFY_HOST_TEST -DSETTINGS_HOST_TEST \
		-DFLASH_INT_PAGE_SIZE_HOST=4096 -Iconfig -Isrc/app -Isrc/radio \
		test/test_classify.c src/app/classify.c src/app/settings.c \
		-o $(OUTPUT_DIRECTORY)/test_classify
	@$(OUTPUT_DIRECTORY)/test_classify
	$(CC_HOST) -std=c11 -Wall -Wextra -Isrc/radio \
		test/test_sweep_order.c src/radio/sweep_order.c \
		-o $(OUTPUT_DIRECTORY)/test_sweep_order
	@$(OUTPUT_DIRECTORY)/test_sweep_order
	$(CC_HOST) -std=c11 -Wall -Wextra -Iconfig -Isrc/app -Isrc/radio -Isrc/gfx -Isrc/drivers \
		test/test_spectrum_render.c src/gfx/gfx.c src/gfx/font3x5.c src/gfx/font5x7.c \
		src/app/channels.c -o $(OUTPUT_DIRECTORY)/test_spectrum_render
	@$(OUTPUT_DIRECTORY)/test_spectrum_render
	$(CC_HOST) -std=c11 -Wall -Wextra -Isrc/radio \
		test/test_ble.c src/radio/ble_adv.c src/radio/ble_sensor.c src/radio/ble_devtab.c \
		-o $(OUTPUT_DIRECTORY)/test_ble
	@$(OUTPUT_DIRECTORY)/test_ble
	@python3 test/test_blsettings.py

CC_HOST ?= cc

TEMPLATE_PATH := $(SDK_ROOT)/components/toolchain/gcc
include $(TEMPLATE_PATH)/Makefile.common

$(foreach target, $(TARGETS), $(call define_target, $(target)))

.PHONY: flash settings packages test

# Flash over SWD with a J-Link, application plus a matching bootloader
# settings page, so the device still boots and still accepts OTA afterwards.
# Usage:
#   make flash                          (flashes PixlAnalyzerOLED)
#   make flash VARIANT=PixlAnalyzerLCD
flash: $(VARIANT)
	tools/flash_jlink.sh $(VARIANT)

# Same, through OpenOCD (J-Link by default, OPENOCD_IF=... for another probe)
flash-openocd: $(VARIANT)
	tools/flash_openocd.sh $(VARIANT)

# The settings page on its own, e.g. to inspect it before flashing
settings: $(VARIANT)
	python3 tools/mkblsettings.py $(OUTPUT_DIRECTORY)/$(VARIANT).bin \
		$(OUTPUT_DIRECTORY)/$(VARIANT)-settings.hex

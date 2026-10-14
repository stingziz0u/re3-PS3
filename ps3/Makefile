#---------------------------------------------------------------------------------
# re3-PS3 -- re3 (GTA III) for PS3 homebrew (PSL1GHT), librw with an RSX backend
#
#   make -C ps3 STAGE=1     headless test: loads the game and runs it without
#                           video/audio/pad, all to the log  -> re3-ps3-stage1.pkg
#   make -C ps3             the game (stage 2)               -> re3-ps3-<version>.pkg
#   make -C ps3 DEBUG=1     debug build (-g, asserts)        -> ...-debug.pkg
#   make -C ps3 FTRACE=1    with the function trace of peds, weapons, collision,
#                           HUD, radar... dumped to the log at a crash
#                                                            -> ...-ftrace.pkg
#   make -C ps3 verify      patch level of the tree and of the built PKG
#   make -C ps3 crash LOG=<re3-ps3.log>
#                           where the [crash] lines of a log are in the code
#   make -C ps3 clean       remove obj/ and build/ (not the .pkg files)
#
# Each variant has its own obj/ and build/ folder: switching STAGE, DEBUG or
# FTRACE does not need a make clean.
#---------------------------------------------------------------------------------

ifeq ($(strip $(PS3DEV)),)
  $(error "Set PS3DEV in your environment: export PS3DEV=/usr/local/ps3dev")
endif
ifeq ($(strip $(PSL1GHT)),)
  export PSL1GHT := $(PS3DEV)
endif

VERSION := 1.2-alpha
STAGE   ?= 2
DEBUG   ?= 0
FTRACE  ?= 0

APPID   := RE3PS3000
ifeq ($(STAGE),1)
  TITLE   := re3 (stage 1)
  VARIANT := stage1
else
  TITLE   := re3
  VARIANT := $(VERSION)
endif
ifeq ($(DEBUG),1)
  VARIANT := $(VARIANT)-debug
endif
ifeq ($(FTRACE),1)
  VARIANT := $(VARIANT)-ftrace
endif

ROOT     := $(abspath $(CURDIR)/..)
TARGET   := re3-ps3
OUT_PKG  := $(TARGET)-$(VARIANT).pkg
OBJDIR   := $(CURDIR)/obj/$(VARIANT)
BUILDDIR := $(CURDIR)/build/$(VARIANT)
PKGFILES := $(BUILDDIR)/pkgfiles

# Patch level of the tree (ps3/PATCHLEVEL, bumped by every patch): goes to the
# first log line and to USRDIR/VERSION.TXT.
PATCHLEVEL := $(shell cat $(CURDIR)/PATCHLEVEL 2>/dev/null || echo 0)

include $(PSL1GHT)/ppu_rules

# after the include: ppu_rules sets ICON0 with ?= (PSL1GHT's default icon
# until the port has its own)
ifneq ($(wildcard $(CURDIR)/ICON0.PNG),)
  ICON0 := $(CURDIR)/ICON0.PNG
endif

#---------------------------------------------------------------------------------
# Sources
#---------------------------------------------------------------------------------
RW  := $(ROOT)/vendor/librw/src
SRC := $(ROOT)/src

LIBRW_SRCS := $(addprefix $(RW)/, \
  anim.cpp base.cpp bmp.cpp camera.cpp charset.cpp clump.cpp engine.cpp error.cpp \
  frame.cpp geometry.cpp geoplg.cpp hanim.cpp image.cpp light.cpp matfx.cpp \
  pipeline.cpp plg.cpp png.cpp prim.cpp raster.cpp render.cpp skin.cpp texture.cpp \
  tga.cpp tristrip.cpp userdata.cpp uvanim.cpp world.cpp \
  d3d/d3d8.cpp d3d/d3d8matfx.cpp d3d/d3d8render.cpp d3d/d3d8skin.cpp \
  d3d/d3d9.cpp d3d/d3d9matfx.cpp d3d/d3d9render.cpp d3d/d3d9skin.cpp \
  d3d/d3d.cpp d3d/d3ddevice.cpp d3d/d3dimmed.cpp d3d/d3drender.cpp \
  d3d/xbox.cpp d3d/xboxmatfx.cpp d3d/xboxskin.cpp d3d/xboxvfmt.cpp \
  gl/gl3.cpp gl/gl3device.cpp gl/gl3immed.cpp gl/gl3matfx.cpp gl/gl3pipe.cpp \
  gl/gl3raster.cpp gl/gl3render.cpp gl/gl3shader.cpp gl/gl3skin.cpp gl/wdgl.cpp \
  ps2/pds.cpp ps2/ps2.cpp ps2/ps2device.cpp ps2/ps2matfx.cpp ps2/ps2raster.cpp ps2/ps2skin.cpp \
  ps3/ps3device.cpp ps3/ps3raster.cpp ps3/ps3mem.cpp ps3/ps3video.cpp ps3/ps3pipe.cpp \
  ps3/ps3immed.cpp ps3/ps3vmx.cpp \
  lodepng/lodepng.cpp)

# every game directory, minus the PC platform layers and the audio backends
GAME_DIRS := animation audio buildings collision control core entities extras \
  fakerw math modelinfo objects peds render rw save skel text vehicles weapons
GAME_SRCS := $(foreach d,$(GAME_DIRS),$(wildcard $(SRC)/$(d)/*.cpp))
GAME_SRCS := $(filter-out \
  $(SRC)/core/CdStream.cpp $(SRC)/core/CdStreamPosix.cpp \
  $(SRC)/audio/sampman_oal.cpp $(SRC)/audio/sampman_miles.cpp \
  $(SRC)/extras/custompipes_d3d9.cpp $(SRC)/extras/custompipes_gl.cpp, $(GAME_SRCS))

PS3_SRCS := $(addprefix $(SRC)/skel/ps3/, \
  ps3.cpp ps3_log.cpp ps3_fs.cpp ps3_pad.cpp ps3_cdstream.cpp ps3_prof.cpp ps3_autotest.cpp)

GAME_INCS := $(foreach d,$(GAME_DIRS),-I$(SRC)/$(d)) -I$(SRC)/skel/ps3 -I$(SRC)/audio/eax

#---------------------------------------------------------------------------------
# Flags
#---------------------------------------------------------------------------------
# re3 and librw were written on x86 (char signed); PPU is big-endian, LP64.
COMMON_FLAGS := -O2 -mcpu=cell -mtune=cell -mno-altivec -fno-math-errno -fno-trapping-math -fsigned-char -fno-strict-aliasing -fno-common \
  -MMD -MP -Wall -Wno-unused -Wno-unused-variable -Wno-unused-but-set-variable \
  -Wno-missing-braces -Wno-sign-compare -Wno-parentheses -Wno-multichar \
  -Wno-char-subscripts -Wno-unknown-pragmas -Wno-switch -Wno-narrowing \
  -Wno-class-memaccess -Wno-misleading-indentation \
  -D__PS3__ -D__lv2ppu__ -DBIGENDIAN -DLIBRW -DRW_PS3 \
  -DPS3_STAGE=$(STAGE) -DPS3_APPID=\"$(APPID)\" \
  -D_GLIBCXX11_USE_C99_STDIO \
  $(LIBPSL1GHT_INC) -I$(PORTLIBS)/include

# stage 2: the RSX renderer of librw's PS3 backend
ifneq ($(STAGE),1)
  COMMON_FLAGS += -DRW_PS3_RSX -DAUDIO_PS3
endif

# The whole EBOOT shares one 64 KB TOC: -mminimal-toc gives each object its
# own TOC block (see the check target).
COMMON_FLAGS += -mminimal-toc

# re3 was written for MSVC, which doesn't exploit undefined behaviour the way
# GCC does. reVC-PS3's Riot crash was a loop that read one past an array
# before its bound check: GCC took the read as proof the loop ends inside the
# array and dropped the check. These keep GCC's code close to what MSVC made:
# loop bounds aren't derived from UB, null checks after a dereference stay,
# and signed overflow wraps.
COMMON_FLAGS += -fno-aggressive-loop-optimizations -fno-delete-null-pointer-checks -fwrapv

ifeq ($(DEBUG),1)
  COMMON_FLAGS += -g -DPS3_DEBUG
else
  COMMON_FLAGS += -DNDEBUG
endif
COMMON_FLAGS += $(EXTRA_CFLAGS)

LIBRW_FLAGS := $(COMMON_FLAGS) -I$(RW) -I$(ROOT)/vendor/librw
GAME_FLAGS  := $(COMMON_FLAGS) -I$(ROOT)/vendor/librw -I$(SRC) $(GAME_INCS) -I$(OBJDIR) \
  -DGTA3_PS3

# libc calls with relative paths and the game's stdout/stderr (see ps3_fs.cpp,
# ps3_log.cpp)
WRAP := fopen remove rename mkdir stat opendir unlink \
  printf vprintf puts putchar fprintf vfprintf fputs
WRAPFLAGS := $(foreach w,$(WRAP),-Wl,--wrap=$(w))

# --no-multi-toc: ppu-gcc 7.2 has no -mcmodel, so a TOC over 64 KB makes ld
# split it and emit r2off stubs, which hang the PS3 at boot. Fail the link
# instead.
LDFLAGS := -Wl,--no-multi-toc $(WRAPFLAGS) $(LIBPSL1GHT_LIB) -L$(PORTLIBS)/lib
LIBS    := -llv2dbg -lio -lsysmodule -lsysutil -lrt -llv2 -lm
ifneq ($(STAGE),1)
  LIBS  := -lrsx -lgcm_sys -laudio $(LIBS)
endif

#---------------------------------------------------------------------------------
# Objects
#---------------------------------------------------------------------------------
LIBRW_OBJS := $(patsubst $(ROOT)/%.cpp,$(OBJDIR)/%.o,$(LIBRW_SRCS))
GAME_OBJS  := $(patsubst $(ROOT)/%.cpp,$(OBJDIR)/%.o,$(GAME_SRCS) $(PS3_SRCS)) \
              $(OBJDIR)/GitSHA1.o

ELF := $(OBJDIR)/$(TARGET).elf

# Diagnostics (FTRACE=1): the code where crashes are most likely is built
# with a function entry/exit trace (ps3_log.cpp keeps the last 16384 events and
# dumps them when the game thread dies). The small math/entity/list helpers
# are left out. It costs a little CPU on every call, so the release is
# built without it; the crumbs stay in both.
FTRACE_DIRS := audio vehicles peds weapons collision render/Shadows render/Particle core/World \
  render/Hud core/Radar render/Font render/Sprite2d render/SpecialFX text/Messages \
  control/Darkel control/Pickups control/Garages animation/AnimManager \
  animation/RpAnimBlend animation/AnimBlendAssociation animation/AnimBlendClumpData \
  core/References core/EventList core/Accident core/Fire entities/Physical \
  render/Glass render/WaterCannon core/Ropes render/Skidmarks render/Antennas render/Rubbish \
  render/Coronas render/PointLights render/Fluff weapons/WeaponEffects
FTRACE_EXCLUDE := math/ Vector Matrix Quaternion General.h Timer.h Rect.h templates.h \
  common.h Lists.h Placeable.h Entity.h Physical.h PtrList.h PtrNode.h ColPoint.h \
  ColModel.h ColTriangle.h ColBox.h ColSphere.h ColLine.h CompressedVector.h \
  World.h Pools.h ModelInfo.h Camera.h Pad.h Stats.h librw /include/ c++/
comma := ,
FTRACE_ALL := $(filter $(foreach d,$(FTRACE_DIRS),$(OBJDIR)/src/$(d)%),$(GAME_OBJS))
ifeq ($(FTRACE),1)
$(FTRACE_ALL): OBJ_FLAGS := -finstrument-functions \
  -finstrument-functions-exclude-file-list=$(subst $() ,$(comma),$(strip $(FTRACE_EXCLUDE)))
endif
# the flags live here: rebuild those objects when this file changes, or when a
# tree built with the trace on is built without it
$(FTRACE_ALL): $(CURDIR)/Makefile $(OBJDIR)/ftrace.mode
$(OBJDIR)/ftrace.mode: FORCE
	@mkdir -p $(dir $@)
	@echo 'ftrace $(FTRACE)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp
.DEFAULT_GOAL := all

#---------------------------------------------------------------------------------
# Rules
#---------------------------------------------------------------------------------
.PHONY: all clean check verify crash pkgfiles FORCE

all: $(OUT_PKG)

$(OUT_PKG): $(OBJDIR)/$(TARGET).pkg
	@cp $(OBJDIR)/$(TARGET).gnpdrm.pkg $@
	@echo "==> $@"

# ppu_rules: %.pkg <- %.self <- %.elf. PKGFILES is refreshed on every build:
# the folders the user fills are created empty.
$(OBJDIR)/$(TARGET).pkg: pkgfiles

# the GTA III folders the user copies from the PC game (created empty)
DATA_DIRS := anim audio data models mp3 skins text txd
# re3's own files (not in the PC game): PlayStation button icons for the
# on-screen help, and the DualShock 3 picture of the gamepad settings page
RE3_FILES := ps3btns.txd frontend_ds3.txd

pkgfiles:
	@rm -rf $(PKGFILES)
	@mkdir -p $(PKGFILES)/USRDIR/userfiles
	@for d in $(DATA_DIRS); do mkdir -p $(PKGFILES)/USRDIR/$$d; done
	@for f in $(RE3_FILES); do cp $(ROOT)/gamefiles/models/$$f $(PKGFILES)/USRDIR/models/; done
	@printf "%s\n%s\npatch %s\nbuilt %s\n" "$(TITLE)" "$(TARGET) $(VARIANT)" "$(PATCHLEVEL)" \
	  "$$(date '+%Y-%m-%d %H:%M')" > $(PKGFILES)/USRDIR/VERSION.TXT

$(ELF): $(LIBRW_OBJS) $(GAME_OBJS)
	@echo "LD $(notdir $@)"
	@$(CXX) $(COMMON_FLAGS) $(LDFLAGS) $(LIBRW_OBJS) $(GAME_OBJS) $(LIBS) -o $@
	@$(MAKE) --no-print-directory check

# TOC guard (see LDFLAGS): .got must stay under 64 KB and no r2off stubs.
check:
	@got=$$(ppu-readelf -SW $(ELF) | awk '$$2==".got"{print $$6}'); \
	 stubs=$$(ppu-nm $(ELF) | grep -c r2off || true); \
	 echo "   .got = 0x$$got (limit 0x10000), r2off stubs = $$stubs"; \
	 if [ $$((0x$$got)) -ge $$((0x10000)) ] || [ "$$stubs" != "0" ]; then \
	   echo "ERROR: TOC over 64 KB -- this EBOOT would hang at boot"; exit 1; fi

# rewritten only when the patch level changes, so ps3.cpp recompiles exactly then
$(OBJDIR)/ps3_rev.h: FORCE
	@mkdir -p $(dir $@)
	@echo '#define PS3_PATCHLEVEL "$(PATCHLEVEL)"' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

$(OBJDIR)/src/skel/ps3/ps3.o: $(OBJDIR)/ps3_rev.h

$(OBJDIR)/GitSHA1.cpp:
	@mkdir -p $(dir $@)
	@echo '#define GIT_SHA1 "re3-ps3 patch $(PATCHLEVEL)"' > $@
	@echo 'const char* g_GIT_SHA1 = GIT_SHA1;' >> $@

$(OBJDIR)/GitSHA1.o: $(OBJDIR)/GitSHA1.cpp $(OBJDIR)/flags.mode
	@$(CXX) $(GAME_FLAGS) -c $< -o $@

# Every object depends on the compiler flags: a tree built with other flags
# (before patch 41) is rebuilt without a make clean.
$(OBJDIR)/flags.mode: FORCE
	@mkdir -p $(dir $@)
	@echo '$(subst ','',$(GAME_FLAGS) $(LIBRW_FLAGS))' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

$(OBJDIR)/vendor/%.o: $(ROOT)/vendor/%.cpp $(OBJDIR)/flags.mode
	@mkdir -p $(dir $@)
	@echo "CXX $(patsubst $(ROOT)/%,%,$<)"
	@$(CXX) $(LIBRW_FLAGS) $(VMX_FLAGS) -c $< -o $@

# AltiVec (VMX): only the files that use it (mip levels, the hardware probe's test)
$(OBJDIR)/vendor/librw/src/ps3/ps3vmx.o: VMX_FLAGS := -maltivec

$(OBJDIR)/src/%.o: $(SRC)/%.cpp $(OBJDIR)/flags.mode
	@mkdir -p $(dir $@)
	@echo "CXX $(patsubst $(ROOT)/%,%,$<)"
	@$(CXX) $(GAME_FLAGS) $(OBJ_FLAGS) -c $< -o $@

# Which patch level the tree and the built PKG are at
verify:
	@echo "tree : patch $(PATCHLEVEL)"
	@if [ -f $(OUT_PKG) ]; then \
	   echo "pkg  : patch $$(sed -n 's/^patch //p' $(PKGFILES)/USRDIR/VERSION.TXT 2>/dev/null), $(OUT_PKG) built $$(date -r $(OUT_PKG) '+%Y-%m-%d %H:%M')"; \
	 else echo "pkg  : $(OUT_PKG) not built yet"; fi

# The [crash] lines carry the address of the code that left each step
# (@xxxxxxxx) and the [ftrace] lines the last calls: function names from this
# build's ELF (addr2line can't read PPU64 function symbols, see the script).
crash:
	@if [ -z "$(LOG)" ]; then echo "usage: make -C ps3 crash LOG=<re3-ps3.log>"; exit 1; fi
	@python3 $(CURDIR)/tools/crashaddr.py $(ELF) "$(LOG)"

clean:
	@rm -rf obj build
	@echo "clean (the .pkg files are kept)"

-include $(LIBRW_OBJS:.o=.d) $(GAME_OBJS:.o=.d)

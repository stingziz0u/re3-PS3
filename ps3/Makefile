#---------------------------------------------------------------------------------
# re3-PS3 -- re3 (GTA III) for PS3 homebrew (PSL1GHT), librw with an RSX backend
#
#   make -C ps3 STAGE=1     headless test: loads the game and runs it without
#                           video/audio/pad, all to the log  -> re3-ps3-stage1.pkg
#   make -C ps3             the game (stage 2)               -> re3-ps3-<version>.pkg
#   make -C ps3 DEBUG=1     debug build (-g, asserts)        -> ...-debug.pkg
#   make -C ps3 verify      patch level of the tree and of the built PKG
#   make -C ps3 clean       remove obj/ and build/ (not the .pkg files)
#
# Each variant has its own obj/ and build/ folder: switching STAGE or DEBUG
# does not need a make clean.
#---------------------------------------------------------------------------------

ifeq ($(strip $(PS3DEV)),)
  $(error "Set PS3DEV in your environment: export PS3DEV=/usr/local/ps3dev")
endif
ifeq ($(strip $(PSL1GHT)),)
  export PSL1GHT := $(PS3DEV)
endif

VERSION := 1.0-alpha
STAGE   ?= 2
DEBUG   ?= 0

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
  ps3/ps3immed.cpp \
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
LIBS    := -lio -lsysmodule -lsysutil -lrt -llv2 -lm
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

#---------------------------------------------------------------------------------
# Rules
#---------------------------------------------------------------------------------
.PHONY: all clean check verify pkgfiles FORCE

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

$(OBJDIR)/GitSHA1.o: $(OBJDIR)/GitSHA1.cpp
	@$(CXX) $(GAME_FLAGS) -c $< -o $@

$(OBJDIR)/vendor/%.o: $(ROOT)/vendor/%.cpp
	@mkdir -p $(dir $@)
	@echo "CXX $(patsubst $(ROOT)/%,%,$<)"
	@$(CXX) $(LIBRW_FLAGS) -c $< -o $@

$(OBJDIR)/src/%.o: $(SRC)/%.cpp
	@mkdir -p $(dir $@)
	@echo "CXX $(patsubst $(ROOT)/%,%,$<)"
	@$(CXX) $(GAME_FLAGS) -c $< -o $@

# Which patch level the tree and the built PKG are at
verify:
	@echo "tree : patch $(PATCHLEVEL)"
	@if [ -f $(OUT_PKG) ]; then \
	   echo "pkg  : patch $$(sed -n 's/^patch //p' $(PKGFILES)/USRDIR/VERSION.TXT 2>/dev/null), $(OUT_PKG) built $$(date -r $(OUT_PKG) '+%Y-%m-%d %H:%M')"; \
	 else echo "pkg  : $(OUT_PKG) not built yet"; fi

clean:
	@rm -rf obj build
	@echo "clean (the .pkg files are kept)"

-include $(LIBRW_OBJS:.o=.d) $(GAME_OBJS:.o=.d)

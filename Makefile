#---------------------------------------------------------------------------------
# Clear the implicit built in rules
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

include $(PSL1GHT)/ppu_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing extra header files
#---------------------------------------------------------------------------------
TARGET		:=	ps3_health
BUILD		:=	build
SOURCES		:=	source
INCLUDES	:=
PKGFILES	:=	$(CURDIR)/pkgfiles
SFOXML		:=	$(CURDIR)/sfo.xml

TITLE		:=	PS3 Health
APPID		:=	HDSH00001
CONTENTID	:=	EP0001-$(APPID)_00-0000000000000000

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------

CFLAGS		=	-O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)

LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

# qrcodegen's asserts would put this folder's path into the ELF (__FILE__;
# gcc 7.2 has no -fmacro-prefix-map): its object is built without them
qrcodegen.o: CFLAGS += -DNDEBUG

#---------------------------------------------------------------------------------
# any extra libraries we wish to link with the project: ps3gfx first, GNU ld resolves left to right
#---------------------------------------------------------------------------------
LIBS	:=	-lps3gfx -lgcm_sys -lio -lm -lsysutil -lnet -lnetctl -laudio -lsysmodule

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib: ps3gfx, installed there by sdk_setup.sh
#---------------------------------------------------------------------------------
LIBDIRS	:=	$(PSL1GHT)/portlibs/ppu

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

export BUILDDIR	:=	$(CURDIR)/$(BUILD)

#---------------------------------------------------------------------------------
# automatically build a list of object files for our project
#---------------------------------------------------------------------------------
CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))

#---------------------------------------------------------------------------------
# link with CC (ppu-gcc): base_rules' LD ?= would keep make's built-in LD, ld
#---------------------------------------------------------------------------------
export LD	:=	$(CC)

export OFILES	:=	$(CFILES:.c=.o)

#---------------------------------------------------------------------------------
# build a list of include paths
#---------------------------------------------------------------------------------
export INCLUDE	:=	$(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					$(LIBPSL1GHT_INC) \
					-I$(CURDIR)/$(BUILD)

#---------------------------------------------------------------------------------
# build a list of library paths
#---------------------------------------------------------------------------------
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
					$(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) clean

#---------------------------------------------------------------------------------
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) *.elf *.self *.pkg

#---------------------------------------------------------------------------------
pkg:	$(BUILD) $(OUTPUT).pkg

#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
# The CEX self is not used: the pkg rule signs the ELF again with make_self_npdrm.
# The macOS ARM64 make_self crashes with a bus error when make runs it, so this
# keeps only the strip and sprxlinker steps of ppu_rules. The empty .self is a
# stamp for ppu_rules' "%.pkg: %.self"; the real executable is
# build/pkg/USRDIR/EBOOT.BIN (there is no ps3load target: the stamp is empty).
$(OUTPUT).self: $(OUTPUT).elf
	@mkdir -p $(BUILDDIR)
	@$(STRIP) $< -o $(BUILDDIR)/$(notdir $<)
	@$(SPRX) $(BUILDDIR)/$(notdir $<)
	@touch $@
$(OUTPUT).elf:	$(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

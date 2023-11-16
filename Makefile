# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
OUTPUT := .output
CLANG ?= clang
LLVM_STRIP ?= llvm-strip
LIBBPF_SRC := $(abspath libbpf/src)
BPFTOOL_SRC := $(abspath bpftool/src)
LIBBPF_OBJ := $(abspath $(OUTPUT)/libbpf.a)
BPFTOOL_OUTPUT ?= $(abspath $(OUTPUT)/bpftool)
BPFTOOL ?= $(BPFTOOL_OUTPUT)/bootstrap/bpftool
# LIBBLAZESYM_SRC := $(abspath blazesym/)
# LIBBLAZESYM_OBJ := $(abspath $(OUTPUT)/libblazesym.a)
# LIBBLAZESYM_HEADER := $(abspath $(OUTPUT)/blazesym.h)
ARCH := $(shell uname -m | sed 's/x86_64/x86/' | sed 's/aarch64/arm64/' | sed 's/ppc64le/powerpc/' | sed 's/mips.*/mips/')
VMLINUX := vmlinux/$(ARCH)/vmlinux.h
# Use our own libbpf API headers and Linux UAPI headers distributed with
# libbpf to avoid dependency on system-wide headers, which could be missing or
# outdated
INCLUDES := -I$(OUTPUT) -I../../libbpf/include/uapi -I$(dir $(VMLINUX))
CFLAGS := -g -Wall
ALL_LDFLAGS := $(LDFLAGS) $(EXTRA_LDFLAGS)

DESTDIR  =
SBINPATH = /usr/sbin
SYSDPATH = /lib/systemd/system

# APPS = minimal minimal_legacy bootstrap uprobe netatop fentry usdt sockfilter tc
APPS = netatop
 
# CARGO ?= $(shell which cargo)
# ifeq ($(strip $(CARGO)),)
# BZS_APPS :=
# else
# BZS_APPS := profile
# APPS += $(BZS_APPS)
# # Required by libblazesym
# ALL_LDFLAGS += -lrt -ldl -lpthread -lm
# endif

# Get Clang's default includes on this system. We'll explicitly add these dirs
# to the includes list when compiling with `-target bpf` because otherwise some
# architecture-specific dirs will be "missing" on some architectures/distros -
# headers such as asm/types.h, asm/byteorder.h, asm/socket.h, asm/sockios.h,
# sys/cdefs.h etc. might be missing.
#
# Use '-idirafter': Don't interfere with include mechanics except where the
# build would have failed anyways.
CLANG_BPF_SYS_INCLUDES = $(shell $(CLANG) -v -E - </dev/null 2>&1 \
	| sed -n '/<...> search starts here:/,/End of search list./{ s| \(/.*\)|-idirafter \1|p }')

ifeq ($(V),1)
	Q =
	msg =
else
	Q = @
	msg = @printf '  %-8s %s%s\n'					\
		      "$(1)"						\
		      "$(patsubst $(abspath $(OUTPUT))/%,%,$(2))"	\
		      "$(if $(3), $(3))";
	MAKEFLAGS += --no-print-directory
endif

define allow-override
  $(if $(or $(findstring environment,$(origin $(1))),\
            $(findstring command line,$(origin $(1)))),,\
    $(eval $(1) = $(2)))
endef

$(call allow-override,CC,$(CROSS_COMPILE)cc)
$(call allow-override,LD,$(CROSS_COMPILE)ld)

.PHONY: all
all: $(APPS)

.PHONY: clean
clean:
	$(call msg,CLEAN)
	$(Q)rm -rf $(OUTPUT) netatop

$(OUTPUT) $(OUTPUT)/libbpf $(BPFTOOL_OUTPUT):
	$(call msg,MKDIR,$@)
	$(Q)mkdir -p $@

# Build libbpf
$(LIBBPF_OBJ): $(wildcard $(LIBBPF_SRC)/*.[ch] $(LIBBPF_SRC)/Makefile) | $(OUTPUT)/libbpf
	$(call msg,LIB,$@)
	$(Q)$(MAKE) -C $(LIBBPF_SRC) BUILD_STATIC_ONLY=1		      \
		    OBJDIR=$(dir $@)/libbpf DESTDIR=$(dir $@)		      \
		    INCLUDEDIR= LIBDIR= UAPIDIR=			      \
		    install

# Build bpftool
$(BPFTOOL): | $(BPFTOOL_OUTPUT)
	$(call msg,BPFTOOL,$@)
	$(Q)$(MAKE) ARCH= CROSS_COMPILE= OUTPUT=$(BPFTOOL_OUTPUT)/ -C $(BPFTOOL_SRC) bootstrap


# $(LIBBLAZESYM_SRC)/target/release/libblazesym.a::
# 	$(Q)cd $(LIBBLAZESYM_SRC) && $(CARGO) build --features=cheader --release

# $(LIBBLAZESYM_OBJ): $(LIBBLAZESYM_SRC)/target/release/libblazesym.a | $(OUTPUT)
# 	$(call msg,LIB, $@)
# 	$(Q)cp $(LIBBLAZESYM_SRC)/target/release/libblazesym.a $@

# $(LIBBLAZESYM_HEADER): $(LIBBLAZESYM_SRC)/target/release/libblazesym.a | $(OUTPUT)
# 	$(call msg,LIB,$@)
# 	$(Q)cp $(LIBBLAZESYM_SRC)/target/release/blazesym.h $@

# Build BPF code
$(OUTPUT)/%.bpf.o: %.bpf.c $(LIBBPF_OBJ) $(wildcard %.h) $(VMLINUX) | $(OUTPUT)
	$(call msg,BPF,$@)
	$(Q)$(CLANG) -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH) $(INCLUDES) $(CLANG_BPF_SYS_INCLUDES) -c $(filter %.c,$^) -o $@
	$(Q)$(LLVM_STRIP) -g $@ # strip useless DWARF info

# Generate BPF skeletons
$(OUTPUT)/%.skel.h: $(OUTPUT)/%.bpf.o | $(OUTPUT) $(BPFTOOL)
	$(call msg,GEN-SKEL,$@)
	$(Q)$(BPFTOOL) gen skeleton $< > $@

# Build user-space code
$(patsubst %,$(OUTPUT)/%.o,$(APPS)): %.o: %.skel.h

$(OUTPUT)/%.o: %.c $(wildcard %.h) | $(OUTPUT)
	$(call msg,CC,$@)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $(filter %.c,$^) -o $@

$(patsubst %,$(OUTPUT)/%.o,$(APPS)): $(LIBBLAZESYM_HEADER)

# $(BZS_APPS): $(LIBBLAZESYM_OBJ)

OBJ1 := $(OUTPUT)/server.o
OBJ2 := $(OUTPUT)/deal.o

${OBJ1}: server.c server.h
	$(CC) -c server.c -o ${OBJ1}
	
${OBJ2}: deal.c deal.h
	$(CC) -c deal.c -o ${OBJ2}

# Build application binary
$(APPS): %: $(OUTPUT)/%.o ${OBJ1} ${OBJ2} $(LIBBPF_OBJ) | $(OUTPUT) 
	$(call msg,BINARY,$@)
	$(Q)$(CC) $(CFLAGS) $^ $(ALL_LDFLAGS) -lelf -lz -o $@

# # Build application binary
# $(APPS): %: $(OUTPUT)/%.o $(LIBBPF_OBJ) | $(OUTPUT)
# 	$(call msg,BINARY,$@)
# 	$(Q)$(CC) deal.c server.c $(CFLAGS) $^ $(ALL_LDFLAGS) -lelf -lz -o $@

# delete failed targets
.DELETE_ON_ERROR:

# keep intermediate (.skel.h, .bpf.o, etc) targets
.SECONDARY:

install:
		if [ ! -d $(DESTDIR)$(SBINPATH) ]; 		\
		then mkdir -p $(DESTDIR)$(SBINPATH); fi
		if [ ! -d $(DESTDIR)$(SYSDPATH) ]; 			\
		then	mkdir -p $(DESTDIR)$(SYSDPATH); fi

		cp netatop-bpf.service $(DESTDIR)$(SYSDPATH)/netatop-bpf.service
		chmod 0644             $(DESTDIR)$(SYSDPATH)/netatop-bpf.service
		
		cp netatop   		$(DESTDIR)$(SBINPATH)/netatop
		chmod 0711 		$(DESTDIR)$(SBINPATH)/netatop

		if [ -z "$(DESTDIR)" -a -f /bin/systemctl ]; 		\
		then	/bin/systemctl disable --now netatop-bpf     2> /dev/null; \
			/bin/systemctl daemon-reload;			\
			/bin/systemctl enable  --now netatop-bpf;		\
		fi
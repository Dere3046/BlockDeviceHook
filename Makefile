obj-m := hbd.o

hbd-y := src/main.o \
	lib/hbd_core.o lib/hbd_shadow.o lib/hbd_io.o lib/hbd_del.o \
	deps/LKMhook/lib/hk.o deps/LKMhook/lib/hk_ksym.o \
	deps/LKMhook/lib/hk_patch.o deps/LKMhook/lib/hk_flush.o \
	deps/LKMhook/lib/hk_ptr.o deps/LKMhook/lib/hk_inline.o \
	deps/LKMhook/lib/hk_kprobe.o deps/LKMhook/lib/hk_kretprobe.o \
	deps/KallRecon/lib/core.o deps/KallRecon/lib/slide.o \
	deps/KallRecon/lib/anchor.o

ccflags-y += -std=gnu11
ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wno-unused-variable
ccflags-y += -Wno-unused-function
ccflags-y += -Wno-strict-prototypes
ccflags-y += -I$(src)/lib
ccflags-y += -I$(src)/deps/LKMhook/lib
ccflags-y += -I$(src)/deps/KallRecon/lib

KDIR := $(KDIR)
MDIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ODIR := $(MDIR)/out/$(VER)

$(info -- KDIR: $(KDIR))
$(info -- MDIR: $(MDIR))
$(info -- ODIR: $(ODIR))

all:
	make -C $(KDIR) M=$(ODIR) src=$(MDIR) modules

clean:
	make -C $(KDIR) M=$(ODIR) src=$(MDIR) clean

$(obj)/%.o: $(src)/%.c $(recordmcount_source) FORCE
	$(call if_changed_rule,cc_o_c)
	$(call cmd,force_checksrc)

$(obj)/%.o: $(src)/%.S FORCE
	$(call if_changed_rule,as_o_S)

# musbfix - out-of-tree kernel module
#
# Builds against the running kernel's headers. The module includes a few
# internal drivers/usb/musb/*.h headers (not shipped in linux-headers), so the
# `headers` target fetches the matching versions from kernel.org stable for the
# running kernel's base version. They are compiled against the kernel's own
# .config, so struct offsets match the running build.

KREL ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KREL)/build

# Strip the Armbian/distro suffix: 6.18.35-current-sunxi -> 6.18.35
KVER := $(shell echo $(KREL) | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+')

HDR_DIR  := hdr
BASE_URL := https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/drivers/usb/musb
MUSB_HDRS := musb_core.h musb_gadget.h musb_host.h musb_regs.h musb_io.h \
             musb_dma.h musb_debug.h musb_trace.h tusb6010.h cppi_dma.h

obj-m += musbfix.o
ccflags-y += -I$(src)/$(HDR_DIR)

all: headers
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

# Fetch the internal musb headers for the running kernel version (idempotent).
headers:
	@mkdir -p $(HDR_DIR)
	@for h in $(MUSB_HDRS); do \
		if [ ! -s $(HDR_DIR)/$$h ]; then \
			echo "  FETCH   $$h (v$(KVER))"; \
			wget -q "$(BASE_URL)/$$h?h=v$(KVER)" -O $(HDR_DIR)/$$h || \
				echo "  WARN    could not fetch $$h (may be optional)"; \
		fi; \
	done

clean:
	-$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -rf $(HDR_DIR)

.PHONY: all headers clean

obj-m += katana_usb_audio.o 

PWD := $(CURDIR)
KDIR := /lib/modules/$(shell uname -r)/build
UDEV_RULES_DIR := /etc/udev/rules.d
MODULE_DIR := /lib/modules/$(shell uname -r)/extra

katana_usb_audio-objs := src/card.o src/control.o src/pcm.o src/usb.o src/katana_usb_audio.o

all:
	make -C $(KDIR) M=$(PWD) modules

clean: 
	make -C $(KDIR) M=$(PWD) clean

install: all
	@echo "Installing Katana USB Audio driver..."
	mkdir -p $(MODULE_DIR)
	cp katana_usb_audio.ko $(MODULE_DIR)/
	depmod -a
	@echo "Installing udev rule for driver priority..."
	cp 99-katana-usb-audio.rules $(UDEV_RULES_DIR)/
	udevadm control --reload-rules
	@echo "Installation complete!"
	@echo ""
	@echo "To load the driver now, run:"
	@echo "  sudo modprobe katana_usb_audio"
	@echo ""
	@echo "To unload conflicting snd-usb-audio driver:"
	@echo "  sudo modprobe -r snd_usb_audio"
	@echo "  sudo modprobe katana_usb_audio"

uninstall:
	@echo "Uninstalling Katana USB Audio driver..."
	modprobe -r katana_usb_audio 2>/dev/null || true
	rm -f $(MODULE_DIR)/katana_usb_audio.ko
	rm -f $(UDEV_RULES_DIR)/99-katana-usb-audio.rules
	depmod -a
	udevadm control --reload-rules
	@echo "Uninstall complete!"

dkms-install:
	@echo "Installing via DKMS..."
	@chmod +x install-dkms.sh
	@./install-dkms.sh

dkms-uninstall:
	@echo "Uninstalling from DKMS..."
	@chmod +x uninstall-dkms.sh
	@./uninstall-dkms.sh

dkms-status:
	@echo "DKMS status for katana-usb-audio:"
	@dkms status katana-usb-audio 2>/dev/null || echo "Package not found in DKMS"

.PHONY: all clean install uninstall dkms-install dkms-uninstall dkms-status
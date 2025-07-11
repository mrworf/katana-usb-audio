import usb.core
dev = usb.core.find(idVendor=0x041e, idProduct=0x3247)
dev.detach_kernel_driver(1)  # Detach only interface 0
dev.detach_kernel_driver(2)  # Detach only interface 0
dev.detach_kernel_driver(0)  # Detach only interface 0

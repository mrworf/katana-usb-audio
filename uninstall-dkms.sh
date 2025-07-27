#!/bin/bash

# Katana USB Audio Driver DKMS Uninstallation Script
# This script removes the driver from DKMS

set -e

PACKAGE_NAME="katana-usb-audio"
PACKAGE_VERSION="1.0"
MODULE_DIR="/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"
UDEV_RULES_DIR="/etc/udev/rules.d"

echo "Uninstalling Katana USB Audio Driver from DKMS..."

# Check if running as root
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root (use sudo)" 
   exit 1
fi

# Check if DKMS is installed
if ! command -v dkms &> /dev/null; then
    echo "DKMS is not installed, but will continue with manual cleanup..."
fi

# Unload the module if it's loaded
echo "Unloading katana_usb_audio module..."
modprobe -r katana_usb_audio 2>/dev/null || true

# Remove from DKMS if present
if command -v dkms &> /dev/null && dkms status | grep -q "$PACKAGE_NAME"; then
    echo "Removing from DKMS..."
    dkms remove "$PACKAGE_NAME/$PACKAGE_VERSION" --all 2>/dev/null || true
fi

# Remove source directory
if [ -d "$MODULE_DIR" ]; then
    echo "Removing source directory: $MODULE_DIR"
    rm -rf "$MODULE_DIR"
fi

# Remove udev rules
if [ -f "$UDEV_RULES_DIR/99-katana-usb-audio.rules" ]; then
    echo "Removing udev rules..."
    rm -f "$UDEV_RULES_DIR/99-katana-usb-audio.rules"
    udevadm control --reload-rules
fi

# Update module dependencies
echo "Updating module dependencies..."
depmod -a

echo ""
echo "✅ DKMS uninstallation complete!"
echo ""
echo "The katana-usb-audio driver has been completely removed."
echo "The generic snd-usb-audio driver will now handle USB audio devices." 
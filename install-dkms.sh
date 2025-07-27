#!/bin/bash

# Katana USB Audio Driver DKMS Installation Script
# This script installs the driver using DKMS for automatic rebuilding on kernel updates

set -e

PACKAGE_NAME="katana-usb-audio"
PACKAGE_VERSION="1.0"
MODULE_DIR="/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"
UDEV_RULES_DIR="/etc/udev/rules.d"

echo "Installing Katana USB Audio Driver via DKMS..."

# Check if running as root
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root (use sudo)" 
   exit 1
fi

# Check if DKMS is installed
if ! command -v dkms &> /dev/null; then
    echo "Error: DKMS is not installed. Please install dkms package first."
    echo "On Ubuntu/Debian: sudo apt install dkms"
    echo "On Fedora/RHEL: sudo dnf install dkms"
    echo "On Arch: sudo pacman -S dkms"
    exit 1
fi

# Remove existing installation if present
if dkms status | grep -q "$PACKAGE_NAME"; then
    echo "Removing existing DKMS installation..."
    dkms remove "$PACKAGE_NAME/$PACKAGE_VERSION" --all 2>/dev/null || true
fi

# Remove old source directory if it exists
if [ -d "$MODULE_DIR" ]; then
    echo "Removing old source directory..."
    rm -rf "$MODULE_DIR"
fi

# Create source directory
echo "Creating source directory: $MODULE_DIR"
mkdir -p "$MODULE_DIR"

# Copy source files
echo "Copying source files..."
cp -r src/ "$MODULE_DIR/"
cp Makefile "$MODULE_DIR/"
cp dkms.conf "$MODULE_DIR/"
cp 99-katana-usb-audio.rules "$MODULE_DIR/"

# Add to DKMS
echo "Adding module to DKMS..."
dkms add "$PACKAGE_NAME/$PACKAGE_VERSION"

# Build the module
echo "Building module with DKMS..."
dkms build "$PACKAGE_NAME/$PACKAGE_VERSION"

# Install the module
echo "Installing module with DKMS..."
dkms install "$PACKAGE_NAME/$PACKAGE_VERSION"

# Install udev rules
echo "Installing udev rules..."
cp 99-katana-usb-audio.rules "$UDEV_RULES_DIR/"
udevadm control --reload-rules

# Unload conflicting driver if loaded
if lsmod | grep -q "snd_usb_audio"; then
    echo "Unloading conflicting snd-usb-audio driver..."
    modprobe -r snd_usb_audio 2>/dev/null || true
fi

# Load the new driver
echo "Loading katana_usb_audio driver..."
modprobe katana_usb_audio

echo ""
echo "✅ DKMS installation complete!"
echo ""
echo "The driver will now automatically rebuild when you install new kernels."
echo ""
echo "To check installation status:"
echo "  dkms status"
echo ""
echo "To manually load/unload the driver:"
echo "  sudo modprobe katana_usb_audio     # Load"
echo "  sudo modprobe -r katana_usb_audio  # Unload"
echo ""
echo "To uninstall, run: sudo ./uninstall-dkms.sh" 
# SoundBlasterX Katana Linux USB Audio Driver

A specialized Linux kernel driver for the Creative SoundBlaster X Katana USB speaker system. This driver provides full ALSA integration with proper volume controls and audio streaming, replacing the limited functionality of the generic snd-usb-audio driver.

**Current state**: Full functionality including USB audio streaming, hardware volume controls, mute functionality, and proper ALSA integration.

## Features

- ✅ Hardware volume control with mute/unmute control
- ✅ PulseAudio/PipeWire integration
- ✅ Proper audio format support
- ✅ Stereo output (2 channels)
- ✅ Automatic driver priority over snd-usb-audio using udev rules


## Credit

All this work was inspired by [Print3M](https://github.com/Print3M) who created [pyusb-katana-driver](https://github.com/Print3M/pyusb-katana-driver) and later [katana-usb-audio](https://github.com/Print3M/katana-usb-audio) which inspired me. 

While Linux's default `snd-usb-audio` "works" with Katana, you aren't able to leverage the hardware audio control, and the buttons on the soundbar would both change the hardware amp (because of the direct connection) and send USB HID events (essentially keypresses) emulating volume up/down. 

The lack of working hardware buttons wasn't really a big issue, but the fact that the hardware amplifier wasn't being control by Linux would cause quite the frustration. 

## Installation

### DKMS Installation (Recommended)
DKMS (Dynamic Kernel Module Support) automatically rebuilds the driver when you install new kernels, ensuring continuous functionality across kernel updates.

**Benefits of DKMS:**
- ✅ Automatic rebuilding on kernel updates
- ✅ No manual intervention required after system updates  
- ✅ Persistent installation across reboots
- ✅ Easy management with standard DKMS commands

```bash
# Install DKMS if not already installed
# Ubuntu/Debian: sudo apt install dkms
# Fedora/RHEL: sudo dnf install dkms  
# Arch: sudo pacman -S dkms

# Install the driver via DKMS
sudo ./install-dkms.sh

# Alternative: Use Makefile target
sudo make dkms-install
```

This will:
- Install the driver using DKMS for automatic kernel compatibility
- Install udev rules for driver priority
- Automatically rebuild on kernel updates
- Load the driver immediately

To uninstall DKMS version:
```bash
sudo ./uninstall-dkms.sh

# Alternative: Use Makefile target
sudo make dkms-uninstall
```

### Quick Install (Traditional)
```bash
make
sudo make install
```

This will:
- Build the kernel module
- Install it to `/lib/modules/$(uname -r)/extra/`
- Install udev rules for driver priority
- Update module dependencies

**Note**: Traditional installation requires manual rebuilding after kernel updates.

### Manual Installation
```bash
make                                # Build the kernel module
sudo insmod katana_usb_audio.ko     # Load module manually
sudo lsmod | grep katana            # Check if loaded
```

### Loading the Driver
```bash
# If snd-usb-audio is already loaded for the Katana:
sudo modprobe -r snd_usb_audio
sudo modprobe katana_usb_audio

# Or simply:
sudo modprobe katana_usb_audio
```

### DKMS Management
If you installed via DKMS, you can use these commands to manage the driver:

```bash
# Check DKMS status
dkms status
# or
make dkms-status

# Manually rebuild (usually not needed)
sudo dkms build katana-usb-audio/1.0
sudo dkms install katana-usb-audio/1.0

# Load/unload driver
sudo modprobe katana_usb_audio     # Load
sudo modprobe -r katana_usb_audio  # Unload

# Check if driver is loaded
lsmod | grep katana
```

### Uninstalling
```bash
sudo make uninstall
```

## Usage

```bash
make                                # Build the kernel module
sudo insmod katana_usb_audio.ko     # Load module
sudo lsmod | grep katana            # Check if loaded
make clean                          # Clean build files
sudo rmmod katana_usb_audio         # Remove module
sudo dmesg                          # Read kernel logs
```

## Audio Controls

The driver provides the following ALSA controls:
- **PCM Playback Volume**: Controls the output volume (0-100%)
- **PCM Playback Switch**: Mutes/unmutes the audio output

You can control these using:
```bash
# Volume control
amixer -c katana-usb-audio sset "PCM Playback Volume" 50%

# Mute control
amixer -c katana-usb-audio sset "PCM Playback Switch" off
amixer -c katana-usb-audio sset "PCM Playback Switch" on
```

### Volume resolution

While the Katana shows volumes as "absolutes" ranging from 0 to 50, they're inversely logarithmic, meaning that when alsa makes volume changes in the lower range, the visual indicator on the bar may not change. This is different from Windows and MacOS where the mapping is done based on the USB device's scale, with the drawback of poor granular control at low volumes.

I've not attempted to solve this, since I've often been frustrated with the lack of control at low levels.

## PulseAudio Integration

The driver is automatically detected by PulseAudio and will appear in:
- KDE System Settings → Audio
- PulseAudio Volume Control
- `pactl list short sinks` command

## Troubleshooting

If it doesn't seem to work properly, use `lsusb` and `lsusb -t` to see what driver is powering the katana. If it states `snd-usb-audio`, you've most likely not installed the udev rule and rebooted.

### Common Issues

1. **Device not appearing in PulseAudio**: Ensure the driver is loaded and check kernel logs with `dmesg`
2. **No audio output**: Check if the device is selected as the default audio output in your desktop environment
3. **Volume control not working**: Verify the ALSA controls are accessible with `amixer -c katana-usb-audio contents`

### Debug Information

All driver logs can be seen using the `dmesg` command. The driver provides detailed logging for:
- Device attachment/detachment
- PCM device creation
- Audio control operations
- Playback state changes


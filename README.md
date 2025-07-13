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

### Quick Install (Recommended)
```bash
make
sudo make install
```

This will:
- Build the kernel module
- Install it to `/lib/modules/$(uname -r)/extra/`
- Install udev rules for driver priority
- Update module dependencies

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


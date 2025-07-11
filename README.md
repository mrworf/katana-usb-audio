# SoundBlasterX Katana (Linux Kernel Module) audio control driver

First attempt at implementing the ALSA driver in order to finally get my SoundBlaster X Katana (USB) loudspeaker be handled properly under Linux and to learn how the Linux kernel-space drivers work. This module uses the ALSA and the USB core framework. Because of its educational purpose, there are a lot of comments in the code, which are most most probably not necessary for more experienced driver developers.

**Current state of the project**: The USB driver is attached to the Katana device successfully. It creates an ALSA card structure with proper PCM playback support and volume/mute controls. The driver is now visible in PulseAudio and KDE's audio settings, allowing for volume control, muting, and audio playback.

## Features

- ✅ USB device detection and attachment
- ✅ ALSA sound card creation
- ✅ PCM playback device with proper audio streaming support
- ✅ Volume control (0-100%)
- ✅ Mute/unmute control
- ✅ PulseAudio integration
- ✅ KDE audio device detection
- ✅ Multiple audio format support (S16_LE, S24_LE, S32_LE)
- ✅ Multiple sample rate support (8kHz - 96kHz)
- ✅ Stereo output (2 channels)

## Usage

```bash
make                                # Build the kernel module
sudo insmod katana_usb_audio.ko     # Load module
sudo lsmod | grep katana            # Check if loaded
./test_audio.sh                     # Run comprehensive tests
make clean                          # Clean build files
sudo rmmod katana_usb_audio         # Remove module
sudo dmesg                          # Read kernel logs
```

## Testing

Run the included test script to verify all functionality:

```bash
./test_audio.sh
```

This will:
1. Check if the driver is loaded
2. Verify ALSA device detection
3. Check PulseAudio integration
4. Test volume control
5. Test mute control
6. Attempt audio playback

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

## PulseAudio Integration

The driver is automatically detected by PulseAudio and will appear in:
- KDE System Settings → Audio
- PulseAudio Volume Control
- `pactl list short sinks` command

## Troubleshooting

> **NOTICE**: Sometimes the Linux kernel stubbornly probes the `snd-usb-audio` driver first. Manual removal of this default driver might be helpful (`sudo rmmod snd-usb-audio`). It will be loaded anyway but most probably after probing of this custom driver.

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

## Technical Details

### Supported Audio Formats
- Sample rates: 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000 Hz
- Formats: S16_LE, S24_LE, S32_LE
- Channels: 2 (stereo)
- Buffer size: Up to 32KB
- Periods: 2-8

### USB Interface Handling
- **Interface 0**: Audio Control (volume, mute controls)
- **Interface 1**: Audio Streaming (PCM playback)

## Resources to write USB driver

* [O'Reilly, USB Drivers](https://www.oreilly.com/library/view/linux-device-drivers/0596005903/ch13.html)
* [Linux Kernel docs - Writing USB Device Drivers](https://docs.kernel.org/driver-api/usb/writing_usb_driver.html)
* [Linux Kernel docs - USB Host Side API](https://www.kernel.org/doc/html/latest/driver-api/usb/usb.html)
* [ALSA Driver API docs](https://www.kernel.org/doc/html/latest/sound/kernel-api/alsa-driver-api.html)
* [Writing an ALSA driver](https://www.kernel.org/doc/html/latest/sound/kernel-api/writing-an-alsa-driver.html)

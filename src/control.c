#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/dma-mapping.h>
#include <linux/usb.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include "control.h"

// Global volume range variables (set once during initialization)
static int16_t katana_vol_min = -20480;  // Default fallback
static int16_t katana_vol_max = 0;       // Default fallback
static int16_t katana_vol_res = 1;       // Default fallback
static int katana_vol_range_initialized = 0;

// Removed auto-unmute logic - let ALSA handle mute/unmute properly

// Forward declarations
static int katana_set_hardware_mute(struct usb_device *usb_dev, int mute);

// Get volume range from device using USB Audio Class standard requests
static int katana_get_volume_range(struct usb_device *usb_dev, int16_t *min_vol, int16_t *max_vol, int16_t *res_vol)
{
	int err;
	unsigned char *volume_data;
	dma_addr_t dma_addr;
	
	// Allocate USB coherent memory for control transfer
	volume_data = usb_alloc_coherent(usb_dev, 2, GFP_KERNEL, &dma_addr);
	if (!volume_data) {
		pr_err("Katana Control: Failed to allocate coherent memory for volume range query\n");
		return -ENOMEM;
	}
	
	// Get MIN value
	err = usb_control_msg(usb_dev,
			      usb_rcvctrlpipe(usb_dev, 0),
			      0x82,  // GET_MIN
			      0xA1,  // bmRequestType
			      0x0201, // wValue: Volume Control, channel 1
			      0x0100, // wIndex: Interface 0, Feature Unit 1
			      volume_data,
			      2,
			      1000);
	
	if (err >= 0) {
		*min_vol = volume_data[0] | (volume_data[1] << 8);
		pr_info("Katana Control: Volume MIN = %d (0x%04x)\n", *min_vol, (uint16_t)*min_vol);
	} else {
		pr_warn("Katana Control: Failed to get volume MIN: %d\n", err);
		*min_vol = -20480; // fallback
	}
	
	// Get MAX value
	err = usb_control_msg(usb_dev,
			      usb_rcvctrlpipe(usb_dev, 0),
			      0x83,  // GET_MAX
			      0xA1,  // bmRequestType
			      0x0201, // wValue: Volume Control, channel 1
			      0x0100, // wIndex: Interface 0, Feature Unit 1
			      volume_data,
			      2,
			      1000);
	
	if (err >= 0) {
		*max_vol = volume_data[0] | (volume_data[1] << 8);
		pr_info("Katana Control: Volume MAX = %d (0x%04x)\n", *max_vol, (uint16_t)*max_vol);
	} else {
		pr_warn("Katana Control: Failed to get volume MAX: %d\n", err);
		*max_vol = 0; // fallback
	}
	
	// Get RES value
	err = usb_control_msg(usb_dev,
			      usb_rcvctrlpipe(usb_dev, 0),
			      0x84,  // GET_RES
			      0xA1,  // bmRequestType
			      0x0201, // wValue: Volume Control, channel 1
			      0x0100, // wIndex: Interface 0, Feature Unit 1
			      volume_data,
			      2,
			      1000);
	
	if (err >= 0) {
		*res_vol = volume_data[0] | (volume_data[1] << 8);
		pr_info("Katana Control: Volume RES = %d (0x%04x)\n", *res_vol, (uint16_t)*res_vol);
	} else {
		pr_warn("Katana Control: Failed to get volume RES: %d\n", err);
		*res_vol = 1; // fallback
	}
	
	// Update global variables
	katana_vol_min = *min_vol;
	katana_vol_max = *max_vol;
	katana_vol_res = *res_vol;
	katana_vol_range_initialized = 1;
	
	usb_free_coherent(usb_dev, 2, volume_data, dma_addr);
	return 0;
}

// Set raw hardware volume value
static int katana_set_hardware_volume_raw(struct usb_device *usb_dev, int16_t volume_value)
{
	int err;
	unsigned char *volume_data;
	dma_addr_t dma_addr;
	
	// Initialize volume range if not done already
	if (!katana_vol_range_initialized) {
		int16_t min_vol, max_vol, res_vol;
		pr_info("Katana Control: Initializing volume range...\n");
		katana_get_volume_range(usb_dev, &min_vol, &max_vol, &res_vol);
	}
	
	// Allocate USB coherent memory for control transfer
	volume_data = usb_alloc_coherent(usb_dev, 2, GFP_KERNEL, &dma_addr);
	if (!volume_data) {
		pr_err("Katana Control: Failed to allocate coherent memory for volume control\n");
		return -ENOMEM;
	}
	
	// Pack volume value into 2-byte little-endian format
	volume_data[0] = volume_value & 0xff;
	volume_data[1] = (volume_value >> 8) & 0xff;
	
	// Send SET_CUR request for volume control
	// USB Audio Class 1.0 specification: SET_CUR request for Feature Unit
	// bmRequestType: 0x21 = Class request, Interface recipient, Host-to-device
	// bRequest: 0x01 = SET_CUR
	// wValue: (0x02 << 8) | 0x01 = Volume Control (0x02) on channel 1 (left)
	// wIndex: 0x0100 = Interface 0, Feature Unit ID 1 (speaker output)
	err = usb_control_msg(usb_dev,
			      usb_sndctrlpipe(usb_dev, 0),
			      0x01,  // SET_CUR
			      0x21,  // bmRequestType
			      0x0201, // wValue: Volume Control, channel 1
			      0x0100, // wIndex: Interface 0, Feature Unit 1
			      volume_data,
			      2,     // 2 bytes for volume
			      1000); // timeout
	
	if (err < 0) {
		pr_err("Katana Control: Failed to set hardware volume %d: %d\n", volume_value, err);
		usb_free_coherent(usb_dev, 2, volume_data, dma_addr);
		return err;
	}
	
	// Also set right channel (channel 2)
	err = usb_control_msg(usb_dev,
			      usb_sndctrlpipe(usb_dev, 0),
			      0x01,  // SET_CUR
			      0x21,  // bmRequestType
			      0x0202, // wValue: Volume Control, channel 2
			      0x0100, // wIndex: Interface 0, Feature Unit 1
			      volume_data,
			      2,     // 2 bytes for volume
			      1000); // timeout
	
	if (err < 0) {
		pr_err("Katana Control: Failed to set hardware volume right channel %d: %d\n", volume_value, err);
		usb_free_coherent(usb_dev, 2, volume_data, dma_addr);
		return err;
	}
	
	pr_info("Katana Control: Set raw hardware volume to %d (0x%04x)\n", volume_value, (uint16_t)volume_value);
	
	usb_free_coherent(usb_dev, 2, volume_data, dma_addr);
	return 0;
}

// USB Audio Class hardware volume control functions (legacy percentage interface)
static int katana_set_hardware_volume(struct usb_device *usb_dev, int volume_percent)
{
	int err;
	unsigned char *volume_data;
	dma_addr_t dma_addr;
	
	// Initialize volume range if not done already
	if (!katana_vol_range_initialized) {
		int16_t min_vol, max_vol, res_vol;
		pr_info("Katana Control: Initializing volume range...\n");
		katana_get_volume_range(usb_dev, &min_vol, &max_vol, &res_vol);
	}
	
	// Allocate USB coherent memory for control transfer
	volume_data = usb_alloc_coherent(usb_dev, 2, GFP_KERNEL, &dma_addr);
	if (!volume_data) {
		pr_err("Katana Control: Failed to allocate coherent memory for volume control\n");
		return -ENOMEM;
	}
	
	// Convert percentage (0-100) to 16-bit signed volume value using device range
	int16_t volume_value;
	
	if (volume_percent <= 0) {
		volume_value = katana_vol_min;
	} else if (volume_percent >= 100) {
		volume_value = katana_vol_max;
	} else {
		// Linear scaling from min to max using device range
		int32_t raw_value = katana_vol_min + (volume_percent * (katana_vol_max - katana_vol_min)) / 100;
		
		// Quantize to device resolution (steps of katana_vol_res)
		if (katana_vol_res > 1) {
			// Round to nearest multiple of resolution
			int32_t steps = (raw_value - katana_vol_min + katana_vol_res/2) / katana_vol_res;
			volume_value = katana_vol_min + (steps * katana_vol_res);
		} else {
			volume_value = raw_value;
		}
	}
	
	// Pack volume value into 2-byte little-endian format
	volume_data[0] = volume_value & 0xff;
	volume_data[1] = (volume_value >> 8) & 0xff;
	
	// Send SET_CUR request for volume control
	// USB Audio Class 1.0 specification: SET_CUR request for Feature Unit
	// bmRequestType: 0x21 = Class request, Interface recipient, Host-to-device
	// bRequest: 0x01 = SET_CUR
	// wValue: (0x02 << 8) | 0x01 = Volume Control (0x02) on channel 1 (left)
	// wIndex: 0x0100 = Interface 0, Feature Unit ID 1 (speaker output)
	err = usb_control_msg(usb_dev,
			      usb_sndctrlpipe(usb_dev, 0),
			      0x01,  // SET_CUR
			      0x21,  // bmRequestType
			      0x0201, // wValue: Volume Control, channel 1
			      0x0100, // wIndex: Interface 0, Feature Unit 1
			      volume_data,
			      2,     // 2 bytes for volume
			      1000); // timeout
	
	if (err < 0) {
		pr_err("Katana Control: Failed to set hardware volume %d%%: %d\n", volume_percent, err);
		usb_free_coherent(usb_dev, 2, volume_data, dma_addr);
		return err;
	}
	
	// Also set right channel (channel 2)
	err = usb_control_msg(usb_dev,
			      usb_sndctrlpipe(usb_dev, 0),
			      0x01,  // SET_CUR
			      0x21,  // bmRequestType
			      0x0202, // wValue: Volume Control, channel 2
			      0x0100, // wIndex: Interface 0, Feature Unit 1
			      volume_data,
			      2,     // 2 bytes for volume
			      1000); // timeout
	
	if (err < 0) {
		pr_err("Katana Control: Failed to set hardware volume right channel %d%%: %d\n", volume_percent, err);
		usb_free_coherent(usb_dev, 2, volume_data, dma_addr);
		return err;
	}
	
	pr_info("Katana Control: Set hardware volume to %d%% (0x%04x) [range: %d to %d, res: %d]\n", 
		volume_percent, (uint16_t)volume_value, katana_vol_min, katana_vol_max, katana_vol_res);
	
	// If setting a non-zero volume, try to unmute the device
	if (volume_percent > 0) {
		pr_info("Katana Control: Auto-unmuting device for non-zero volume\n");
		katana_set_hardware_mute(usb_dev, 0); // unmute
	}
	
	usb_free_coherent(usb_dev, 2, volume_data, dma_addr);
	return 0;
}

// Get raw hardware volume value (not percentage)
static int16_t katana_get_hardware_volume_raw(struct usb_device *usb_dev)
{
	int err;
	unsigned char *volume_data;
	dma_addr_t dma_addr;
	
	// Initialize volume range if not done already
	if (!katana_vol_range_initialized) {
		int16_t min_vol, max_vol, res_vol;
		pr_info("Katana Control: Initializing volume range...\n");
		katana_get_volume_range(usb_dev, &min_vol, &max_vol, &res_vol);
	}
	
	// Allocate USB coherent memory for control transfer
	volume_data = usb_alloc_coherent(usb_dev, 2, GFP_KERNEL, &dma_addr);
	if (!volume_data) {
		pr_err("Katana Control: Failed to allocate coherent memory for volume control\n");
		return katana_vol_min; // Return minimum on error
	}
	
	// Send GET_CUR request for volume control
	// USB Audio Class 1.0 specification: GET_CUR request for Feature Unit
	// bmRequestType: 0xA1 = Class request, Interface recipient, Device-to-host
	// bRequest: 0x81 = GET_CUR
	// wValue: (0x02 << 8) | 0x01 = Volume Control (0x02) on channel 1 (left)
	// wIndex: 0x0100 = Interface 0, Feature Unit ID 1 (speaker output)
	err = usb_control_msg(usb_dev,
			      usb_rcvctrlpipe(usb_dev, 0),
			      0x81,  // GET_CUR
			      0xA1,  // bmRequestType
			      0x0201, // wValue: Volume Control, channel 1
			      0x0100, // wIndex: Interface 0, Feature Unit 1
			      volume_data,
			      2,     // 2 bytes for volume
			      1000); // timeout
	
	if (err < 0) {
		pr_err("Katana Control: Failed to get hardware volume: %d\n", err);
		usb_free_coherent(usb_dev, 2, volume_data, dma_addr);
		return katana_vol_min; // Return minimum on error
	}
	
	// Return raw 16-bit signed volume value
	int16_t volume_value = volume_data[0] | (volume_data[1] << 8);
	
	pr_info("Katana Control: Got raw hardware volume 0x%04x (%d)\n", 
		(uint16_t)volume_value, volume_value);
	usb_free_coherent(usb_dev, 2, volume_data, dma_addr);
	return volume_value;
}

// Get hardware volume using USB Audio Class control requests (returns percentage)
static int katana_get_hardware_volume(struct usb_device *usb_dev)
{
	int err;
	unsigned char *volume_data;
	dma_addr_t dma_addr;
	int volume_percent;
	
	// Initialize volume range if not done already
	if (!katana_vol_range_initialized) {
		int16_t min_vol, max_vol, res_vol;
		pr_info("Katana Control: Initializing volume range...\n");
		katana_get_volume_range(usb_dev, &min_vol, &max_vol, &res_vol);
	}
	
	// Allocate USB coherent memory for control transfer
	volume_data = usb_alloc_coherent(usb_dev, 2, GFP_KERNEL, &dma_addr);
	if (!volume_data) {
		pr_err("Katana Control: Failed to allocate coherent memory for volume control\n");
		return -1;
	}
	
	// Send GET_CUR request for volume control
	// USB Audio Class 1.0 specification: GET_CUR request for Feature Unit
	// bmRequestType: 0xA1 = Class request, Interface recipient, Device-to-host
	// bRequest: 0x81 = GET_CUR
	// wValue: (0x02 << 8) | 0x01 = Volume Control (0x02) on channel 1 (left)
	// wIndex: 0x0100 = Interface 0, Feature Unit ID 1 (speaker output)
	err = usb_control_msg(usb_dev,
			      usb_rcvctrlpipe(usb_dev, 0),
			      0x81,  // GET_CUR
			      0xA1,  // bmRequestType
			      0x0201, // wValue: Volume Control, channel 1
			      0x0100, // wIndex: Interface 0, Feature Unit 1
			      volume_data,
			      2,     // 2 bytes for volume
			      1000); // timeout
	
	if (err < 0) {
		pr_err("Katana Control: Failed to get hardware volume: %d\n", err);
		usb_free_coherent(usb_dev, 2, volume_data, dma_addr);
		return -1; // Return error as negative value
	}
	
	// Convert from 16-bit signed volume value to percentage using device range
	int16_t volume_value = volume_data[0] | (volume_data[1] << 8);
	
	if (volume_value <= katana_vol_min) {
		volume_percent = 0; // Minimum volume
	} else if (volume_value >= katana_vol_max) {
		volume_percent = 100; // Maximum volume
	} else {
		// Linear scaling from min to max using device range
		volume_percent = ((volume_value - katana_vol_min) * 100) / (katana_vol_max - katana_vol_min);
	}
	
	pr_info("Katana Control: Got hardware volume %d%% (0x%04x) [raw: %d, range: %d to %d]\n", 
		volume_percent, (uint16_t)volume_value, volume_value, katana_vol_min, katana_vol_max);
	usb_free_coherent(usb_dev, 2, volume_data, dma_addr);
	return volume_percent;
}

// Set hardware mute using USB Audio Class control requests
static int katana_set_hardware_mute(struct usb_device *usb_dev, int mute)
{
	int err;
	unsigned char *mute_data;
	dma_addr_t dma_addr;
	
	// Allocate USB coherent memory for control transfer
	mute_data = usb_alloc_coherent(usb_dev, 1, GFP_KERNEL, &dma_addr);
	if (!mute_data) {
		pr_err("Katana Control: Failed to allocate coherent memory for mute control\n");
		return -ENOMEM;
	}
	
	// Convert mute value: device uses inverted logic (0 = muted, 1 = unmuted)
	mute_data[0] = mute ? 0 : 1;
	
	// Send SET_CUR request for mute control
	// USB Audio Class 1.0 specification: SET_CUR request for Feature Unit
	// bmRequestType: 0x21 = Class request, Interface recipient, Host-to-device
	// bRequest: 0x01 = SET_CUR
	// wValue: (0x01 << 8) | 0x00 = Mute Control (0x01) on channel 0 (master)
	// wIndex: 0x0100 = Interface 0, Feature Unit ID 1 (speaker output)
	err = usb_control_msg(usb_dev,
			      usb_sndctrlpipe(usb_dev, 0),
			      0x01,  // SET_CUR
			      0x21,  // bmRequestType
			      0x0100, // wValue: Mute Control, channel 0 (master)
			      0x0100, // wIndex: Interface 0, Feature Unit 1
			      mute_data,
			      1,     // 1 byte for mute
			      1000); // timeout
	
	if (err < 0) {
		pr_err("Katana Control: Failed to set hardware mute %d: %d\n", mute, err);
		usb_free_coherent(usb_dev, 1, mute_data, dma_addr);
		return err;
	}
	
	pr_info("Katana Control: Set hardware mute to %s (sent %d to device)\n", mute ? "ON" : "OFF", mute_data[0]);
	usb_free_coherent(usb_dev, 1, mute_data, dma_addr);
	return 0;
}

// Get hardware mute using USB Audio Class control requests
static int katana_get_hardware_mute(struct usb_device *usb_dev)
{
	int err;
	unsigned char *mute_data;
	dma_addr_t dma_addr;
	int mute;
	
	// Allocate USB coherent memory for control transfer
	mute_data = usb_alloc_coherent(usb_dev, 1, GFP_KERNEL, &dma_addr);
	if (!mute_data) {
		pr_err("Katana Control: Failed to allocate coherent memory for mute control\n");
		return -1;
	}
	
	// Send GET_CUR request for mute control
	// USB Audio Class 1.0 specification: GET_CUR request for Feature Unit
	// bmRequestType: 0xA1 = Class request, Interface recipient, Device-to-host
	// bRequest: 0x81 = GET_CUR
	// wValue: (0x01 << 8) | 0x00 = Mute Control (0x01) on channel 0 (master)
	// wIndex: 0x0100 = Interface 0, Feature Unit ID 1 (speaker output)
	err = usb_control_msg(usb_dev,
			      usb_rcvctrlpipe(usb_dev, 0),
			      0x81,  // GET_CUR
			      0xA1,  // bmRequestType
			      0x0100, // wValue: Mute Control, channel 0 (master)
			      0x0100, // wIndex: Interface 0, Feature Unit 1
			      mute_data,
			      1,     // 1 byte for mute
			      1000); // timeout
	
	if (err < 0) {
		pr_err("Katana Control: Failed to get hardware mute: %d\n", err);
		usb_free_coherent(usb_dev, 1, mute_data, dma_addr);
		return -1; // Return error as negative value
	}
	
	// Return mute state: device uses inverted logic (0 = muted, 1 = unmuted)
	// Convert to ALSA standard: 1 = muted, 0 = unmuted
	mute = mute_data[0] ? 0 : 1;
	pr_info("Katana Control: Got hardware mute %s (device returned %d)\n", mute ? "ON" : "OFF", mute_data[0]);
	usb_free_coherent(usb_dev, 1, mute_data, dma_addr);
	return mute;
}

// Helper function to get USB device from control
static struct usb_device *get_usb_device_from_control(struct snd_kcontrol *kctl)
{
	struct snd_card *card = kctl->private_data;
	if (!card || !card->private_data) {
		pr_err("Katana Control: No USB device available\n");
		return NULL;
	}
	return (struct usb_device *)card->private_data;
}

int katana_volume_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct usb_device *usb_dev = get_usb_device_from_control(kctl);
	if (!usb_dev) {
		ucontrol->value.integer.value[0] = 0; // Default value
		return 0;
	}
	
	// Get raw volume from device
	int16_t raw_volume = katana_get_hardware_volume_raw(usb_dev);
	if (raw_volume < katana_vol_min) {
		ucontrol->value.integer.value[0] = 0; // Default on error
		return 0;
	}
	
	// Convert raw volume to ALSA steps
	int alsa_steps = (raw_volume - katana_vol_min) / katana_vol_res;
	
	ucontrol->value.integer.value[0] = alsa_steps;
	pr_debug("Katana Control: Volume get - %d steps (raw: %d)\n", alsa_steps, raw_volume);
	return 0;
}

int katana_volume_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct usb_device *usb_dev = get_usb_device_from_control(kctl);
	if (!usb_dev) {
		return 0;
	}
	
	pr_info("Katana Control: Setting volume to %ld steps\n", ucontrol->value.integer.value[0]);
	
	// Initialize volume range if not done already
	if (!katana_vol_range_initialized) {
		int16_t min_vol, max_vol, res_vol;
		pr_info("Katana Control: Initializing volume range...\n");
		katana_get_volume_range(usb_dev, &min_vol, &max_vol, &res_vol);
	}
	
	int alsa_steps = ucontrol->value.integer.value[0];
	
	// Convert ALSA steps to raw volume value
	int16_t raw_volume = katana_vol_min + (alsa_steps * katana_vol_res);
	
	// Clamp to valid range
	if (raw_volume < katana_vol_min) raw_volume = katana_vol_min;
	if (raw_volume > katana_vol_max) raw_volume = katana_vol_max;
	
	int err = katana_set_hardware_volume_raw(usb_dev, raw_volume);
	
	pr_info("Katana Control: Volume set to %d steps -> raw %d (0x%04x) (result: %d)\n", 
		alsa_steps, raw_volume, (uint16_t)raw_volume, err);
	return (err == 0) ? 1 : 0; // Return 1 if changed successfully
}

int katana_volume_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	/*
		Info control callback:
		Write detailed information of this control functionality into snd_ctl_elem_info
		structure.

		More:
		https://www.kernel.org/doc/html/latest/sound/kernel-api/writing-an-alsa-driver.html#info-callback
	*/
	
	// Initialize volume range if not done already (get USB device from control)
	if (!katana_vol_range_initialized) {
		struct usb_device *usb_dev = get_usb_device_from_control(kctl);
		if (usb_dev) {
			int16_t min_vol, max_vol, res_vol;
			pr_info("Katana Control: Initializing volume range for ALSA info...\n");
			katana_get_volume_range(usb_dev, &min_vol, &max_vol, &res_vol);
		}
	}
	
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	
	// Calculate number of steps based on device resolution
	int steps = (katana_vol_max - katana_vol_min) / katana_vol_res;
	
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = steps;
	
	pr_info("Katana Control: ALSA volume range: 0 to %d steps (device: %d to %d, res: %d)\n",
		steps, katana_vol_min, katana_vol_max, katana_vol_res);

	return 0;
}

// Volume control callbacks
int katana_mute_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct usb_device *usb_dev = get_usb_device_from_control(kctl);
	if (!usb_dev) {
		ucontrol->value.integer.value[0] = 1; // Default value
		return 0;
	}
	
	int mute = katana_get_hardware_mute(usb_dev);
	if (mute < 0) {
		mute = 1; // Default on error
	}
	
	ucontrol->value.integer.value[0] = mute;
	pr_debug("Katana Control: Mute get - %d\n", mute);
	return 0;
}

int katana_mute_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct usb_device *usb_dev = get_usb_device_from_control(kctl);
	if (!usb_dev) {
		return 0;
	}
	
	int new_mute = ucontrol->value.integer.value[0];
	
	pr_info("Katana Control: Setting mute to %d\n", new_mute);
	
	int err = katana_set_hardware_mute(usb_dev, new_mute);
	
	pr_info("Katana Control: Mute set to %d (result: %d)\n", new_mute, err);
	return (err == 0) ? 1 : 0; // Return 1 if changed successfully
}

int katana_mute_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;

	return 0;
}

// Control structure templates
struct snd_kcontrol_new katana_vol_ctl = {
	.iface	       = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name	       = "PCM Playback Volume", // SOURCE - DIRECTION - FUNCTION
	.index	       = 0,
	.access	       = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.get	       = katana_volume_get,
	.put	       = katana_volume_put,
	.info	       = katana_volume_info,
};

struct snd_kcontrol_new katana_mute_ctl = {
	.iface	       = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name	       = "PCM Playback Switch", // Mute control
	.index	       = 0,
	.access	       = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.get	       = katana_mute_get,
	.put	       = katana_mute_put,
	.info	       = katana_mute_info,
};
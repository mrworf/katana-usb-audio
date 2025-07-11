#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/usb.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/pcm.h>

#include "control.h"
#include "usb.h"
#include "card.h"
#include "pcm.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Print3M");
MODULE_DESCRIPTION("Katana USB AudioControl driver");

static struct snd_card *card = NULL;
static int control_interface_ready = 0;
static int stream_interface_ready = 0;

// Define supported devices (Katana only)
static struct usb_device_id usb_table[] = {
	{ USB_DEVICE(KATANA_VENDOR_ID, KATANA_PRODUCT_ID) },
	{} // Terminator
};

// These devices will be probed with this kernel module.
MODULE_DEVICE_TABLE(usb, usb_table);

static int katana_usb_probe(struct usb_interface *iface, const struct usb_device_id *id)
{
	/*
		Determine whether the provided interface is that one we want to work with.
		This function is called for each interface of the matching device.
		Return SUCCESS for the AudioControl interface only.
	*/
	pr_info("======= KATANA PROBE =======\n");

	// Map the device's interface to the device itself and get its data
	struct usb_device *dev = interface_to_usbdev(iface);

	// Exit if this is not the desired interface
	int ifnum = iface->cur_altsetting->desc.bInterfaceNumber;
	dev_info(&iface->dev, "Processing interface %d (looking for %d and %d)\n", 
		 ifnum, AUDIO_CONTROL_IFACE_ID, AUDIO_STREAM_IFACE_ID);
	
	if (ifnum != AUDIO_CONTROL_IFACE_ID && ifnum != AUDIO_STREAM_IFACE_ID) {
		dev_info(&iface->dev, "Wrong interface: %d\n", ifnum);
		goto __error;
	}

	dev_info(&iface->dev, "Attached to USB device %04X:%04X\n", dev->descriptor.idVendor,
		 dev->descriptor.idProduct);

	int err;

	// Create a new ALSA card structure if not already created
	if (card == NULL) {
		// Find first free index for a new ALSA card
		int idx = 0;
		while (snd_card_ref(idx) != NULL) {
			idx++;
		}

		err = snd_card_new(&dev->dev, idx, "katana-usb-audio", THIS_MODULE, 0, &card);
		if (err != 0) {
			dev_err(&iface->dev, "ALSA card creation failed: %d\n", err);
			goto __error;
		}

		// Basic info about the new sound card
		strcpy(card->driver, "katana_ac");
		strcpy(card->shortname, "Katana Audio");
		strcpy(card->longname, "SoundBlaster X Katana");
		card->dev = &dev->dev;

		dev_info(&iface->dev, "New ALSA card created: %s\n", card->longname);
	}

	// Setup Audio Control component
	if (ifnum == AUDIO_CONTROL_IFACE_ID && !control_interface_ready) {
		// Init volume control
		struct snd_kcontrol *kctl_vol = snd_ctl_new1(&katana_vol_ctl, NULL);
		if (kctl_vol == NULL) {
			dev_err(&iface->dev, "Volume control creation failed\n");
			goto __error;
		}

		// Attach volume control
		err = snd_ctl_add(card, kctl_vol);
		if (err != 0) {
			dev_err(&iface->dev, "Adding volume control failed: %d\n", err);
			snd_ctl_free_one(kctl_vol);
			goto __error;
		}

		// Init mute control
		struct snd_kcontrol *kctl_mute = snd_ctl_new1(&katana_mute_ctl, NULL);
		if (kctl_mute == NULL) {
			dev_err(&iface->dev, "Mute control creation failed\n");
			goto __error;
		}

		// Attach mute control
		err = snd_ctl_add(card, kctl_mute);
		if (err != 0) {
			dev_err(&iface->dev, "Adding mute control failed: %d\n", err);
			snd_ctl_free_one(kctl_mute);
			goto __error;
		}

		control_interface_ready = 1;
		dev_info(&iface->dev, "Audio controls added successfully\n");
	}

	// Setup Audio Stream component
	if (ifnum == AUDIO_STREAM_IFACE_ID && !stream_interface_ready) {
		// Create PCM device
		struct snd_pcm *pcm;
		err = katana_pcm_new(card, &pcm);
		if (err != 0) {
			dev_err(&iface->dev, "PCM device creation failed: %d\n", err);
			goto __error;
		}
		
		stream_interface_ready = 1;
		dev_info(&iface->dev, "PCM device created successfully\n");
	}
	
		// Register the card only after both interfaces are ready
	if (control_interface_ready && stream_interface_ready) {
		err = snd_card_register(card);
		if (err != 0) {
			dev_err(&iface->dev, "ALSA card registration failed: %d\n", err);
			goto __error;
		}
		dev_info(&iface->dev, "ALSA card registered successfully with all components\n");
	} else {
		dev_info(&iface->dev, "Interface %d processed, waiting for other interface...\n", ifnum);
	}

	dev_info(&iface->dev, "Everything works. Ifnum: %d\n", ifnum);

	return 0; // SUCCESS - there is a match

__error:
	// Standard error if the driver doesn't want to work with this interface
	return -ENODEV;
}

static void katana_usb_disconnect(struct usb_interface *iface)
{
	/*
		This function is called when the driver is not able already to control the device.
		Its main purpose is to clean everything up after the driver usage.
	*/
	if (card) {
		snd_card_free(card);
		card = NULL;
	}
	control_interface_ready = 0;
	stream_interface_ready = 0;
	
	struct usb_device *dev = interface_to_usbdev(iface);
	dev_info(&dev->dev, "The driver has been disconnected\n");
}

// Main USB driver structure
static struct usb_driver usb_ac_driver = {
	.name	    = "katana_usb_audio",    // Should be unique and the same as the module name
	.probe	    = katana_usb_probe,	     // See if the driver is willing to work with the iface
	.disconnect = katana_usb_disconnect, // Called when the interface is no longer accessible
	.id_table   = usb_table,	     // Required or the driver's probe will never get called
};

/*
	This is a shortcut to avoid boilerplate __init and __exit functions to
	only register (usb_register) and deregister (usb_deregister) the USB driver.
	This helper handles it automatically.
*/
module_usb_driver(usb_ac_driver);
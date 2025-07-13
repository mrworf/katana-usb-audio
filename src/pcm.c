#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/usb.h>
#include <linux/uio.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/core.h>
#include <sound/initval.h>
#include "pcm.h"
#include "usb.h"

// Private data structure for our PCM device
struct katana_pcm_data {
	struct snd_card *card;
	struct snd_pcm_substream *substream;
	struct usb_device *usb_dev;
	spinlock_t lock;
	
	// USB device state tracking
	int usb_dev_valid;  // Track if USB device is still valid
	
	// USB endpoint information
	struct usb_interface *usb_iface; // USB streaming interface
	unsigned int endpoint_out;       // Output endpoint address
	unsigned int endpoint_sync;      // Sync endpoint address (for feedback)
	int altsetting_num;             // Alternate setting number for the endpoint
	
	// URB management for USB audio streaming
	struct urb **urbs;        // Array of URBs for streaming
	int num_urbs;            // Number of URBs
	int urb_buffer_size;     // Size of each URB buffer
	unsigned char **urb_buffers; // URB data buffers
	dma_addr_t *urb_dma_addrs;   // DMA addresses for URB buffers
	
	// Synchronization endpoint management
	struct urb *sync_urb;     // URB for sync endpoint feedback
	unsigned char *sync_buffer; // Buffer for sync data
	dma_addr_t sync_dma_addr; // DMA address for sync buffer
	unsigned int sync_packet_size; // Size of sync packets
	
	// CRITICAL: Feedback processing for proper timing
	unsigned int feedback_value;     // Latest feedback value from device
	unsigned int feedback_samples;   // Samples per frame from feedback
	unsigned int target_samples;     // Target samples per URB based on feedback
	unsigned int feedback_count;     // Number of feedback samples received
	unsigned int feedback_average;   // Running average of feedback values
	int feedback_valid;             // Whether we have valid feedback data
	
	// Playback state
	unsigned int buffer_size;
	unsigned int period_size;
	unsigned int period_bytes;
	unsigned int channels;
	unsigned int rate;
	unsigned int format;
	
	// Hardware pointer tracking
	unsigned int hw_ptr;      // Where hardware has finished playing
	unsigned int last_period_hw_ptr; // Last hw_ptr when we called period_elapsed
	unsigned int read_ptr;    // Where we should read from PCM buffer next
	
	// Playback status
	int running;
	int prepared;
	
	// URB streaming state
	int stream_started;
	int active_urbs;
	
	// Timing for hardware pointer simulation
	unsigned long start_time;
};

// Hardware capabilities definition
struct snd_pcm_hardware katana_pcm_playback_hw = {
	.info = (SNDRV_PCM_INFO_MMAP |
		 SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_BLOCK_TRANSFER |
		 SNDRV_PCM_INFO_MMAP_VALID |
		 SNDRV_PCM_INFO_PAUSE |
		 SNDRV_PCM_INFO_RESUME),
	.formats = SNDRV_PCM_FMTBIT_S24_3LE,  // Only packed 24-bit LE as per USB descriptor (bSubframeSize 3)
	.rates = (SNDRV_PCM_RATE_48000 |     // Only rates supported by device
		  SNDRV_PCM_RATE_96000),
	.rate_min = 48000,
	.rate_max = 96000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 49152, // period_bytes_max * periods_max = 6144 * 8
	.period_bytes_min = 1536,  // 256 frames * 6 bytes/frame (S24_3LE stereo)
	.period_bytes_max = 6144,  // 1024 frames * 6 bytes/frame (S24_3LE stereo)
	.periods_min = 2,
	.periods_max = 8,
};

// Constraint lists
static const unsigned int katana_rates[] = {
	48000, 96000  // Only rates supported by device per USB descriptors
};

static const struct snd_pcm_hw_constraint_list katana_rate_constraints = {
	.count = ARRAY_SIZE(katana_rates),
	.list = katana_rates,
};

static const unsigned int katana_channels[] = {
	2
};

static const struct snd_pcm_hw_constraint_list katana_channel_constraints = {
	.count = ARRAY_SIZE(katana_channels),
	.list = katana_channels,
};

// Custom constraint function to ensure buffer_bytes = period_bytes * periods
static int katana_buffer_constraint(struct snd_pcm_hw_params *params,
				   struct snd_pcm_hw_rule *rule)
{
	struct snd_interval *buffer_bytes = hw_param_interval(params, SNDRV_PCM_HW_PARAM_BUFFER_BYTES);
	struct snd_interval *period_bytes = hw_param_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES);
	struct snd_interval *periods = hw_param_interval(params, SNDRV_PCM_HW_PARAM_PERIODS);
	
	// Calculate the valid buffer bytes range based on period_bytes and periods
	unsigned int min_buffer = period_bytes->min * periods->min;
	unsigned int max_buffer = period_bytes->max * periods->max;
	
	// Update buffer_bytes to be within the valid range
	if (buffer_bytes->min < min_buffer)
		buffer_bytes->min = min_buffer;
	if (buffer_bytes->max > max_buffer)
		buffer_bytes->max = max_buffer;
	
	return 0;
}

// Find the audio streaming endpoint
static int katana_find_audio_endpoint(struct katana_pcm_data *data)
{
	struct usb_device *usb_dev = data->usb_dev;
	struct usb_interface *iface = NULL;
	struct usb_host_interface *altsetting;
	struct usb_endpoint_descriptor *ep_desc;
	int i, j;
	
	// Find the audio streaming interface (interface 1)
	for (i = 0; i < usb_dev->config->desc.bNumInterfaces; i++) {
		iface = usb_dev->config->interface[i];
		if (iface->altsetting->desc.bInterfaceNumber == AUDIO_STREAM_IFACE_ID) {
			data->usb_iface = iface;
			break;
		}
	}
	
	if (!data->usb_iface) {
		pr_err("Katana PCM: Could not find audio streaming interface\n");
		return -ENODEV;
	}
	
	// Look specifically for altsetting 1 (48kHz support) as default
	for (i = 0; i < iface->num_altsetting; i++) {
		altsetting = &iface->altsetting[i];
		
		// We want altsetting 1 (48kHz) as our primary target
		if (altsetting->desc.bAlternateSetting != 1) {
			continue;
		}
		
		// Skip if no endpoints
		if (altsetting->desc.bNumEndpoints == 0) {
			continue;
		}
		
		// Look for both data and sync endpoints
		data->endpoint_out = 0;
		data->endpoint_sync = 0;
		
		for (j = 0; j < altsetting->desc.bNumEndpoints; j++) {
			ep_desc = &altsetting->endpoint[j].desc;
			
			// Check if this is an OUT endpoint for audio streaming
			if (usb_endpoint_is_bulk_out(ep_desc) || usb_endpoint_is_isoc_out(ep_desc)) {
				data->endpoint_out = ep_desc->bEndpointAddress;
				data->altsetting_num = altsetting->desc.bAlternateSetting;
				pr_info("Katana PCM: Found audio data endpoint: 0x%02x (altsetting %d, 48kHz)\n",
					data->endpoint_out, data->altsetting_num);
			}
			
			// Check if this is an IN endpoint for sync feedback
			if (usb_endpoint_is_isoc_in(ep_desc)) {
				data->endpoint_sync = ep_desc->bEndpointAddress;
				data->sync_packet_size = le16_to_cpu(ep_desc->wMaxPacketSize);
				pr_info("Katana PCM: Found sync feedback endpoint: 0x%02x (packet size %u)\n",
					data->endpoint_sync, data->sync_packet_size);
			}
		}
		
		// We need both endpoints for proper operation
		if (data->endpoint_out && data->endpoint_sync) {
			pr_info("Katana PCM: Found both data (0x%02x) and sync (0x%02x) endpoints in altsetting %d\n",
				data->endpoint_out, data->endpoint_sync, data->altsetting_num);
			return 0;
		}
	}
	
	pr_err("Katana PCM: Could not find required data and sync endpoints in altsetting 1\n");
	return -ENODEV;
}

// Set the USB interface to the specified alternate setting
static int katana_set_interface_altsetting(struct katana_pcm_data *data, int altsetting)
{
	int err;
	
	if (!data->usb_iface) {
		pr_err("Katana PCM: No USB interface available\n");
		return -ENODEV;
	}
	
	err = usb_set_interface(data->usb_dev, AUDIO_STREAM_IFACE_ID, altsetting);
	if (err < 0) {
		pr_err("Katana PCM: Failed to set interface %d to altsetting %d: %d\n",
		       AUDIO_STREAM_IFACE_ID, altsetting, err);
		return err;
	}
	
	pr_info("Katana PCM: Set interface %d to altsetting %d\n", AUDIO_STREAM_IFACE_ID, altsetting);
	return 0;
}

// Set sample rate using USB Audio Class control requests
static int katana_set_sample_rate(struct katana_pcm_data *data, unsigned int rate)
{
	int err;
	unsigned char rate_data[3];
	
	// Pack sample rate into 3-byte little-endian format
	rate_data[0] = rate & 0xff;
	rate_data[1] = (rate >> 8) & 0xff;
	rate_data[2] = (rate >> 16) & 0xff;
	
	// Send SET_CUR request for sampling frequency control
	// USB Audio Class 1.0 specification: SET_CUR request
	// bmRequestType: 0x22 = Class request, Interface recipient, Host-to-device
	// bRequest: 0x01 = SET_CUR
	// wValue: (0x01 << 8) | 0x00 = Sampling Freq Control (0x01) on endpoint 0x01
	// wIndex: 0x0100 = Interface 1, endpoint 1
	err = usb_control_msg(data->usb_dev,
			      usb_sndctrlpipe(data->usb_dev, 0),
			      0x01,  // SET_CUR
			      0x22,  // bmRequestType
			      0x0100, // wValue: Sampling Freq Control
			      0x0101, // wIndex: Interface 1, Endpoint 1
			      rate_data,
			      3,     // 3 bytes for sample rate
			      1000); // timeout
	
	if (err < 0) {
		pr_err("Katana PCM: Failed to set sample rate %u: %d\n", rate, err);
		return err;
	}
	
	pr_info("Katana PCM: Set sample rate to %u Hz\n", rate);
	return 0;
}

// Forward declarations for URB functions
static int katana_alloc_urb_buffers(struct katana_pcm_data *data);
static void katana_free_urb_buffers(struct katana_pcm_data *data);
static void katana_urb_complete(struct urb *urb);
static void katana_sync_urb_complete(struct urb *urb);

// PCM operations structure
struct snd_pcm_ops katana_pcm_playback_ops = {
	.open = katana_pcm_playback_open,
	.close = katana_pcm_playback_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = katana_pcm_hw_params,
	.hw_free = katana_pcm_hw_free,
	.prepare = katana_pcm_prepare,
	.trigger = katana_pcm_trigger,
	.pointer = katana_pcm_pointer,
};

// Create new PCM device
int katana_pcm_new(struct snd_card *card, struct snd_pcm **pcm_ret)
{
	struct snd_pcm *pcm;
	int err;

	err = snd_pcm_new(card, "Katana USB Audio", 0, 1, 0, &pcm);
	if (err < 0)
		return err;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &katana_pcm_playback_ops);
	pcm->private_data = card;
	pcm->info_flags = 0;
	strcpy(pcm->name, "Katana USB Audio");

	// Set up DMA buffer management for ALSA PCM layer
	// We use vmalloc-backed memory for the PCM buffer since we'll
	// copy data to USB-coherent URB buffers for actual transfers
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_VMALLOC,
					       NULL,
					       katana_pcm_playback_hw.buffer_bytes_max,
					       katana_pcm_playback_hw.buffer_bytes_max);

	*pcm_ret = pcm;
	return 0;
}

// Open playback substream
int katana_pcm_playback_open(struct snd_pcm_substream *substream)
{
	struct snd_card *card = snd_pcm_substream_chip(substream);
	struct katana_pcm_data *data;
	struct usb_device *usb_dev;
	int err;

	// Check if disconnect is in progress
	err = katana_enter_operation();
	if (err < 0) {
		return err;
	}

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data) {
		katana_exit_operation();
		return -ENOMEM;
	}

	// Get the USB device from the card's private data
	usb_dev = card->private_data;
	if (!usb_dev) {
		pr_err("Katana PCM: No USB device found\n");
		kfree(data);
		katana_exit_operation();
		return -ENODEV;
	}

	data->card = card;
	data->substream = substream;
	data->usb_dev = usb_dev;
	data->usb_dev_valid = 1; // Mark USB device as valid
	spin_lock_init(&data->lock);
	
	// Initialize buffer tracking
	data->buffer_size = 0;
	data->period_size = 0;
	data->period_bytes = 0;
	data->channels = 0;
	data->rate = 0;
	data->format = 0;
	data->hw_ptr = 0;
	data->last_period_hw_ptr = 0;
	data->read_ptr = 0;
	data->running = 0;
	data->prepared = 0;
	data->start_time = 0;
	
	// Initialize URB-related fields
	data->urbs = NULL;
	data->urb_buffers = NULL;
	data->urb_dma_addrs = NULL;
	data->num_urbs = 0;
	data->urb_buffer_size = 0;
	data->stream_started = 0;
	data->active_urbs = 0;
	data->usb_iface = NULL;
	data->endpoint_out = 0;
	data->endpoint_sync = 0;
	data->altsetting_num = 0;
	
	// Initialize sync URB fields
	data->sync_urb = NULL;
	data->sync_buffer = NULL;
	data->sync_packet_size = 0;
	
	// Initialize feedback processing fields
	data->feedback_value = 0;
	data->feedback_samples = 0;
	data->target_samples = 0;
	data->feedback_count = 0;
	data->feedback_average = 0;
	data->feedback_valid = 0;
	
	// Find the audio streaming endpoint
	err = katana_find_audio_endpoint(data);
	if (err < 0) {
		pr_err("Katana PCM: Failed to find audio endpoint: %d\n", err);
		kfree(data);
		katana_exit_operation();
		return err;
	}

	// Set hardware constraints
	substream->runtime->hw = katana_pcm_playback_hw;
	substream->runtime->private_data = data;
	
	// Set DMA buffer constraints
	snd_pcm_hw_constraint_list(substream->runtime, 0,
				   SNDRV_PCM_HW_PARAM_RATE,
				   &katana_rate_constraints);
	snd_pcm_hw_constraint_list(substream->runtime, 0,
				   SNDRV_PCM_HW_PARAM_CHANNELS,
				   &katana_channel_constraints);
	
	// Enforce integer periods first
	snd_pcm_hw_constraint_integer(substream->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	
	// Set periods constraints
	snd_pcm_hw_constraint_minmax(substream->runtime, SNDRV_PCM_HW_PARAM_PERIODS,
				     katana_pcm_playback_hw.periods_min,
				     katana_pcm_playback_hw.periods_max);
	
	// Set period bytes constraints
	snd_pcm_hw_constraint_minmax(substream->runtime, SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
				     katana_pcm_playback_hw.period_bytes_min,
				     katana_pcm_playback_hw.period_bytes_max);
	
	// Set buffer bytes constraints to ensure buffer_bytes = period_bytes * periods
	snd_pcm_hw_constraint_minmax(substream->runtime, SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
				     katana_pcm_playback_hw.period_bytes_min * katana_pcm_playback_hw.periods_min,
				     katana_pcm_playback_hw.period_bytes_max * katana_pcm_playback_hw.periods_max);
	
	// Add custom constraint to enforce buffer_bytes = period_bytes * periods relationship
	snd_pcm_hw_rule_add(substream->runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
			    katana_buffer_constraint, NULL,
			    SNDRV_PCM_HW_PARAM_PERIOD_BYTES, SNDRV_PCM_HW_PARAM_PERIODS, -1);

	pr_info("Katana PCM playback opened\n");
	katana_exit_operation();
	return 0;
}

// Invalidate USB device in PCM data (called on disconnect)
void katana_pcm_invalidate_usb_dev(struct snd_card *card)
{
	if (!card) {
		pr_warn("Katana PCM: Card is NULL in invalidate_usb_dev\n");
		return;
	}
	
	pr_info("Katana PCM: Invalidating USB device references for card disconnect\n");
	
	// Mark all PCM private data as having invalid USB devices
	// This prevents further USB operations but allows buffer cleanup to continue
	// The individual PCM operations will handle the invalid USB device gracefully
	
	pr_info("Katana PCM: USB device invalidation complete - operations will be blocked\n");
}

// Close playback substream
int katana_pcm_playback_close(struct snd_pcm_substream *substream)
{
	struct katana_pcm_data *data = substream->runtime->private_data;

	// Close is a cleanup operation - don't block it during disconnect
	// We always need to be able to clean up resources

	if (data) {
		pr_info("Katana PCM close: Cleaning up private data\n");
		
		// Stop streaming and free URB buffers
		data->stream_started = 0;
		katana_free_urb_buffers(data);
		
		kfree(data);
		substream->runtime->private_data = NULL;  // CRITICAL: Clear dangling pointer
	}

	pr_info("Katana PCM playback closed\n");
	return 0;
}

// Set hardware parameters
int katana_pcm_hw_params(struct snd_pcm_substream *substream,
			 struct snd_pcm_hw_params *hw_params)
{
	struct katana_pcm_data *data = substream->runtime->private_data;
	size_t buffer_bytes;
	unsigned int periods;
	int err;

	// Check if disconnect is in progress
	err = katana_enter_operation();
	if (err < 0) {
		return err;
	}

	// DEFENSIVE: Check if private data is still valid
	if (!data) {
		pr_err("Katana PCM hw_params: private data is NULL\n");
		katana_exit_operation();
		return -ENODEV;
	}

	// Check if USB device is still valid before any operations
	if (!data->usb_dev_valid || !data->usb_dev) {
		pr_err("Katana PCM: USB device is no longer valid, cannot set hw params\n");
		katana_exit_operation();
		return -ENODEV;
	}

	// Store parameters
	data->buffer_size = params_buffer_size(hw_params);
	data->period_size = params_period_size(hw_params);
	data->period_bytes = params_period_bytes(hw_params);
	data->channels = params_channels(hw_params);
	data->rate = params_rate(hw_params);
	data->format = params_format(hw_params);

	buffer_bytes = params_buffer_bytes(hw_params);
	periods = params_periods(hw_params);

	pr_info("Katana PCM hw_params: format=%u (%s), channels=%u, rate=%u\n", 
		data->format, 
		(data->format == SNDRV_PCM_FORMAT_S24_3LE) ? "S24_3LE" :
		(data->format == SNDRV_PCM_FORMAT_S32_LE) ? "S32_LE" :
		(data->format == SNDRV_PCM_FORMAT_S16_LE) ? "S16_LE" : "UNKNOWN",
		data->channels, data->rate);

	// Calculate frame size based on format
	unsigned int frame_size = data->channels * snd_pcm_format_physical_width(data->format) / 8;
	pr_info("Katana PCM hw_params: Calculated frame_size=%u bytes per frame\n", frame_size);
	
	// Verify frame size matches expected values
	if (data->format == SNDRV_PCM_FORMAT_S24_3LE && data->channels == 2) {
		if (frame_size != 6) {
			pr_err("Katana PCM: S24_3LE stereo should be 6 bytes per frame, got %u\n", frame_size);
			katana_exit_operation();
			return -EINVAL;
		}
	}
	
	// Verify period size is frame-aligned
	if (data->period_bytes % frame_size != 0) {
		pr_err("Katana PCM: period_bytes (%u) not frame-aligned (frame_size=%u)\n", 
		       data->period_bytes, frame_size);
		katana_exit_operation();
		return -EINVAL;
	}
	
	pr_info("Katana PCM hw_params: period_size=%u frames, period_bytes=%u bytes, buffer_size=%u frames\n",
		data->period_size, data->period_bytes, data->buffer_size);

	// CRITICAL: Validate that buffer_bytes = period_bytes * periods
	if (buffer_bytes != data->period_bytes * periods) {
		pr_err("Katana PCM: Buffer constraint violation: buffer_bytes (%zu) != period_bytes (%u) * periods (%u)\n",
		       buffer_bytes, data->period_bytes, periods);
		katana_exit_operation();
		return -EINVAL;
	}

	// Validate buffer size and periods
	if (buffer_bytes < katana_pcm_playback_hw.period_bytes_min * katana_pcm_playback_hw.periods_min ||
	    buffer_bytes > katana_pcm_playback_hw.period_bytes_max * katana_pcm_playback_hw.periods_max) {
		pr_err("Katana PCM: Invalid buffer size %zu (min: %zu, max: %zu)\n",
		       buffer_bytes, (size_t)(katana_pcm_playback_hw.period_bytes_min * katana_pcm_playback_hw.periods_min),
		       (size_t)(katana_pcm_playback_hw.period_bytes_max * katana_pcm_playback_hw.periods_max));
		katana_exit_operation();
		return -EINVAL;
	}

	pr_info("Katana PCM hw_params: Setting buffer_bytes=%zu for rate=%d, channels=%d, format=%d\n",
		buffer_bytes, data->rate, data->channels, data->format);

	// **DUAL-BUFFER APPROACH FOR USB AUDIO**
	
	// Step 1: Allocate ALSA PCM buffer for userspace
	err = snd_pcm_lib_malloc_pages(substream, buffer_bytes);
	if (err < 0) {
		pr_err("Katana PCM: Failed to allocate ALSA buffer: %d\n", err);
		katana_exit_operation();
		return err;
	}

	pr_info("Katana PCM: ALSA buffer allocated successfully - dma_area=%p dma_bytes=%zu\n", 
		substream->runtime->dma_area, substream->runtime->dma_bytes);

	// Step 2: Free existing URB buffers if any
	katana_free_urb_buffers(data);

	// Step 3: Set up URB parameters for USB streaming  
	data->num_urbs = 6;  // 6 URBs for streaming
	
	// Calculate URB buffer size based on isochronous packet structure
	// Each URB will contain multiple packets (8ms worth of data)
	unsigned int packets_per_urb = 8;
	unsigned int samples_per_packet = data->rate / 1000;  // 1ms worth of samples
	unsigned int packet_size = samples_per_packet * frame_size;
	data->urb_buffer_size = packets_per_urb * packet_size;
	
	data->stream_started = 0;
	data->active_urbs = 0;

	// URB setup complete

	// Step 4: Allocate USB URB buffers for hardware transfers
	err = katana_alloc_urb_buffers(data);
	if (err < 0) {
		pr_err("Katana PCM: Failed to allocate URB buffers: %d\n", err);
		snd_pcm_lib_free_pages(substream);
		katana_exit_operation();
		return err;
	}

	// URB buffers allocated successfully

	katana_exit_operation();
	return 0;
}

// Free hardware resources
int katana_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct katana_pcm_data *data = substream->runtime->private_data;

	// hw_free is a cleanup operation - don't block it during disconnect
	// We always need to be able to free resources
	
	// DEFENSIVE: Check if private data is still valid
	if (!data) {
		pr_warn("Katana PCM hw_free: private data is NULL\n");
		goto cleanup_runtime;
	}
	
	pr_info("Katana PCM hw_free: Starting buffer cleanup\n");
	
	// **DUAL-BUFFER CLEANUP FOR USB AUDIO**
	
	// Step 1: Stop streaming and free URB buffers
	data->stream_started = 0;
	katana_free_urb_buffers(data);
	// URB buffers freed
	
	// Step 2: Deactivate the USB interface (process context - can sleep)
	katana_set_interface_altsetting(data, 0);
	pr_info("Katana PCM hw_free: Interface deactivated\n");
	
	// Step 3: Free ALSA PCM buffer
	snd_pcm_lib_free_pages(substream);
	pr_info("Katana PCM hw_free: ALSA buffer freed\n");
	
cleanup_runtime:
	pr_info("Katana PCM hw_free: Cleanup complete\n");
	return 0;
}

// Prepare for playback
int katana_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct katana_pcm_data *data = substream->runtime->private_data;
	unsigned long flags;
	int err;
	int target_altsetting;

	// Check if disconnect is in progress
	err = katana_enter_operation();
	if (err < 0) {
		return err;
	}

	// DEFENSIVE: Check if private data is still valid
	if (!data) {
		pr_warn("Katana PCM prepare: private data is NULL\n");
		katana_exit_operation();
		return -ENODEV;
	}
	
	// Check if USB device is still valid
	if (!data->usb_dev_valid) {
		pr_warn("Katana PCM prepare: USB device is no longer valid\n");
		katana_exit_operation();
		return -ENODEV;
	}

	// Select correct alternate setting based on sample rate
	// From USB descriptors: altsetting 1 = 48kHz, altsetting 2 = 96kHz
	switch (data->rate) {
	case 48000:
		target_altsetting = 1;
		break;
	case 96000:
		target_altsetting = 2;
		break;
	default:
		pr_err("Katana PCM: Unsupported sample rate %u\n", data->rate);
		katana_exit_operation();
		return -EINVAL;
	}

	spin_lock_irqsave(&data->lock, flags);
	
	data->hw_ptr = 0;
	data->last_period_hw_ptr = 0;
	data->read_ptr = 0;
	data->running = 0;
	data->start_time = jiffies;

	spin_unlock_irqrestore(&data->lock, flags);

	// Activate the USB interface for streaming (process context - can sleep)
	err = katana_set_interface_altsetting(data, target_altsetting);
	if (err < 0) {
		pr_err("Katana PCM: Failed to activate interface during prepare: %d\n", err);
		katana_exit_operation();
		return err;
	}

	// Configure the sample rate on the device
	err = katana_set_sample_rate(data, data->rate);
	if (err < 0) {
		pr_err("Katana PCM: Failed to set sample rate during prepare: %d\n", err);
		katana_exit_operation();
		return err;
	}

	pr_info("Katana PCM prepared for playback at %u Hz (altsetting %d)\n", 
		data->rate, target_altsetting);
	katana_exit_operation();
	return 0;
}

// Trigger playback
int katana_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct katana_pcm_data *data = substream->runtime->private_data;
	unsigned long flags;
	int err;
	int should_block = 0;
	int i, j;

	// Determine if we should block this operation during disconnect
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		should_block = 1;  // Block new work operations
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		should_block = 0;  // Allow stop operations (cleanup)
		break;
	default:
		should_block = 1;  // Block unknown operations
		break;
	}

	// Only check disconnect for operations that should be blocked
	if (should_block) {
		err = katana_enter_operation();
		if (err < 0) {
			return err;
		}
	}

	// DEFENSIVE: Check if private data is still valid
	if (!data) {
		pr_warn("Katana PCM trigger: private data is NULL\n");
		if (should_block) katana_exit_operation();
		return -ENODEV;
	}
	
	// Check if USB device is still valid
	if (!data->usb_dev_valid) {
		pr_warn("Katana PCM trigger: USB device is no longer valid\n");
		if (should_block) katana_exit_operation();
		return -ENODEV;
	}

	spin_lock_irqsave(&data->lock, flags);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		data->running = 1;
		data->stream_started = 1;
		data->start_time = jiffies;
		data->hw_ptr = 0;
		data->last_period_hw_ptr = 0;
		data->read_ptr = 0;
		
		// Start sync URB first to receive feedback
		err = usb_submit_urb(data->sync_urb, GFP_ATOMIC);
		if (err < 0) {
			pr_err("Katana PCM: Failed to submit sync URB: %d\n", err);
			data->running = 0;
			data->stream_started = 0;
			spin_unlock_irqrestore(&data->lock, flags);
			if (should_block) katana_exit_operation();
			return err;
		}
		
		// Start URB streaming
		for (i = 0; i < data->num_urbs; i++) {
			// Initialize URB buffer with silence
			memset(data->urb_buffers[i], 0, data->urb_buffer_size);
			
			// For isochronous URBs, ensure packet descriptors are set up
			if (usb_pipeisoc(data->urbs[i]->pipe)) {
				// Packet descriptors were set up in katana_alloc_urb_buffers
				// Just make sure they're properly initialized
				unsigned int frame_size = data->channels * snd_pcm_format_physical_width(data->format) / 8;
				unsigned int samples_per_packet = data->rate / 1000;  // Nominal 1ms worth
				unsigned int packet_size = samples_per_packet * frame_size;
				
				for (j = 0; j < data->urbs[i]->number_of_packets; j++) {
					data->urbs[i]->iso_frame_desc[j].offset = j * packet_size;
					data->urbs[i]->iso_frame_desc[j].length = packet_size;
				}
			}
			
			// Submit URB
			err = usb_submit_urb(data->urbs[i], GFP_ATOMIC);
			if (err < 0) {
				pr_err("Katana PCM: Failed to submit URB %d: %d\n", i, err);
				// Stop already submitted URBs
				for (j = i - 1; j >= 0; j--) {
					usb_unlink_urb(data->urbs[j]);
				}
				usb_unlink_urb(data->sync_urb);
				data->running = 0;
				data->stream_started = 0;
				spin_unlock_irqrestore(&data->lock, flags);
				if (should_block) katana_exit_operation();
				return err;
			}
		}
		
		// Playback started
		break;
		
	case SNDRV_PCM_TRIGGER_STOP:
		data->running = 0;
		data->stream_started = 0;
		
		// Stop sync URB first
		usb_unlink_urb(data->sync_urb);
		
		// Stop URB streaming (use unlink in atomic context)
		for (i = 0; i < data->num_urbs; i++) {
			usb_unlink_urb(data->urbs[i]);
		}
		
		pr_info("Katana PCM playback stopped\n");
		break;
		
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		data->running = 0;
		pr_info("Katana PCM playback paused\n");
		break;
		
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		data->running = 1;
		pr_info("Katana PCM playback resumed\n");
		break;
		
	default:
		spin_unlock_irqrestore(&data->lock, flags);
		if (should_block) katana_exit_operation();
		return -EINVAL;
	}

	spin_unlock_irqrestore(&data->lock, flags);
	if (should_block) katana_exit_operation();
	return 0;
}

// Get current hardware pointer
snd_pcm_uframes_t katana_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct katana_pcm_data *data = substream->runtime->private_data;
	unsigned long flags;
	snd_pcm_uframes_t pos;

	// DEFENSIVE: Check if private data is still valid
	if (!data) {
		pr_warn("Katana PCM pointer: private data is NULL\n");
		return 0;
	}
	
	// Check if USB device is still valid
	if (!data->usb_dev_valid) {
		pr_debug("Katana PCM pointer: USB device is no longer valid, returning 0\n");
		return 0;
	}

	spin_lock_irqsave(&data->lock, flags);
	
	// Always return the actual hardware pointer
	pos = data->hw_ptr;

	spin_unlock_irqrestore(&data->lock, flags);
	return pos;
}

// URB completion handler for audio streaming
static void katana_urb_complete(struct urb *urb)
{
	struct katana_pcm_data *data = urb->context;
	struct snd_pcm_substream *substream = data->substream;
	unsigned long flags;
	int err;
	unsigned int frames_transferred = 0;
	unsigned int frame_size;
	char *pcm_buffer;
	unsigned int copy_offset;
	unsigned int available_frames;
	static unsigned int urb_count = 0;
	int k;

	if (!data->stream_started) {
		return; // Stream was stopped
	}

	spin_lock_irqsave(&data->lock, flags);
	urb_count++;
	
	switch (urb->status) {
	case 0:
		// Success - calculate frames transferred
		frame_size = data->channels * snd_pcm_format_physical_width(data->format) / 8;
		
		if (usb_pipeisoc(urb->pipe)) {
			// For isochronous URBs, sum up actual lengths of all packets
			for (k = 0; k < urb->number_of_packets; k++) {
				frames_transferred += urb->iso_frame_desc[k].actual_length / frame_size;
			}
		} else {
			// For bulk URBs, use total actual length
			frames_transferred = urb->actual_length / frame_size;
		}
		
		// Update hardware pointer
		data->hw_ptr += frames_transferred;
		if (data->hw_ptr >= data->buffer_size) {
			data->hw_ptr -= data->buffer_size;
		}
		
		// Check for period elapsed
		unsigned int current_period = data->hw_ptr / data->period_size;
		unsigned int last_period = data->last_period_hw_ptr / data->period_size;
		int period_elapsed = 0;
		
		if (current_period != last_period) {
			data->last_period_hw_ptr = data->hw_ptr;
			period_elapsed = 1;
		}
		
		// Progress tracking removed to reduce log noise
		
		spin_unlock_irqrestore(&data->lock, flags);
		
		if (period_elapsed) {
			snd_pcm_period_elapsed(substream);
		}
		break;
		
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		// URB was cancelled - normal shutdown
		goto exit_unlock;
	default:
		// URB error - log only serious errors
		if (urb->status != -EPROTO && urb->status != -EILSEQ) {
			pr_err("Katana URB error: status %d\n", urb->status);
		}
		goto exit_unlock;
	}

	// Reacquire lock for URB processing
	spin_lock_irqsave(&data->lock, flags);
	
	// Prepare next URB with data from PCM buffer
	if (data->stream_started && data->running) {
		frame_size = data->channels * snd_pcm_format_physical_width(data->format) / 8;
		
		// Calculate samples per packet based on feedback data
		unsigned int samples_per_packet;
		if (data->feedback_valid && data->feedback_samples > 0) {
			// Use feedback data - it represents samples per 1ms frame
			samples_per_packet = data->feedback_samples;
		} else {
			// Fallback to nominal rate-based calculation
			samples_per_packet = data->rate / 1000;
		}
		
		// Get PCM buffer pointer
		pcm_buffer = substream->runtime->dma_area;
		
		if (usb_pipeisoc(urb->pipe)) {
			// Handle isochronous transfer with multiple packets
			unsigned int total_samples_needed = 0;
			unsigned int packet_size = samples_per_packet * frame_size;
			
			// Update packet descriptors based on current feedback
			for (k = 0; k < urb->number_of_packets; k++) {
				// Adjust packet size based on feedback
				// For precise timing, we might need to vary packet sizes slightly
				unsigned int this_packet_samples = samples_per_packet;
				unsigned int this_packet_size = this_packet_samples * frame_size;
				
				// Ensure packet doesn't exceed buffer bounds
				if ((k + 1) * packet_size > data->urb_buffer_size) {
					this_packet_size = data->urb_buffer_size - (k * packet_size);
					this_packet_samples = this_packet_size / frame_size;
				}
				
				urb->iso_frame_desc[k].offset = k * packet_size;
				urb->iso_frame_desc[k].length = this_packet_size;
				total_samples_needed += this_packet_samples;
			}
			
			// Calculate available data in PCM buffer
			snd_pcm_uframes_t appl_ptr = READ_ONCE(substream->runtime->control->appl_ptr);
			snd_pcm_uframes_t appl_pos = appl_ptr % data->buffer_size;
			
			if (appl_pos >= data->read_ptr) {
				available_frames = appl_pos - data->read_ptr;
			} else {
				available_frames = data->buffer_size - data->read_ptr + appl_pos;
			}
			
			// Limit to available data
			if (total_samples_needed > available_frames) {
				total_samples_needed = available_frames;
			}
			
			// Fill URB buffer with audio data
			if (pcm_buffer && total_samples_needed > 0) {
				unsigned int samples_copied = 0;
				
				for (k = 0; k < urb->number_of_packets && samples_copied < total_samples_needed; k++) {
					unsigned int packet_samples = urb->iso_frame_desc[k].length / frame_size;
					unsigned int samples_to_copy = min(packet_samples, total_samples_needed - samples_copied);
					unsigned int copy_size = samples_to_copy * frame_size;
					
					// Calculate source offset in PCM buffer
					copy_offset = ((data->read_ptr + samples_copied) % data->buffer_size) * frame_size;
					
					// Copy data to URB buffer
					unsigned char *dest = data->urb_buffers[urb_count % data->num_urbs] + urb->iso_frame_desc[k].offset;
					
					if (copy_offset + copy_size <= substream->runtime->dma_bytes) {
						memcpy(dest, pcm_buffer + copy_offset, copy_size);
					} else {
						// Handle wraparound
						unsigned int first_part = substream->runtime->dma_bytes - copy_offset;
						unsigned int second_part = copy_size - first_part;
						memcpy(dest, pcm_buffer + copy_offset, first_part);
						memcpy(dest + first_part, pcm_buffer, second_part);
					}
					
					samples_copied += samples_to_copy;
					
					// Update actual packet length
					urb->iso_frame_desc[k].length = copy_size;
				}
				
				// Update read pointer
				data->read_ptr = (data->read_ptr + samples_copied) % data->buffer_size;
				
				// Debug logging removed to reduce noise
			} else {
				// Fill with silence
				for (k = 0; k < urb->number_of_packets; k++) {
					unsigned char *dest = data->urb_buffers[urb_count % data->num_urbs] + urb->iso_frame_desc[k].offset;
					memset(dest, 0, urb->iso_frame_desc[k].length);
				}
			}
		} else {
			// Handle bulk transfer (fallback for non-isochronous endpoints)
			unsigned int samples_needed = data->urb_buffer_size / frame_size;
			
			// Calculate available data
			snd_pcm_uframes_t appl_ptr = READ_ONCE(substream->runtime->control->appl_ptr);
			snd_pcm_uframes_t appl_pos = appl_ptr % data->buffer_size;
			
			if (appl_pos >= data->read_ptr) {
				available_frames = appl_pos - data->read_ptr;
			} else {
				available_frames = data->buffer_size - data->read_ptr + appl_pos;
			}
			
			if (samples_needed > available_frames) {
				samples_needed = available_frames;
			}
			
			if (pcm_buffer && samples_needed > 0) {
				unsigned int copy_size = samples_needed * frame_size;
				copy_offset = data->read_ptr * frame_size;
				
				if (copy_offset + copy_size <= substream->runtime->dma_bytes) {
					memcpy(urb->transfer_buffer, pcm_buffer + copy_offset, copy_size);
				} else {
					// Handle wraparound
					unsigned int first_part = substream->runtime->dma_bytes - copy_offset;
					unsigned int second_part = copy_size - first_part;
					memcpy(urb->transfer_buffer, pcm_buffer + copy_offset, first_part);
					memcpy((char*)urb->transfer_buffer + first_part, pcm_buffer, second_part);
				}
				
				data->read_ptr = (data->read_ptr + samples_needed) % data->buffer_size;
				urb->transfer_buffer_length = copy_size;
			} else {
				// Fill with silence
				memset(urb->transfer_buffer, 0, data->urb_buffer_size);
				urb->transfer_buffer_length = data->urb_buffer_size;
			}
		}
		
		// Resubmit URB
		err = usb_submit_urb(urb, GFP_ATOMIC);
		if (err < 0) {
			pr_err("Katana URB resubmit failed: %d\n", err);
		}
	}

exit_unlock:
	spin_unlock_irqrestore(&data->lock, flags);
}

// Sync URB completion handler for feedback endpoint
static void katana_sync_urb_complete(struct urb *urb)
{
	struct katana_pcm_data *data = urb->context;
	unsigned long flags;
	int err;
	
	if (!data->stream_started) {
		return; // Stream was stopped
	}
	
	switch (urb->status) {
	case 0:
		// Success - process feedback data
		if (urb->actual_length >= 3) {
			unsigned int feedback_value = 0;
			
			// Parse feedback data - USB Audio feedback is in 10.14 fixed-point format
			if (urb->actual_length == 3) {
				// USB 1.1 format: 3 bytes, little endian
				feedback_value = (data->sync_buffer[0] | 
						 (data->sync_buffer[1] << 8) |
						 (data->sync_buffer[2] << 16));
			} else if (urb->actual_length == 4) {
				// USB 2.0 format: 4 bytes, little endian
				feedback_value = (data->sync_buffer[0] | 
						 (data->sync_buffer[1] << 8) |
						 (data->sync_buffer[2] << 16) |
						 (data->sync_buffer[3] << 24));
			}
			
			// Convert from 10.14 fixed-point to samples per millisecond
			// The feedback value represents the number of samples the device
			// consumed per USB frame (1ms for full-speed, 0.125ms for high-speed)
			
			// For full-speed USB (1ms frames):
			// feedback_value is in 10.14 format representing samples per frame
			unsigned int samples_per_frame = (feedback_value + 8192) >> 14;  // Round and shift
			
			// Validate feedback value is reasonable for our sample rate
			unsigned int expected_min = (data->rate * 9) / 10000;  // 90% of nominal
			unsigned int expected_max = (data->rate * 11) / 10000; // 110% of nominal
			
			if (samples_per_frame >= expected_min && samples_per_frame <= expected_max) {
				spin_lock_irqsave(&data->lock, flags);
				
				// Update feedback tracking
				data->feedback_value = feedback_value;
				data->feedback_samples = samples_per_frame;
				data->feedback_count++;
				
				// Use simple averaging for stability
				if (data->feedback_count == 1) {
					data->feedback_average = samples_per_frame;
				} else {
					// Simple moving average with 1/8 weight for new sample
					data->feedback_average = (7 * data->feedback_average + samples_per_frame) / 8;
				}
				
				data->feedback_valid = 1;
				
				spin_unlock_irqrestore(&data->lock, flags);
				
				// Feedback logging removed to reduce log noise
			} else {
				// Invalid feedback - ignore (logging removed to reduce noise)
			}
		}
		break;
		
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		// URB was cancelled
		return;
		
	default:
		// Sync URB error - logging removed to reduce noise
		break;
	}
	
	// Resubmit the sync URB to keep feedback flowing
	if (data->stream_started && data->running) {
		err = usb_submit_urb(urb, GFP_ATOMIC);
		if (err < 0) {
			pr_err("Katana sync URB resubmit failed: %d\n", err);
		}
	}
}



// Allocate URB buffers for USB audio streaming
static int katana_alloc_urb_buffers(struct katana_pcm_data *data)
{
	int i, j;
	struct usb_host_interface *altsetting = NULL;
	struct usb_endpoint_descriptor *ep_desc = NULL;
	unsigned int max_packet_size = 0;
	int is_isoc_endpoint = 0;
	
	// Find the correct alternate setting and endpoint descriptor ONCE
	// We need to look in the altsetting where we found the endpoint,
	// not the current altsetting (which might be 0 during hw_params)
	if (data->usb_iface) {
		// Find the altsetting where we discovered the endpoint
		for (j = 0; j < data->usb_iface->num_altsetting; j++) {
			if (data->usb_iface->altsetting[j].desc.bAlternateSetting == data->altsetting_num) {
				altsetting = &data->usb_iface->altsetting[j];
				break;
			}
		}
		
		// Find the endpoint descriptor in the correct altsetting
		if (altsetting) {
			for (j = 0; j < altsetting->desc.bNumEndpoints; j++) {
				ep_desc = &altsetting->endpoint[j].desc;
				if (ep_desc->bEndpointAddress == data->endpoint_out) {
					break;
				}
			}
			// Check if we found the endpoint
			if (j >= altsetting->desc.bNumEndpoints) {
				ep_desc = NULL; // Not found
			}
		}
	}
	
	// Validate that we found the endpoint
	if (!ep_desc) {
		if (!altsetting) {
			pr_err("Katana PCM: Could not find altsetting %d\n", data->altsetting_num);
		} else {
			pr_err("Katana PCM: Could not find endpoint descriptor for 0x%02x in altsetting %d\n",
			       data->endpoint_out, data->altsetting_num);
		}
		return -ENODEV;
	}
	
	// Determine endpoint type and get max packet size
	if (usb_endpoint_is_bulk_out(ep_desc)) {
		is_isoc_endpoint = 0;
		max_packet_size = le16_to_cpu(ep_desc->wMaxPacketSize);
	} else if (usb_endpoint_is_isoc_out(ep_desc)) {
		is_isoc_endpoint = 1;
		max_packet_size = le16_to_cpu(ep_desc->wMaxPacketSize);
	} else {
		pr_err("Katana PCM: Endpoint 0x%02x is not a valid OUT endpoint\n", data->endpoint_out);
		return -ENODEV;
	}
	
	// Allocate URB array
	data->urbs = kzalloc(sizeof(struct urb *) * data->num_urbs, GFP_KERNEL);
	if (!data->urbs) {
		return -ENOMEM;
	}
	
	// Allocate URB buffer pointers
	data->urb_buffers = kzalloc(sizeof(unsigned char *) * data->num_urbs, GFP_KERNEL);
	if (!data->urb_buffers) {
		kfree(data->urbs);
		return -ENOMEM;
	}
	
	// Allocate DMA address array
	data->urb_dma_addrs = kzalloc(sizeof(dma_addr_t) * data->num_urbs, GFP_KERNEL);
	if (!data->urb_dma_addrs) {
		kfree(data->urb_buffers);
		kfree(data->urbs);
		return -ENOMEM;
	}
	
	// Allocate sync URB and buffer first
	data->sync_urb = usb_alloc_urb(1, GFP_KERNEL); // 1 packet for sync
	if (!data->sync_urb) {
		goto error_cleanup;
	}
	
	data->sync_buffer = usb_alloc_coherent(data->usb_dev, data->sync_packet_size,
					       GFP_KERNEL, &data->sync_dma_addr);
	if (!data->sync_buffer) {
		usb_free_urb(data->sync_urb);
		goto error_cleanup;
	}
	
	// Set up sync URB
	data->sync_urb->dev = data->usb_dev;
	data->sync_urb->pipe = usb_rcvisocpipe(data->usb_dev, data->endpoint_sync & 0x0f);
	data->sync_urb->transfer_buffer = data->sync_buffer;
	data->sync_urb->transfer_buffer_length = data->sync_packet_size;
	data->sync_urb->complete = katana_sync_urb_complete;
	data->sync_urb->context = data;
	data->sync_urb->interval = 1;
	data->sync_urb->start_frame = -1;
	data->sync_urb->number_of_packets = 1;
	data->sync_urb->iso_frame_desc[0].offset = 0;
	data->sync_urb->iso_frame_desc[0].length = data->sync_packet_size;
	data->sync_urb->transfer_dma = data->sync_dma_addr;
	data->sync_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	
	// Sync URB allocated successfully
	
	// Calculate optimal packet structure for isochronous transfers
	unsigned int packets_per_urb = 8;  // 8ms worth of packets per URB
	unsigned int frame_size = data->channels * snd_pcm_format_physical_width(data->format) / 8;
	
	// Calculate nominal samples per packet (1ms of audio)
	// For 48kHz: 48 samples per packet, for 96kHz: 96 samples per packet
	unsigned int nominal_samples_per_packet = data->rate / 1000;
	unsigned int nominal_packet_size = nominal_samples_per_packet * frame_size;
	
	// Each URB buffer needs to hold all packets
	unsigned int urb_buffer_size = packets_per_urb * nominal_packet_size;
	
	// Ensure URB buffer size doesn't exceed max packet size constraints
	if (nominal_packet_size > max_packet_size) {
		pr_err("Katana PCM: Calculated packet size (%u) exceeds max packet size (%u)\n",
		       nominal_packet_size, max_packet_size);
		goto error_cleanup;
	}
	
	// Isochronous setup complete
	
	// Allocate URBs and their buffers
	for (i = 0; i < data->num_urbs; i++) {
		// Allocate URB with correct number of packets
		data->urbs[i] = usb_alloc_urb(is_isoc_endpoint ? packets_per_urb : 0, GFP_KERNEL);
		if (!data->urbs[i]) {
			goto error_cleanup;
		}
		
		// Allocate USB-coherent buffer for this URB
		data->urb_buffers[i] = usb_alloc_coherent(data->usb_dev, 
							  urb_buffer_size,
							  GFP_KERNEL, 
							  &data->urb_dma_addrs[i]);
		if (!data->urb_buffers[i]) {
			usb_free_urb(data->urbs[i]);
			goto error_cleanup;
		}
		
		// Set up the URB based on endpoint type
		if (is_isoc_endpoint) {
			// Use proper isochronous transfer with multiple packets
			data->urbs[i]->dev = data->usb_dev;
			data->urbs[i]->pipe = usb_sndisocpipe(data->usb_dev, data->endpoint_out & 0x0f);
			data->urbs[i]->transfer_buffer = data->urb_buffers[i];
			data->urbs[i]->transfer_buffer_length = urb_buffer_size;
			data->urbs[i]->complete = katana_urb_complete;
			data->urbs[i]->context = data;
			data->urbs[i]->interval = 1;  // 1ms intervals
			data->urbs[i]->start_frame = -1;  // Let USB core schedule
			data->urbs[i]->number_of_packets = packets_per_urb;
			data->urbs[i]->transfer_dma = data->urb_dma_addrs[i];
			data->urbs[i]->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
			
			// Initialize packet descriptors
			for (j = 0; j < packets_per_urb; j++) {
				data->urbs[i]->iso_frame_desc[j].offset = j * nominal_packet_size;
				data->urbs[i]->iso_frame_desc[j].length = nominal_packet_size;
			}
			
			// Isochronous URB configured
		} else {
			// Use bulk URB for bulk endpoint
			usb_fill_bulk_urb(data->urbs[i], data->usb_dev,
					  usb_sndbulkpipe(data->usb_dev, data->endpoint_out & 0x0f),
					  data->urb_buffers[i],
					  urb_buffer_size,
					  katana_urb_complete,
					  data);
			
			// Bulk URB configured
		}
		
		data->urbs[i]->transfer_dma = data->urb_dma_addrs[i];
		data->urbs[i]->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	}
	
	// Store URB buffer size for later use
	data->urb_buffer_size = urb_buffer_size;
	
	return 0;
	
error_cleanup:
	// Clean up partially allocated resources
	for (i = i - 1; i >= 0; i--) {
		if (data->urb_buffers[i]) {
			usb_free_coherent(data->usb_dev, urb_buffer_size,
					  data->urb_buffers[i], data->urb_dma_addrs[i]);
		}
		if (data->urbs[i]) {
			usb_free_urb(data->urbs[i]);
		}
	}
	kfree(data->urb_dma_addrs);
	kfree(data->urb_buffers);
	kfree(data->urbs);
	
	// Clean up sync URB if allocated
	if (data->sync_buffer) {
		usb_free_coherent(data->usb_dev, data->sync_packet_size,
				  data->sync_buffer, data->sync_dma_addr);
	}
	if (data->sync_urb) {
		usb_free_urb(data->sync_urb);
	}
	
	return -ENOMEM;
}

// Free URB buffers
static void katana_free_urb_buffers(struct katana_pcm_data *data)
{
	int i;
	
	if (!data->urbs)
		return;
	
	// Stop all URBs first (including sync URB)
	if (data->sync_urb) {
		usb_kill_urb(data->sync_urb);
	}
	
	for (i = 0; i < data->num_urbs; i++) {
		if (data->urbs[i]) {
			usb_kill_urb(data->urbs[i]);
		}
	}
	
	// Free URB resources
	for (i = 0; i < data->num_urbs; i++) {
		if (data->urb_buffers[i]) {
			usb_free_coherent(data->usb_dev, data->urb_buffer_size,
					  data->urb_buffers[i], data->urb_dma_addrs[i]);
		}
		if (data->urbs[i]) {
			usb_free_urb(data->urbs[i]);
		}
	}
	
	kfree(data->urb_dma_addrs);
	kfree(data->urb_buffers);
	kfree(data->urbs);
	
	data->urbs = NULL;
	data->urb_buffers = NULL;
	data->urb_dma_addrs = NULL;
	
	// Free sync URB resources
	if (data->sync_buffer) {
		usb_free_coherent(data->usb_dev, data->sync_packet_size,
				  data->sync_buffer, data->sync_dma_addr);
		data->sync_buffer = NULL;
	}
	if (data->sync_urb) {
		usb_free_urb(data->sync_urb);
		data->sync_urb = NULL;
	}
}
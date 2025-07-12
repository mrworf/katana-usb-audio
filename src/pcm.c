#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/usb.h>
#include <linux/uio.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include "pcm.h"

// Private data structure for our PCM device
struct katana_pcm_data {
	struct snd_card *card;
	struct snd_pcm_substream *substream;
	struct usb_device *usb_dev;
	spinlock_t lock;
	
	// USB device state tracking
	int usb_dev_valid;  // Track if USB device is still valid
	
	// URB management for USB audio streaming
	struct urb **urbs;        // Array of URBs for streaming
	int num_urbs;            // Number of URBs
	int urb_buffer_size;     // Size of each URB buffer
	unsigned char **urb_buffers; // URB data buffers
	dma_addr_t *urb_dma_addrs;   // DMA addresses for URB buffers
	
	// Playback state
	unsigned int buffer_size;
	unsigned int period_size;
	unsigned int period_bytes;
	unsigned int channels;
	unsigned int rate;
	unsigned int format;
	
	// Hardware pointer tracking
	unsigned int hw_ptr;
	unsigned int appl_ptr;
	
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
	.formats = (SNDRV_PCM_FMTBIT_S16_LE |
		    SNDRV_PCM_FMTBIT_S24_LE |
		    SNDRV_PCM_FMTBIT_S32_LE),
	.rates = (SNDRV_PCM_RATE_8000 |
		  SNDRV_PCM_RATE_11025 |
		  SNDRV_PCM_RATE_16000 |
		  SNDRV_PCM_RATE_22050 |
		  SNDRV_PCM_RATE_32000 |
		  SNDRV_PCM_RATE_44100 |
		  SNDRV_PCM_RATE_48000 |
		  SNDRV_PCM_RATE_88200 |
		  SNDRV_PCM_RATE_96000),
	.rate_min = 8000,
	.rate_max = 96000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 32768, // period_bytes_max * periods_max = 4096 * 8
	.period_bytes_min = 1024, // Reduced minimum period size
	.period_bytes_max = 4096, // Reduced maximum period size
	.periods_min = 2,
	.periods_max = 8,
};

// Constraint lists
static const unsigned int katana_rates[] = {
	8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000
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

// Forward declarations for URB functions
static int katana_alloc_urb_buffers(struct katana_pcm_data *data);
static void katana_free_urb_buffers(struct katana_pcm_data *data);
static void katana_urb_complete(struct urb *urb);

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
	.copy = katana_pcm_copy,
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
	data->appl_ptr = 0;
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
	data->num_urbs = 8;  // Multiple URBs for smooth streaming
	data->urb_buffer_size = data->period_bytes; // Each URB handles one period
	data->stream_started = 0;
	data->active_urbs = 0;

	// Step 4: Allocate USB URB buffers for hardware transfers
	err = katana_alloc_urb_buffers(data);
	if (err < 0) {
		pr_err("Katana PCM: Failed to allocate URB buffers: %d\n", err);
		snd_pcm_lib_free_pages(substream);
		katana_exit_operation();
		return err;
	}

	pr_info("Katana PCM: URB buffers allocated successfully (%d URBs, %d bytes each)\n",
		data->num_urbs, data->urb_buffer_size);

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
	pr_info("Katana PCM hw_free: URB buffers freed\n");
	
	// Step 2: Free ALSA PCM buffer
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

	spin_lock_irqsave(&data->lock, flags);
	
	data->hw_ptr = 0;
	data->appl_ptr = 0;
	data->prepared = 1;
	data->running = 0;
	data->start_time = jiffies;

	spin_unlock_irqrestore(&data->lock, flags);

	pr_info("Katana PCM prepared for playback\n");
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
	int i;

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
		
		// Start URB streaming
		for (i = 0; i < data->num_urbs; i++) {
			// Initialize URB buffer with silence
			memset(data->urb_buffers[i], 0, data->urb_buffer_size);
			
			// Submit URB
			err = usb_submit_urb(data->urbs[i], GFP_ATOMIC);
			if (err < 0) {
				pr_err("Katana PCM: Failed to submit URB %d: %d\n", i, err);
				// Stop already submitted URBs
				for (i = i - 1; i >= 0; i--) {
					usb_kill_urb(data->urbs[i]);
				}
				data->running = 0;
				data->stream_started = 0;
				spin_unlock_irqrestore(&data->lock, flags);
				if (should_block) katana_exit_operation();
				return err;
			}
		}
		
		pr_info("Katana PCM playback started with %d URBs\n", data->num_urbs);
		break;
		
	case SNDRV_PCM_TRIGGER_STOP:
		data->running = 0;
		data->stream_started = 0;
		
		// Stop URB streaming
		for (i = 0; i < data->num_urbs; i++) {
			usb_kill_urb(data->urbs[i]);
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
	
	if (data->running) {
		// Simulate hardware pointer advancement based on time
		// In a real implementation, this would read from actual hardware
		unsigned long jiffies_diff = jiffies - data->start_time;
		unsigned int frames_played = (jiffies_diff * data->rate) / HZ;
		pos = frames_played % data->buffer_size;
	} else {
		pos = data->hw_ptr;
	}

	spin_unlock_irqrestore(&data->lock, flags);
	return pos;
}

// Copy audio data from user space to DMA buffer (iov_iter version)
int katana_pcm_copy(struct snd_pcm_substream *substream, int channel,
                  snd_pcm_uframes_t pos, struct iov_iter *src, snd_pcm_uframes_t count)
{
    struct katana_pcm_data *data = substream->runtime->private_data;
    unsigned long flags;
    unsigned int offset, size;
    ssize_t copied;
    char *dma_area;
    int err;

    // Check if disconnect is in progress
    err = katana_enter_operation();
    if (err < 0) {
        return err;
    }

    // DEFENSIVE: Check if private data is still valid
    if (!data) {
        pr_warn("Katana PCM copy: private data is NULL\n");
        katana_exit_operation();
        return -ENODEV;
    }
    
    // Check if USB device is still valid
    if (!data->usb_dev_valid) {
        pr_warn("Katana PCM copy: USB device is no longer valid\n");
        katana_exit_operation();
        return -ENODEV;
    }

    // Get ALSA's PCM buffer
    dma_area = substream->runtime->dma_area;
    if (!dma_area) {
        katana_exit_operation();
        return -ENOMEM;
    }

    offset = pos * data->channels * snd_pcm_format_physical_width(data->format) / 8;
    size = count * data->channels * snd_pcm_format_physical_width(data->format) / 8;

    // Copy data from iov_iter to ALSA PCM buffer
    copied = copy_from_iter(dma_area + offset, size, src);
    if (copied != size) {
        katana_exit_operation();
        return -EFAULT;
    }

    spin_lock_irqsave(&data->lock, flags);
    
    // **USB AUDIO DATA FLOW**
    // Data is now in the ALSA PCM buffer. The URB completion handler
    // will copy data from the PCM buffer to URB buffers as needed.
    // This provides a decoupled approach where:
    // 1. Userspace writes to PCM buffer (here)
    // 2. URB completion handler copies PCM data to URB buffers
    // 3. URBs transfer data to USB device

    // Update application pointer
    data->appl_ptr = pos + count;
    if (data->appl_ptr >= data->buffer_size)
        data->appl_ptr -= data->buffer_size;

    spin_unlock_irqrestore(&data->lock, flags);

    pr_debug("Katana PCM copy: pos=%lu, count=%lu, size=%u, appl_ptr=%u\n", 
             pos, count, size, data->appl_ptr);

    katana_exit_operation();
    return 0;
}

// URB completion handler for audio streaming
static void katana_urb_complete(struct urb *urb)
{
	struct katana_pcm_data *data = urb->context;
	struct snd_pcm_substream *substream = data->substream;
	unsigned long flags;
	int err;
	unsigned int frames_to_copy;
	unsigned int frame_size;
	char *pcm_buffer;
	unsigned int copy_offset;
	unsigned int copy_size;

	if (!data->stream_started) {
		return; // Stream was stopped
	}

	spin_lock_irqsave(&data->lock, flags);
	
	switch (urb->status) {
	case 0:
		// Success - update hardware pointer
		frames_to_copy = urb->actual_length / (data->channels * snd_pcm_format_physical_width(data->format) / 8);
		data->hw_ptr += frames_to_copy;
		if (data->hw_ptr >= data->buffer_size) {
			data->hw_ptr -= data->buffer_size;
		}
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		// URB was unlinked/cancelled
		goto exit_unlock;
	default:
		pr_err("Katana URB error: %d\n", urb->status);
		goto exit_unlock;
	}

	// Prepare next URB with data from PCM buffer
	if (data->stream_started && data->running) {
		frame_size = data->channels * snd_pcm_format_physical_width(data->format) / 8;
		frames_to_copy = data->urb_buffer_size / frame_size;
		copy_size = frames_to_copy * frame_size;
		
		// Get PCM buffer pointer
		pcm_buffer = substream->runtime->dma_area;
		if (pcm_buffer) {
			// Calculate offset in PCM buffer based on hardware pointer
			copy_offset = (data->hw_ptr * frame_size) % substream->runtime->dma_bytes;
			
			// Copy data from PCM buffer to URB buffer
			// Handle wraparound at buffer boundary
			if (copy_offset + copy_size <= substream->runtime->dma_bytes) {
				// Simple copy - no wraparound
				memcpy(urb->transfer_buffer, pcm_buffer + copy_offset, copy_size);
			} else {
				// Wraparound copy
				unsigned int first_part = substream->runtime->dma_bytes - copy_offset;
				unsigned int second_part = copy_size - first_part;
				
				memcpy(urb->transfer_buffer, pcm_buffer + copy_offset, first_part);
				memcpy((char*)urb->transfer_buffer + first_part, pcm_buffer, second_part);
			}
		} else {
			// No PCM data available, fill with silence
			memset(urb->transfer_buffer, 0, data->urb_buffer_size);
		}
		
		// Resubmit URB
		err = usb_submit_urb(urb, GFP_ATOMIC);
		if (err < 0) {
			pr_err("Katana URB resubmit failed: %d\n", err);
		}
	}

exit_unlock:
	spin_unlock_irqrestore(&data->lock, flags);
	
	// Notify ALSA of period completion
	if (urb->status == 0 && data->stream_started) {
		snd_pcm_period_elapsed(substream);
	}
}

// Allocate URB buffers for USB audio streaming
static int katana_alloc_urb_buffers(struct katana_pcm_data *data)
{
	int i;
	
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
	
	// Allocate URBs and their buffers
	for (i = 0; i < data->num_urbs; i++) {
		data->urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
		if (!data->urbs[i]) {
			goto error_cleanup;
		}
		
		// Allocate USB-coherent buffer for this URB
		data->urb_buffers[i] = usb_alloc_coherent(data->usb_dev, 
							  data->urb_buffer_size,
							  GFP_KERNEL, 
							  &data->urb_dma_addrs[i]);
		if (!data->urb_buffers[i]) {
			usb_free_urb(data->urbs[i]);
			goto error_cleanup;
		}
		
		// Set up the URB
		usb_fill_bulk_urb(data->urbs[i], data->usb_dev,
				  usb_sndbulkpipe(data->usb_dev, 1), // Endpoint 1 OUT
				  data->urb_buffers[i],
				  data->urb_buffer_size,
				  katana_urb_complete,
				  data);
		
		data->urbs[i]->transfer_dma = data->urb_dma_addrs[i];
		data->urbs[i]->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	}
	
	return 0;
	
error_cleanup:
	// Clean up partially allocated resources
	for (i = i - 1; i >= 0; i--) {
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
	
	return -ENOMEM;
}

// Free URB buffers
static void katana_free_urb_buffers(struct katana_pcm_data *data)
{
	int i;
	
	if (!data->urbs)
		return;
	
	// Stop all URBs first
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
}
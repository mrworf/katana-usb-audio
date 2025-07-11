#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include "pcm.h"
#include <linux/uio.h>
#include <sound/pcm.h>

#ifndef SNDRV_DMA_ADDR_INVALID
#define SNDRV_DMA_ADDR_INVALID (~0UL)
#endif

// Private data structure for our PCM device
struct katana_pcm_data {
	struct snd_card *card;
	struct snd_pcm_substream *substream;
	spinlock_t lock;
	
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

	*pcm_ret = pcm;
	return 0;
}

// Open playback substream
int katana_pcm_playback_open(struct snd_pcm_substream *substream)
{
	struct snd_card *card = snd_pcm_substream_chip(substream);
	struct katana_pcm_data *data;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->card = card;
	data->substream = substream;
	spin_lock_init(&data->lock);

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
	return 0;
}

// Close playback substream
int katana_pcm_playback_close(struct snd_pcm_substream *substream)
{
	struct katana_pcm_data *data = substream->runtime->private_data;

	if (data) {
		kfree(data);
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

	// Store parameters first
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
		return -EINVAL;
	}

	// Validate buffer size and periods
	if (buffer_bytes < katana_pcm_playback_hw.period_bytes_min * katana_pcm_playback_hw.periods_min ||
	    buffer_bytes > katana_pcm_playback_hw.period_bytes_max * katana_pcm_playback_hw.periods_max) {
		pr_err("Katana PCM: Invalid buffer size %zu (min: %zu, max: %zu)\n",
		       buffer_bytes, (size_t)(katana_pcm_playback_hw.period_bytes_min * katana_pcm_playback_hw.periods_min),
		       (size_t)(katana_pcm_playback_hw.period_bytes_max * katana_pcm_playback_hw.periods_max));
		return -EINVAL;
	}
	if (periods < katana_pcm_playback_hw.periods_min ||
	    periods > katana_pcm_playback_hw.periods_max) {
		pr_err("Katana PCM: Invalid periods %u (min: %u, max: %u)\n",
		       periods, katana_pcm_playback_hw.periods_min, katana_pcm_playback_hw.periods_max);
		return -EINVAL;
	}

	pr_info("buffer_bytes=%zu period_bytes=%u periods=%d\n",
        params_buffer_bytes(hw_params),
        params_period_bytes(hw_params),
        params_periods(hw_params));

	pr_info("hw: buffer_bytes_max=%zu, period_bytes_min=%zu, period_bytes_max=%zu, periods_min=%u, periods_max=%u\n",
		katana_pcm_playback_hw.buffer_bytes_max,
		katana_pcm_playback_hw.period_bytes_min,
		katana_pcm_playback_hw.period_bytes_max,
		katana_pcm_playback_hw.periods_min,
		katana_pcm_playback_hw.periods_max);
		

	// For USB audio, we need to allocate regular kernel memory, not DMA pages
	substream->runtime->dma_area = kmalloc(buffer_bytes, GFP_KERNEL);
	if (!substream->runtime->dma_area) {
		pr_err("Katana PCM: Failed to allocate buffer memory: buffer_bytes=%zu\n", buffer_bytes);
		return -ENOMEM;
	}
	
	substream->runtime->dma_bytes = buffer_bytes;
	substream->runtime->dma_addr = SNDRV_DMA_ADDR_INVALID; // Not used for USB
	
	pr_info("Katana PCM: Allocated buffer: dma_area=%p dma_bytes=%zu\n", 
		substream->runtime->dma_area, substream->runtime->dma_bytes);

	pr_info("Katana PCM hw_params: rate=%d, channels=%d, format=%d, buffer_size=%d, buffer_bytes=%zu, period_bytes=%u, periods=%u\n",
		data->rate, data->channels, data->format, data->buffer_size, buffer_bytes, data->period_bytes, periods);

	return 0;
}

// Free hardware resources
int katana_pcm_hw_free(struct snd_pcm_substream *substream)
{
	// For USB audio, free the allocated kernel memory
	if (substream->runtime->dma_area) {
		kfree(substream->runtime->dma_area);
		substream->runtime->dma_area = NULL;
		substream->runtime->dma_bytes = 0;
		substream->runtime->dma_addr = SNDRV_DMA_ADDR_INVALID;
		pr_info("Katana PCM: Freed buffer memory\n");
	}
	return 0;
}

// Prepare for playback
int katana_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct katana_pcm_data *data = substream->runtime->private_data;
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);
	
	data->hw_ptr = 0;
	data->appl_ptr = 0;
	data->prepared = 1;
	data->running = 0;
	data->start_time = jiffies;

	spin_unlock_irqrestore(&data->lock, flags);

	pr_info("Katana PCM prepared for playback\n");
	return 0;
}

// Trigger playback
int katana_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct katana_pcm_data *data = substream->runtime->private_data;
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		data->running = 1;
		data->start_time = jiffies;
		pr_info("Katana PCM playback started\n");
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		data->running = 0;
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
		return -EINVAL;
	}

	spin_unlock_irqrestore(&data->lock, flags);
	return 0;
}

// Get current hardware pointer
snd_pcm_uframes_t katana_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct katana_pcm_data *data = substream->runtime->private_data;
	unsigned long flags;
	snd_pcm_uframes_t pos;

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

    // Get ALSA's DMA buffer
    dma_area = substream->runtime->dma_area;
    if (!dma_area)
        return -ENOMEM;

    offset = pos * data->channels * snd_pcm_format_physical_width(data->format) / 8;
    size = count * data->channels * snd_pcm_format_physical_width(data->format) / 8;

    // Copy data from iov_iter to DMA buffer
    copied = copy_from_iter(dma_area + offset, size, src);
    if (copied != size)
        return -EFAULT;

    spin_lock_irqsave(&data->lock, flags);
    // Update hardware pointer
    if (data->running) {
        data->hw_ptr += count;
        if (data->hw_ptr >= data->buffer_size)
            data->hw_ptr = 0;
    }
    spin_unlock_irqrestore(&data->lock, flags);

    // In a real implementation, you would send this data to the USB device
    pr_debug("Katana PCM copy: pos=%lu, count=%lu, size=%u\n", pos, count, size);

    return 0;
}
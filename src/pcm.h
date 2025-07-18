#pragma once

#include <sound/pcm.h>
#include <sound/core.h>

// Operation tracking functions for disconnect synchronization
int katana_enter_operation(void);
void katana_exit_operation(void);

// PCM operations structure
extern struct snd_pcm_ops katana_pcm_playback_ops;

// PCM hardware definition
extern struct snd_pcm_hardware katana_pcm_playback_hw;

// Function declarations
int katana_pcm_new(struct snd_card *card, struct snd_pcm **pcm_ret);
int katana_pcm_playback_open(struct snd_pcm_substream *substream);
int katana_pcm_playback_close(struct snd_pcm_substream *substream);
int katana_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *hw_params);
int katana_pcm_hw_free(struct snd_pcm_substream *substream);
int katana_pcm_prepare(struct snd_pcm_substream *substream);
int katana_pcm_trigger(struct snd_pcm_substream *substream, int cmd);
snd_pcm_uframes_t katana_pcm_pointer(struct snd_pcm_substream *substream);
void katana_pcm_invalidate_usb_dev(struct snd_card *card);
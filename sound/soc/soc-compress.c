/*
 * soc-compress.c  --  ALSA SoC Compress
 *
 * Copyright (C) 2012 Intel Corp.
 *
 * Authors: Namarta Kohli <namartax.kohli@intel.com>
 *          Ramesh Babu K V <ramesh.babu@linux.intel.com>
 *          Vinod Koul <vinod.koul@linux.intel.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/compress_params.h>
#include <sound/compress_driver.h>
#include <sound/soc.h>
#include <sound/initval.h>
#include <sound/soc-dpcm.h>

#ifdef CONFIG_VENDOR_SMARTISAN
static inline int param_is_mask(int p)
{
	return ((p >= SNDRV_PCM_HW_PARAM_FIRST_MASK) &&
			(p <= SNDRV_PCM_HW_PARAM_LAST_MASK));
}

static inline int param_is_interval(int p)
{
	return (p >= SNDRV_PCM_HW_PARAM_FIRST_INTERVAL) &&
			(p <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL);
}

static inline struct snd_interval *param_to_interval(struct snd_pcm_hw_params *p, int n)
{
	return &(p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL]);
}

static inline struct snd_mask *param_to_mask(struct snd_pcm_hw_params *p, int n)
{
	return &(p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK]);
}

static void param_set_mask(struct snd_pcm_hw_params *p, int n, unsigned bit)
{
	if (bit >= SNDRV_MASK_MAX)
		return;
	if (param_is_mask(n)) {
		struct snd_mask *m = param_to_mask(p, n);
		m->bits[0] = 0;
		m->bits[1] = 0;
		m->bits[bit >> 5] |= (1 << (bit & 31));
	}
}

static void param_set_min(struct snd_pcm_hw_params *p, int n, unsigned int val)
{
	if (param_is_interval(n)) {
		struct snd_interval *i = param_to_interval(p, n);
		i->min = val;
	}
}

static void param_set_int(struct snd_pcm_hw_params *p, int n, unsigned int val)
{
	if (param_is_interval(n)) {
		struct snd_interval *i = param_to_interval(p, n);
		i->min = val;
		i->max = val;
		i->integer = 1;
	}
}
#endif

static int soc_compr_open(struct snd_compr_stream *cstream)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret = 0;

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	if (platform->driver->compr_ops && platform->driver->compr_ops->open) {
		ret = platform->driver->compr_ops->open(cstream);
		if (ret < 0) {
			pr_err("compress asoc: can't open platform %s\n", platform->name);
			goto out;
		}
	}

	if (rtd->dai_link->compr_ops && rtd->dai_link->compr_ops->startup) {
		ret = rtd->dai_link->compr_ops->startup(cstream);
		if (ret < 0) {
			pr_err("compress asoc: %s startup failed\n", rtd->dai_link->name);
			goto machine_err;
		}
	}

	if (cstream->direction == SND_COMPRESS_PLAYBACK) {
		cpu_dai->playback_active++;
		codec_dai->playback_active++;
	} else {
		cpu_dai->capture_active++;
		codec_dai->capture_active++;
	}

	cpu_dai->active++;
	codec_dai->active++;
	rtd->codec->active++;

	mutex_unlock(&rtd->pcm_mutex);

	return 0;

machine_err:
	if (platform->driver->compr_ops && platform->driver->compr_ops->free)
		platform->driver->compr_ops->free(cstream);
out:
	mutex_unlock(&rtd->pcm_mutex);
	return ret;
}

static int soc_compr_open_fe(struct snd_compr_stream *cstream)
{
	struct snd_soc_pcm_runtime *fe = cstream->private_data;
	struct snd_pcm_substream *fe_substream = fe->pcm->streams[0].substream;
	struct snd_soc_platform *platform = fe->platform;
	struct snd_soc_dpcm_params *dpcm;
	struct snd_soc_dapm_widget_list *list;

	struct snd_soc_dai *cpu_dai = fe->cpu_dai;
	struct snd_soc_dai *codec_dai = fe->codec_dai;

	int stream;
	int ret = 0;

	if (cstream->direction == SND_COMPRESS_PLAYBACK)
		stream = SNDRV_PCM_STREAM_PLAYBACK;
	else
		stream = SNDRV_PCM_STREAM_CAPTURE;

	mutex_lock(&fe->card->dpcm_mutex);

	if (platform->driver->compr_ops && platform->driver->compr_ops->open) {
		ret = platform->driver->compr_ops->open(cstream);
		if (ret < 0) {
			pr_err("compress asoc: can't open platform %s\n", platform->name);
			goto out;
		}
	}

	if (fe->dai_link->compr_ops && fe->dai_link->compr_ops->startup) {
		ret = fe->dai_link->compr_ops->startup(cstream);
		if (ret < 0) {
			pr_err("compress asoc: %s startup failed\n", fe->dai_link->name);
			goto machine_err;
		}
	}

	fe->dpcm[stream].runtime = fe_substream->runtime;

	if (dpcm_path_get(fe, stream, &list) <= 0) {
		dev_dbg(fe->dev, "ASoC: %s no valid %s route\n",
			fe->dai_link->name, stream ? "capture" : "playback");
	}

	/* calculate valid and active FE <-> BE dpcms */
	dpcm_process_paths(fe, stream, &list, 1);

	fe->dpcm[stream].runtime_update = SND_SOC_DPCM_UPDATE_FE;

	ret = dpcm_be_dai_startup(fe, stream);
	if (ret < 0) {
		/* clean up all links */
		list_for_each_entry(dpcm, &fe->dpcm[stream].be_clients, list_be)
			dpcm->state = SND_SOC_DPCM_LINK_STATE_FREE;

		dpcm_be_disconnect(fe, stream);
		fe->dpcm[stream].runtime = NULL;
		goto fe_err;
	}

	dpcm_clear_pending_state(fe, stream);
	dpcm_path_put(&list);

	fe->dpcm[stream].state = SND_SOC_DPCM_STATE_OPEN;
	fe->dpcm[stream].runtime_update = SND_SOC_DPCM_UPDATE_NO;

	if (cstream->direction == SND_COMPRESS_PLAYBACK) {
		cpu_dai->playback_active++;
		codec_dai->playback_active++;
	} else {
		cpu_dai->capture_active++;
		codec_dai->capture_active++;
	}

	cpu_dai->active++;
	codec_dai->active++;
	fe->codec->active++;

	mutex_unlock(&fe->card->dpcm_mutex);

	return 0;

fe_err:
	if (fe->dai_link->compr_ops && fe->dai_link->compr_ops->shutdown)
		fe->dai_link->compr_ops->shutdown(cstream);
machine_err:
	if (platform->driver->compr_ops && platform->driver->compr_ops->free)
		platform->driver->compr_ops->free(cstream);
out:
	fe->dpcm[stream].runtime_update = SND_SOC_DPCM_UPDATE_NO;
	mutex_unlock(&fe->card->dpcm_mutex);
	return ret;
}

/*
 * Power down the audio subsystem pmdown_time msecs after close is called.
 * This is to ensure there are no pops or clicks in between any music tracks
 * due to DAPM power cycling.
 */
static void close_delayed_work(struct work_struct *work)
{
	struct snd_soc_pcm_runtime *rtd =
			container_of(work, struct snd_soc_pcm_runtime, delayed_work.work);
	struct snd_soc_dai *codec_dai = rtd->codec_dai;

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	dev_dbg(rtd->dev, "ASoC: pop wq checking: %s status: %s waiting: %s\n",
		 codec_dai->driver->playback.stream_name,
		 codec_dai->playback_active ? "active" : "inactive",
		     codec_dai->pop_wait ? "yes" : "no");

	/* are we waiting on this codec DAI stream */
	if (codec_dai->pop_wait == 1) {
	  codec_dai->pop_wait = 0;
		snd_soc_dapm_stream_event(rtd, SNDRV_PCM_STREAM_PLAYBACK,
					  SND_SOC_DAPM_STREAM_STOP);
	}

	mutex_unlock(&rtd->pcm_mutex);
}

static int soc_compr_free(struct snd_compr_stream *cstream)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_codec *codec = rtd->codec;

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	if (cstream->direction == SND_COMPRESS_PLAYBACK) {
		cpu_dai->playback_active--;
		codec_dai->playback_active--;
		snd_soc_dai_digital_mute(codec_dai, 1);
	} else {
		cpu_dai->capture_active--;
		codec_dai->capture_active--;
	}

	cpu_dai->active--;
	codec_dai->active--;
	codec->active--;

	if (!cpu_dai->active)
		cpu_dai->rate = 0;

	if (!codec_dai->active)
		codec_dai->rate = 0;


	if (rtd->dai_link->compr_ops && rtd->dai_link->compr_ops->shutdown)
		rtd->dai_link->compr_ops->shutdown(cstream);

	if (platform->driver->compr_ops && platform->driver->compr_ops->free)
		platform->driver->compr_ops->free(cstream);
	cpu_dai->runtime = NULL;

	if (cstream->direction == SND_COMPRESS_PLAYBACK) {
		if (!rtd->pmdown_time || codec->ignore_pmdown_time ||
		    rtd->dai_link->ignore_pmdown_time) {
			snd_soc_dapm_stream_event(rtd,
					codec_dai->driver->playback.stream_name,
					SND_SOC_DAPM_STREAM_STOP);
		} else {
			codec_dai->pop_wait = 1;
			schedule_delayed_work(&rtd->delayed_work,
				msecs_to_jiffies(rtd->pmdown_time));
		}
	} else {
		/* capture streams can be powered down now */
		snd_soc_dapm_stream_event(rtd,
			codec_dai->driver->capture.stream_name,
			SND_SOC_DAPM_STREAM_STOP);
	}

	mutex_unlock(&rtd->pcm_mutex);
	return 0;
}

static int soc_compr_free_fe(struct snd_compr_stream *cstream)
{
	struct snd_soc_pcm_runtime *fe = cstream->private_data;
	struct snd_soc_platform *platform = fe->platform;
	struct snd_soc_dpcm_params *dpcm;
	int stream, ret;

	struct snd_soc_dai *cpu_dai = fe->cpu_dai;
	struct snd_soc_dai *codec_dai = fe->codec_dai;
	struct snd_soc_codec *codec = fe->codec;

	if (cstream->direction == SND_COMPRESS_PLAYBACK)
		stream = SNDRV_PCM_STREAM_PLAYBACK;
	else
		stream = SNDRV_PCM_STREAM_CAPTURE;

	mutex_lock(&fe->card->dpcm_mutex);
	if (cstream->direction == SND_COMPRESS_PLAYBACK) {
		cpu_dai->playback_active--;
		codec_dai->playback_active--;
		snd_soc_dai_digital_mute(codec_dai, 1);
	} else {
		cpu_dai->capture_active--;
		codec_dai->capture_active--;
	}

	cpu_dai->active--;
	codec_dai->active--;
	codec->active--;

	fe->dpcm[stream].runtime_update = SND_SOC_DPCM_UPDATE_FE;

	ret = dpcm_be_dai_hw_free(fe, stream);
	if (ret < 0)
		dev_err(fe->dev, "compressed hw_free failed %d\n", ret);

	ret = dpcm_be_dai_shutdown(fe, stream);

	/* mark FE's links ready to prune */
	list_for_each_entry(dpcm, &fe->dpcm[stream].be_clients, list_be)
		dpcm->state = SND_SOC_DPCM_LINK_STATE_FREE;

	if (stream == SNDRV_PCM_STREAM_PLAYBACK)
		dpcm_dapm_stream_event(fe, stream,
				fe->cpu_dai->driver->playback.stream_name,
				SND_SOC_DAPM_STREAM_STOP);
	else
		dpcm_dapm_stream_event(fe, stream,
				fe->cpu_dai->driver->capture.stream_name,
				SND_SOC_DAPM_STREAM_STOP);

	fe->dpcm[stream].state = SND_SOC_DPCM_STATE_CLOSE;
	fe->dpcm[stream].runtime_update = SND_SOC_DPCM_UPDATE_NO;

	dpcm_be_disconnect(fe, stream);

	fe->dpcm[stream].runtime = NULL;

	if (fe->dai_link->compr_ops && fe->dai_link->compr_ops->shutdown)
		fe->dai_link->compr_ops->shutdown(cstream);

	if (platform->driver->compr_ops && platform->driver->compr_ops->free)
		platform->driver->compr_ops->free(cstream);
	//cpu_dai->runtime = NULL;

	mutex_unlock(&fe->card->dpcm_mutex);
	return 0;
}

static int soc_compr_trigger(struct snd_compr_stream *cstream, int cmd)
{

	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret = 0;

	/* for partial drain and drain cmd, don't acquire lock while invoking DSP.
	 * These calls will be blocked till these operation can complete which
	 * will be a while. And during that time, app can invoke STOP, PAUSE etc
	 */
	if (cmd == SND_COMPR_TRIGGER_PARTIAL_DRAIN ||
				cmd == SND_COMPR_TRIGGER_DRAIN) {
		if (platform->driver->compr_ops &&
					platform->driver->compr_ops->trigger)
			return platform->driver->compr_ops->trigger(cstream, cmd);
	}

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	if (platform->driver->compr_ops && platform->driver->compr_ops->trigger) {
		ret = platform->driver->compr_ops->trigger(cstream, cmd);
		if (ret < 0)
			goto out;
	}

	if (cstream->direction == SND_COMPRESS_PLAYBACK) {
		switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
			snd_soc_dai_digital_mute(codec_dai, 0);
			break;
		case SNDRV_PCM_TRIGGER_STOP:
			snd_soc_dai_digital_mute(codec_dai, 1);
			break;
		}
	}

out:
	mutex_unlock(&rtd->pcm_mutex);
	return ret;
}

static int soc_compr_trigger_fe(struct snd_compr_stream *cstream, int cmd)
{
	struct snd_soc_pcm_runtime *fe = cstream->private_data;
	struct snd_soc_platform *platform = fe->platform;
	int ret = 0, stream;

	/* for partial drain and drain cmd, don't acquire lock while invoking DSP.
	 * These calls will be blocked till these operation can complete which
	 * will be a while. And during that time, app can invoke STOP, PAUSE etc
	 */
	if (cmd == SND_COMPR_TRIGGER_PARTIAL_DRAIN ||
				cmd == SND_COMPR_TRIGGER_DRAIN) {
		if (platform->driver->compr_ops &&
					platform->driver->compr_ops->trigger)
			return platform->driver->compr_ops->trigger(cstream, cmd);
	}

	if (cstream->direction == SND_COMPRESS_PLAYBACK)
		stream = SNDRV_PCM_STREAM_PLAYBACK;
	else
		stream = SNDRV_PCM_STREAM_CAPTURE;


	mutex_lock(&fe->card->dpcm_mutex);
	if (platform->driver->compr_ops && platform->driver->compr_ops->trigger) {
		ret = platform->driver->compr_ops->trigger(cstream, cmd);
		if (ret < 0)
			goto out;
	}

	fe->dpcm[stream].runtime_update = SND_SOC_DPCM_UPDATE_FE;

	ret = dpcm_be_dai_trigger(fe, stream, cmd);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		fe->dpcm[stream].state = SND_SOC_DPCM_STATE_START;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		fe->dpcm[stream].state = SND_SOC_DPCM_STATE_STOP;
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		fe->dpcm[stream].state = SND_SOC_DPCM_STATE_PAUSED;
		break;
	}

out:
	fe->dpcm[stream].runtime_update = SND_SOC_DPCM_UPDATE_NO;
	mutex_unlock(&fe->card->dpcm_mutex);
	return ret;
}

static int soc_compr_set_params(struct snd_compr_stream *cstream,
					struct snd_compr_params *params)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret = 0;

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	/* first we call set_params for the platform driver
	 * this should configure the soc side
	 * if the machine has compressed ops then we call that as well
	 * expectation is that platform and machine will configure everything
	 * for this compress path, like configuring pcm port for codec
	 */
	if (platform->driver->compr_ops && platform->driver->compr_ops->set_params) {
		ret = platform->driver->compr_ops->set_params(cstream, params);
		if (ret < 0)
			goto err;
	}

	if (rtd->dai_link->compr_ops && rtd->dai_link->compr_ops->set_params) {
		ret = rtd->dai_link->compr_ops->set_params(cstream);
		if (ret < 0)
			goto err;
	}

	snd_soc_dapm_stream_event(rtd,
				codec_dai->driver->playback.stream_name,
				SND_SOC_DAPM_STREAM_START);

	/* cancel any delayed stream shutdown that is pending */
	codec_dai->pop_wait = 0;
	mutex_unlock(&rtd->pcm_mutex);

	cancel_delayed_work_sync(&rtd->delayed_work);

	return ret;

err:
	mutex_unlock(&rtd->pcm_mutex);
	return ret;
}

static int soc_compr_set_params_fe(struct snd_compr_stream *cstream,
					struct snd_compr_params *params)
{
	struct snd_soc_pcm_runtime *fe = cstream->private_data;
	struct snd_pcm_substream *fe_substream = fe->pcm->streams[0].substream;
	struct snd_soc_platform *platform = fe->platform;
	int ret = 0, stream;
#ifdef CONFIG_VENDOR_SMARTISAN
	struct snd_pcm_hw_params *hw_params;
	unsigned int bit_width = 16;
	unsigned int sample_rate = 48000;
#endif

	if (cstream->direction == SND_COMPRESS_PLAYBACK)
		stream = SNDRV_PCM_STREAM_PLAYBACK;
	else
		stream = SNDRV_PCM_STREAM_CAPTURE;

#ifdef CONFIG_VENDOR_SMARTISAN
	hw_params = kzalloc(sizeof(*hw_params), GFP_KERNEL);
	if (hw_params == NULL)
		return -ENOMEM;

	switch (params->codec.sample_rate) {
	case SNDRV_PCM_RATE_8000:
		sample_rate = 8000;
		break;
	case SNDRV_PCM_RATE_11025:
		sample_rate = 11025;
		break;
	/* ToDo: What about 12K and 24K sample rates ? */
	case SNDRV_PCM_RATE_16000:
		sample_rate = 16000;
		break;
	case SNDRV_PCM_RATE_22050:
		sample_rate = 22050;
		break;
	case SNDRV_PCM_RATE_32000:
		sample_rate = 32000;
		break;
	case SNDRV_PCM_RATE_44100:
		sample_rate = 44100;
		break;
	case SNDRV_PCM_RATE_48000:
		sample_rate = 48000;
		break;
	case SNDRV_PCM_RATE_64000:
		sample_rate = 64000;
		break;
	case SNDRV_PCM_RATE_88200:
		sample_rate = 88200;
		break;
	case SNDRV_PCM_RATE_96000:
		sample_rate = 96000;
		break;
	case SNDRV_PCM_RATE_176400:
		sample_rate = 176400;
		break;
	case SNDRV_PCM_RATE_192000:
		sample_rate = 192000;
		break;
	}

	param_set_mask(hw_params, SNDRV_PCM_HW_PARAM_FORMAT, params->codec.format);
	param_set_mask(hw_params, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);

	if (params->codec.format == SNDRV_PCM_FORMAT_S24_LE)
		bit_width = 32;

	param_set_int(hw_params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, bit_width);
	param_set_int(hw_params, SNDRV_PCM_HW_PARAM_FRAME_BITS, bit_width * params->codec.ch_in);
	param_set_int(hw_params, SNDRV_PCM_HW_PARAM_CHANNELS, params->codec.ch_in);
	param_set_int(hw_params, SNDRV_PCM_HW_PARAM_RATE, sample_rate);

	if ((params->codec.sample_rate & (SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000)) != 0)
		param_set_min(hw_params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 512);
	else
		param_set_min(hw_params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 128);

	param_set_int(hw_params, SNDRV_PCM_HW_PARAM_PERIODS, 8);
#endif

	mutex_lock(&fe->card->dpcm_mutex);
	/* first we call set_params for the platform driver
	 * this should configure the soc side
	 * if the machine has compressed ops then we call that as well
	 * expectation is that platform and machine will configure everything
	 * for this compress path, like configuring pcm port for codec
	 */
	if (platform->driver->compr_ops && platform->driver->compr_ops->set_params) {
		ret = platform->driver->compr_ops->set_params(cstream, params);
		if (ret < 0)
			goto out;
	}

	if (fe->dai_link->compr_ops && fe->dai_link->compr_ops->set_params) {
		ret = fe->dai_link->compr_ops->set_params(cstream);
		if (ret < 0)
			goto out;
	}

#ifdef CONFIG_VENDOR_SMARTISAN
	memcpy(&fe->dpcm[fe_substream->stream].hw_params, hw_params,
			sizeof(struct snd_pcm_hw_params));
#else
	memcpy(&fe->dpcm[fe_substream->stream].hw_params, params,
			sizeof(struct snd_pcm_hw_params));
#endif

	fe->dpcm[stream].runtime_update = SND_SOC_DPCM_UPDATE_FE;

	ret = dpcm_be_dai_hw_params(fe, stream);
	if (ret < 0)
		goto out;

	ret = dpcm_be_dai_prepare(fe, stream);
	if (ret < 0)
		goto out;

	if (stream == SNDRV_PCM_STREAM_PLAYBACK)
		dpcm_dapm_stream_event(fe, stream,
				fe->cpu_dai->driver->playback.stream_name,
				SND_SOC_DAPM_STREAM_START);
	else
		dpcm_dapm_stream_event(fe, stream,
				fe->cpu_dai->driver->capture.stream_name,
				SND_SOC_DAPM_STREAM_START);

	fe->dpcm[stream].state = SND_SOC_DPCM_STATE_PREPARE;

out:
	fe->dpcm[stream].runtime_update = SND_SOC_DPCM_UPDATE_NO;
	mutex_unlock(&fe->card->dpcm_mutex);
	return ret;
}

static int soc_compr_get_params(struct snd_compr_stream *cstream,
					struct snd_codec *params)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	int ret = 0;

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	if (platform->driver->compr_ops && platform->driver->compr_ops->get_params)
		ret = platform->driver->compr_ops->get_params(cstream, params);

	mutex_unlock(&rtd->pcm_mutex);
	return ret;
}

static int soc_compr_get_caps(struct snd_compr_stream *cstream,
				struct snd_compr_caps *caps)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	int ret = 0;

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	if (platform->driver->compr_ops && platform->driver->compr_ops->get_caps)
		ret = platform->driver->compr_ops->get_caps(cstream, caps);

	mutex_unlock(&rtd->pcm_mutex);
	return ret;
}

static int soc_compr_get_codec_caps(struct snd_compr_stream *cstream,
				struct snd_compr_codec_caps *codec)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	int ret = 0;

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	if (platform->driver->compr_ops && platform->driver->compr_ops->get_codec_caps)
		ret = platform->driver->compr_ops->get_codec_caps(cstream, codec);

	mutex_unlock(&rtd->pcm_mutex);
	return ret;
}

static int soc_compr_ack(struct snd_compr_stream *cstream, size_t bytes)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	int ret = 0;

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	if (platform->driver->compr_ops && platform->driver->compr_ops->ack)
		ret = platform->driver->compr_ops->ack(cstream, bytes);

	mutex_unlock(&rtd->pcm_mutex);
	return ret;
}

static int soc_compr_pointer(struct snd_compr_stream *cstream,
			struct snd_compr_tstamp *tstamp)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	int ret = 0;

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	if (platform->driver->compr_ops && platform->driver->compr_ops->pointer)
		ret = platform->driver->compr_ops->pointer(cstream, tstamp);

	mutex_unlock(&rtd->pcm_mutex);
	return ret;
}

static int soc_compr_copy(struct snd_compr_stream *cstream,
			  char __user *buf, size_t count)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	int ret = 0;

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	if (platform->driver->compr_ops && platform->driver->compr_ops->copy)
		ret = platform->driver->compr_ops->copy(cstream, buf, count);

	mutex_unlock(&rtd->pcm_mutex);
	return ret;
}

static int sst_compr_set_metadata(struct snd_compr_stream *cstream,
				struct snd_compr_metadata *metadata)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	int ret = 0;

	if (platform->driver->compr_ops && platform->driver->compr_ops->set_metadata)
		ret = platform->driver->compr_ops->set_metadata(cstream, metadata);

	return ret;
}

static int sst_compr_get_metadata(struct snd_compr_stream *cstream,
				struct snd_compr_metadata *metadata)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	int ret = 0;

	if (platform->driver->compr_ops && platform->driver->compr_ops->get_metadata)
		ret = platform->driver->compr_ops->get_metadata(cstream, metadata);

	return ret;
}
/* ASoC Compress operations */
static struct snd_compr_ops soc_compr_ops = {
	.open		= soc_compr_open,
	.free		= soc_compr_free,
	.set_params	= soc_compr_set_params,
	.set_metadata   = sst_compr_set_metadata,
	.get_metadata	= sst_compr_get_metadata,
	.get_params	= soc_compr_get_params,
	.trigger	= soc_compr_trigger,
	.pointer	= soc_compr_pointer,
	.ack		= soc_compr_ack,
	.get_caps	= soc_compr_get_caps,
	.get_codec_caps = soc_compr_get_codec_caps
};

/* ASoC Dynamic Compress operations */
static struct snd_compr_ops soc_compr_dyn_ops = {
	.open		= soc_compr_open_fe,
	.free		= soc_compr_free_fe,
	.set_params	= soc_compr_set_params_fe,
	.get_params	= soc_compr_get_params,
	.set_metadata   = sst_compr_set_metadata,
	.get_metadata	= sst_compr_get_metadata,
	.trigger	= soc_compr_trigger_fe,
	.pointer	= soc_compr_pointer,
	.ack		= soc_compr_ack,
	.get_caps	= soc_compr_get_caps,
	.get_codec_caps = soc_compr_get_codec_caps
};

/* create a new compress */
int soc_new_compress(struct snd_soc_pcm_runtime *rtd, int num)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_platform *platform = rtd->platform;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_compr *compr;
	struct snd_pcm *be_pcm;
	char new_name[64];
	int ret = 0, direction = 0;

	/* check client and interface hw capabilities */
	snprintf(new_name, sizeof(new_name), "%s %s-%d",
			rtd->dai_link->stream_name, codec_dai->name, num);
	direction = SND_COMPRESS_PLAYBACK;
	compr = kzalloc(sizeof(*compr), GFP_KERNEL);
	if (compr == NULL) {
		snd_printk(KERN_ERR "Cannot allocate compr\n");
		return -ENOMEM;
	}

	compr->ops = devm_kzalloc(rtd->card->dev, sizeof(soc_compr_ops),
				  GFP_KERNEL);
	if (compr->ops == NULL) {
		dev_err(rtd->card->dev, "Cannot allocate compressed ops\n");
		ret = -ENOMEM;
		goto compr_err;
	}

	if (rtd->dai_link->dynamic) {
		snprintf(new_name, sizeof(new_name), "(%s)",
			rtd->dai_link->stream_name);

		ret = snd_pcm_new_internal(rtd->card->snd_card, new_name, num,
				1, 0, &be_pcm);
		if (ret < 0) {
			dev_err(rtd->card->dev, "ASoC: can't create compressed for %s\n",
				rtd->dai_link->name);
			goto compr_err;
		}

		rtd->pcm = be_pcm;
		rtd->fe_compr = 1;
		be_pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream->private_data = rtd;
		//be_pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream->private_data = rtd;
		memcpy(compr->ops, &soc_compr_dyn_ops, sizeof(soc_compr_dyn_ops));
	} else
		memcpy(compr->ops, &soc_compr_ops, sizeof(soc_compr_ops));

	/* Add copy callback for not memory mapped DSPs */
	if (platform->driver->compr_ops && platform->driver->compr_ops->copy)
		compr->ops->copy = soc_compr_copy;

	mutex_init(&compr->lock);
	ret = snd_compress_new(rtd->card->snd_card, num, direction, compr);
	if (ret < 0) {
		pr_err("compress asoc: can't create compress for codec %s\n",
			codec->name);
		goto compr_err;
	}

	/* DAPM dai link stream work */
	INIT_DELAYED_WORK(&rtd->delayed_work, close_delayed_work);

	rtd->compr = compr;
	compr->private_data = rtd;

	if (platform->driver->pcm_new) {
		ret = platform->driver->pcm_new(rtd);
		if (ret < 0) {
			pr_err("asoc: compress pcm constructor failed\n");
			goto compr_err;
		}
	}

	printk(KERN_INFO "compress asoc: %s <-> %s mapping ok\n", codec_dai->name,
		cpu_dai->name);
	return ret;

compr_err:
	kfree(compr);
	return ret;
}

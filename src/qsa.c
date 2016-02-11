/* QSA Output.
 *
 * Copyright (C) 2016 Reece H. Dunn
 * Copyright (C) 2016 Kaj-Michael Lang
 *
 * This file is part of pcaudiolib.
 *
 * pcaudiolib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pcaudiolib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pcaudiolib.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "audio_priv.h"

#ifdef HAVE_SYS_ASOUNDLIB_H

#include <sys/asound.h>
#include <sys/asoundlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

struct qsa_object
{
	struct audio_object vtable;
	snd_pcm_t *handle;
	uint8_t sample_size;
	char *device;
};

#define to_qsa_object(object) container_of(object, struct qsa_object, vtable)

int
qsa_object_open(struct audio_object *object,
                enum audio_object_format format,
                uint32_t rate,
                uint8_t channels)
{
	struct qsa_object *self = to_qsa_object(object);
	if (self->handle)
		return -EEXIST;

	int pcm_format;
#define FORMAT(srcfmt, dstfmt, size) case srcfmt: pcm_format = dstfmt; self->sample_size = size; break;
	switch (format)
	{
	FORMAT(AUDIO_OBJECT_FORMAT_U8,     SND_PCM_SFMT_U8, 1)
	FORMAT(AUDIO_OBJECT_FORMAT_S8,     SND_PCM_SFMT_S8, 1)
	FORMAT(AUDIO_OBJECT_FORMAT_S16LE,  SND_PCM_SFMT_S16_LE, 2)
	default:                           return -EINVAL;
	}
#undef  FORMAT

	snd_pcm_info_t pi;
	snd_pcm_channel_info_t pci;
	snd_pcm_channel_params_t pp;
	snd_pcm_channel_setup_t setup;

	int err = 0;
	if (self->device) {
		fprintf (stderr, "qsa: open %s\n", self->device);
		if ((err = snd_pcm_open_name(&self->handle, self->device, SND_PCM_OPEN_PLAYBACK)) < 0)
			goto error;
	} else {
		fprintf (stderr, "qsa: open preferred\n");
		if ((err = snd_pcm_open_preferred(&self->handle, NULL, NULL, SND_PCM_OPEN_PLAYBACK)) < 0)
			goto error;
	}

	memset (&pi, 0, sizeof (pi));
	fprintf (stderr, "qsa: info\n");
	if ((err = snd_pcm_info (self->handle, &pi)) < 0)
		goto error;

	memset (&pci, 0, sizeof (pci));
	pci.channel = SND_PCM_CHANNEL_PLAYBACK;

	fprintf (stderr, "qsa: plugin info\n");
	if ((err = snd_pcm_plugin_info (self->handle, &pci)) < 0)
		goto error;

	memset (&pp, 0, sizeof (pp));
	pp.mode = SND_PCM_MODE_BLOCK;
	pp.channel = SND_PCM_CHANNEL_PLAYBACK;
	pp.start_mode = SND_PCM_START_FULL;
	pp.stop_mode = SND_PCM_STOP_STOP;

	fprintf (stderr, "qsa: max frag size %d\n", pci.max_fragment_size);
	fprintf (stderr, "qsa: min frag size %d\n", pci.min_fragment_size);

	pp.buf.block.frag_size = pci.max_fragment_size;
	pp.buf.block.frags_max = 4; // XXX: What should this be?
	pp.buf.block.frags_min = 1;

	pp.format.interleave = 1;
	pp.format.rate = rate;
	pp.format.voices = channels;
	pp.format.format = pcm_format;

	fprintf (stderr, "qsa: params\n");
	if ((err = snd_pcm_plugin_params (self->handle, &pp)) < 0)
		goto error;

	fprintf (stderr, "qsa: prepare\n");
	if ((err = snd_pcm_plugin_prepare (self->handle, SND_PCM_CHANNEL_PLAYBACK)) < 0)
		goto error;

	return 0;
error:
	fprintf (stderr, "qsa: snd error %s\n", snd_strerror(err));
	if (self->handle) {
		snd_pcm_close(self->handle);
		self->handle = NULL;
	}
	return err;
}

void
qsa_object_close(struct audio_object *object)
{
	struct qsa_object *self = to_qsa_object(object);

	if (self->handle) {
		snd_pcm_close(self->handle);
		self->handle = NULL;
	}
}

void
qsa_object_destroy(struct audio_object *object)
{
	struct qsa_object *self = to_qsa_object(object);

	free(self->device);
	free(self);
}

int
qsa_object_drain(struct audio_object *object)
{
	struct qsa_object *self = to_qsa_object(object);

	return snd_pcm_plugin_playback_drain(self->handle);
}

int
qsa_object_flush(struct audio_object *object)
{
	struct qsa_object *self = to_qsa_object(object);

	return snd_pcm_plugin_flush(self->handle, SND_PCM_CHANNEL_PLAYBACK);
}

int
qsa_object_write(struct audio_object *object,
                 const void *data,
                 size_t bytes)
{
	struct qsa_object *self = to_qsa_object(object);

	int err = snd_pcm_plugin_write(self->handle, data, bytes);
	if (err == -EPIPE) {// underrun
		fprintf (stderr, "qsa: pipe error %s\n", snd_strerror(err));
		err = snd_pcm_plugin_prepare(self->handle, SND_PCM_CHANNEL_PLAYBACK);
		if (err < 0)
			fprintf (stderr, "qsa: prepare error %s\n", snd_strerror(err));
	} else if (err <0 ) {
			fprintf (stderr, "qsa: write error %s\n", snd_strerror(err));
	}
	return err;
}

const char *
qsa_object_strerror(struct audio_object *object,
                    int error)
{
	return snd_strerror(error);
}

struct audio_object *
create_qsa_object(const char *device,
                  const char *application_name,
                  const char *description)
{
	struct qsa_object *self = malloc(sizeof(struct qsa_object));
	if (!self)
		return NULL;

	self->handle = NULL;
	self->sample_size = 0;
	self->device = device ? strdup(device) : NULL;

	self->vtable.open = qsa_object_open;
	self->vtable.close = qsa_object_close;
	self->vtable.destroy = qsa_object_destroy;
	self->vtable.write = qsa_object_write;
	self->vtable.drain = qsa_object_drain;
	self->vtable.flush = qsa_object_flush;
	self->vtable.strerror = qsa_object_strerror;

	return &self->vtable;
}

#else

struct audio_object *
create_qsa_object(const char *device,
                   const char *application_name,
                   const char *description)
{
	return NULL;
}

#endif
/*
 * Minimal ISO-BMFF (MP4) audio track demuxer.
 *
 * Companion to mp4_demux.c: that one extracts the first H.264 video track,
 * this one extracts the first AAC audio track (mp4a + esds).  Only what is
 * needed to feed faad2 and the sample byte ranges are provided.
 */

#ifndef _VDEC_MP4_AUDIO_DEMUX_H_
#define _VDEC_MP4_AUDIO_DEMUX_H_

#include <stdbool.h>
#include <stdint.h>

struct mp4_audio_sample {
	uint64_t offset;	/* byte offset in file */
	uint32_t size;
};

struct mp4_audio {
	int fd;
	const uint8_t *map;	/* transient: whole-file mmap, only alive during open */
	uint64_t map_size;

	uint32_t timescale;	/* mdhd media timescale */
	uint32_t sample_rate;	/* from AudioSampleEntry */
	unsigned int channels;	/* from AudioSampleEntry */

	uint8_t *asc;		/* AudioSpecificConfig from esds (owned copy) */
	unsigned int asc_size;

	struct mp4_audio_sample *samples;
	unsigned int samples_count;

	/* reusable per-sample read buffer (sized to max_sample_size) */
	uint8_t *sample_buf;
	uint32_t sample_buf_cap;
	uint32_t max_sample_size;
};

enum {
	MP4_AUDIO_OK        =  0,
	MP4_AUDIO_ERR_RANGE = -1,
	MP4_AUDIO_ERR_IO    = -2,
};

int mp4_audio_open(struct mp4_audio *a, const char *path);
void mp4_audio_close(struct mp4_audio *a);

int mp4_audio_read_sample(struct mp4_audio *a, unsigned int index,
			  const uint8_t **data, uint32_t *size);

#endif /* _VDEC_MP4_AUDIO_DEMUX_H_ */

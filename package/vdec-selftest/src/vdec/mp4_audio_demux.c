/*
 * Minimal ISO-BMFF (MP4) AAC audio track demuxer. See mp4_audio_demux.h.
 *
 * The parser is deliberately narrow: first 'soun' track with an 'mp4a'
 * sample entry carrying an 'esds' with an AudioSpecificConfig.  That covers
 * the AAC-LC/HE-AAC tracks found in ordinary H.264 MP4 files.  The sample
 * table is expanded into a flat array (same approach as the video demux).
 */

#include "mp4_audio_demux.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define FOURCC(a, b, c, d) \
	(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
	 ((uint32_t)(c) << 8) | (uint32_t)(d))

#define MP4_MAX_SAMPLE_SIZE   (4u * 1024 * 1024)
#define MP4_MAX_ASC           (4u * 1024)

static uint32_t rd_u16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t rd_u32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t rd_u64(const uint8_t *p)
{
	return ((uint64_t)rd_u32(p) << 32) | rd_u32(p + 4);
}

struct box_iter {
	const uint8_t *p;
	const uint8_t *end;
};

static void box_iter_init(struct box_iter *it, const uint8_t *data, uint64_t size)
{
	it->p = data;
	it->end = data + size;
}

static bool box_next(struct box_iter *it, uint32_t *type,
		     const uint8_t **payload, uint64_t *payload_size)
{
	const uint8_t *p = it->p;
	uint64_t size;
	unsigned int hdr = 8;

	uint64_t avail = (uint64_t)(it->end - p);
	if (avail < 8)
		return false;

	size = rd_u32(p);
	*type = rd_u32(p + 4);

	if (size == 1) {
		if (avail < 16)
			return false;
		size = rd_u64(p + 8);
		hdr = 16;
	} else if (size == 0) {
		size = avail;
	}

	if (size < hdr || size > avail)
		return false;

	*payload = p + hdr;
	*payload_size = size - hdr;
	it->p = p + size;
	return true;
}

static bool find_box(const uint8_t *data, uint64_t size, uint32_t type,
		     const uint8_t **payload, uint64_t *payload_size)
{
	struct box_iter it;
	uint32_t t;

	box_iter_init(&it, data, size);
	while (box_next(&it, &t, payload, payload_size))
		if (t == type)
			return true;
	return false;
}

/* ---- MPEG-4 descriptors (esds) ---- */

static bool desc_next(const uint8_t **pp, const uint8_t *end, uint8_t *tag,
		      const uint8_t **payload, uint32_t *len)
{
	const uint8_t *p = *pp;
	uint32_t size = 0;
	int i;

	if (p >= end)
		return false;
	*tag = *p++;
	for (i = 0; i < 4; i++) {
		uint8_t b;

		if (p >= end)
			return false;
		b = *p++;
		size = (size << 7) | (b & 0x7f);
		if (!(b & 0x80))
			break;
	}
	if (i == 4 || size > (uint32_t)(end - p))
		return false;

	*payload = p;
	*len = size;
	*pp = p + size;
	return true;
}

static bool parse_desc_chain(struct mp4_audio *a, const uint8_t *p,
			     const uint8_t *end, int depth)
{
	if (depth > 8)
		return false;

	while (p < end) {
		const uint8_t *payload, *pe;
		uint32_t len;
		uint8_t tag;

		if (!desc_next(&p, end, &tag, &payload, &len))
			return false;
		pe = payload + len;

		switch (tag) {
		case 0x03: {	/* ES_Descriptor */
			const uint8_t *q = payload;
			uint8_t flags;

			if (len < 3)
				break;
			flags = q[2];
			q += 3;
			if (flags & 0x80)	/* streamDependenceFlag */
				q += 2;
			if (flags & 0x40) {	/* URL_Flag */
				if (q >= pe)
					return false;
				q += 1 + q[0];
			}
			if (flags & 0x20)	/* OCRstreamFlag */
				q += 2;
			if (q > pe)
				return false;
			if (!parse_desc_chain(a, q, pe, depth + 1))
				return false;
			break;
		}
		case 0x04:	/* DecoderConfigDescriptor */
			if (len < 13)
				break;
			if (!parse_desc_chain(a, payload + 13, pe, depth + 1))
				return false;
			break;
		case 0x05:	/* DecoderSpecificInfo = AudioSpecificConfig */
			if (a->asc || len == 0 || len > MP4_MAX_ASC)
				break;
			a->asc = malloc(len);
			if (!a->asc)
				return false;
			memcpy(a->asc, payload, len);
			a->asc_size = len;
			break;
		default:
			break;	/* unknown descriptor: skip */
		}
	}

	return a->asc != NULL;
}

static bool parse_esds(struct mp4_audio *a, const uint8_t *data, uint64_t size)
{
	if (size < 4)		/* version/flags */
		return false;
	return parse_desc_chain(a, data + 4, data + size, 0);
}

/* ---- sample tables ---- */

static bool parse_stsd(struct mp4_audio *a, const uint8_t *data, uint64_t size)
{
	const uint8_t *entry, *children, *esds, *wave;
	uint64_t children_size, esds_size, wave_size;
	uint32_t entry_count, fmt;
	unsigned int child_off;
	uint16_t version;

	if (size < 8)
		return false;
	entry_count = rd_u32(data + 4);
	if (entry_count == 0)
		return false;

	entry = data + 8;
	if (entry + 36 > data + size)
		return false;

	fmt = rd_u32(entry + 4);
	if (fmt != FOURCC('m', 'p', '4', 'a') &&
	    fmt != FOURCC('m', 'p', '4', 's')) {
		fprintf(stderr, "mp4a: unsupported sample format '%.4s'\n",
			(const char *)(entry + 4));
		return false;
	}

	version = rd_u16(entry + 16);
	a->channels = rd_u16(entry + 24);
	a->sample_rate = rd_u32(entry + 32) >> 16;

	switch (version) {
	case 1:  child_off = 8 + 28 + 16; break;
	case 2:  child_off = 8 + 28 + 36; break;
	default: child_off = 8 + 28; break;
	}

	{
		uint64_t entry_size = rd_u32(entry);

		if (entry_size < child_off || entry + entry_size > data + size)
			return false;
		children = entry + child_off;
		children_size = entry_size - child_off;
	}

	if (find_box(children, children_size, FOURCC('e', 's', 'd', 's'),
		     &esds, &esds_size))
		return parse_esds(a, esds, esds_size);

	/* QuickTime sometimes wraps esds in a 'wave' box. */
	if (find_box(children, children_size, FOURCC('w', 'a', 'v', 'e'),
		     &wave, &wave_size) &&
	    find_box(wave, wave_size, FOURCC('e', 's', 'd', 's'), &esds,
		     &esds_size))
		return parse_esds(a, esds, esds_size);

	fprintf(stderr, "mp4a: no esds/AudioSpecificConfig\n");
	return false;
}

static int build_samples(struct mp4_audio *a, const uint8_t *stbl,
			 uint64_t stbl_size)
{
	const uint8_t *stsz, *stsc, *stco, *co64;
	uint64_t stsz_sz, stsc_sz, stco_sz, co64_sz;
	uint32_t sample_count, sample_size_default;
	uint32_t chunk_count;
	bool is_co64 = false;
	unsigned int s;

	if (!find_box(stbl, stbl_size, FOURCC('s', 't', 's', 'z'), &stsz, &stsz_sz)) {
		fprintf(stderr, "mp4a: missing stsz\n");
		return -1;
	}
	if (!find_box(stbl, stbl_size, FOURCC('s', 't', 's', 'c'), &stsc, &stsc_sz)) {
		fprintf(stderr, "mp4a: missing stsc\n");
		return -1;
	}
	if (find_box(stbl, stbl_size, FOURCC('s', 't', 'c', 'o'), &stco, &stco_sz)) {
		is_co64 = false;
	} else if (find_box(stbl, stbl_size, FOURCC('c', 'o', '6', '4'), &co64,
			    &co64_sz)) {
		is_co64 = true;
		stco = co64;
		stco_sz = co64_sz;
	} else {
		fprintf(stderr, "mp4a: missing stco/co64\n");
		return -1;
	}

	if (stsz_sz < 12)
		return -1;
	sample_size_default = rd_u32(stsz + 4);
	sample_count = rd_u32(stsz + 8);
	if (sample_count == 0)
		return -1;
	if (sample_size_default == 0) {
		if ((uint64_t)sample_count > (stsz_sz - 12) / 4)
			return -1;
	} else if ((uint64_t)sample_count * sample_size_default > a->map_size) {
		return -1;
	}

	chunk_count = (stco_sz >= 8) ? rd_u32(stco + 4) : 0;
	if (chunk_count == 0)
		return -1;
	if (chunk_count > (stco_sz - 8) / (is_co64 ? 8 : 4))
		return -1;

	a->samples = calloc(sample_count, sizeof(*a->samples));
	if (!a->samples)
		return -1;
	a->samples_count = sample_count;

	for (s = 0; s < sample_count; s++) {
		if (sample_size_default != 0) {
			a->samples[s].size = sample_size_default;
		} else {
			const uint8_t *e = stsz + 12 + (uint64_t)s * 4;

			if (e + 4 > stsz + stsz_sz)
				goto fail;
			a->samples[s].size = rd_u32(e);
		}
		if (a->samples[s].size > a->max_sample_size)
			a->max_sample_size = a->samples[s].size;
	}

	{
		uint32_t stsc_entries = (stsc_sz >= 8) ? rd_u32(stsc + 4) : 0;
		const uint8_t *e = stsc + 8;
		uint32_t sample_idx = 0;
		uint32_t ci;

		if (stsc_entries == 0)
			goto fail;

		for (ci = 0; ci < chunk_count && sample_idx < sample_count; ci++) {
			uint32_t chunk_no = ci + 1;
			uint32_t spc = 0;
			uint32_t k;
			uint64_t off;

			for (k = 0; k < stsc_entries; k++) {
				const uint8_t *ek = e + (uint64_t)k * 12;
				uint32_t first_chunk;

				if (ek + 12 > stsc + stsc_sz)
					goto fail;
				first_chunk = rd_u32(ek);
				if (first_chunk <= chunk_no)
					spc = rd_u32(ek + 4);
				else
					break;
			}
			if (spc == 0)
				continue;

			if (is_co64)
				off = rd_u64(stco + 8 + (uint64_t)ci * 8);
			else
				off = rd_u32(stco + 8 + (uint64_t)ci * 4);

			for (k = 0; k < spc && sample_idx < sample_count; k++) {
				a->samples[sample_idx].offset = off;
				off += a->samples[sample_idx].size;
				sample_idx++;
			}
		}
		if (sample_idx < sample_count) {
			fprintf(stderr, "mp4a: only mapped %u/%u samples\n",
				sample_idx, sample_count);
			a->samples_count = sample_idx;
		}
	}

	return 0;

fail:
	free(a->samples);
	a->samples = NULL;
	a->samples_count = 0;
	return -1;
}

static bool parse_trak(struct mp4_audio *a, const uint8_t *trak, uint64_t size)
{
	const uint8_t *mdia, *hdlr, *minf, *stbl, *stsd, *mdhd;
	uint64_t mdia_sz, hdlr_sz, minf_sz, stbl_sz, stsd_sz, mdhd_sz;

	if (!find_box(trak, size, FOURCC('m', 'd', 'i', 'a'), &mdia, &mdia_sz))
		return false;
	if (!find_box(mdia, mdia_sz, FOURCC('h', 'd', 'l', 'r'), &hdlr, &hdlr_sz))
		return false;
	if (hdlr_sz < 12 || rd_u32(hdlr + 8) != FOURCC('s', 'o', 'u', 'n'))
		return false;

	if (!find_box(mdia, mdia_sz, FOURCC('m', 'i', 'n', 'f'), &minf, &minf_sz))
		return false;
	if (!find_box(minf, minf_sz, FOURCC('s', 't', 'b', 'l'), &stbl, &stbl_sz))
		return false;
	if (!find_box(stbl, stbl_sz, FOURCC('s', 't', 's', 'd'), &stsd, &stsd_sz))
		return false;

	if (find_box(mdia, mdia_sz, FOURCC('m', 'd', 'h', 'd'), &mdhd, &mdhd_sz) &&
	    mdhd_sz >= 16)
		a->timescale = rd_u32(mdhd + (mdhd[0] == 1 ? 20 : 12));

	if (!parse_stsd(a, stsd, stsd_sz))
		return false;
	if (build_samples(a, stbl, stbl_sz) < 0 || a->samples_count == 0)
		return false;

	return true;
}

int mp4_audio_open(struct mp4_audio *a, const char *path)
{
	struct stat st;
	const uint8_t *moov;
	uint64_t moov_sz;
	struct box_iter it;
	uint32_t type;
	const uint8_t *payload;
	uint64_t payload_sz;
	bool found = false;

	memset(a, 0, sizeof(*a));
	a->fd = -1;

	a->fd = open(path, O_RDONLY);
	if (a->fd < 0) {
		fprintf(stderr, "mp4a: cannot open %s\n", path);
		return -1;
	}
	if (fstat(a->fd, &st) < 0 || st.st_size <= 0)
		goto fail;

	a->map_size = (uint64_t)st.st_size;
	a->map = mmap(NULL, a->map_size, PROT_READ, MAP_PRIVATE, a->fd, 0);
	if (a->map == MAP_FAILED) {
		a->map = NULL;
		fprintf(stderr, "mp4a: mmap failed\n");
		goto fail;
	}

	if (!find_box(a->map, a->map_size, FOURCC('m', 'o', 'o', 'v'), &moov,
		      &moov_sz)) {
		fprintf(stderr, "mp4a: no moov box\n");
		goto fail;
	}

	box_iter_init(&it, moov, moov_sz);
	while (box_next(&it, &type, &payload, &payload_sz)) {
		if (type != FOURCC('t', 'r', 'a', 'k'))
			continue;
		if (parse_trak(a, payload, payload_sz)) {
			found = true;
			break;
		}
		/* A rejected track may have left a partial parse behind
		 * (e.g. an esds parsed before its sample table failed). */
		free(a->asc);
		a->asc = NULL;
		a->asc_size = 0;
		free(a->samples);
		a->samples = NULL;
		a->samples_count = 0;
		a->max_sample_size = 0;
		a->timescale = 0;
		a->sample_rate = 0;
		a->channels = 0;
	}

	if (!found) {
		fprintf(stderr, "mp4a: no usable AAC audio track\n");
		goto fail;
	}
	if (!a->asc || a->asc_size == 0) {
		fprintf(stderr, "mp4a: no AudioSpecificConfig\n");
		goto fail;
	}
	if (a->max_sample_size > MP4_MAX_SAMPLE_SIZE) {
		fprintf(stderr, "mp4a: implausible sample size %u\n",
			a->max_sample_size);
		goto fail;
	}

	a->sample_buf_cap = a->max_sample_size;
	a->sample_buf = malloc(a->sample_buf_cap ? a->sample_buf_cap : 1);
	if (!a->sample_buf)
		goto fail;

	munmap((void *)a->map, a->map_size);
	a->map = NULL;
	a->map_size = 0;
	return 0;

fail:
	mp4_audio_close(a);
	return -1;
}

void mp4_audio_close(struct mp4_audio *a)
{
	if (a->samples)
		free(a->samples);
	if (a->map)
		munmap((void *)a->map, a->map_size);
	if (a->asc)
		free(a->asc);
	if (a->sample_buf)
		free(a->sample_buf);
	if (a->fd >= 0)
		close(a->fd);
	memset(a, 0, sizeof(*a));
	a->fd = -1;
}

int mp4_audio_read_sample(struct mp4_audio *a, unsigned int index,
			  const uint8_t **data, uint32_t *size)
{
	const struct mp4_audio_sample *s;
	ssize_t n;

	if (index >= a->samples_count)
		return MP4_AUDIO_ERR_RANGE;
	s = &a->samples[index];
	if (s->size > a->sample_buf_cap)
		return MP4_AUDIO_ERR_RANGE;

	n = pread(a->fd, a->sample_buf, s->size, (off_t)s->offset);
	if (n < 0 || (uint32_t)n != s->size)
		return MP4_AUDIO_ERR_IO;

	if (data)
		*data = a->sample_buf;
	if (size)
		*size = s->size;
	return MP4_AUDIO_OK;
}

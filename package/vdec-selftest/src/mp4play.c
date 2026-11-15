/*
 * mp4play — SD-card H.264/AAC MP4 player for the Allwinner F1C100s.
 *
 * Video: cedrus V4L2 stateless decoder -> DRM plane (tiled NV12), using the
 * same decode/DPB contract as vdec_selftest.
 *
 * Audio: minimal MP4 AAC track demuxer -> faad2 -> tinyalsa.  Audio is the
 * master clock for video presentation when it is healthy; playback falls
 * back to the monotonic clock if the audio device is missing or fails.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <neaacdec.h>
#include <tinyalsa/asoundlib.h>

#include "h264_dpb.h"
#include "h264_parser.h"
#include "mp4_audio_demux.h"
#include "mp4_demux.h"
#include "nalu.h"
#include "vdec_v4l2.h"

#ifndef DRM_FORMAT_MOD_ALLWINNER_TILED
#define DRM_FORMAT_MOD_VENDOR_ALLWINNER 0x09
#define DRM_FORMAT_MOD_ALLWINNER_TILED fourcc_mod_code(ALLWINNER, 1)
#endif

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((unsigned int)(a) - 1))

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static int64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static const char *slice_type_str(uint8_t t)
{
	switch (t % 5) {
	case H264_SLICE_P: return "P";
	case H264_SLICE_B: return "B";
	case H264_SLICE_I: return "I";
	case H264_SLICE_SP: return "SP";
	case H264_SLICE_SI: return "SI";
	default: return "?";
	}
}

static void h264_dimensions(const struct h264_sps *sps, unsigned int *w,
			    unsigned int *h)
{
	*w = (sps->pic_width_in_mbs_minus1 + 1) * 16;
	*h = (sps->pic_height_in_map_units_minus1 + 1) * 16 *
	     (sps->frame_mbs_only_flag ? 1 : 2);
}

static const struct h264_sps *first_sps(struct h264_parser *p)
{
	int i;

	for (i = 0; i < 32; i++) {
		const struct h264_sps *s = h264_parser_get_sps(p, i);
		if (s)
			return s;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* DRM display                                                         */
/* ------------------------------------------------------------------ */

struct display {
	int fd;
	uint32_t crtc_id;
	uint32_t plane_id;
	unsigned int screen_w, screen_h;
	unsigned int disp_w, disp_h;	/* container display dimensions */
	unsigned int src_w, src_h;	/* DRM source rectangle */
	unsigned int fb_w, fb_h;	/* DRM framebuffer dimensions */
	int coded_fb;			/* full coded FB + padding-aware geometry */
	int fill;			/* coded FB: scale to fill, clip padding */
	int error_reported;
	int geometry_logged;
	uint32_t fb_ids[VDEC_MAX_CAP_BUFS];
};

static void display_close(struct display *d, struct vdec_ctx *v);

static const char *display_mode_name(const struct display *d)
{
	if (!d->coded_fb)
		return "visible";
	return d->fill ? "coded-fill" : "coded";
}

static int display_open(struct display *d, struct vdec_ctx *v,
			int plane_index, unsigned int disp_w,
			unsigned int disp_h, int coded_fb, int fill)
{
	drmModeRes *res;
	drmModePlaneRes *planes;
	drmModeCrtc *crtc;
	unsigned int i;

	memset(d, 0, sizeof(*d));
	d->fd = -1;
	d->coded_fb = coded_fb;
	d->fill = fill;
	d->disp_w = disp_w ? disp_w : v->cap_width;
	d->disp_h = disp_h ? disp_h : v->cap_height;
	if (d->disp_w > v->cap_width)
		d->disp_w = v->cap_width;
	if (d->disp_h > v->cap_height)
		d->disp_h = v->cap_height;
	d->src_w = coded_fb ? v->cap_width : d->disp_w;
	d->src_h = coded_fb ? v->cap_height : d->disp_h;
	d->fb_w = d->src_w;
	d->fb_h = d->src_h;

	d->fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (d->fd < 0) {
		fprintf(stderr, "open /dev/dri/card0: %s\n", strerror(errno));
		return -1;
	}
	drmSetClientCap(d->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

	res = drmModeGetResources(d->fd);
	if (!res || !res->count_crtcs) {
		if (res)
			drmModeFreeResources(res);
		display_close(d, v);
		return -1;
	}
	d->crtc_id = res->crtcs[0];
	crtc = drmModeGetCrtc(d->fd, d->crtc_id);
	if (crtc) {
		d->screen_w = crtc->mode.hdisplay;
		d->screen_h = crtc->mode.vdisplay;
		drmModeFreeCrtc(crtc);
	}
	drmModeFreeResources(res);

	planes = drmModeGetPlaneResources(d->fd);
	if (!planes || (unsigned int)plane_index >= planes->count_planes) {
		fprintf(stderr, "bad plane index %d\n", plane_index);
		display_close(d, v);
		return -1;
	}
	d->plane_id = planes->planes[plane_index];
	drmModeFreePlaneResources(planes);

	for (i = 0; i < v->cap_count; i++) {
		uint32_t handle = 0;
		uint32_t handles[4] = { 0 }, pitches[4] = { 0 },
			 offsets[4] = { 0 };
		uint64_t modifiers[4] = { 0 };

		if (drmPrimeFDToHandle(d->fd, v->cap[i].dmabuf_fd, &handle) < 0) {
			fprintf(stderr, "PrimeFDToHandle[%u]: %s\n", i,
				strerror(errno));
			display_close(d, v);
			return -1;
		}
		handles[0] = handles[1] = handle;
		pitches[0] = pitches[1] = v->cap_bytesperline;
		offsets[0] = 0;
		offsets[1] = v->cap_uv_offset;
		modifiers[0] = modifiers[1] = DRM_FORMAT_MOD_ALLWINNER_TILED;

		if (drmModeAddFB2WithModifiers(d->fd, d->fb_w, d->fb_h,
					       DRM_FORMAT_NV12, handles, pitches,
					       offsets, modifiers, &d->fb_ids[i],
					       DRM_MODE_FB_MODIFIERS) < 0) {
			fprintf(stderr, "AddFB2[%u] %ux%u: %s\n", i,
				d->fb_w, d->fb_h, strerror(errno));
			display_close(d, v);
			return -1;
		}
	}

	printf("display: plane %u crtc %u screen %ux%u fb %ux%u src %ux%u (%s FB)\n",
	       d->plane_id, d->crtc_id, d->screen_w, d->screen_h,
	       d->fb_w, d->fb_h, d->src_w, d->src_h, display_mode_name(d));
	return 0;
}

static int display_show(struct display *d, struct vdec_ctx *v, int slot)
{
	unsigned int disp_w = d->disp_w, disp_h = d->disp_h;
	unsigned int src_w = d->src_w, src_h = d->src_h;
	unsigned int dst_w = disp_w, dst_h = disp_h;
	unsigned int dst_x = 0, dst_y = 0;
	unsigned int plane_w, plane_h;

	(void)v;

	/*
	 * Fit the *container* display size to the panel, not the coded size.
	 * The coded size may include bottom/right macroblock padding (e.g.
	 * 1024x600 arrives as 1024x608), which previously produced wrong
	 * pillarboxes and asymmetric letterboxes.
	 */
	if (d->screen_w && d->screen_h && disp_w && disp_h) {
		uint64_t by_w = (uint64_t)d->screen_w * disp_h;
		uint64_t by_h = (uint64_t)d->screen_h * disp_w;

		if (by_w <= by_h) {
			dst_w = d->screen_w;
			dst_h = (unsigned int)(((uint64_t)disp_h * d->screen_w +
						disp_w / 2) / disp_w);
		} else {
			dst_h = d->screen_h;
			dst_w = (unsigned int)(((uint64_t)disp_w * d->screen_h +
						disp_h / 2) / disp_h);
		}
		if (dst_w < 1)
			dst_w = 1;
		if (dst_h < 1)
			dst_h = 1;
		dst_x = (d->screen_w - dst_w) / 2;
		dst_y = (d->screen_h - dst_h) / 2;
	}

	plane_w = dst_w;
	plane_h = dst_h;

	if (d->coded_fb && !d->fill && disp_w && disp_h) {
		/*
		 * The FB still contains the coded padding below/right of the
		 * visible picture.  Keep the visible content centered and
		 * shrink it by at most a few pixels so the whole padded plane
		 * fits on screen; the padding then falls into the bottom/right
		 * black bar.
		 */
		while (dst_w > 1 && dst_h > 1) {
			plane_w = (unsigned int)((uint64_t)src_w * dst_w /
						 disp_w);
			plane_h = (unsigned int)((uint64_t)src_h * dst_h /
						 disp_h);
			if (plane_w < 1)
				plane_w = 1;
			if (plane_h < 1)
				plane_h = 1;
			dst_x = (d->screen_w - dst_w) / 2;
			dst_y = (d->screen_h - dst_h) / 2;
			if (dst_x + plane_w <= d->screen_w &&
			    dst_y + plane_h <= d->screen_h)
				break;
			dst_w--;
			dst_h = (unsigned int)((uint64_t)disp_h * dst_w /
					       disp_w);
			if (dst_h < 1)
				dst_h = 1;
		}
	}

	if (d->coded_fb && d->fill && disp_w && disp_h) {
		/*
		 * Fill mode: keep the coded padding in the plane but let it
		 * extend past the bottom/right edge.  The display controller
		 * clips it, so the visible content gets the exact geometry
		 * computed above instead of being shrunk to fit the padding.
		 */
		plane_w = (unsigned int)(((uint64_t)src_w * dst_w +
					  disp_w - 1) / disp_w);
		plane_h = (unsigned int)(((uint64_t)src_h * dst_h +
					  disp_h - 1) / disp_h);
		if (plane_w < 1)
			plane_w = 1;
		if (plane_h < 1)
			plane_h = 1;
	}

	if (!d->geometry_logged) {
		fprintf(stderr,
			"display: src %ux%u -> content %ux%u+%u+%u, plane %ux%u (%s FB, screen %ux%u)\n",
			src_w, src_h, dst_w, dst_h, dst_x, dst_y,
			plane_w, plane_h, display_mode_name(d),
			d->screen_w, d->screen_h);
		d->geometry_logged = 1;
	}

	{
		int ret;

		ret = drmModeSetPlane(d->fd, d->plane_id, d->crtc_id,
				      d->fb_ids[slot], 0, dst_x, dst_y,
				      plane_w, plane_h, 0, 0,
				      src_w << 16, src_h << 16);
		if (ret < 0 && !d->error_reported) {
			fprintf(stderr, "SetPlane %ux%u+%u+%u: %s\n",
				plane_w, plane_h, dst_x, dst_y,
				strerror(errno));
			d->error_reported = 1;
		}
		return ret;
	}
}

static void display_close(struct display *d, struct vdec_ctx *v)
{
	unsigned int i;

	if (d->fd < 0)
		return;
	for (i = 0; i < v->cap_count; i++)
		if (d->fb_ids[i])
			drmModeRmFB(d->fd, d->fb_ids[i]);
	close(d->fd);
	d->fd = -1;
}

/* ------------------------------------------------------------------ */
/* Audio: faad2 + tinyalsa                                             */
/* ------------------------------------------------------------------ */

struct audio_player {
	struct mp4_audio demux;
	NeAACDecHandle dec;
	struct pcm *pcm;
	struct mixer *mixer;
	struct mixer_ctl *hp_pa;
	pthread_t thread;

	pthread_mutex_t lock;
	pthread_cond_t cond;
	int start;
	int started;
	int stop;
	int done;
	int failed;
	int hp_pa_on;

	uint64_t frames_written;
	int64_t clock_us;
	unsigned int rate;
	unsigned int channels;

	int16_t *stereo;
	unsigned int stereo_frames;
};

/* Digital silence below this peak (S16) counts as "no audio". */
#define HP_PA_SILENCE_PEAK	16
#define HP_PA_SILENCE_HOLD_US	500000

static void audio_set_pa(struct audio_player *ap, int on)
{
	if (!ap || !ap->hp_pa || ap->hp_pa_on == on)
		return;
	if (mixer_ctl_set_value(ap->hp_pa, 0, on) == 0)
		ap->hp_pa_on = on;
}

static void mixer_set_enum(struct mixer *mx, const char *name, const char *val)
{
	struct mixer_ctl *c = mixer_get_ctl_by_name(mx, name);

	if (!c)
		return;
	if (mixer_ctl_set_enum_by_string(c, val) < 0)
		fprintf(stderr, "mixer: cannot set %s=%s\n", name, val);
}

static void mixer_set_all(struct mixer *mx, const char *name, int value)
{
	struct mixer_ctl *c = mixer_get_ctl_by_name(mx, name);
	unsigned int i, n;

	if (!c)
		return;
	n = mixer_ctl_get_num_values(c);
	for (i = 0; i < n; i++)
		mixer_ctl_set_value(c, i, value);
}

static void mixer_set_volume(struct mixer *mx, const char *name, int percent)
{
	struct mixer_ctl *c = mixer_get_ctl_by_name(mx, name);
	unsigned int i, n;
	int min, max, val;

	if (!c)
		return;
	n = mixer_ctl_get_num_values(c);
	min = mixer_ctl_get_range_min(c);
	max = mixer_ctl_get_range_max(c);
	val = min + (max - min) * percent / 100;
	for (i = 0; i < n; i++)
		mixer_ctl_set_value(c, i, val);
}

static void audio_mixer_setup(struct audio_player *ap)
{
	struct mixer *mx = mixer_open(0);

	if (!mx) {
		fprintf(stderr, "audio: cannot open mixer card 0\n");
		return;
	}

	/* suniv codec: route the DAC straight to the headphone amp. */
	mixer_set_enum(mx, "Headphone Source Playback Route", "DAC");
	mixer_set_all(mx, "Headphone Playback Switch", 1);
	/*
	 * Headphone TLV is 0 dB at max; DAC digital volume is inverted
	 * (max user value = 0 dB).  Keep the analog stage at full and trim
	 * the DAC a little so typical movie tracks do not clip.
	 */
	mixer_set_volume(mx, "Headphone Playback Volume", 100);
	mixer_set_volume(mx, "DAC Playback Volume", 85);

	ap->mixer = mx;
	ap->hp_pa = mixer_get_ctl_by_name(mx, "Headphone PA Enable");
	if (ap->hp_pa) {
		mixer_ctl_set_value(ap->hp_pa, 0, 1);
		ap->hp_pa_on = 1;
	} else {
		fprintf(stderr, "audio: no 'Headphone PA Enable' control\n");
	}
}

static void audio_publish_locked(struct audio_player *ap)
{
	long delay = pcm_get_delay(ap->pcm);
	uint64_t written = ap->frames_written;
	int64_t clock;

	if (delay < 0)
		delay = 0;
	if ((uint64_t)delay > written)
		delay = (long)written;
	clock = (int64_t)((written - (uint64_t)delay) * 1000000ULL /
			  ap->rate);
	ap->clock_us = clock;
}

static int audio_stereo_reserve(struct audio_player *ap, unsigned int frames)
{
	int16_t *p;
	unsigned int cap;

	if (frames <= ap->stereo_frames)
		return 0;
	cap = frames + frames / 2 + 64;
	p = realloc(ap->stereo, (size_t)cap * 2 * sizeof(int16_t));
	if (!p)
		return -1;
	ap->stereo = p;
	ap->stereo_frames = cap;
	return 0;
}

static void *audio_thread(void *arg)
{
	struct audio_player *ap = arg;
	unsigned int i;
	unsigned int decode_errors = 0;

	int64_t silent_us = 0;

	pthread_mutex_lock(&ap->lock);
	while (!ap->start)
		pthread_cond_wait(&ap->cond, &ap->lock);
	pthread_mutex_unlock(&ap->lock);

	if (ap->stop)
		goto out;

	for (i = 0; i < ap->demux.samples_count; i++) {
		const uint8_t *data = NULL;
		uint32_t size = 0;
		NeAACDecFrameInfo info;
		void *pcm;
		unsigned int frames;
		int rc;

		if (ap->stop || g_stop)
			break;
		if (mp4_audio_read_sample(&ap->demux, i, &data, &size) !=
		    MP4_AUDIO_OK) {
			fprintf(stderr, "audio: read sample %u failed\n", i);
			ap->failed = 1;
			break;
		}

		memset(&info, 0, sizeof(info));
		pcm = NeAACDecDecode(ap->dec, &info, (unsigned char *)data,
				     size);
		if (!pcm || info.error) {
			if (decode_errors++ < 5)
				fprintf(stderr, "audio: AAC error %d at sample %u\n",
					info.error, i);
			if (decode_errors > 50) {
				ap->failed = 1;
				break;
			}
			continue;
		}
		if (!info.channels)
			continue;
		frames = info.samples / info.channels;
		if (!frames)
			continue;

		/*
		 * The suniv Class-D headphone PA has an audible hiss/squeak
		 * even with zeroed DAC input.  Power it down after a run of
		 * digital silence and bring it back as soon as real audio
		 * returns.  This keeps silent scenes quiet without gating
		 * normal music.
		 */
		{
			const int16_t *s = pcm;
			unsigned int peak = 0;
			unsigned int k;

			for (k = 0; k < info.samples; k++) {
				int v = s[k];

				if (v < 0)
					v = -v;
				if ((unsigned int)v > peak)
					peak = (unsigned int)v;
			}
			if (peak <= HP_PA_SILENCE_PEAK) {
				silent_us += (int64_t)frames * 1000000 /
					     ap->rate;
				if (silent_us >= HP_PA_SILENCE_HOLD_US)
					audio_set_pa(ap, 0);
			} else {
				silent_us = 0;
				audio_set_pa(ap, 1);
			}
		}

		if (info.channels == 1) {
			unsigned int j;
			const int16_t *src = pcm;

			if (audio_stereo_reserve(ap, frames) < 0) {
				ap->failed = 1;
				break;
			}
			for (j = 0; j < frames; j++) {
				ap->stereo[2 * j] = src[j];
				ap->stereo[2 * j + 1] = src[j];
			}
			rc = pcm_writei(ap->pcm, ap->stereo, frames);
		} else if (info.channels == 2) {
			rc = pcm_writei(ap->pcm, pcm, frames);
		} else {
			/* downMatrix should have made this stereo already */
			continue;
		}

		if (rc < 0) {
			fprintf(stderr, "audio: pcm write failed: %s\n",
				pcm_get_error(ap->pcm));
			ap->failed = 1;
			break;
		}

		pthread_mutex_lock(&ap->lock);
		ap->frames_written += frames;
		audio_publish_locked(ap);
		pthread_mutex_unlock(&ap->lock);
	}

	/* Let the hardware FIFO/buffer play out before closing the PCM. */
	if (!ap->stop && !g_stop) {
		int tries;

		for (tries = 0; tries < 200; tries++) {
			long delay;

			pthread_mutex_lock(&ap->lock);
			audio_publish_locked(ap);
			delay = pcm_get_delay(ap->pcm);
			pthread_mutex_unlock(&ap->lock);
			if (delay <= 0)
				break;
			usleep(10000);
		}
	}

out:
	pthread_mutex_lock(&ap->lock);
	ap->done = 1;
	pthread_mutex_unlock(&ap->lock);
	return NULL;
}

static int audio_open(struct audio_player *ap, const char *path)
{
	NeAACDecConfigurationPtr cfg;
	unsigned long rate = 0;
	unsigned char channels = 0;
	struct pcm_config pcm_cfg;

	memset(ap, 0, sizeof(*ap));
	ap->demux.fd = -1;
	pthread_mutex_init(&ap->lock, NULL);
	pthread_cond_init(&ap->cond, NULL);

	if (mp4_audio_open(&ap->demux, path) < 0)
		return -1;

	ap->dec = NeAACDecOpen();
	if (!ap->dec) {
		fprintf(stderr, "audio: NeAACDecOpen failed\n");
		return -1;
	}
	cfg = NeAACDecGetCurrentConfiguration(ap->dec);
	if (!cfg) {
		fprintf(stderr, "audio: no faad2 config\n");
		return -1;
	}
	cfg->defSampleRate = ap->demux.sample_rate ?
		ap->demux.sample_rate : 44100;
	cfg->outputFormat = FAAD_FMT_16BIT;
	cfg->downMatrix = 1;
	cfg->useOldADTSFormat = 0;
	cfg->dontUpSampleImplicitSBR = 0;
	/* faad2: SetConfiguration returns 1 on success; Init2 returns -1 on
	 * error but is declared 'char', which is unsigned on ARM. */
	if (!NeAACDecSetConfiguration(ap->dec, cfg)) {
		fprintf(stderr, "audio: faad2 config rejected\n");
		return -1;
	}

	if ((signed char)NeAACDecInit2(ap->dec, ap->demux.asc,
				       ap->demux.asc_size, &rate,
				       &channels) < 0) {
		fprintf(stderr, "audio: NeAACDecInit2 failed (ASC %u bytes)\n",
			ap->demux.asc_size);
		return -1;
	}
	if (!rate)
		rate = ap->demux.sample_rate;
	if (!channels)
		channels = ap->demux.channels;
	if (!rate) {
		fprintf(stderr, "audio: no sample rate\n");
		return -1;
	}
	ap->rate = rate;
	ap->channels = channels;

	memset(&pcm_cfg, 0, sizeof(pcm_cfg));
	pcm_cfg.channels = 2;
	pcm_cfg.rate = ap->rate;
	pcm_cfg.period_size = 512;
	pcm_cfg.period_count = 4;
	pcm_cfg.format = PCM_FORMAT_S16_LE;
	pcm_cfg.start_threshold = 512;
	pcm_cfg.stop_threshold = 512 * 4;
	pcm_cfg.silence_threshold = 0;

	ap->pcm = pcm_open(0, 0, PCM_OUT, &pcm_cfg);
	if (!ap->pcm || !pcm_is_ready(ap->pcm)) {
		fprintf(stderr, "audio: pcm open failed: %s\n",
			ap->pcm ? pcm_get_error(ap->pcm) : "no memory");
		return -1;
	}

	audio_mixer_setup(ap);

	printf("audio: AAC %u Hz, %u ch -> ALSA %u Hz stereo\n",
	       ap->demux.sample_rate, ap->demux.channels, ap->rate);
	return 0;
}

static int audio_start(struct audio_player *ap)
{
	int rc;

	if (!ap || !ap->pcm)
		return -1;
	rc = pthread_create(&ap->thread, NULL, audio_thread, ap);
	if (rc != 0) {
		fprintf(stderr, "audio: pthread_create: %s\n", strerror(rc));
		ap->failed = 1;
		return -1;
	}
	ap->started = 1;
	return 0;
}

/*
 * The audio thread is created before video decode but must not write a
 * single sample until the first video frame is actually on screen, or the
 * sound track runs ahead by the decoder warm-up time.
 */
static void audio_begin(struct audio_player *ap)
{
	if (!ap || !ap->started)
		return;
	pthread_mutex_lock(&ap->lock);
	ap->start = 1;
	pthread_cond_broadcast(&ap->cond);
	pthread_mutex_unlock(&ap->lock);
}

static int64_t audio_clock_us(struct audio_player *ap)
{
	int64_t v;

	if (!ap)
		return -1;
	pthread_mutex_lock(&ap->lock);
	audio_publish_locked(ap);
	v = ap->clock_us;
	pthread_mutex_unlock(&ap->lock);
	return v;
}

static void audio_close(struct audio_player *ap)
{
	if (!ap)
		return;
	if (ap->started) {
		pthread_mutex_lock(&ap->lock);
		ap->stop = 1;
		ap->start = 1;
		pthread_cond_broadcast(&ap->cond);
		pthread_mutex_unlock(&ap->lock);
		pthread_join(ap->thread, NULL);
		ap->started = 0;
	}
	if (ap->pcm) {
		pcm_close(ap->pcm);
		ap->pcm = NULL;
	}
	if (ap->mixer) {
		mixer_close(ap->mixer);
		ap->mixer = NULL;
		ap->hp_pa = NULL;
	}
	if (ap->dec) {
		NeAACDecClose(ap->dec);
		ap->dec = NULL;
	}
	mp4_audio_close(&ap->demux);
	free(ap->stereo);
	ap->stereo = NULL;
	pthread_mutex_destroy(&ap->lock);
	pthread_cond_destroy(&ap->cond);
}

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

struct play_clock {
	struct audio_player *audio;
	int64_t mono_start;
	int first_shown;
};

static int64_t play_now_us(struct play_clock *pc)
{
	int64_t mono = now_us() - pc->mono_start;
	int64_t a;

	if (!pc->audio || !pc->audio->started || pc->audio->done ||
	    pc->audio->failed)
		return mono;

	a = audio_clock_us(pc->audio);
	if (a < 0)
		return mono;
	/*
	 * The audio clock is authoritative while it stays close to real
	 * time.  A large error means an underrun or a driver hiccup; do not
	 * let it stall (or rush) video in that case.
	 */
	if (a > mono + 250000 || a < mono - 500000)
		return mono;
	return a;
}

static void play_wait_until(int64_t target_us, struct play_clock *pc)
{
	while (!g_stop) {
		int64_t now = play_now_us(pc);

		if (now >= target_us)
			break;
		if (target_us - now > 20000)
			usleep(5000);
		else if (target_us - now > 2000)
			usleep(1000);
		else
			break;
	}
}

static int display_frame(struct display *disp, struct vdec_ctx *vdec,
			 struct h264_dpb *dpb, int slot, int64_t pts_us,
			 int64_t origin_us, struct play_clock *pc,
			 int held[2], unsigned int *shown)
{
	int64_t target;

	if (!pc->first_shown) {
		audio_begin(pc->audio);
		pc->mono_start = now_us();
		pc->first_shown = 1;
		target = 0;
	} else {
		target = pts_us - origin_us;
		if (target < 0)
			target = 0;
		play_wait_until(target, pc);
	}

	if (g_stop)
		return 0;

	/* display_show() logs the first failure with errno; keep going. */
	(void)display_show(disp, vdec, slot);

	h264_dpb_set_on_screen(dpb, slot, true);
	if (held[1] >= 0)
		h264_dpb_set_on_screen(dpb, held[1], false);
	held[1] = held[0];
	held[0] = slot;
	h264_dpb_mark_displayed(dpb, slot);
	(*shown)++;
	return 0;
}

static int device_play(struct mp4_demux *m, int plane_index,
		       struct audio_player *audio, unsigned int buf_override,
		       int coded_fb, int fill)
{
	struct h264_parser parser;
	struct h264_dpb dpb;
	struct vdec_ctx vdec;
	struct display disp;
	const struct h264_sps *sps0;
	char video_path[32], media_path[32];
	unsigned int width, height, cap_count, max_ref, max_frame_num = 16;
	unsigned int disp_w, disp_h;
	unsigned int reorder_depth = 2;
	unsigned int i, decode_count = 0, display_count = 0;
	unsigned int frame_dur;
	int64_t slot_pts[VDEC_MAX_CAP_BUFS];
	int64_t origin_us = 0;
	int held[2] = { -1, -1 };
	struct play_clock pc;
	int rc = -1, out;

	memset(slot_pts, 0, sizeof(slot_pts));
	memset(&pc, 0, sizeof(pc));
	pc.audio = audio;

	h264_parser_init(&parser);
	h264_parser_parse_avcc(&parser, m->extradata, m->extradata_size);

	sps0 = first_sps(&parser);
	if (!sps0) {
		fprintf(stderr, "no SPS in avcC\n");
		return -1;
	}
	h264_dimensions(sps0, &width, &height);
	width = ALIGN_UP(width, 32);
	height = ALIGN_UP(height, 32);
	max_frame_num = 1 << (sps0->log2_max_frame_num_minus4 + 4);
	max_ref = sps0->max_num_ref_frames ? sps0->max_num_ref_frames : 1;

	{
		unsigned int reorder = sps0->vui_reorder_valid ?
			sps0->vui_max_num_reorder_frames :
			(max_ref < 2 ? 2 : max_ref);
		unsigned int cap_max = width * height >= 600 * 1000 ? 9 : 16;

		cap_count = sps0->vui_reorder_valid ?
			(unsigned int)sps0->vui_max_dec_frame_buffering + 5 :
			max_ref + reorder + 3;
		disp_w = m->width ? m->width : width;
		disp_h = m->height ? m->height : height;

		if (disp_w > width)
			disp_w = width;
		if (disp_h > height)
			disp_h = height;

		if (cap_count < 6)
			cap_count = 6;
		if (cap_count > cap_max)
			cap_count = cap_max;
		/*
		 * The board's default CMA is sized for 1024x600.  A 720p
		 * stream still fits with four buffers, while the automatic
		 * VUI-derived count (usually 7) can exhaust CMA after the
		 * framebuffer has fragmented it.  Keep four as the automatic
		 * choice above the panel size; -b still overrides it.
		 */
		if (!buf_override && (disp_w > 1024 || disp_h > 600) &&
		    cap_count > 4)
			cap_count = 4;
		if (buf_override >= 4 && buf_override <= cap_max)
			cap_count = buf_override;
		reorder_depth = reorder;

		if (disp_w > 1280 || disp_h > 720)
			fprintf(stderr,
				"warning: %ux%u is above the 720p real-time "
				"capability; playback may be slow or fail\n",
				disp_w, disp_h);
	}

	frame_dur = m->frame_duration_us ? m->frame_duration_us : 33333;
	(void)frame_dur;

	printf("video: %ux%u display %ux%u max_ref=%u cap_bufs=%u samples=%u\n",
	       width, height, disp_w, disp_h, max_ref, cap_count,
	       m->samples_count);

	if (vdec_find_device(video_path, sizeof(video_path), media_path,
			     sizeof(media_path)) < 0)
		return -1;
	printf("device: %s + %s\n", video_path, media_path);

	if (vdec_open(&vdec, video_path, media_path, width, height, cap_count,
		      2, 512 * 1024) < 0)
		return -1;

	printf("capture: %ux%u bpl=%u sizeimage=%u uv_off=%u\n",
	       vdec.cap_width, vdec.cap_height, vdec.cap_bytesperline,
	       vdec.cap_sizeimage, vdec.cap_uv_offset);

	if (m->max_sample_size > 512 * 1024) {
		fprintf(stderr, "max sample %u exceeds output buffer\n",
			m->max_sample_size);
		goto out_vdec;
	}

	if (display_open(&disp, &vdec, plane_index, disp_w, disp_h,
			 coded_fb, fill) < 0) {
		if (coded_fb)
			goto out_vdec;
		/*
		 * A display-sized FB can be rejected when the decoder output
		 * has a non-32-aligned visible height.  Fall back to the full
		 * coded FB in fill mode, which is always accepted and keeps
		 * the visible content geometry exact.
		 */
		fprintf(stderr,
			"display: visible-size FB failed, retrying with coded-fill FB\n");
		if (display_open(&disp, &vdec, plane_index, disp_w, disp_h,
				 1, 1) < 0)
			goto out_vdec;
	}

	h264_dpb_init(&dpb, cap_count, max_ref, max_frame_num, reorder_depth);

	if (m->samples_count) {
		unsigned int s;

		origin_us = m->samples[0].pts_us;
		for (s = 1; s < m->samples_count; s++)
			if (m->samples[s].pts_us < origin_us)
				origin_us = m->samples[s].pts_us;
	}

	if (audio && audio_start(audio) < 0)
		audio = NULL;
	pc.audio = audio;

	for (i = 0; i < m->samples_count && !g_stop; i++) {
		const uint8_t *au = NULL;
		unsigned int cursor = 0, vcl_count = 0;
		struct nalu n, vcl = { 0 };
		struct h264_slice_hdr hdr;
		struct h264_poc poc;
		struct vdec_h264_ctrls ctrl;
		bool have_hdr = false;
		int slot;
		uint64_t ts;

		if (mp4_read_sample(m, i, &au, NULL) != MP4_OK || !au)
			break;

		while (nalu_next_length_prefixed(au, m->samples[i].size,
						 m->nal_length_size,
						 &cursor, &n)) {
			unsigned int t = nalu_h264_type(&n);

			if (t == H264_NAL_SPS || t == H264_NAL_PPS) {
				h264_parser_parse_param_nal(&parser, &n);
			} else if (t == H264_NAL_SLICE || t == H264_NAL_IDR) {
				vcl_count++;
				if (have_hdr)
					continue;
				if (h264_parser_parse_slice(&parser, &n,
							    &hdr) < 0)
					break;
				h264_parser_compute_poc(&parser, &hdr, &poc);
				vcl = n;
				have_hdr = true;
			}
		}
		if (!have_hdr)
			continue;
		if (vcl_count > 1) {
			fprintf(stderr, "frame %u: %u slices, unsupported\n",
				i, vcl_count);
			rc = -1;
			goto out_display;
		}

		if (hdr.idr) {
			while ((out = h264_dpb_next_output(&dpb, true)) >= 0)
				display_frame(&disp, &vdec, &dpb, out,
					      slot_pts[out], origin_us, &pc,
					      held, &display_count);
		}

		if (h264_parser_fill_controls(&parser, &hdr, &poc, &ctrl) < 0)
			continue;

		slot = h264_dpb_begin_frame(&dpb, &hdr, &poc, &ts, &ctrl);
		if (slot < 0) {
			fprintf(stderr, "no free slot @%u\n", i);
			continue;
		}
		slot_pts[slot] = m->samples[i].pts_us;

		if (vdec_decode(&vdec, slot, ts, vcl.data, vcl.size,
				&ctrl) < 0) {
			fprintf(stderr, "decode failed frame %u (%s)\n", i,
				slice_type_str(hdr.slice_type));
			h264_dpb_abort_frame(&dpb);
			continue;
		}
		decode_count++;

		h264_dpb_end_frame(&dpb, &hdr);

		while ((out = h264_dpb_next_output(&dpb, false)) >= 0)
			display_frame(&disp, &vdec, &dpb, out, slot_pts[out],
				      origin_us, &pc, held, &display_count);
	}

	if (!g_stop) {
		while ((out = h264_dpb_next_output(&dpb, true)) >= 0)
			display_frame(&disp, &vdec, &dpb, out, slot_pts[out],
				      origin_us, &pc, held, &display_count);
	}

	/* If the audio track outlasts the video, let it finish. */
	if (!g_stop && audio) {
		while (!audio->done && !audio->failed && !g_stop)
			usleep(20000);
	}

	printf("played %u decoded, %u displayed frames%s\n", decode_count,
	       display_count, audio ? (audio->failed ? " (audio error)" : "") :
	       " (no audio)");
	rc = decode_count > 0 ? 0 : -1;

out_display:
	display_close(&disp, &vdec);
out_vdec:
	vdec_close(&vdec);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

static int play_file(const char *path, int plane_index, int mute,
		     unsigned int buf_override, int coded_fb, int fill)
{
	struct mp4_demux video;
	struct audio_player audio;
	bool have_audio = false;
	int rc;

	memset(&audio, 0, sizeof(audio));

	if (mp4_open(&video, path) < 0)
		return -1;
	if (video.codec != MP4_CODEC_H264) {
		fprintf(stderr, "%s: not an H.264 MP4\n", path);
		mp4_close(&video);
		return -1;
	}

	if (!mute) {
		if (audio_open(&audio, path) == 0) {
			have_audio = true;
		} else {
			audio_close(&audio);
			fprintf(stderr, "playing video without audio\n");
		}
	}

	rc = device_play(&video, plane_index, have_audio ? &audio : NULL,
			 buf_override, coded_fb, fill);

	if (have_audio)
		audio_close(&audio);
	mp4_close(&video);
	return rc;
}

static void usage(const char *name)
{
	fprintf(stderr,
		"usage: %s [-p plane] [-m] [-l] [-b buffers] [-C|-V|-F] file.mp4\n"
		"  -p plane   DRM plane index (default 0 = primary)\n"
		"  -m         mute / video only\n"
		"  -l         loop playback until interrupted\n"
		"  -b buffers override capture buffer count (4..9; for 1080p/CMA)\n"
		"  -V         visible-size FB (default; exact display-size crop)\n"
		"  -C         coded-size FB, shrink padding into the black bars (fallback)\n"
		"  -F         coded-size FB, scale to fill and let the display clip padding (fallback)\n",
		name);
}

int main(int argc, char *argv[])
{
	const char *input = NULL;
	int plane_index = 0;
	int mute = 0, loop = 0, coded_fb = 0, fill = 0;
	unsigned int buf_override = 0;
	int opt, rc = 0;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	while ((opt = getopt(argc, argv, "p:mlhb:CVF")) != -1) {
		switch (opt) {
		case 'p':
			plane_index = atoi(optarg);
			break;
		case 'm':
			mute = 1;
			break;
		case 'l':
			loop = 1;
			break;
		case 'b':
			buf_override = (unsigned int)atoi(optarg);
			break;
		case 'C':
			coded_fb = 1;
			fill = 0;
			break;
		case 'V':
			coded_fb = 0;
			fill = 0;
			break;
		case 'F':
			coded_fb = 1;
			fill = 1;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}
	if (optind >= argc) {
		usage(argv[0]);
		return 1;
	}
	input = argv[optind];

	do {
		rc = play_file(input, plane_index, mute, buf_override,
			       coded_fb, fill);
	} while (loop && rc == 0 && !g_stop);

	return rc ? 1 : 0;
}

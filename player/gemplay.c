/*
 * GemPlay: an audio player, drawn as GEM would have. A deck at the top
 * (the album's cover in the ST's colours, the track's title, artist and
 * album, where it's got to, and GEM buttons for Previous, Play, Stop and
 * Next) over the playlist.
 *
 * FFmpeg reads and decodes (libavformat, libavcodec), and libswresample
 * turns everything into 48 kHz stereo float, which a thread writes into a
 * ring buffer that a PipeWire stream plays from. When a track ends the
 * thread goes straight on to the next, so albums play without gaps; a
 * marker for where each track starts in the stream tells the window which
 * is playing, as it's heard. Pausing pauses the stream.
 *
 * It's an MPRIS player (org.mpris.MediaPlayer2.gemplay), so media keys
 * (gemwm-media) and playerctl drive it. The playlist is kept in
 * ~/.local/state/gemwm/play.
 */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <math.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/ringbuffer.h>
#include <stdatomic.h>
#include <string.h>
#include "app-menu.h"
#include "dither.h"
#include "gem-alert.h"
#include "gem-draw.h"
#include "gem-file.h"
#include "gem-popup.h"
#include "gem-scrollbar.h"
#include "gem-ui.h"

#define RATE 48000
#define CHANNELS 2
#define STRIDE (CHANNELS * sizeof(float))
#define RING_SIZE (1u << 20)          /* bytes: a few seconds */
#define RING_MASK (RING_SIZE - 1)

#define COVER 96
#define DECK_H (GEM_PAD + COVER + GEM_PAD + GEM_ROW_H + GEM_PAD + BUTTON_H + GEM_PAD)
#define BUTTON_W 34
#define BUTTON_H 22
#define INFO_H (GEM_ROW_H + 1)
#define PREVIOUS_RESTARTS 3.0           /* seconds in: Previous restarts it */

#define APP "gemplay"
#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_NAME "org.mpris.MediaPlayer2.gemplay"

static const char *const audio_exts[] = {
	"mp3", "flac", "ogg", "oga", "opus", "m4a", "m4b", "aac", "wav", "wv",
	"ape", "aif", "aiff", "wma", "mka", "mpc", "alac", "dsf", NULL,
};

struct track {
	guint id;
	char *path;
	char *title, *artist, *album;   /* NULL until read */
	double length;                  /* seconds; 0 if not known */
	bool selected;
};

enum state { STOPPED, PLAYING, PAUSED };
enum repeat { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE };

/* Where each track starts in what's been played: base seconds into it at
 * frame start. id 0 is the end. */
struct marker {
	guint id;
	guint64 start;
	double base;
};

static struct {
	/* PipeWire's side. */
	struct pw_thread_loop *loop;
	struct pw_stream *stream;
	struct spa_ringbuffer ring;
	uint8_t *buf;
	_Atomic guint64 written, consumed;   /* frames */

	/* The decoding thread, and what it and the window share (under
	 * lock): the playlist's order, the markers, shuffle and repeat. */
	GThread *thread;
	_Atomic bool stop;
	GMutex lock;
	GQueue markers;                      /* struct marker */
	GHashTable *played;                  /* ids, for shuffle */
} eng;

static struct {
	GtkApplication *app;
	GtkWidget *window, *deck, *list, *info;
	GtkOverlay *host;
	GtkAdjustment *vadj;
	struct app_menu *menu;
	struct gem_popup *popup;
	GArray *deck_hits;

	GPtrArray *tracks;          /* struct track, in order (under eng.lock) */
	guint next_id;
	enum state state;
	bool shuffle;
	enum repeat repeat;
	guint current;              /* the track playing (or paused, or last) */
	double position;            /* seconds into it, as last worked out */
	bool scrubbing;
	double scrub_to;
	int anchor, cursor;
	cairo_surface_t *cover;     /* the current track's, dithered */
	guint cover_for;
	char *message;
	guint message_id;

	GDBusConnection *bus;
	guint mpris_owner;
} ui;

static void update(void);
static void mpris_changed(const char *iface, const char *prop, GVariant *v);
static void mpris_seeked(void);
static void save_playlist(void);
static void probe(GPtrArray *ids);

/* ---- The playlist ------------------------------------------------------------- */

static struct track *track_at(int i) {
	return ui.tracks->pdata[i];
}

static int n_tracks(void) {
	return (int)ui.tracks->len;
}

static int index_of(guint id) {
	for (int i = 0; i < n_tracks(); i++) {
		if (track_at(i)->id == id) {
			return i;
		}
	}
	return -1;
}

static struct track *track_of(guint id) {
	int i = index_of(id);
	return i >= 0 ? track_at(i) : NULL;
}

static void track_free(gpointer p) {
	struct track *t = p;
	g_free(t->path);
	g_free(t->title);
	g_free(t->artist);
	g_free(t->album);
	g_free(t);
}

static bool is_audio(const char *name) {
	const char *dot = strrchr(name, '.');
	if (dot == NULL) {
		return false;
	}
	for (int i = 0; audio_exts[i] != NULL; i++) {
		if (g_ascii_strcasecmp(dot + 1, audio_exts[i]) == 0) {
			return true;
		}
	}
	return false;
}

/* What a track's called: its title, else its file's name. */
static char *track_name(const struct track *t) {
	if (t->title != NULL && t->title[0] != '\0') {
		return g_strdup(t->title);
	}
	char *base = g_path_get_basename(t->path);
	char *dot = strrchr(base, '.');
	if (dot != NULL && dot != base) {
		*dot = '\0';
	}
	return base;
}

static char *minutes(double s) {
	int t = (int)(s < 0 ? 0 : s);
	return t >= 3600 ? g_strdup_printf("%d:%02d:%02d", t / 3600, t / 60 % 60,
		t % 60) : g_strdup_printf("%d:%02d", t / 60, t % 60);
}

/* After id, as the order goes (under eng.lock). user: Next was pressed,
 * so Repeat One moves on. 0 at the end. */
static guint next_after(guint id, bool user) {
	int n = n_tracks();
	if (n == 0) {
		return 0;
	}
	int i = index_of(id);
	if (ui.repeat == REPEAT_ONE && !user && i >= 0) {
		return id;
	}
	if (ui.shuffle && n > 1) {
		g_hash_table_add(eng.played, GUINT_TO_POINTER(id));
		GArray *left = g_array_new(FALSE, FALSE, sizeof(guint));
		for (int k = 0; k < n; k++) {
			guint tid = track_at(k)->id;
			if (!g_hash_table_contains(eng.played, GUINT_TO_POINTER(tid))) {
				g_array_append_val(left, tid);
			}
		}
		if (left->len == 0 && (ui.repeat == REPEAT_ALL || user)) {
			g_hash_table_remove_all(eng.played);
			g_hash_table_add(eng.played, GUINT_TO_POINTER(id));
			for (int k = 0; k < n; k++) {
				if (track_at(k)->id != id) {
					g_array_append_val(left, track_at(k)->id);
				}
			}
		}
		guint pick = left->len > 0 ?
			g_array_index(left, guint, g_random_int_range(0, left->len)) : 0;
		g_array_unref(left);
		return pick;
	}
	if (i + 1 < n) {
		return track_at(i + 1)->id;
	}
	return ui.repeat == REPEAT_ALL ? track_at(0)->id : 0;
}

/* ---- PipeWire: playing what's in the ring ------------------------------------- */

static void on_process(void *data) {
	struct pw_buffer *b = pw_stream_dequeue_buffer(eng.stream);
	if (b == NULL) {
		return;
	}
	struct spa_data *d = &b->buffer->datas[0];
	uint8_t *dst = d->data;
	if (dst == NULL) {
		pw_stream_queue_buffer(eng.stream, b);
		return;
	}
	uint32_t frames = d->maxsize / STRIDE;
	if (b->requested > 0 && b->requested < frames) {
		frames = b->requested;
	}
	uint32_t want = frames * STRIDE, idx;
	int32_t avail = spa_ringbuffer_get_read_index(&eng.ring, &idx);
	uint32_t take = avail > 0 ? MIN((uint32_t)avail, want) : 0;
	take -= take % STRIDE;
	if (take > 0) {
		spa_ringbuffer_read_data(&eng.ring, eng.buf, RING_SIZE, idx & RING_MASK,
			dst, take);
		spa_ringbuffer_read_update(&eng.ring, idx + take);
	}
	if (take < want) {
		memset(dst + take, 0, want - take); /* nothing yet, or the end */
	}
	atomic_fetch_add(&eng.consumed, take / STRIDE);
	d->chunk->offset = 0;
	d->chunk->stride = STRIDE;
	d->chunk->size = want;
	pw_stream_queue_buffer(eng.stream, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
};

static void audio_init(void) {
	pw_init(NULL, NULL);
	eng.buf = g_malloc0(RING_SIZE);
	spa_ringbuffer_init(&eng.ring);
	eng.loop = pw_thread_loop_new("gemplay", NULL);
	pw_thread_loop_lock(eng.loop);
	pw_thread_loop_start(eng.loop);
	struct pw_properties *props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Playback",
		PW_KEY_MEDIA_ROLE, "Music",
		PW_KEY_APP_NAME, "GemPlay",
		PW_KEY_APP_ID, "org.gemwm.Play",
		PW_KEY_NODE_NAME, "GemPlay",
		PW_KEY_NODE_LATENCY, "2048/48000",
		NULL);
	eng.stream = pw_stream_new_simple(pw_thread_loop_get_loop(eng.loop),
		"GemPlay", props, &stream_events, NULL);
	uint8_t pod[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod, sizeof(pod));
	const struct spa_pod *params[1];
	params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
		&SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32,
			.channels = CHANNELS, .rate = RATE,
			.position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR }));
	pw_stream_connect(eng.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
		PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
		PW_STREAM_FLAG_INACTIVE, params, 1);
	pw_thread_loop_unlock(eng.loop);
}

static void stream_active(bool on) {
	pw_thread_loop_lock(eng.loop);
	pw_stream_set_active(eng.stream, on);
	pw_thread_loop_unlock(eng.loop);
}

/* ---- Decoding, in a thread ------------------------------------------------------ */

struct source {
	AVFormatContext *fmt;
	AVCodecContext *dec;
	SwrContext *swr;
	AVPacket *pkt;
	AVFrame *frame;
	int stream;
	double skip_to;        /* after a seek: what comes before is dropped */
	float *out;
	int out_cap;           /* frames */
};

static void source_close(struct source *s) {
	swr_free(&s->swr);
	avcodec_free_context(&s->dec);
	avformat_close_input(&s->fmt);
	av_packet_free(&s->pkt);
	av_frame_free(&s->frame);
	g_free(s->out);
	memset(s, 0, sizeof(*s));
}

static bool source_open(struct source *s, const char *path, double start,
		char **error) {
	memset(s, 0, sizeof(*s));
	int r = avformat_open_input(&s->fmt, path, NULL, NULL);
	const AVCodec *codec = NULL;
	if (r >= 0) {
		r = avformat_find_stream_info(s->fmt, NULL);
	}
	if (r >= 0) {
		r = s->stream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1,
			&codec, 0);
	}
	if (r >= 0) {
		s->dec = avcodec_alloc_context3(codec);
		r = avcodec_parameters_to_context(s->dec,
			s->fmt->streams[s->stream]->codecpar);
	}
	if (r >= 0) {
		r = avcodec_open2(s->dec, codec, NULL);
	}
	if (r >= 0) {
		AVChannelLayout in = { 0 };
		if (s->dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
			av_channel_layout_default(&in, s->dec->ch_layout.nb_channels);
		} else {
			av_channel_layout_copy(&in, &s->dec->ch_layout);
		}
		AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
		r = swr_alloc_set_opts2(&s->swr, &out, AV_SAMPLE_FMT_FLT, RATE, &in,
			s->dec->sample_fmt, s->dec->sample_rate, 0, NULL);
		av_channel_layout_uninit(&in);
	}
	if (r >= 0) {
		r = swr_init(s->swr);
	}
	if (r < 0) {
		char why[AV_ERROR_MAX_STRING_SIZE] = "";
		av_strerror(r, why, sizeof(why));
		*error = g_strdup(why);
		source_close(s);
		return false;
	}
	s->pkt = av_packet_alloc();
	s->frame = av_frame_alloc();
	if (start > 0) {
		av_seek_frame(s->fmt, -1, (int64_t)(start * AV_TIME_BASE),
			AVSEEK_FLAG_BACKWARD);
		avcodec_flush_buffers(s->dec);
		s->skip_to = start;
	}
	return true;
}

/* frames of stereo float into the ring, waiting for room. False if told
 * to stop meanwhile. */
static bool ring_write(const float *data, int frames) {
	const uint8_t *p = (const uint8_t *)data;
	uint32_t bytes = frames * STRIDE;
	while (bytes > 0) {
		if (atomic_load(&eng.stop)) {
			return false;
		}
		uint32_t idx;
		int32_t filled = spa_ringbuffer_get_write_index(&eng.ring, &idx);
		uint32_t room = RING_SIZE - (uint32_t)filled;
		room -= room % STRIDE;
		if (room == 0) {
			g_usleep(10000);
			continue;
		}
		uint32_t n = MIN(room, bytes);
		spa_ringbuffer_write_data(&eng.ring, eng.buf, RING_SIZE, idx & RING_MASK,
			p, n);
		spa_ringbuffer_write_update(&eng.ring, idx + n);
		atomic_fetch_add(&eng.written, n / STRIDE);
		p += n;
		bytes -= n;
	}
	return true;
}

/* A decoded frame (or, frame NULL, what the resampler still holds),
 * resampled and written. */
static bool emit(struct source *s, AVFrame *frame) {
	int in = frame != NULL ? frame->nb_samples : 0;
	int cap = swr_get_out_samples(s->swr, in);
	if (cap <= 0) {
		return true;
	}
	if (cap > s->out_cap) {
		s->out = g_realloc(s->out, cap * STRIDE);
		s->out_cap = cap;
	}
	uint8_t *out[1] = { (uint8_t *)s->out };
	int got = swr_convert(s->swr, out, cap,
		frame != NULL ? (const uint8_t **)frame->extended_data : NULL, in);
	if (got <= 0) {
		return true;
	}
	int drop = 0;
	if (s->skip_to > 0 && frame != NULL && frame->pts != AV_NOPTS_VALUE) {
		double t = frame->pts * av_q2d(s->fmt->streams[s->stream]->time_base);
		drop = (int)MIN((double)got, MAX(0.0, (s->skip_to - t) * RATE));
		if (drop < got) {
			s->skip_to = 0;
		}
	}
	return ring_write(s->out + drop * CHANNELS, got - drop);
}

/* The whole of a track, or until told to stop: false then. */
static bool decode(struct source *s) {
	bool eof = false;
	while (!eof) {
		if (atomic_load(&eng.stop)) {
			return false;
		}
		int r = av_read_frame(s->fmt, s->pkt);
		if (r < 0) {
			avcodec_send_packet(s->dec, NULL); /* drain */
			eof = true;
		} else if (s->pkt->stream_index != s->stream) {
			av_packet_unref(s->pkt);
			continue;
		} else {
			avcodec_send_packet(s->dec, s->pkt);
			av_packet_unref(s->pkt);
		}
		while (avcodec_receive_frame(s->dec, s->frame) == 0) {
			bool ok = emit(s, s->frame);
			av_frame_unref(s->frame);
			if (!ok) {
				return false;
			}
		}
	}
	return emit(s, NULL);
}

struct failed {
	guint id;
	char *why;
};

static gboolean show_failed(gpointer data);

struct start {
	guint id;
	double at;
};

static void push_marker(guint id, guint64 start, double base) {
	struct marker *m = g_new(struct marker, 1);
	m->id = id;
	m->start = start;
	m->base = base;
	g_mutex_lock(&eng.lock);
	g_queue_push_tail(&eng.markers, m);
	g_mutex_unlock(&eng.lock);
}

static gpointer decoder(gpointer data) {
	struct start *st = data;
	guint id = st->id;
	double at = st->at;
	g_free(st);
	int failures = 0;
	while (id != 0 && !atomic_load(&eng.stop)) {
		g_mutex_lock(&eng.lock);
		struct track *t = track_of(id);
		char *path = t != NULL ? g_strdup(t->path) : NULL;
		g_mutex_unlock(&eng.lock);
		if (path == NULL) {
			break;
		}
		struct source s;
		char *why = NULL;
		bool ok = source_open(&s, path, at, &why);
		g_free(path);
		at = 0;
		if (ok) {
			failures = 0;
			ok = decode(&s);
			source_close(&s);
			if (!ok) {
				return NULL; /* stopped */
			}
		} else {
			struct failed *f = g_new(struct failed, 1);
			f->id = id;
			f->why = why;
			g_idle_add(show_failed, f);
		}
		g_mutex_lock(&eng.lock);
		guint next = ++failures > n_tracks() ? 0 : next_after(id, false);
		g_mutex_unlock(&eng.lock);
		push_marker(next, atomic_load(&eng.written), 0);
		id = next;
	}
	return NULL;
}

static void decoder_stop(void) {
	if (eng.thread != NULL) {
		atomic_store(&eng.stop, true);
		g_thread_join(eng.thread);
		eng.thread = NULL;
	}
	atomic_store(&eng.stop, false);
}

/* Everything queued thrown away: the ring, and the markers. */
static void flush(void) {
	pw_thread_loop_lock(eng.loop);
	spa_ringbuffer_init(&eng.ring);
	atomic_store(&eng.written, 0);
	atomic_store(&eng.consumed, 0);
	pw_thread_loop_unlock(eng.loop);
	g_mutex_lock(&eng.lock);
	g_queue_clear_full(&eng.markers, g_free);
	g_mutex_unlock(&eng.lock);
}

/* ---- Playing, pausing, stopping ------------------------------------------------- */

static void play_at(guint id, double at) {
	if (track_of(id) == NULL) {
		return;
	}
	decoder_stop();
	flush();
	push_marker(id, 0, at);
	g_mutex_lock(&eng.lock);
	g_hash_table_add(eng.played, GUINT_TO_POINTER(id));
	g_mutex_unlock(&eng.lock);
	bool changed = ui.current != id;
	ui.current = id;
	ui.position = at;
	struct start *st = g_new(struct start, 1);
	st->id = id;
	st->at = at;
	eng.thread = g_thread_new("gemplay-decoder", decoder, st);
	if (ui.state != PAUSED) {
		ui.state = PLAYING;
		stream_active(true);
	}
	if (changed) {
		save_playlist();
	}
	update();
	mpris_seeked();
}

static void stop(void) {
	decoder_stop();
	stream_active(false);
	flush();
	ui.state = STOPPED;
	ui.position = 0;
	update();
}

static void pause_toggle(void) {
	if (ui.state == PLAYING) {
		stream_active(false);
		ui.state = PAUSED;
	} else if (ui.state == PAUSED) {
		stream_active(true);
		ui.state = PLAYING;
	} else if (n_tracks() > 0) {
		/* Stopped: what's picked, else where it was, else the first. */
		guint id = ui.cursor >= 0 && ui.cursor < n_tracks() &&
			track_at(ui.cursor)->selected ? track_at(ui.cursor)->id :
			track_of(ui.current) != NULL ? ui.current : track_at(0)->id;
		ui.state = PLAYING;
		play_at(id, 0);
		return;
	}
	update();
}

static void play_pause(bool play) {
	if (play != (ui.state == PLAYING)) {
		pause_toggle();
	}
}

static void next(void) {
	if (n_tracks() == 0) {
		return;
	}
	g_mutex_lock(&eng.lock);
	guint id = next_after(ui.current, true);
	g_mutex_unlock(&eng.lock);
	if (id != 0) {
		play_at(id, 0);
	}
}

static void previous(void) {
	int i = index_of(ui.current);
	if (i < 0) {
		return;
	}
	if (ui.position > PREVIOUS_RESTARTS || (i == 0 && ui.repeat != REPEAT_ALL)) {
		play_at(ui.current, 0);
	} else {
		play_at(track_at(i > 0 ? i - 1 : n_tracks() - 1)->id, 0);
	}
}

static void seek_to(double at) {
	if (ui.state == STOPPED || track_of(ui.current) == NULL) {
		return;
	}
	struct track *t = track_of(ui.current);
	at = CLAMP(at, 0, t->length > 0 ? t->length - 0.5 : at);
	play_at(ui.current, at);
}

/* Ten times a second: where it's got to, as heard. A marker that's been
 * reached is a track changing, or the end. */
static gboolean tick(gpointer data) {
	if (ui.state == STOPPED) {
		return G_SOURCE_CONTINUE;
	}
	guint64 heard = atomic_load(&eng.consumed);
	guint id = 0;
	bool changed = false, ended = false;
	double pos = 0;
	g_mutex_lock(&eng.lock);
	struct marker *m;
	while (g_queue_get_length(&eng.markers) > 1 &&
			((struct marker *)g_queue_peek_nth(&eng.markers, 1))->start <= heard) {
		g_free(g_queue_pop_head(&eng.markers));
		changed = true;
	}
	m = g_queue_peek_head(&eng.markers);
	if (m != NULL) {
		id = m->id;
		pos = m->base + (double)(heard - MIN(heard, m->start)) / RATE;
		ended = id == 0;
	}
	g_mutex_unlock(&eng.lock);
	if (ended) {
		stop();
		return G_SOURCE_CONTINUE;
	}
	if (m == NULL) {
		return G_SOURCE_CONTINUE;
	}
	int old = (int)ui.position;
	ui.position = pos;
	if (changed && id != ui.current) {
		ui.current = id;
		save_playlist();
		update();
	} else if ((int)pos != old) {
		gtk_widget_queue_draw(ui.deck);
	}
	return G_SOURCE_CONTINUE;
}

/* ---- Tags and covers, read in threads ------------------------------------------- */

static char *tag(AVFormatContext *fmt, int stream, const char *key) {
	AVDictionaryEntry *e = av_dict_get(fmt->metadata, key, NULL, 0);
	if (e == NULL && stream >= 0) {
		e = av_dict_get(fmt->streams[stream]->metadata, key, NULL, 0);
	}
	return e != NULL && e->value[0] != '\0' ? g_strdup(e->value) : NULL;
}

struct probed {
	guint id;
	char *title, *artist, *album;
	double length;
};

static gboolean apply_probed(gpointer data) {
	struct probed *p = data;
	struct track *t = track_of(p->id);
	if (t != NULL) {
		g_free(t->title);
		g_free(t->artist);
		g_free(t->album);
		t->title = p->title;
		t->artist = p->artist;
		t->album = p->album;
		t->length = p->length;
		if (p->id == ui.current) {
			update();
		} else {
			gtk_widget_queue_draw(ui.list);
			gtk_widget_queue_draw(ui.info);
		}
	} else {
		g_free(p->title);
		g_free(p->artist);
		g_free(p->album);
	}
	g_free(p);
	return G_SOURCE_REMOVE;
}

struct probe_job {
	GArray *ids;
	GPtrArray *paths;
};

static void probe_thread(GTask *task, gpointer src, gpointer data,
		GCancellable *c) {
	struct probe_job *job = data;
	for (guint i = 0; i < job->ids->len; i++) {
		AVFormatContext *fmt = NULL;
		const char *path = job->paths->pdata[i];
		struct probed *p = g_new0(struct probed, 1);
		p->id = g_array_index(job->ids, guint, i);
		if (avformat_open_input(&fmt, path, NULL, NULL) >= 0) {
			avformat_find_stream_info(fmt, NULL);
			int s = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
			p->title = tag(fmt, s, "title");
			p->artist = tag(fmt, s, "artist");
			if (p->artist == NULL) {
				p->artist = tag(fmt, s, "album_artist");
			}
			p->album = tag(fmt, s, "album");
			if (fmt->duration != AV_NOPTS_VALUE) {
				p->length = (double)fmt->duration / AV_TIME_BASE;
			}
			avformat_close_input(&fmt);
		}
		g_idle_add(apply_probed, p);
	}
	g_task_return_boolean(task, TRUE);
}

static void probe_job_free(gpointer data) {
	struct probe_job *job = data;
	g_array_unref(job->ids);
	g_ptr_array_unref(job->paths);
	g_free(job);
}

static void probe(GPtrArray *tracks) {
	if (tracks->len == 0) {
		return;
	}
	struct probe_job *job = g_new0(struct probe_job, 1);
	job->ids = g_array_new(FALSE, FALSE, sizeof(guint));
	job->paths = g_ptr_array_new_with_free_func(g_free);
	for (guint i = 0; i < tracks->len; i++) {
		struct track *t = tracks->pdata[i];
		g_array_append_val(job->ids, t->id);
		g_ptr_array_add(job->paths, g_strdup(t->path));
	}
	GTask *task = g_task_new(NULL, NULL, NULL, NULL);
	g_task_set_task_data(task, job, probe_job_free);
	g_task_run_in_thread(task, probe_thread);
	g_object_unref(task);
}

/* A picture's bytes as a COVER-square surface (the middle of it, if it
 * isn't square), in the ST's colours. */
static cairo_surface_t *cover_from(GBytes *bytes) {
	GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
	bool ok = gdk_pixbuf_loader_write_bytes(loader, bytes, NULL) &&
		gdk_pixbuf_loader_close(loader, NULL);
	GdkPixbuf *pb = ok ? gdk_pixbuf_loader_get_pixbuf(loader) : NULL;
	cairo_surface_t *out = NULL;
	if (pb != NULL) {
		GdkPixbuf *rgba = gdk_pixbuf_add_alpha(pb, FALSE, 0, 0, 0);
		int w = gdk_pixbuf_get_width(rgba), h = gdk_pixbuf_get_height(rgba);
		int rs = gdk_pixbuf_get_rowstride(rgba);
		const guchar *px = gdk_pixbuf_read_pixels(rgba);
		cairo_surface_t *full = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
		guint32 *dst = (guint32 *)cairo_image_surface_get_data(full);
		int ds = cairo_image_surface_get_stride(full) / 4;
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				const guchar *p = px + y * rs + x * 4;
				dst[y * ds + x] = 0xff000000u | p[0] << 16 | p[1] << 8 | p[2];
			}
		}
		cairo_surface_mark_dirty(full);
		out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, COVER, COVER);
		cairo_t *cr = cairo_create(out);
		double s = (double)COVER / MIN(w, h);
		cairo_scale(cr, s, s);
		cairo_set_source_surface(cr, full, -(w - MIN(w, h)) / 2.0,
			-(h - MIN(w, h)) / 2.0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_destroy(cr);
		cairo_surface_destroy(full);
		g_object_unref(rgba);
		dither_surface(out, 1, DITHER_ST16, DITHER_DIFFUSE);
	}
	g_object_unref(loader);
	return out;
}

/* The picture in the file, else one beside it (cover.jpg, folder.jpg...). */
static void cover_thread(GTask *task, gpointer src, gpointer data,
		GCancellable *c) {
	const char *path = data;
	GBytes *bytes = NULL;
	AVFormatContext *fmt = NULL;
	if (avformat_open_input(&fmt, path, NULL, NULL) >= 0) {
		for (unsigned i = 0; i < fmt->nb_streams && bytes == NULL; i++) {
			AVStream *st = fmt->streams[i];
			if (st->disposition & AV_DISPOSITION_ATTACHED_PIC &&
					st->attached_pic.size > 0) {
				bytes = g_bytes_new(st->attached_pic.data, st->attached_pic.size);
			}
		}
		avformat_close_input(&fmt);
	}
	if (bytes == NULL) {
		static const char *const names[] = { "cover", "Cover", "folder",
			"Folder", "front", "Front", "album", "Album", NULL };
		static const char *const exts[] = { "jpg", "jpeg", "png", NULL };
		char *dir = g_path_get_dirname(path);
		for (int n = 0; names[n] != NULL && bytes == NULL; n++) {
			for (int e = 0; exts[e] != NULL && bytes == NULL; e++) {
				char *file = g_strdup_printf("%s/%s.%s", dir, names[n], exts[e]);
				char *contents;
				gsize len;
				if (g_file_get_contents(file, &contents, &len, NULL)) {
					bytes = g_bytes_new_take(contents, len);
				}
				g_free(file);
			}
		}
		g_free(dir);
	}
	cairo_surface_t *cover = bytes != NULL ? cover_from(bytes) : NULL;
	if (bytes != NULL) {
		g_bytes_unref(bytes);
	}
	g_task_return_pointer(task, cover, (GDestroyNotify)cairo_surface_destroy);
}

static void cover_done(GObject *src, GAsyncResult *res, gpointer data) {
	guint id = GPOINTER_TO_UINT(data);
	cairo_surface_t *cover = g_task_propagate_pointer(G_TASK(res), NULL);
	if (id == ui.current) {
		g_clear_pointer(&ui.cover, cairo_surface_destroy);
		ui.cover = cover;
		gtk_widget_queue_draw(ui.deck);
	} else if (cover != NULL) {
		cairo_surface_destroy(cover);
	}
}

static void load_cover(void) {
	struct track *t = track_of(ui.current);
	if (t == NULL || ui.cover_for == ui.current) {
		return;
	}
	ui.cover_for = ui.current;
	g_clear_pointer(&ui.cover, cairo_surface_destroy);
	GTask *task = g_task_new(NULL, NULL, cover_done, GUINT_TO_POINTER(t->id));
	g_task_set_task_data(task, g_strdup(t->path), g_free);
	g_task_run_in_thread(task, cover_thread);
	g_object_unref(task);
}

/* ---- Adding to the playlist ------------------------------------------------------ */

static struct track *track_new(const char *path) {
	struct track *t = g_new0(struct track, 1);
	t->id = ui.next_id++;
	t->path = g_strdup(path);
	return t;
}

static gint by_name(gconstpointer a, gconstpointer b, gpointer data) {
	return g_utf8_collate(a, b);
}

/* Audio files in path, a file or a folder (and those in it), sorted. */
static void collect(const char *path, GPtrArray *out, int depth) {
	if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
		if (depth > 16) {
			return;
		}
		GDir *d = g_dir_open(path, 0, NULL);
		if (d == NULL) {
			return;
		}
		GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
		const char *name;
		while ((name = g_dir_read_name(d)) != NULL) {
			if (name[0] != '.') {
				g_ptr_array_add(names, g_strdup(name));
			}
		}
		g_dir_close(d);
		g_ptr_array_sort_values_with_data(names, by_name, NULL);
		for (guint i = 0; i < names->len; i++) {
			char *full = g_build_filename(path, names->pdata[i], NULL);
			collect(full, out, depth + 1);
			g_free(full);
		}
		g_ptr_array_unref(names);
	} else if (is_audio(path) && g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
		g_ptr_array_add(out, g_strdup(path));
	}
}

struct adding {
	char **paths;
	bool play;
};

static void collect_thread(GTask *task, gpointer src, gpointer data,
		GCancellable *c) {
	struct adding *a = data;
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	for (int i = 0; a->paths[i] != NULL; i++) {
		collect(a->paths[i], out, 0);
	}
	g_task_return_pointer(task, out, (GDestroyNotify)g_ptr_array_unref);
}

static void adding_free(gpointer data) {
	struct adding *a = data;
	g_strfreev(a->paths);
	g_free(a);
}

static void say(char *message);

static void collected(GObject *src, GAsyncResult *res, gpointer data) {
	bool play = GPOINTER_TO_INT(data);
	GPtrArray *paths = g_task_propagate_pointer(G_TASK(res), NULL);
	if (paths == NULL || paths->len == 0) {
		say(g_strdup("Nothing there GemPlay can play."));
		if (paths != NULL) {
			g_ptr_array_unref(paths);
		}
		return;
	}
	GPtrArray *added = g_ptr_array_new();
	g_mutex_lock(&eng.lock);
	for (guint i = 0; i < paths->len; i++) {
		struct track *t = track_new(paths->pdata[i]);
		g_ptr_array_add(ui.tracks, t);
		g_ptr_array_add(added, t);
	}
	g_mutex_unlock(&eng.lock);
	probe(added);
	guint first = ((struct track *)added->pdata[0])->id;
	g_ptr_array_unref(added);
	g_ptr_array_unref(paths);
	save_playlist();
	if (play) {
		ui.state = PLAYING;
		play_at(first, 0);
	} else {
		update();
	}
}

/* Files and folders onto the end of the playlist; play: the first of them
 * starts. */
static void add_paths(char **paths, bool play) {
	struct adding *a = g_new0(struct adding, 1);
	a->paths = g_strdupv(paths);
	GTask *task = g_task_new(NULL, NULL, collected, GINT_TO_POINTER(play));
	g_task_set_task_data(task, a, adding_free);
	g_task_run_in_thread(task, collect_thread);
	g_object_unref(task);
}

static void remove_selected(void) {
	bool current_gone = false;
	g_mutex_lock(&eng.lock);
	for (int i = n_tracks() - 1; i >= 0; i--) {
		if (track_at(i)->selected) {
			current_gone |= track_at(i)->id == ui.current;
			g_ptr_array_remove_index(ui.tracks, i);
		}
	}
	g_mutex_unlock(&eng.lock);
	if (current_gone && ui.state != STOPPED) {
		stop();
	}
	ui.cursor = ui.anchor = -1;
	save_playlist();
	update();
}

static void clear_playlist(void) {
	stop();
	g_mutex_lock(&eng.lock);
	g_ptr_array_set_size(ui.tracks, 0);
	g_hash_table_remove_all(eng.played);
	g_mutex_unlock(&eng.lock);
	ui.current = 0;
	ui.cursor = ui.anchor = -1;
	g_clear_pointer(&ui.cover, cairo_surface_destroy);
	ui.cover_for = 0;
	save_playlist();
	update();
}

/* ---- The playlist on disk ----------------------------------------------------------- */

static char *state_path(void) {
	return g_build_filename(g_get_user_state_dir(), "gemwm", "play", NULL);
}

/* "current N", "shuffle", "repeat all|one", then a path a line. */
static void save_playlist(void) {
	GString *s = g_string_new(NULL);
	g_string_append_printf(s, "current %d\n", index_of(ui.current));
	if (ui.shuffle) {
		g_string_append(s, "shuffle\n");
	}
	if (ui.repeat != REPEAT_OFF) {
		g_string_append_printf(s, "repeat %s\n",
			ui.repeat == REPEAT_ALL ? "all" : "one");
	}
	for (int i = 0; i < n_tracks(); i++) {
		g_string_append_printf(s, "%s\n", track_at(i)->path);
	}
	char *path = state_path();
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	g_file_set_contents_full(path, s->str, -1, G_FILE_SET_CONTENTS_CONSISTENT,
		0600, NULL);
	g_free(dir);
	g_free(path);
	g_string_free(s, TRUE);
}

static void load_playlist(void) {
	char *path = state_path(), *text = NULL;
	if (!g_file_get_contents(path, &text, NULL, NULL)) {
		g_free(path);
		return;
	}
	char **lines = g_strsplit(text, "\n", -1);
	int current = -1;
	for (int i = 0; lines[i] != NULL; i++) {
		const char *l = lines[i];
		if (g_str_has_prefix(l, "current ")) {
			current = atoi(l + 8);
		} else if (strcmp(l, "shuffle") == 0) {
			ui.shuffle = true;
		} else if (g_str_has_prefix(l, "repeat ")) {
			ui.repeat = strcmp(l + 7, "one") == 0 ? REPEAT_ONE : REPEAT_ALL;
		} else if (l[0] == '/') {
			g_ptr_array_add(ui.tracks, track_new(l));
		}
	}
	if (current >= 0 && current < n_tracks()) {
		ui.current = track_at(current)->id;
	}
	g_strfreev(lines);
	g_free(text);
	g_free(path);
	probe(ui.tracks);
}

/* ---- Drawing: the deck -------------------------------------------------------------- */

enum { HIT_PREVIOUS = 1, HIT_PLAY, HIT_STOP, HIT_NEXT, HIT_BAR };

static int bar_x(void) {
	return GEM_PAD + (int)gem_measure("00:00") + GEM_PAD;
}

static int bar_w(int w) {
	return w - 2 * bar_x();
}

static int bar_y(void) {
	return GEM_PAD + COVER + GEM_PAD;
}

static void triangle(cairo_t *cr, int x, int y, int h, bool right) {
	for (int r = 0; r < h; r++) {
		int len = r < h / 2 ? r + 1 : h - r;
		gem_fill(cr, right ? x : x + h / 2 - len + 1, y + r, len, 1);
	}
}

/* A GEM button with a picture on it instead of words. */
static void deck_button(cairo_t *cr, int x, int y, int id, bool on) {
	gem_white(cr);
	gem_fill(cr, x, y, BUTTON_W, BUTTON_H);
	gem_black(cr);
	gem_frame(cr, x, y, BUTTON_W, BUTTON_H, on ? 2 : 1);
	int cx = x + BUTTON_W / 2, cy = y + BUTTON_H / 2;
	switch (id) {
	case HIT_PREVIOUS:
		gem_fill(cr, cx - 6, cy - 5, 2, 11);
		triangle(cr, cx - 4, cy - 5, 11, false);
		break;
	case HIT_PLAY:
		if (ui.state == PLAYING) {
			gem_fill(cr, cx - 5, cy - 5, 3, 11);
			gem_fill(cr, cx + 2, cy - 5, 3, 11);
		} else {
			triangle(cr, cx - 3, cy - 5, 11, true);
		}
		break;
	case HIT_STOP:
		gem_fill(cr, cx - 5, cy - 5, 10, 10);
		break;
	case HIT_NEXT:
		triangle(cr, cx - 3, cy - 5, 11, true);
		gem_fill(cr, cx + 4, cy - 5, 2, 11);
		break;
	}
	gem_hit_add(ui.deck_hits, x, y, BUTTON_W, BUTTON_H, id, 0);
}

/* A note, for a track with no cover. */
static void no_cover(cairo_t *cr, int x, int y) {
	gem_dither(cr, x + 1, y + 1, COVER - 2, COVER - 2);
	gem_white(cr);
	gem_fill(cr, x + 30, y + 22, 40, 52);
	gem_black(cr);
	gem_fill(cr, x + 52, y + 28, 3, 36);           /* the stem */
	for (int r = 0; r < 9; r++) {                    /* the flag */
		gem_fill(cr, x + 55, y + 28 + r, 9 - r, 1);
	}
	for (int r = 0; r < 9; r++) {                    /* the head */
		int half = r < 4 ? r + 2 : 10 - r;
		gem_fill(cr, x + 47 - half, y + 60 + r, 2 * half + 2, 1);
	}
}

static void paint_deck(cairo_t *cr, int w, int h, void *data) {
	g_array_set_size(ui.deck_hits, 0);
	struct track *t = track_of(ui.current);
	int x = GEM_PAD, y = GEM_PAD;
	gem_black(cr);
	gem_frame(cr, x, y, COVER, COVER, 1);
	if (ui.cover != NULL) {
		cairo_set_source_surface(cr, ui.cover, x, y);
		cairo_paint(cr);
		gem_black(cr);
	} else {
		no_cover(cr, x, y);
	}
	int tx = x + COVER + 2 * GEM_PAD, tw = w - tx - GEM_PAD;
	if (t != NULL) {
		char *name = track_name(t);
		gem_text_clipped(cr, name, tx, y + 8, tw, GEM_ROW_H);
		gem_text_clipped(cr, name, tx + 1, y + 8, tw - 1, GEM_ROW_H); /* bold */
		g_free(name);
		if (t->artist != NULL) {
			gem_text_clipped(cr, t->artist, tx, y + 8 + GEM_ROW_H + 4, tw,
				GEM_ROW_H);
		}
		if (t->album != NULL) {
			gem_text_clipped(cr, t->album, tx, y + 8 + 2 * (GEM_ROW_H + 4), tw,
				GEM_ROW_H);
		}
	} else {
		gem_text(cr, "Nothing playing.", tx, y + 8, GEM_ROW_H);
		gem_text(cr, "File > Open, or drop music here.", tx,
			y + 8 + GEM_ROW_H + 4, GEM_ROW_H);
	}

	/* Where it's got to: a bar filled as far as that, GEM's way. */
	double pos = ui.scrubbing ? ui.scrub_to : ui.position;
	double len = t != NULL ? t->length : 0;
	int by = bar_y(), bx = bar_x(), bw = bar_w(w);
	char *now = minutes(ui.state == STOPPED && !ui.scrubbing ? 0 : pos);
	char *total = minutes(len);
	gem_text(cr, now, bx - GEM_PAD - (int)gem_text_width(cr, now), by, GEM_ROW_H);
	gem_text(cr, total, bx + bw + GEM_PAD, by, GEM_ROW_H);
	g_free(now);
	g_free(total);
	int bh = 11, bt = by + (GEM_ROW_H - bh) / 2;
	gem_dither(cr, bx + 1, bt + 1, bw - 2, bh - 2);
	gem_frame(cr, bx, bt, bw, bh, 1);
	if (len > 0 && (ui.state != STOPPED || ui.scrubbing)) {
		int filled = (int)((bw - 2) * CLAMP(pos / len, 0, 1));
		gem_fill(cr, bx + 1, bt + 1, filled, bh - 2);
	}
	gem_hit_add(ui.deck_hits, bx, by, bw, GEM_ROW_H, HIT_BAR, 0);

	int row = by + GEM_ROW_H + GEM_PAD;
	int total_w = 4 * BUTTON_W + 3 * GEM_PAD;
	int b0 = (w - total_w) / 2;
	deck_button(cr, b0, row, HIT_PREVIOUS, false);
	deck_button(cr, b0 + (BUTTON_W + GEM_PAD), row, HIT_PLAY, true);
	deck_button(cr, b0 + 2 * (BUTTON_W + GEM_PAD), row, HIT_STOP, false);
	deck_button(cr, b0 + 3 * (BUTTON_W + GEM_PAD), row, HIT_NEXT, false);
	gem_black(cr);
	gem_fill(cr, 0, h - 1, w, 1);
}

/* ---- Drawing: the playlist --------------------------------------------------------- */

static double scroll_y(void) {
	return gtk_adjustment_get_value(ui.vadj);
}

static void fit_scroll(void) {
	int h = gtk_widget_get_height(ui.list);
	int total = MAX(n_tracks() * GEM_ROW_H + 4, h);
	gtk_adjustment_configure(ui.vadj, CLAMP(scroll_y(), 0, MAX(total - h, 0)), 0,
		total, GEM_ROW_H, MAX(h - GEM_ROW_H, GEM_ROW_H), h);
}

static void paint_list(cairo_t *cr, int w, int h, void *data) {
	int y0 = (int)scroll_y();
	int len_w = (int)gem_text_width(cr, "00:00:00");
	int name_x = GEM_PAD + 12;
	int artist_x = name_x + (w - name_x - len_w - 2 * GEM_PAD) * 3 / 5;
	if (n_tracks() == 0) {
		const char *s = "The playlist is empty.";
		gem_text(cr, s, (w - (int)gem_text_width(cr, s)) / 2, h / 3, GEM_ROW_H);
		gem_grey_out(cr, 0, h / 3, w, GEM_ROW_H);
		return;
	}
	for (int i = 0; i < n_tracks(); i++) {
		int y = 2 + i * GEM_ROW_H - y0;
		if (y + GEM_ROW_H < 0 || y > h) {
			continue;
		}
		struct track *t = track_at(i);
		gem_black(cr);
		if (t->selected) {
			gem_fill(cr, 0, y, w, GEM_ROW_H);
			gem_white(cr);
		}
		if (t->id == ui.current && ui.state != STOPPED) {
			if (ui.state == PAUSED) {
				gem_fill(cr, GEM_PAD, y + 5, 2, 9);
				gem_fill(cr, GEM_PAD + 4, y + 5, 2, 9);
			} else {
				triangle(cr, GEM_PAD, y + 5, 9, true);
			}
		}
		char *name = track_name(t);
		gem_text_clipped(cr, name, name_x, y, artist_x - name_x - GEM_PAD,
			GEM_ROW_H);
		g_free(name);
		if (t->artist != NULL) {
			gem_text_clipped(cr, t->artist, artist_x, y,
				w - artist_x - len_w - 2 * GEM_PAD, GEM_ROW_H);
		}
		if (t->length > 0) {
			char *l = minutes(t->length);
			gem_text(cr, l, w - GEM_PAD - (int)gem_text_width(cr, l), y, GEM_ROW_H);
			g_free(l);
		}
	}
	gem_black(cr);
}

static void paint_info(cairo_t *cr, int w, int h, void *data) {
	gem_black(cr);
	gem_fill(cr, 0, 0, w, 1);
	char *s;
	if (ui.message != NULL) {
		s = g_strdup(ui.message);
	} else {
		double total = 0;
		for (int i = 0; i < n_tracks(); i++) {
			total += track_at(i)->length;
		}
		char *t = minutes(total);
		s = g_strdup_printf("%d %s, %s%s%s", n_tracks(),
			n_tracks() == 1 ? "track" : "tracks", t,
			ui.shuffle ? ", shuffled" : "",
			ui.repeat == REPEAT_ALL ? ", repeating" :
			ui.repeat == REPEAT_ONE ? ", repeating one" : "");
		g_free(t);
	}
	gem_text_clipped(cr, s, GEM_PAD, 1, w - 2 * GEM_PAD, h - 1);
	g_free(s);
}

static gboolean message_gone(gpointer data) {
	ui.message_id = 0;
	g_clear_pointer(&ui.message, g_free);
	gtk_widget_queue_draw(ui.info);
	return G_SOURCE_REMOVE;
}

static void say(char *message) {
	g_free(ui.message);
	ui.message = message;
	if (ui.message_id != 0) {
		g_source_remove(ui.message_id);
	}
	ui.message_id = g_timeout_add(5000, message_gone, NULL);
	gtk_widget_queue_draw(ui.info);
}

static gboolean show_failed(gpointer data) {
	struct failed *f = data;
	struct track *t = track_of(f->id);
	if (t != NULL) {
		char *name = track_name(t);
		say(g_strdup_printf("Can't play %s: %s", name, f->why));
		g_free(name);
	}
	g_free(f->why);
	g_free(f);
	return G_SOURCE_REMOVE;
}

static GVariant *metadata(void);

/* Everything shown again, and MPRIS told. */
static void update(void) {
	if (ui.window == NULL) {
		return;
	}
	load_cover();
	fit_scroll();
	gtk_widget_queue_draw(ui.deck);
	gtk_widget_queue_draw(ui.list);
	gtk_widget_queue_draw(ui.info);
	app_menu_update(ui.menu);
	static const char *const states[] = { "Stopped", "Playing", "Paused" };
	mpris_changed("org.mpris.MediaPlayer2.Player", "PlaybackStatus",
		g_variant_new_string(states[ui.state]));
	mpris_changed("org.mpris.MediaPlayer2.Player", "Metadata", metadata());
}

/* ---- The pointer and keys ----------------------------------------------------------- */

static void deck_draw(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer d) {
	gem_draw_pixelated(cr, w, h, paint_deck, NULL);
}

static void list_draw(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer d) {
	gem_draw_pixelated(cr, w, h, paint_list, NULL);
}

static double bar_time(double x) {
	struct track *t = track_of(ui.current);
	int w = gtk_widget_get_width(ui.deck);
	double f = CLAMP((x - bar_x()) / (double)MAX(bar_w(w), 1), 0, 1);
	return t != NULL ? f * t->length : 0;
}

static void deck_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	if (gem_alert_up(ui.host)) {
		return;
	}
	const struct gem_hit *hit = gem_hit_at(ui.deck_hits, x, y);
	if (hit == NULL) {
		return;
	}
	switch (hit->id) {
	case HIT_PREVIOUS: previous(); break;
	case HIT_PLAY: pause_toggle(); break;
	case HIT_STOP: stop(); break;
	case HIT_NEXT: next(); break;
	}
}

/* Dragging along the bar (or clicking it) goes there. */
static void scrub_begin(GtkGestureDrag *g, double x, double y, gpointer data) {
	const struct gem_hit *hit = gem_hit_at(ui.deck_hits, x, y);
	struct track *t = track_of(ui.current);
	if (hit == NULL || hit->id != HIT_BAR || t == NULL || t->length <= 0 ||
			ui.state == STOPPED) {
		gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
		return;
	}
	ui.scrubbing = true;
	ui.scrub_to = bar_time(x);
	gtk_widget_queue_draw(ui.deck);
}

static void scrub_update(GtkGestureDrag *g, double dx, double dy, gpointer data) {
	double sx, sy;
	gtk_gesture_drag_get_start_point(g, &sx, &sy);
	ui.scrub_to = bar_time(sx + dx);
	gtk_widget_queue_draw(ui.deck);
}

static void scrub_end(GtkGestureDrag *g, double dx, double dy, gpointer data) {
	if (ui.scrubbing) {
		ui.scrubbing = false;
		seek_to(ui.scrub_to);
	}
}

static int row_at(double y) {
	int i = (int)((y + scroll_y() - 2) / GEM_ROW_H);
	return y + scroll_y() >= 2 && i >= 0 && i < n_tracks() ? i : -1;
}

static void pick_only(int i) {
	for (int k = 0; k < n_tracks(); k++) {
		track_at(k)->selected = k == i;
	}
	ui.cursor = ui.anchor = i;
}

static void scroll_to_row(int i) {
	int h = gtk_widget_get_height(ui.list);
	double y = 2 + i * GEM_ROW_H;
	if (y < scroll_y()) {
		gtk_adjustment_set_value(ui.vadj, y - 2);
	} else if (y + GEM_ROW_H > scroll_y() + h) {
		gtk_adjustment_set_value(ui.vadj, y + GEM_ROW_H - h + 2);
	}
}

enum {
	ACT_OPEN = 1, ACT_ADD_FOLDER, ACT_CLEAR, ACT_CLOSE, ACT_REMOVE,
	ACT_SELECT_ALL, ACT_PLAY_PAUSE, ACT_STOP, ACT_NEXT, ACT_PREVIOUS,
	ACT_SHUFFLE, ACT_REPEAT_OFF, ACT_REPEAT_ALL, ACT_REPEAT_ONE, ACT_PLAY_PICKED,
};

static void run(int id);

static void popup_chosen(int id, void *data) {
	if (id > 0) {
		run(id);
	}
}

static void list_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	if (gem_alert_up(ui.host)) {
		return;
	}
	gtk_widget_grab_focus(ui.list);
	guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
	GdkModifierType mods = gtk_event_controller_get_current_event_state(
		GTK_EVENT_CONTROLLER(g));
	int i = row_at(y);
	if (button == GDK_BUTTON_SECONDARY) {
		if (i >= 0 && !track_at(i)->selected) {
			pick_only(i);
		}
		gem_popup_clear(ui.popup);
		gem_popup_add(ui.popup, ACT_PLAY_PICKED, "Play", NULL,
			i >= 0 ? 0 : GEM_POPUP_DISABLED);
		gem_popup_add(ui.popup, ACT_REMOVE, "Remove", NULL,
			i >= 0 ? 0 : GEM_POPUP_DISABLED);
		gem_popup_add_separator(ui.popup);
		gem_popup_add(ui.popup, ACT_OPEN, "Add Files...", NULL, 0);
		gem_popup_add(ui.popup, ACT_ADD_FOLDER, "Add Folder...", NULL, 0);
		gem_popup_show(ui.popup, x, y);
		update();
		return;
	}
	if (i < 0) {
		pick_only(-1);
	} else if (n == 2) {
		pick_only(i);
		ui.state = PLAYING;
		play_at(track_at(i)->id, 0);
		return;
	} else if (mods & GDK_SHIFT_MASK) {
		int from = ui.anchor >= 0 ? ui.anchor : i;
		for (int k = 0; k < n_tracks(); k++) {
			track_at(k)->selected = k >= MIN(from, i) && k <= MAX(from, i);
		}
		ui.cursor = i;
	} else if (mods & GDK_CONTROL_MASK) {
		track_at(i)->selected = !track_at(i)->selected;
		ui.cursor = ui.anchor = i;
	} else {
		pick_only(i);
	}
	update();
}

static gboolean list_scrolled(GtkEventControllerScroll *c, double dx, double dy,
		gpointer data) {
	gtk_adjustment_set_value(ui.vadj, scroll_y() + dy * 3 * GEM_ROW_H);
	return TRUE;
}

static gboolean dropped(GtkDropTarget *t, const GValue *value, double x,
		double y, gpointer data) {
	if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
		return FALSE;
	}
	GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
	GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
	for (GSList *l = files; l != NULL; l = l->next) {
		char *p = g_file_get_path(l->data);
		if (p != NULL) {
			g_ptr_array_add(paths, p);
		}
	}
	g_ptr_array_add(paths, NULL);
	add_paths((char **)paths->pdata, ui.state == STOPPED);
	g_ptr_array_unref(paths);
	g_slist_free(files);
	return TRUE;
}

static void move_cursor(int to) {
	if (n_tracks() == 0) {
		return;
	}
	to = CLAMP(to, 0, n_tracks() - 1);
	pick_only(to);
	scroll_to_row(to);
	update();
}

static gboolean key_pressed(GtkEventControllerKey *c, guint key, guint code,
		GdkModifierType mods, gpointer data) {
	if (gem_alert_up(ui.host)) {
		return FALSE;
	}
	if (mods & GDK_CONTROL_MASK) {
		switch (gdk_keyval_to_lower(key)) {
		case GDK_KEY_o: run(ACT_OPEN); return TRUE;
		case GDK_KEY_w: case GDK_KEY_q: run(ACT_CLOSE); return TRUE;
		case GDK_KEY_a: run(ACT_SELECT_ALL); return TRUE;
		case GDK_KEY_Right: run(ACT_NEXT); return TRUE;
		case GDK_KEY_Left: run(ACT_PREVIOUS); return TRUE;
		}
		return FALSE;
	}
	switch (key) {
	case GDK_KEY_space: run(ACT_PLAY_PAUSE); return TRUE;
	case GDK_KEY_Return: case GDK_KEY_KP_Enter: run(ACT_PLAY_PICKED); return TRUE;
	case GDK_KEY_Delete: case GDK_KEY_KP_Delete: run(ACT_REMOVE); return TRUE;
	case GDK_KEY_Up: move_cursor(ui.cursor < 0 ? 0 : ui.cursor - 1); return TRUE;
	case GDK_KEY_Down: move_cursor(ui.cursor < 0 ? 0 : ui.cursor + 1); return TRUE;
	case GDK_KEY_Home: move_cursor(0); return TRUE;
	case GDK_KEY_End: move_cursor(n_tracks() - 1); return TRUE;
	case GDK_KEY_Left: seek_to(ui.position - 5); return TRUE;
	case GDK_KEY_Right: seek_to(ui.position + 5); return TRUE;
	}
	return FALSE;
}

/* ---- Menus ----------------------------------------------------------------------- */

static void build_menus(struct app_menu *m, void *data) {
	bool any = n_tracks() > 0, picked = false;
	for (int i = 0; i < n_tracks(); i++) {
		picked |= track_at(i)->selected;
	}
	app_menu_add_menu(m, "File");
	app_menu_add_item(m, ACT_OPEN, "Add Files...", "^O", 0);
	app_menu_add_item(m, ACT_ADD_FOLDER, "Add Folder...", NULL, 0);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_CLEAR, "Clear Playlist", NULL,
		any ? 0 : APP_MENU_DISABLED);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_CLOSE, "Quit", "^Q", 0);
	app_menu_add_menu(m, "Edit");
	app_menu_add_item(m, ACT_REMOVE, "Remove", "Del",
		picked ? 0 : APP_MENU_DISABLED);
	app_menu_add_item(m, ACT_SELECT_ALL, "Select All", "^A",
		any ? 0 : APP_MENU_DISABLED);
	app_menu_add_menu(m, "Controls");
	app_menu_add_item(m, ACT_PLAY_PAUSE, ui.state == PLAYING ? "Pause" : "Play",
		"Space", any ? 0 : APP_MENU_DISABLED);
	app_menu_add_item(m, ACT_STOP, "Stop", NULL,
		ui.state != STOPPED ? 0 : APP_MENU_DISABLED);
	app_menu_add_item(m, ACT_NEXT, "Next", "^Right", any ? 0 : APP_MENU_DISABLED);
	app_menu_add_item(m, ACT_PREVIOUS, "Previous", "^Left",
		any ? 0 : APP_MENU_DISABLED);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_SHUFFLE, "Shuffle", NULL,
		ui.shuffle ? APP_MENU_CHECKED : 0);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_REPEAT_OFF, "Repeat Off", NULL,
		ui.repeat == REPEAT_OFF ? APP_MENU_CHECKED : 0);
	app_menu_add_item(m, ACT_REPEAT_ALL, "Repeat All", NULL,
		ui.repeat == REPEAT_ALL ? APP_MENU_CHECKED : 0);
	app_menu_add_item(m, ACT_REPEAT_ONE, "Repeat One", NULL,
		ui.repeat == REPEAT_ONE ? APP_MENU_CHECKED : 0);
}

static void chose_file(const char *path, void *data) {
	if (path != NULL) {
		char *paths[] = { (char *)path, NULL };
		add_paths(paths, ui.state == STOPPED);
	}
}

static char *pattern(void) {
	GString *s = g_string_new(NULL);
	for (int i = 0; audio_exts[i] != NULL; i++) {
		char *up = g_ascii_strup(audio_exts[i], -1);
		g_string_append_printf(s, "%s*.%s,*.%s", i > 0 ? "," : "", audio_exts[i],
			up);
		g_free(up);
	}
	return g_string_free(s, FALSE);
}

static char *music_dir(void) {
	const char *m = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
	return g_strdup(m != NULL ? m : g_get_home_dir());
}

static void set_shuffle(bool on) {
	ui.shuffle = on;
	g_mutex_lock(&eng.lock);
	g_hash_table_remove_all(eng.played);
	g_mutex_unlock(&eng.lock);
	save_playlist();
	update();
	mpris_changed("org.mpris.MediaPlayer2.Player", "Shuffle",
		g_variant_new_boolean(on));
}

static void set_repeat(enum repeat r) {
	ui.repeat = r;
	save_playlist();
	update();
	static const char *const loops[] = { "None", "Playlist", "Track" };
	mpris_changed("org.mpris.MediaPlayer2.Player", "LoopStatus",
		g_variant_new_string(loops[r]));
}

static void run(int id) {
	if (gem_alert_up(ui.host)) {
		return;
	}
	switch (id) {
	case ACT_OPEN: {
		char *pat = pattern(), *dir = music_dir();
		gem_file_choose(ui.host, "Add Files", GEM_FILE_OPEN, dir, NULL, pat,
			"Add", chose_file, NULL);
		g_free(pat);
		g_free(dir);
		break;
	}
	case ACT_ADD_FOLDER: {
		char *dir = music_dir();
		gem_file_choose(ui.host, "Add Folder", GEM_FILE_FOLDER, dir, NULL, NULL,
			"Add", chose_file, NULL);
		g_free(dir);
		break;
	}
	case ACT_CLEAR: clear_playlist(); break;
	case ACT_CLOSE: g_application_quit(G_APPLICATION(ui.app)); break;
	case ACT_REMOVE: remove_selected(); break;
	case ACT_SELECT_ALL:
		for (int i = 0; i < n_tracks(); i++) {
			track_at(i)->selected = true;
		}
		update();
		break;
	case ACT_PLAY_PAUSE: pause_toggle(); break;
	case ACT_STOP: stop(); break;
	case ACT_NEXT: next(); break;
	case ACT_PREVIOUS: previous(); break;
	case ACT_SHUFFLE: set_shuffle(!ui.shuffle); break;
	case ACT_REPEAT_OFF: set_repeat(REPEAT_OFF); break;
	case ACT_REPEAT_ALL: set_repeat(REPEAT_ALL); break;
	case ACT_REPEAT_ONE: set_repeat(REPEAT_ONE); break;
	case ACT_PLAY_PICKED:
		for (int i = 0; i < n_tracks(); i++) {
			if (track_at(i)->selected) {
				ui.state = PLAYING;
				play_at(track_at(i)->id, 0);
				break;
			}
		}
		break;
	}
}

static void menu_activate(uint32_t id, void *data) {
	run((int)id);
}

/* ---- MPRIS ------------------------------------------------------------------------ */

static const char mpris_xml[] =
	"<node>"
	" <interface name='org.mpris.MediaPlayer2'>"
	"  <method name='Raise'/><method name='Quit'/>"
	"  <property name='CanQuit' type='b' access='read'/>"
	"  <property name='CanRaise' type='b' access='read'/>"
	"  <property name='HasTrackList' type='b' access='read'/>"
	"  <property name='Identity' type='s' access='read'/>"
	"  <property name='DesktopEntry' type='s' access='read'/>"
	"  <property name='SupportedUriSchemes' type='as' access='read'/>"
	"  <property name='SupportedMimeTypes' type='as' access='read'/>"
	" </interface>"
	" <interface name='org.mpris.MediaPlayer2.Player'>"
	"  <method name='Next'/><method name='Previous'/><method name='Pause'/>"
	"  <method name='PlayPause'/><method name='Stop'/><method name='Play'/>"
	"  <method name='Seek'><arg type='x' name='Offset' direction='in'/></method>"
	"  <method name='SetPosition'><arg type='o' name='TrackId' direction='in'/>"
	"   <arg type='x' name='Position' direction='in'/></method>"
	"  <method name='OpenUri'><arg type='s' name='Uri' direction='in'/></method>"
	"  <signal name='Seeked'><arg type='x' name='Position'/></signal>"
	"  <property name='PlaybackStatus' type='s' access='read'/>"
	"  <property name='LoopStatus' type='s' access='readwrite'/>"
	"  <property name='Rate' type='d' access='readwrite'/>"
	"  <property name='Shuffle' type='b' access='readwrite'/>"
	"  <property name='Metadata' type='a{sv}' access='read'/>"
	"  <property name='Volume' type='d' access='readwrite'/>"
	"  <property name='Position' type='x' access='read'/>"
	"  <property name='MinimumRate' type='d' access='read'/>"
	"  <property name='MaximumRate' type='d' access='read'/>"
	"  <property name='CanGoNext' type='b' access='read'/>"
	"  <property name='CanGoPrevious' type='b' access='read'/>"
	"  <property name='CanPlay' type='b' access='read'/>"
	"  <property name='CanPause' type='b' access='read'/>"
	"  <property name='CanSeek' type='b' access='read'/>"
	"  <property name='CanControl' type='b' access='read'/>"
	" </interface>"
	"</node>";

static char *track_path(guint id) {
	return g_strdup_printf("/org/gemwm/Play/track/%u", id);
}

static GVariant *metadata(void) {
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	struct track *t = track_of(ui.current);
	if (t != NULL) {
		char *path = track_path(t->id);
		g_variant_builder_add(&b, "{sv}", "mpris:trackid",
			g_variant_new_object_path(path));
		g_free(path);
		if (t->length > 0) {
			g_variant_builder_add(&b, "{sv}", "mpris:length",
				g_variant_new_int64((gint64)(t->length * G_USEC_PER_SEC)));
		}
		char *name = track_name(t);
		g_variant_builder_add(&b, "{sv}", "xesam:title",
			g_variant_new_string(name));
		g_free(name);
		if (t->artist != NULL) {
			const char *artists[] = { t->artist, NULL };
			g_variant_builder_add(&b, "{sv}", "xesam:artist",
				g_variant_new_strv(artists, -1));
		}
		if (t->album != NULL) {
			g_variant_builder_add(&b, "{sv}", "xesam:album",
				g_variant_new_string(t->album));
		}
		char *uri = g_filename_to_uri(t->path, NULL, NULL);
		if (uri != NULL) {
			g_variant_builder_add(&b, "{sv}", "xesam:url", g_variant_new_string(uri));
			g_free(uri);
		}
	} else {
		g_variant_builder_add(&b, "{sv}", "mpris:trackid",
			g_variant_new_object_path("/org/mpris/MediaPlayer2/TrackList/NoTrack"));
	}
	return g_variant_builder_end(&b);
}

static void mpris_changed(const char *iface, const char *prop, GVariant *v) {
	if (ui.bus == NULL) {
		g_variant_unref(g_variant_ref_sink(v));
		return;
	}
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", prop, v);
	g_dbus_connection_emit_signal(ui.bus, NULL, MPRIS_PATH,
		"org.freedesktop.DBus.Properties", "PropertiesChanged",
		g_variant_new("(sa{sv}as)", iface, &b, NULL), NULL);
}

static void mpris_seeked(void) {
	if (ui.bus != NULL) {
		g_dbus_connection_emit_signal(ui.bus, NULL, MPRIS_PATH,
			"org.mpris.MediaPlayer2.Player", "Seeked",
			g_variant_new("(x)", (gint64)(ui.position * G_USEC_PER_SEC)), NULL);
	}
}

static void mpris_call(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *method,
		GVariant *params, GDBusMethodInvocation *call, gpointer data) {
	if (strcmp(method, "Raise") == 0) {
		gtk_window_present(GTK_WINDOW(ui.window));
	} else if (strcmp(method, "Quit") == 0) {
		g_application_quit(G_APPLICATION(ui.app));
	} else if (strcmp(method, "Next") == 0) {
		next();
	} else if (strcmp(method, "Previous") == 0) {
		previous();
	} else if (strcmp(method, "Pause") == 0) {
		play_pause(false);
	} else if (strcmp(method, "Play") == 0) {
		play_pause(true);
	} else if (strcmp(method, "PlayPause") == 0) {
		pause_toggle();
	} else if (strcmp(method, "Stop") == 0) {
		stop();
	} else if (strcmp(method, "Seek") == 0) {
		gint64 off;
		g_variant_get(params, "(x)", &off);
		seek_to(ui.position + (double)off / G_USEC_PER_SEC);
	} else if (strcmp(method, "SetPosition") == 0) {
		const char *tid;
		gint64 pos;
		g_variant_get(params, "(&ox)", &tid, &pos);
		char *cur = track_path(ui.current);
		if (strcmp(tid, cur) == 0) {
			seek_to((double)pos / G_USEC_PER_SEC);
		}
		g_free(cur);
	} else if (strcmp(method, "OpenUri") == 0) {
		const char *uri;
		g_variant_get(params, "(&s)", &uri);
		char *p = g_filename_from_uri(uri, NULL, NULL);
		if (p != NULL) {
			char *paths[] = { p, NULL };
			add_paths(paths, true);
			g_free(p);
		}
	}
	g_dbus_method_invocation_return_value(call, NULL);
}

static GVariant *mpris_get(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *prop, GError **error,
		gpointer data) {
	bool any = n_tracks() > 0;
	if (strcmp(prop, "CanQuit") == 0 || strcmp(prop, "CanRaise") == 0 ||
			strcmp(prop, "CanSeek") == 0 || strcmp(prop, "CanControl") == 0) {
		return g_variant_new_boolean(TRUE);
	}
	if (strcmp(prop, "HasTrackList") == 0) {
		return g_variant_new_boolean(FALSE);
	}
	if (strcmp(prop, "Identity") == 0) {
		return g_variant_new_string("GemPlay");
	}
	if (strcmp(prop, "DesktopEntry") == 0) {
		return g_variant_new_string("org.gemwm.Play");
	}
	if (strcmp(prop, "SupportedUriSchemes") == 0) {
		const char *s[] = { "file", NULL };
		return g_variant_new_strv(s, -1);
	}
	if (strcmp(prop, "SupportedMimeTypes") == 0) {
		const char *s[] = { "audio/mpeg", "audio/flac", "audio/ogg", "audio/opus",
			"audio/mp4", "audio/x-wav", "audio/aac", NULL };
		return g_variant_new_strv(s, -1);
	}
	if (strcmp(prop, "PlaybackStatus") == 0) {
		static const char *const states[] = { "Stopped", "Playing", "Paused" };
		return g_variant_new_string(states[ui.state]);
	}
	if (strcmp(prop, "LoopStatus") == 0) {
		static const char *const loops[] = { "None", "Playlist", "Track" };
		return g_variant_new_string(loops[ui.repeat]);
	}
	if (strcmp(prop, "Rate") == 0 || strcmp(prop, "MinimumRate") == 0 ||
			strcmp(prop, "MaximumRate") == 0 || strcmp(prop, "Volume") == 0) {
		return g_variant_new_double(1.0);
	}
	if (strcmp(prop, "Shuffle") == 0) {
		return g_variant_new_boolean(ui.shuffle);
	}
	if (strcmp(prop, "Metadata") == 0) {
		return metadata();
	}
	if (strcmp(prop, "Position") == 0) {
		return g_variant_new_int64(ui.state == STOPPED ? 0 :
			(gint64)(ui.position * G_USEC_PER_SEC));
	}
	if (strcmp(prop, "CanGoNext") == 0 || strcmp(prop, "CanGoPrevious") == 0 ||
			strcmp(prop, "CanPlay") == 0 || strcmp(prop, "CanPause") == 0) {
		return g_variant_new_boolean(any);
	}
	g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
		"No property %s", prop);
	return NULL;
}

static gboolean mpris_set(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *prop, GVariant *value,
		GError **error, gpointer data) {
	if (strcmp(prop, "LoopStatus") == 0) {
		const char *s = g_variant_get_string(value, NULL);
		set_repeat(strcmp(s, "Track") == 0 ? REPEAT_ONE :
			strcmp(s, "Playlist") == 0 ? REPEAT_ALL : REPEAT_OFF);
	} else if (strcmp(prop, "Shuffle") == 0) {
		set_shuffle(g_variant_get_boolean(value));
	}
	return TRUE; /* Rate and Volume: only ever 1 */
}

static void mpris_start(void) {
	ui.bus = g_application_get_dbus_connection(G_APPLICATION(ui.app));
	if (ui.bus == NULL) {
		return;
	}
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(mpris_xml, NULL);
	static const GDBusInterfaceVTable vtable = { mpris_call, mpris_get, mpris_set,
		{ 0 } };
	for (int i = 0; info->interfaces[i] != NULL; i++) {
		g_dbus_connection_register_object(ui.bus, MPRIS_PATH, info->interfaces[i],
			&vtable, NULL, NULL, NULL);
	}
	g_dbus_node_info_unref(info);
	ui.mpris_owner = g_bus_own_name_on_connection(ui.bus, MPRIS_NAME,
		G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
}

/* ---- The window ------------------------------------------------------------------- */

static void list_resized(GtkDrawingArea *a, int w, int h, gpointer data) {
	fit_scroll();
}

static gboolean close_request(GtkWindow *w, gpointer data) {
	g_application_quit(G_APPLICATION(ui.app));
	return TRUE;
}

static void build_window(void) {
	ui.window = gtk_application_window_new(ui.app);
	gtk_widget_add_css_class(ui.window, "gem");
	gtk_window_set_title(GTK_WINDOW(ui.window), "GemPlay");
	gtk_window_set_default_size(GTK_WINDOW(ui.window), 460, 540);
	ui.deck_hits = gem_hits_new();

	ui.deck = gtk_drawing_area_new();
	gtk_widget_set_size_request(ui.deck, 360, DECK_H);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ui.deck), deck_draw, NULL,
		NULL);
	GtkGesture *dclick = gtk_gesture_click_new();
	g_signal_connect(dclick, "pressed", G_CALLBACK(deck_pressed), NULL);
	gtk_widget_add_controller(ui.deck, GTK_EVENT_CONTROLLER(dclick));
	GtkGesture *scrub = gtk_gesture_drag_new();
	g_signal_connect(scrub, "drag-begin", G_CALLBACK(scrub_begin), NULL);
	g_signal_connect(scrub, "drag-update", G_CALLBACK(scrub_update), NULL);
	g_signal_connect(scrub, "drag-end", G_CALLBACK(scrub_end), NULL);
	gtk_widget_add_controller(ui.deck, GTK_EVENT_CONTROLLER(scrub));

	ui.list = gtk_drawing_area_new();
	gtk_widget_set_focusable(ui.list, TRUE);
	gtk_widget_set_hexpand(ui.list, TRUE);
	gtk_widget_set_vexpand(ui.list, TRUE);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ui.list), list_draw, NULL,
		NULL);
	g_signal_connect(ui.list, "resize", G_CALLBACK(list_resized), NULL);
	GtkGesture *lclick = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(lclick), 0);
	g_signal_connect(lclick, "pressed", G_CALLBACK(list_pressed), NULL);
	gtk_widget_add_controller(ui.list, GTK_EVENT_CONTROLLER(lclick));
	GtkEventController *wheel = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	g_signal_connect(wheel, "scroll", G_CALLBACK(list_scrolled), NULL);
	gtk_widget_add_controller(ui.list, wheel);
	ui.vadj = gtk_adjustment_new(0, 0, 1, 1, 1, 1);
	g_signal_connect_swapped(ui.vadj, "value-changed",
		G_CALLBACK(gtk_widget_queue_draw), ui.list);

	ui.info = gem_pixel_area_new(0, INFO_H, paint_info, NULL);

	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(row), ui.list);
	gtk_box_append(GTK_BOX(row), gem_scrollbar_new(GTK_ORIENTATION_VERTICAL,
		ui.vadj));
	gtk_widget_set_vexpand(row, TRUE);
	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(outer), ui.deck);
	gtk_box_append(GTK_BOX(outer), row);
	gtk_box_append(GTK_BOX(outer), ui.info);
	ui.host = GTK_OVERLAY(gtk_overlay_new());
	gtk_overlay_set_child(ui.host, outer);
	gtk_window_set_child(GTK_WINDOW(ui.window), GTK_WIDGET(ui.host));

	/* Music dropped anywhere on it goes on the playlist. */
	GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
	g_signal_connect(drop, "drop", G_CALLBACK(dropped), NULL);
	gtk_widget_add_controller(GTK_WIDGET(ui.host), GTK_EVENT_CONTROLLER(drop));

	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), NULL);
	gtk_widget_add_controller(ui.window, keys);
	g_signal_connect(ui.window, "close-request", G_CALLBACK(close_request), NULL);

	ui.popup = gem_popup_new(ui.list, popup_chosen, NULL);
	ui.menu = app_menu_new(ui.window, build_menus, menu_activate, NULL);
	gtk_window_present(GTK_WINDOW(ui.window));
	gtk_widget_grab_focus(ui.list);
	update();
}

/* ---- The application ------------------------------------------------------------- */

static void activate(GtkApplication *app, gpointer data) {
	if (ui.window != NULL) {
		gtk_window_present(GTK_WINDOW(ui.window));
		return;
	}
	build_window();
}

/* Files given (from GemFiles, say): onto the playlist, the first playing. */
static void open_files(GApplication *app, GFile **files, int n, const char *hint,
		gpointer data) {
	if (ui.window == NULL) {
		build_window();
	}
	GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
	for (int i = 0; i < n; i++) {
		char *p = g_file_get_path(files[i]);
		if (p != NULL) {
			g_ptr_array_add(paths, p);
		}
	}
	g_ptr_array_add(paths, NULL);
	add_paths((char **)paths->pdata, true);
	g_ptr_array_unref(paths);
	gtk_window_present(GTK_WINDOW(ui.window));
}

static void startup(GApplication *app, gpointer data) {
	gem_ui_load_css();
	g_mutex_init(&eng.lock);
	g_queue_init(&eng.markers);
	eng.played = g_hash_table_new(g_direct_hash, g_direct_equal);
	ui.tracks = g_ptr_array_new_with_free_func(track_free);
	ui.next_id = 1;
	ui.cursor = ui.anchor = -1;
	audio_init();
	load_playlist();
	mpris_start();
	g_timeout_add(100, tick, NULL);
}

static void shutdown(GApplication *app, gpointer data) {
	decoder_stop();
	if (eng.loop != NULL) {
		pw_thread_loop_lock(eng.loop);
		pw_stream_destroy(eng.stream);
		pw_thread_loop_unlock(eng.loop);
		pw_thread_loop_stop(eng.loop);
		pw_thread_loop_destroy(eng.loop);
	}
	save_playlist();
}

int main(int argc, char *argv[]) {
	g_set_application_name("GemPlay");
	ui.app = gtk_application_new("org.gemwm.Play", G_APPLICATION_HANDLES_OPEN);
	g_application_set_option_context_parameter_string(G_APPLICATION(ui.app),
		"[FILE|FOLDER...]");
	g_signal_connect(ui.app, "startup", G_CALLBACK(startup), NULL);
	g_signal_connect(ui.app, "activate", G_CALLBACK(activate), NULL);
	g_signal_connect(ui.app, "open", G_CALLBACK(open_files), NULL);
	g_signal_connect(ui.app, "shutdown", G_CALLBACK(shutdown), NULL);
	int status = g_application_run(G_APPLICATION(ui.app), argc, argv);
	g_object_unref(ui.app);
	return status;
}

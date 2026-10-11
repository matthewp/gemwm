/*
 * The player: see player.h. A thread reads and decodes, the filter graph
 * (atempo for speed, then aformat for 48 kHz stereo float) turns it into
 * what PipeWire plays, and a ring buffer of a few seconds lies between.
 * Seeking, or a new speed, throws the ring away and starts the thread
 * again from where it's heard.
 */
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <glib.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/ringbuffer.h>
#include <stdatomic.h>
#include <string.h>
#include "player.h"

#define RATE 48000
#define CHANNELS 2
#define STRIDE (CHANNELS * sizeof(float))
#define RING_SIZE (1u << 20)
#define RING_MASK (RING_SIZE - 1)

static struct {
	struct pw_thread_loop *loop;
	struct pw_stream *stream;
	struct spa_ringbuffer ring;
	uint8_t *buf;
	_Atomic guint64 written, consumed;   /* frames, since the last start */
	_Atomic bool eof;                    /* all of it's in the ring */
	GThread *thread;
	_Atomic bool stop;

	void (*changed)(void *data);
	void (*ended)(void *data);
	void *data;

	char *source;
	enum player_state state;
	double base;            /* seconds into it where the ring starts */
	double speed;
	_Atomic double length;
	char *error;
	guint tick_id;
	bool was_buffering;
} p = { .speed = 1.0 };

/* ---- PipeWire ---- */

static void on_process(void *data) {
	struct pw_buffer *b = pw_stream_dequeue_buffer(p.stream);
	if (b == NULL) {
		return;
	}
	struct spa_data *d = &b->buffer->datas[0];
	uint8_t *dst = d->data;
	if (dst == NULL) {
		pw_stream_queue_buffer(p.stream, b);
		return;
	}
	uint32_t frames = d->maxsize / STRIDE;
	if (b->requested > 0 && b->requested < frames) {
		frames = b->requested;
	}
	uint32_t want = frames * STRIDE, idx;
	int32_t avail = spa_ringbuffer_get_read_index(&p.ring, &idx);
	uint32_t take = avail > 0 ? MIN((uint32_t)avail, want) : 0;
	take -= take % STRIDE;
	if (take > 0) {
		spa_ringbuffer_read_data(&p.ring, p.buf, RING_SIZE, idx & RING_MASK,
			dst, take);
		spa_ringbuffer_read_update(&p.ring, idx + take);
	}
	if (take < want) {
		memset(dst + take, 0, want - take);
	}
	atomic_fetch_add(&p.consumed, take / STRIDE);
	d->chunk->offset = 0;
	d->chunk->stride = STRIDE;
	d->chunk->size = want;
	pw_stream_queue_buffer(p.stream, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
};

static void stream_active(bool on) {
	pw_thread_loop_lock(p.loop);
	pw_stream_set_active(p.stream, on);
	pw_thread_loop_unlock(p.loop);
}

/* ---- Decoding ---- */

static int interrupted(void *data) {
	return atomic_load(&p.stop);
}

struct source {
	AVFormatContext *fmt;
	AVCodecContext *dec;
	AVFilterGraph *graph;
	AVFilterContext *in, *out;
	AVPacket *pkt;
	AVFrame *frame, *filtered;
	int stream;
};

static void source_close(struct source *s) {
	avfilter_graph_free(&s->graph);
	avcodec_free_context(&s->dec);
	avformat_close_input(&s->fmt);
	av_packet_free(&s->pkt);
	av_frame_free(&s->frame);
	av_frame_free(&s->filtered);
}

/* abuffer -> atempo -> aformat (48 kHz stereo float) -> abuffersink. */
static int build_graph(struct source *s, double speed) {
	AVStream *st = s->fmt->streams[s->stream];
	char layout[64] = "";
	AVChannelLayout ch = { 0 };
	if (s->dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
		av_channel_layout_default(&ch, s->dec->ch_layout.nb_channels);
	} else {
		av_channel_layout_copy(&ch, &s->dec->ch_layout);
	}
	av_channel_layout_describe(&ch, layout, sizeof(layout));
	av_channel_layout_uninit(&ch);
	char *args = g_strdup_printf(
		"time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
		st->time_base.num, st->time_base.den, s->dec->sample_rate,
		av_get_sample_fmt_name(s->dec->sample_fmt), layout);
	s->graph = avfilter_graph_alloc();
	int r = avfilter_graph_create_filter(&s->in, avfilter_get_by_name("abuffer"),
		"in", args, NULL, s->graph);
	g_free(args);
	if (r >= 0) {
		r = avfilter_graph_create_filter(&s->out,
			avfilter_get_by_name("abuffersink"), "out", NULL, NULL, s->graph);
	}
	if (r < 0) {
		return r;
	}
	char *desc = g_strdup_printf("aformat=sample_fmts=flt:sample_rates=%d:"
		"channel_layouts=stereo", RATE);
	if (speed != 1.0) {
		char *with = g_strdup_printf("atempo=%.3f,%s", speed, desc);
		g_free(desc);
		desc = with;
	}
	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs = avfilter_inout_alloc();
	outputs->name = av_strdup("in");
	outputs->filter_ctx = s->in;
	inputs->name = av_strdup("out");
	inputs->filter_ctx = s->out;
	r = avfilter_graph_parse_ptr(s->graph, desc, &inputs, &outputs, NULL);
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	g_free(desc);
	if (r >= 0) {
		r = avfilter_graph_config(s->graph, NULL);
	}
	return r;
}

static bool source_open(struct source *s, const char *src, double start,
		double speed, char **error) {
	memset(s, 0, sizeof(*s));
	s->fmt = avformat_alloc_context();
	s->fmt->interrupt_callback.callback = interrupted;
	AVDictionary *opts = NULL;
	av_dict_set(&opts, "user_agent", "GemPod/1.0", 0);
	av_dict_set(&opts, "reconnect", "1", 0);
	av_dict_set(&opts, "reconnect_streamed", "1", 0);
	av_dict_set(&opts, "reconnect_delay_max", "10", 0);
	int r = avformat_open_input(&s->fmt, src, NULL, &opts);
	av_dict_free(&opts);
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
		r = build_graph(s, speed);
	}
	if (r < 0) {
		char why[AV_ERROR_MAX_STRING_SIZE] = "";
		av_strerror(r, why, sizeof(why));
		*error = g_strdup(why);
		source_close(s);
		return false;
	}
	if (s->fmt->duration != AV_NOPTS_VALUE) {
		atomic_store(&p.length, (double)s->fmt->duration / AV_TIME_BASE);
	}
	s->pkt = av_packet_alloc();
	s->frame = av_frame_alloc();
	s->filtered = av_frame_alloc();
	if (start > 0) {
		av_seek_frame(s->fmt, -1, (int64_t)(start * AV_TIME_BASE),
			AVSEEK_FLAG_BACKWARD);
		avcodec_flush_buffers(s->dec);
	}
	return true;
}

static bool ring_write(const uint8_t *data, uint32_t bytes) {
	while (bytes > 0) {
		if (atomic_load(&p.stop)) {
			return false;
		}
		uint32_t idx;
		int32_t filled = spa_ringbuffer_get_write_index(&p.ring, &idx);
		uint32_t room = RING_SIZE - (uint32_t)filled;
		room -= room % STRIDE;
		if (room == 0) {
			g_usleep(10000);
			continue;
		}
		uint32_t n = MIN(room, bytes);
		spa_ringbuffer_write_data(&p.ring, p.buf, RING_SIZE, idx & RING_MASK,
			data, n);
		spa_ringbuffer_write_update(&p.ring, idx + n);
		atomic_fetch_add(&p.written, n / STRIDE);
		data += n;
		bytes -= n;
	}
	return true;
}

/* What the graph has ready, written; skip: seconds of it (after a seek
 * that landed short) still to drop. */
static bool drain(struct source *s, double *skip, double speed) {
	while (av_buffersink_get_frame(s->out, s->filtered) >= 0) {
		int n = s->filtered->nb_samples, drop = 0;
		if (*skip > 0) {
			drop = (int)MIN((double)n, *skip * RATE / speed);
			*skip -= drop * speed / RATE;
		}
		bool ok = ring_write(s->filtered->data[0] + drop * STRIDE,
			(n - drop) * STRIDE);
		av_frame_unref(s->filtered);
		if (!ok) {
			return false;
		}
	}
	return true;
}

struct start {
	char *source;
	double at, speed;
};

static gboolean failed(gpointer data);

static gpointer decoder(gpointer data) {
	struct start *st = data;
	struct source s;
	char *why = NULL;
	if (!source_open(&s, st->source, st->at, st->speed, &why)) {
		if (!atomic_load(&p.stop)) {
			g_idle_add(failed, why);
		} else {
			g_free(why);
		}
		g_free(st->source);
		g_free(st);
		return NULL;
	}
	double skip = 0;
	bool first = st->at > 0, ok = true, eof = false;
	AVRational tb = s.fmt->streams[s.stream]->time_base;
	while (ok && !eof) {
		int r = av_read_frame(s.fmt, s.pkt);
		if (atomic_load(&p.stop)) {
			ok = false;
			break;
		}
		if (r < 0) {
			avcodec_send_packet(s.dec, NULL);
			eof = true;
		} else if (s.pkt->stream_index != s.stream) {
			av_packet_unref(s.pkt);
			continue;
		} else {
			avcodec_send_packet(s.dec, s.pkt);
			av_packet_unref(s.pkt);
		}
		while (ok && avcodec_receive_frame(s.dec, s.frame) == 0) {
			if (first) {
				/* A seek lands on the frame at or before: the rest of
				 * the way is dropped once it's through the graph. */
				first = false;
				if (s.frame->pts != AV_NOPTS_VALUE) {
					skip = MAX(0.0, st->at - s.frame->pts * av_q2d(tb));
				}
			}
			if (av_buffersrc_add_frame(s.in, s.frame) < 0) {
				av_frame_unref(s.frame);
				continue;
			}
			ok = drain(&s, &skip, st->speed);
		}
	}
	if (ok) {
		if (av_buffersrc_add_frame(s.in, NULL) >= 0) { /* flush the graph */
			ok = drain(&s, &skip, st->speed);
		}
	}
	if (ok) {
		atomic_store(&p.eof, true);
	}
	source_close(&s);
	g_free(st->source);
	g_free(st);
	return NULL;
}

static void decoder_stop(void) {
	if (p.thread != NULL) {
		atomic_store(&p.stop, true);
		g_thread_join(p.thread);
		p.thread = NULL;
	}
	atomic_store(&p.stop, false);
}

static void flush(void) {
	pw_thread_loop_lock(p.loop);
	spa_ringbuffer_init(&p.ring);
	atomic_store(&p.written, 0);
	atomic_store(&p.consumed, 0);
	atomic_store(&p.eof, false);
	pw_thread_loop_unlock(p.loop);
}

/* ---- The main thread's side ---- */

static void changed(void) {
	if (p.changed != NULL) {
		p.changed(p.data);
	}
}

double player_position(void) {
	if (p.state == PLAYER_STOPPED) {
		return 0;
	}
	return p.base + (double)atomic_load(&p.consumed) / RATE * p.speed;
}

static gboolean tick(gpointer data) {
	if (p.state != PLAYER_PLAYING) {
		return G_SOURCE_CONTINUE;
	}
	if (atomic_load(&p.eof) &&
			atomic_load(&p.consumed) >= atomic_load(&p.written)) {
		decoder_stop();
		stream_active(false);
		p.state = PLAYER_STOPPED;
		p.base = 0;
		flush();
		if (p.ended != NULL) {
			p.ended(p.data);
		}
		changed();
		return G_SOURCE_CONTINUE;
	}
	bool buffering = player_buffering();
	if (buffering != p.was_buffering) {
		p.was_buffering = buffering;
		changed();
	}
	return G_SOURCE_CONTINUE;
}

static gboolean failed(gpointer data) {
	char *why = data;
	decoder_stop();
	stream_active(false);
	flush();
	p.state = PLAYER_STOPPED;
	g_free(p.error);
	p.error = why;
	changed();
	return G_SOURCE_REMOVE;
}

/* (Re)starts the thread at at, the speed as it is. */
static void start_at(double at) {
	decoder_stop();
	flush();
	p.base = at;
	struct start *st = g_new0(struct start, 1);
	st->source = g_strdup(p.source);
	st->at = at;
	st->speed = p.speed;
	p.thread = g_thread_new("gempod-decoder", decoder, st);
}

void player_open(const char *source, double start, bool paused) {
	g_free(p.source);
	p.source = g_strdup(source);
	g_clear_pointer(&p.error, g_free);
	atomic_store(&p.length, 0.0);
	start_at(MAX(start, 0));
	p.state = paused ? PLAYER_PAUSED : PLAYER_PLAYING;
	stream_active(!paused);
	changed();
}

void player_pause(bool paused) {
	if (p.state == PLAYER_STOPPED) {
		return;
	}
	p.state = paused ? PLAYER_PAUSED : PLAYER_PLAYING;
	stream_active(!paused);
	changed();
}

void player_stop(void) {
	decoder_stop();
	stream_active(false);
	flush();
	p.state = PLAYER_STOPPED;
	p.base = 0;
	changed();
}

void player_seek(double to) {
	if (p.state == PLAYER_STOPPED) {
		return;
	}
	double len = player_length();
	to = MAX(0, len > 0 ? MIN(to, len - 1) : to);
	start_at(to);
	changed();
}

void player_set_speed(double speed) {
	speed = CLAMP(speed, 0.5, 3.0);
	if (speed == p.speed) {
		return;
	}
	double at = player_position();
	p.speed = speed;
	if (p.state != PLAYER_STOPPED) {
		start_at(at);
	}
	changed();
}

enum player_state player_state(void) {
	return p.state;
}

double player_length(void) {
	return atomic_load(&p.length);
}

double player_speed(void) {
	return p.speed;
}

bool player_buffering(void) {
	return p.state == PLAYER_PLAYING && !atomic_load(&p.eof) &&
		atomic_load(&p.written) <= atomic_load(&p.consumed);
}

const char *player_error(void) {
	return p.error;
}

void player_init(void (*changed_fn)(void *), void (*ended_fn)(void *),
		void *data) {
	p.changed = changed_fn;
	p.ended = ended_fn;
	p.data = data;
	pw_init(NULL, NULL);
	p.buf = g_malloc0(RING_SIZE);
	spa_ringbuffer_init(&p.ring);
	p.loop = pw_thread_loop_new("gempod", NULL);
	pw_thread_loop_lock(p.loop);
	pw_thread_loop_start(p.loop);
	struct pw_properties *props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Playback",
		PW_KEY_MEDIA_ROLE, "Music",
		PW_KEY_APP_NAME, "GemPod",
		PW_KEY_APP_ID, "org.gemwm.Pod",
		PW_KEY_NODE_NAME, "GemPod",
		PW_KEY_NODE_LATENCY, "2048/48000",
		NULL);
	p.stream = pw_stream_new_simple(pw_thread_loop_get_loop(p.loop), "GemPod",
		props, &stream_events, NULL);
	uint8_t pod[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod, sizeof(pod));
	const struct spa_pod *params[1];
	params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
		&SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32,
			.channels = CHANNELS, .rate = RATE,
			.position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR }));
	pw_stream_connect(p.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
		PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
		PW_STREAM_FLAG_INACTIVE, params, 1);
	pw_thread_loop_unlock(p.loop);
	avformat_network_init();
	p.tick_id = g_timeout_add(100, tick, NULL);
}

void player_finish(void) {
	decoder_stop();
	if (p.tick_id != 0) {
		g_source_remove(p.tick_id);
	}
	if (p.loop != NULL) {
		pw_thread_loop_lock(p.loop);
		pw_stream_destroy(p.stream);
		pw_thread_loop_unlock(p.loop);
		pw_thread_loop_stop(p.loop);
		pw_thread_loop_destroy(p.loop);
		p.loop = NULL;
	}
}

/*
 * Playing one episode: from its file, or streamed from its URL. FFmpeg
 * reads and decodes it, libavfilter's atempo plays it faster or slower
 * without changing the voice's pitch, and it plays through PipeWire.
 */
#ifndef GEMPOD_PLAYER_H
#define GEMPOD_PLAYER_H

#include <stdbool.h>

enum player_state { PLAYER_STOPPED, PLAYER_PLAYING, PLAYER_PAUSED };

/* changed: the state, the length, or an error (player_error) changed;
 * ended: it played to the end. Both on the main thread. */
void player_init(void (*changed)(void *data), void (*ended)(void *data),
	void *data);
void player_finish(void);

/* From start seconds in; paused: ready, but not playing yet. */
void player_open(const char *source, double start, bool paused);
void player_pause(bool paused);
void player_stop(void);
void player_seek(double to);
/* 0.5 to 3: atempo's range is wider, but that's enough. */
void player_set_speed(double speed);

enum player_state player_state(void);
double player_position(void);   /* seconds into it, as heard */
double player_length(void);     /* 0 until known */
double player_speed(void);
bool player_buffering(void);    /* playing, but waiting for the network */
const char *player_error(void); /* why it stopped, or NULL */

#endif

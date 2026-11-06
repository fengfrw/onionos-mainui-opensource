/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_VIDEO_H
#define MAINUI_VIDEO_H
#include <SDL.h>
#include <stdbool.h>

/* Silent playback of one video file for the media gallery, with Onion's
 * FFmpeg loaded by dlopen() on the decoder thread, so nothing of it is
 * needed to start MainUI and nothing is loaded until a video is shown.
 * The thread exists only while the video is open. UI thread only. */
typedef struct MainUIVideo MainUIVideo;

/* Videos larger than this are not played (decoder memory on the Mini): every
 * video stream must state a size within it before FFmpeg probes the file,
 * which can decode, and a decoder is refused the buffer for a larger picture,
 * while probing too; cover art in the file is not decoded. */
#define MAINUI_VIDEO_MAX_WIDTH 1280
#define MAINUI_VIDEO_MAX_HEIGHT 720

/* Start playing path, scaled to fit max_width x max_height in the pixel
 * format of screen (32 bits per pixel). FFmpeg is looked for by name, then
 * in library_dir (Onion's .tmp_update/lib/parasyte; NULL for none). NULL
 * only when no thread or memory could be had; a file or library problem
 * shows as failed later. */
MainUIVideo *mainui_video_open(const char *path, const char *library_dir, const SDL_Surface *screen,
                               int max_width, int max_height);
/* The frame to show at now: the newest one due, or the one shown before.
 * NULL before the first frame. Owned by the video; valid until the next call
 * or close. Frames are timed from the first one shown; the video repeats. */
SDL_Surface *mainui_video_frame(MainUIVideo *video, Uint32 now);
/* Why the video cannot play ("" while it can): libraries missing, the file
 * unusable or too large. Once set, no frame follows. */
const char *mainui_video_error(MainUIVideo *video);
/* Milliseconds until a decoded frame is due, 0 when one is due now, or -1
 * when none is waiting: the decoder posts an SDL_USEREVENT for each frame. */
int mainui_video_due(MainUIVideo *video, Uint32 now);
/* For snapshots: wait up to timeout_ms for the first frame or an error. */
void mainui_video_wait_first(MainUIVideo *video, Uint32 timeout_ms);
/* Stop the decoder and free everything: thread, FFmpeg contexts, frames and
 * libraries. The decoder stops at its next check: FFmpeg's reads and probing
 * check through an interrupt, and every step between them. What is under way
 * is waited for: a library being loaded, a decode call, or a read blocked on
 * the card until it returns. NULL is allowed. */
void mainui_video_close(MainUIVideo *video);
#endif

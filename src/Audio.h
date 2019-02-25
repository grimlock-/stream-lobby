#pragma once
#include <pthread.h>
#include <ogg/ogg.h>
#include <janus/rtp.h>
#include "Sessions.h"

typedef struct rtp_wrapper {
	rtp_header *data;
	int length;
	uint32_t ssrc;
	uint32_t timestamp;
	uint16_t seq_number;
} rtp_wrapper;

extern unsigned int audio_mix_thread_count;
extern pthread_mutex_t audio_mix_threads_mutex;
extern pthread_cond_t audio_destroy_threads_cond;

void	audio_setup_media(janus_plugin_session*);
void	audio_hangup_media(janus_plugin_session*);
void	audio_hangup_media_no_lock(janus_plugin_session*);
void	audio_incoming_rtp(janus_plugin_session*, int, char*, int);
void	audio_incoming_rtcp(janus_plugin_session*, int, char*, int);
void*	peer_audio_thread(void*);
void*	audio_mix_thread(void*);
int	add_peer_audio(peer*, opus_int16*, int);
int	audio_packet_sort(const void*, const void*);

#include <pthread.h>
#include <stdlib.h> //rand_r
#include <time.h> //nanosleep
#include <sys/time.h> //struct timeval
#include <uuid/uuid.h>
#include <opus/opus.h>

#include <janus/plugins/plugin.h>
#include <janus/rtp.h>
#include <janus/utils.h> //janus_get_monotonic_time

#include "Audio.h"
#include "Lobbies.h"
#include "Sessions.h"
#include "Config.h"
#include "Recording.h"
#include "StreamLobby.h"

unsigned int audio_mix_thread_count;
pthread_mutex_t audio_mix_threads_mutex;
pthread_cond_t audio_destroy_threads_cond;

static int max_sample_count = SETTINGS_OPUS_FRAME_SIZE*SETTINGS_CHANNELS*20;
static struct timeval previous_rtp_time, previous_out_packet_time;
static int threadinit_result;

void audio_setup_media(janus_plugin_session *handle)
{
	JANUS_LOG(LOG_DBG, "setup_media start\n");
	if(stream_lobby_is_stopping() || !stream_lobby_is_initialized() || handle->plugin_handle == NULL)
		return;
	
	peer* dude = handle->plugin_handle;
	pthread_mutex_lock(&dude->mutex);
		dude->buffer_head = dude->buffer_tail = dude->buffer_start = calloc(max_sample_count, sizeof(opus_int32));
		if(dude->buffer_head == NULL)
		{
			char id[37];
			uuid_unparse(dude->uuid, id);
			JANUS_LOG(LOG_ERR, "[Stream Lobby] Unable to create input buffer for \"%s\" (%s)\n", dude->nick, id);
			pthread_mutex_unlock(&dude->mutex);
			return;
		}

		dude->buffer_end = dude->buffer_start + max_sample_count;
		//Decoder
		int error = 0;
		dude->decoder = opus_decoder_create(SETTINGS_SAMPLE_RATE, SETTINGS_CHANNELS, &error);
		if(error != OPUS_OK)
		{
			char id[37];
			uuid_unparse(dude->uuid, id);
			JANUS_LOG(LOG_ERR, "[Stream Lobby] Error creating audio decoder for \'%s\" (%s)\n", dude->nick, id);
			dude->decoder = NULL;
		}
		else
		{
			dude->comms_ready = 1;
			int thread = pthread_create(&dude->decoder_thread, NULL, &peer_audio_thread, dude);
			if(thread != 0)
			{
				char id[37];
				uuid_unparse(dude->uuid, id);
				JANUS_LOG(LOG_ERR, "[Stream Lobby] Couldn't create audio decoder thread for \"%s\" (%s)\n", dude->nick, id);
				switch(threadinit_result) {
					case EAGAIN:
						JANUS_LOG(LOG_ERR, "[Stream Lobby] EAGAIN - Insufficient resources OR a system-imposed thread limit was violated\n");
						break;
					case EINVAL:
						JANUS_LOG(LOG_ERR, "[Stream Lobby] EINVAL - Invalid thread attributes\n");
						break;
					case EPERM:
						JANUS_LOG(LOG_ERR, "[Stream Lobby] EPERM - No permission to set the scheduling policy and parameters specified in attr\n");
						break;
					default:
						JANUS_LOG(LOG_ERR, "[Stream Lobby] Unknown error code for creating thread\n");
						break;
				}
				opus_decoder_destroy(dude->decoder);
				dude->comms_ready = 0;
			}
		}
	pthread_mutex_unlock(&dude->mutex);
	return;
}
void audio_hangup_media(janus_plugin_session *handle)
{
	JANUS_LOG(LOG_DBG, "hangup_media start\n");
	if(handle->plugin_handle == NULL)
		return;
	
	peer* dude = handle->plugin_handle;
	pthread_mutex_lock(&dude->mutex);
		audio_hangup_media_no_lock(handle);
	pthread_mutex_unlock(&dude->mutex);
	return;
}
void audio_hangup_media_no_lock(janus_plugin_session *handle)
{
	JANUS_LOG(LOG_DBG, "hangup_media_no_lock start\n");
	peer* dude = handle->plugin_handle;
	dude->comms_ready = 0;
	free(dude->buffer_start);
	dude->buffer_head = dude->buffer_tail = dude->buffer_start = dude->buffer_end = NULL;
	dude->sample_count = 0;
	opus_decoder_destroy(dude->decoder);
	dude->decoder = NULL;
	while(dude->packets != NULL)
	{
		rtp_wrapper* packet = dude->packets->data;
		dude->packets = g_list_remove(dude->packets, packet);
		free(packet->data);
		free(packet);
	}
}




void audio_incoming_rtp(janus_plugin_session *handle, int video, char *buf, int len)
{
	if(handle == NULL || handle->stopped || handle->plugin_handle == NULL || !stream_lobby_is_initialized() || stream_lobby_is_stopping())
		return;

	peer* dude = handle->plugin_handle;
	pthread_mutex_lock(&dude->mutex);
		if(!dude->comms_ready)
		{
			char id[37];
			uuid_unparse(dude->uuid, id);
			JANUS_LOG(LOG_ERR, "[Stream Lobby] RTP packed recieved after hangup_media() for \"%s\" (%s)\n", dude->nick, id);
			pthread_mutex_unlock(&dude->mutex);
			return;
		}
		lobby* room = dude->current_lobby;
		if(room == NULL)
		{
			JANUS_LOG(LOG_ERR, "[Stream Lobby] Peer is apparently not in a lobby?\n");
			pthread_mutex_unlock(&dude->mutex);
			return;
		}
	pthread_mutex_unlock(&dude->mutex);

	//Get packet info
	rtp_wrapper* input_packet = malloc(sizeof(rtp_wrapper));
	input_packet->data = malloc(len);
	memcpy(input_packet->data, buf, len);
	rtp_header* pkt = input_packet->data;
	input_packet->data = pkt;
	input_packet->timestamp = ntohl(pkt->timestamp);
	input_packet->seq_number = ntohs(pkt->seq_number);
	input_packet->length = len;
	input_packet->ssrc = pkt->ssrc;
	int difference = 0;
	struct timeval rec_time;
	gettimeofday(&rec_time, NULL);
	if(previous_rtp_time.tv_sec != 0)
	{
		difference = rec_time.tv_usec;
		if(rec_time.tv_sec > previous_rtp_time.tv_sec)
			difference += 1000000*(rec_time.tv_sec-previous_rtp_time.tv_sec);
		difference -= previous_rtp_time.tv_usec;
	}
	previous_rtp_time.tv_sec = rec_time.tv_sec;
	previous_rtp_time.tv_usec = rec_time.tv_usec;

	JANUS_LOG(LOG_DBG, "Session: %d, RTP Packet #%u, Packet Timestamp: %u, Unix Timestamp: %u.%.6d, Time since last packet: %uus\n", handle, input_packet->seq_number, input_packet->timestamp, rec_time.tv_sec, rec_time.tv_usec, difference);
	
	opus_int32 plen = 0;
	const unsigned char* payload = (const unsigned char *) janus_rtp_payload(buf, len, &plen);
	if(payload == NULL)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Error accessing the RTP payload\n");
		return;
	}

	//OGG recording code block
	//************************
	ogg_packet* op = op_from_pkt(payload, plen);
	op->granulepos = SETTINGS_OPUS_FRAME_SIZE*ntohs(input_packet->seq_number);
	ogg_stream_packetin(room->in_ss, op);
	free(op);
	ogg_write(room, 'i');
	//************************

	//Get opus info
	uint8_t toc, pkt_configuration = 0, stereo = 0, frame_count = 0;
	memcpy(&toc, payload, 1);
	pkt_configuration = 31 & toc;
	stereo = 32 & toc;
	stereo = stereo >> 5;
	frame_count = 192 & toc;
	frame_count = frame_count >> 6;
	JANUS_LOG(LOG_DBG, "TOC: %u, Packet configuration: %u, stereo: %u, 'c' value: %u\n", toc, pkt_configuration, stereo, frame_count);
	int ret = 0;
	ret = opus_packet_get_nb_frames(payload, plen);
	switch(ret)
	{
		case OPUS_BAD_ARG:
			JANUS_LOG(LOG_DBG, "opus_packet_get_nb_frames: OPUS_BAD_ARG\n");
		break;
		case OPUS_INVALID_PACKET:
			JANUS_LOG(LOG_DBG, "opus_packet_get_nb_frames: OPUS_INVALID_PACKET\n");
		break;
		default:
			JANUS_LOG(LOG_DBG, "opus_packet_get_nb_frames: %d frame(s)\n", ret);
		break;
	}
	ret = opus_decoder_get_nb_samples(dude->decoder, payload, plen);
	switch(ret)
	{
		case OPUS_BAD_ARG:
			JANUS_LOG(LOG_DBG, "opus_decoder_get_nb_samples: OPUS_BAD_ARG\n");
		break;
		case OPUS_INVALID_PACKET:
			JANUS_LOG(LOG_DBG, "opus_decoder_get_nb_samples: OPUS_INVALID_PACKET\n");
		break;
		default:
			JANUS_LOG(LOG_DBG, "opus_decoder_get_nb_samples: %d\n", ret);
		break;
	}
	ret = opus_packet_get_bandwidth(payload);
	switch(ret)
	{
		case OPUS_BANDWIDTH_NARROWBAND:
			JANUS_LOG(LOG_DBG, "opus_packet_get_bandwidth: Narrowband (4kHz bandpass)\n");
		break;
		case OPUS_BANDWIDTH_MEDIUMBAND:
			JANUS_LOG(LOG_DBG, "opus_packet_get_bandwidth: Mediumband (6kHz bandpass)\n");
		break;
		case OPUS_BANDWIDTH_WIDEBAND:
			JANUS_LOG(LOG_DBG, "opus_packet_get_bandwidth: Wideband (8kHz bandpass)\n");
		break;
		case OPUS_BANDWIDTH_SUPERWIDEBAND:
			JANUS_LOG(LOG_DBG, "opus_packet_get_bandwidth: Superwideband (12kHz bandpass)\n");
		break;
		case OPUS_BANDWIDTH_FULLBAND:
			JANUS_LOG(LOG_DBG, "opus_packet_get_bandwidth: Fullband (20kHz bandpass)\n");
		break;
		case OPUS_INVALID_PACKET:
			JANUS_LOG(LOG_DBG, "opus_packet_get_bandwidth: OPUS_INVALID_PACKET\n");
		break;
	}
	JANUS_LOG(LOG_DBG, "opus_packet_get_nb_channels: %d\n", opus_packet_get_nb_channels(payload));

	//Discard packet if it's too old, add it to the peer's audio queue if it isn't
	pthread_mutex_lock(&dude->mutex);
		dude->packets = g_list_insert_sorted(dude->packets, input_packet, &audio_packet_sort);
		if(dude->sample_count == 0)
			dude->next_seq_num = input_packet->seq_number;
	pthread_mutex_unlock(&dude->mutex);
}
void audio_incoming_rtcp(janus_plugin_session *handle, int video, char *buf, int len)
{
	if(handle == NULL || handle->stopped || handle->plugin_handle == NULL || stream_lobby_is_stopping() || !stream_lobby_is_initialized())
		return;
	//TODO - The plugin I'm basing this one from didn't do anything here. Going to have to look into rtcp
}




/*
 * Per-peer audio decoding thread
 * FIXME - Try to make this cleaner and cut down on the mutex locking
 */
void* peer_audio_thread(void* data)
{
	if(data == NULL)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] No data for peer audio thread");
		return NULL;
	}
	peer* dude = data;
	struct timespec sleep_ln;
	sleep_ln.tv_sec = 0;
	sleep_ln.tv_nsec = 1000000; // 1ms
	g_atomic_int_inc(&audio_mix_thread_count);

	while(stream_lobby_is_initialized() && !stream_lobby_is_stopping())
	{
		pthread_mutex_lock(&dude->mutex);
			if(!dude->comms_ready)
			{
				pthread_mutex_unlock(&dude->mutex);
				break;
			}
			if(dude->packets == NULL)
			{
				pthread_mutex_unlock(&dude->mutex);
				nanosleep(&sleep_ln, NULL);
				continue;
			}
			rtp_wrapper* packet = dude->packets->data;
			if(dude->next_seq_num == packet->seq_number)
			{
				opus_int32 plen = 0;
				const unsigned char* payload = (const unsigned char *) janus_rtp_payload((char*) packet->data, packet->length, &plen);
				if(payload == NULL)
				{
					JANUS_LOG(LOG_ERR, "[Stream Lobby] Error accessing the RTP payload\n");
					dude->packets = g_list_remove(dude->packets, packet);
					free(packet->data);
					free(packet);
				}
				//Only decode audio if there's enough free space in the peer's buffer
				if(payload != NULL && opus_decoder_get_nb_samples(dude->decoder, payload, plen) > max_sample_count - dude->sample_count)
				{
					char id[37];
					uuid_unparse(dude->uuid, id);
					JANUS_LOG(LOG_VERB, "[Stream Lobby] Buffer full for \"%s\" (%s). Waiting to decode audio.\n", dude->nick, id);
					pthread_mutex_unlock(&dude->mutex);
					nanosleep(&sleep_ln, NULL);
					continue;
				}
				dude->packets = g_list_remove(dude->packets, packet);
				pthread_mutex_unlock(&dude->mutex);

				opus_int16 pcm[SETTINGS_RAW_BUFFER_SIZE*SETTINGS_CHANNELS];
				int samples = opus_decode(dude->decoder, payload, plen, pcm, SETTINGS_RAW_BUFFER_SIZE, 0);
				if(samples < 0 && payload != NULL)
				{
					JANUS_LOG(LOG_ERR, "[Stream Lobby] Error decoding Opus frame. Err no. %d (%s)\n", samples, opus_strerror(samples));
					//TODO - Should ask around if it's a good idea to treat this as a missing packet in the event of a decoding error
					samples = opus_decode(dude->decoder, NULL, 0, pcm, SETTINGS_RAW_BUFFER_SIZE, 0);
				}
				if(samples < 0)
				{
					JANUS_LOG(LOG_ERR, "[Stream Lobby] Error compensating for missing audio\n");
					pthread_mutex_lock(&dude->mutex);
				}
				else
				{
					pthread_mutex_lock(&dude->mutex);
					int prev_sample_count = dude->sample_count;
					switch(add_peer_audio(dude, pcm, samples))
					{
						case -1:
							JANUS_LOG(LOG_ERR, "[Stream Lobby] Peer's audio buffer is full, could not add audio\n");
							break;

						case 0:
							if(prev_sample_count == 0)
								gettimeofday(&dude->buffering_start, NULL);
							break;

						default:
						{
							char id[37];
							uuid_unparse(dude->uuid, id);
							JANUS_LOG(LOG_WARN, "[Stream Lobby] Some audio data couldn't be added to \"%s\"'s (%s) buffer\n", dude->nick, id);
							break;
						}
					}
				}
				if(dude->next_seq_num == 65535)
					dude->next_seq_num = 0;
				else
					dude->next_seq_num++;
				free(packet->data);
				free(packet);
			}
			else if(packet->seq_number < dude->next_seq_num && abs(packet->seq_number - dude->next_seq_num) > 5)
			{
				//Discard old packet
				dude->packets = g_list_remove(dude->packets, packet);
				char id[37];
				uuid_unparse(dude->uuid, id);
				JANUS_LOG(LOG_DBG, "[Stream Lobby] Discarding old packet for \"%s\" (%s). Seq num: %d, peer's next seq num: %d\n", dude->nick, id, packet->seq_number, dude->next_seq_num);
				free(packet->data);
				free(packet);
			}
			else
			{
				//First packet in queue is a future packet, so we're still waiting on the peer's next packet
				opus_int32 plen = 0;
				const unsigned char* payload = (const unsigned char *) janus_rtp_payload((char*)packet->data, packet->length, &plen);
				if(payload == NULL)
				{
					JANUS_LOG(LOG_ERR, "[Stream Lobby] Error accessing the RTP payload\n");
				}
				if(dude->sample_count < SETTINGS_OPUS_FRAME_SIZE*SETTINGS_CHANNELS)
				{
					pthread_mutex_unlock(&dude->mutex);
					opus_int16 pcm[SETTINGS_RAW_BUFFER_SIZE*SETTINGS_CHANNELS];
					int samples = opus_decode(dude->decoder, NULL, 0, pcm, SETTINGS_RAW_BUFFER_SIZE, 0);
					if(samples < 0)
					{
						JANUS_LOG(LOG_ERR, "[Stream Lobby] Error compensating for missing audio\n");
						pthread_mutex_lock(&dude->mutex);
					}
					else
					{
						pthread_mutex_lock(&dude->mutex);
						//int prev_sample_count = dude->sample_count;
						switch(add_peer_audio(dude, pcm, samples))
						{
							case -1:
								JANUS_LOG(LOG_ERR, "[Stream Lobby] Peer's audio buffer is full, could not add audio\n");
								break;

							case 0:
								break;

							default:
							{
								char id[37];
								uuid_unparse(dude->uuid, id);
								JANUS_LOG(LOG_WARN, "[Stream Lobby] Some audio data couldn't be added to \"%s\"'s (%s) buffer\n", dude->nick, id);
								break;
							}
						}
					}
					if(dude->next_seq_num == 65535)
						dude->next_seq_num = 0;
					else
						dude->next_seq_num++;
				}
			}
		pthread_mutex_unlock(&dude->mutex);
		nanosleep(&sleep_ln, NULL);
	}

	g_atomic_int_dec_and_test(&audio_mix_thread_count);
	pthread_mutex_lock(&audio_mix_threads_mutex);
		if(stream_lobby_is_stopping() && g_atomic_int_get(&audio_mix_thread_count) == 0)
			pthread_cond_signal(&audio_destroy_threads_cond);
	pthread_mutex_unlock(&audio_mix_threads_mutex);
	return NULL;
}


/*
 * Per-lobby audio mixing thread
 */
void* audio_mix_thread(void* data)
{
	if(data == NULL)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] No lobby, abandoning mixing thread!\n");
		return NULL;
	}
	
	lobby* room = data;
	if(room->encoder == NULL)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Lobby \"%s\" has no Opus encoder, abandoning mixing thread!\n", room->name);
		return NULL;
	}

	//OGG recording code block
	//****************************
		char in_fname[261] = {0};
		snprintf(in_fname, 261, "/var/streamlobby/%s_input.ogg", room->name);
		/*input ogg file*/
		//room->in_file = fopen(in_fname, "wb");
		if(room->in_file)
		{
			room->in_ss = malloc(sizeof(ogg_stream_state));
			if(ogg_stream_init(room->in_ss, 1) < 0) return NULL;
			ogg_packet* op = op_opushead();
			ogg_stream_packetin(room->in_ss, op);
			op_free(op);
			op = op_opustags();
			ogg_stream_packetin(room->in_ss, op);
			op_free(op);
			ogg_flush(room, 'i');
		}


		char out_fname[261] = {0};
		snprintf(out_fname, 261, "/var/streamlobby/%s_output.ogg", room->name);
		/*output ogg file*/
		//room->out_file = fopen(out_fname, "wb");
		if(room->out_file)
		{
			room->out_ss = malloc(sizeof(ogg_stream_state));
			if(ogg_stream_init(room->out_ss, 1) < 0) return NULL;
			ogg_packet* op = op_opushead();
			ogg_stream_packetin(room->out_ss, op);
			op_free(op);
			op = op_opustags();
			ogg_stream_packetin(room->out_ss, op);
			op_free(op);
			ogg_flush(room, 'o');
		}
	//****************************

	peer* participants_list[room->max_clients];
	memset(participants_list, 0, sizeof(peer*) * room->max_clients);
	unsigned int peer_count = 0, peers_skipped = 0;

	//Buffers
	int buffer_size = SETTINGS_OPUS_FRAME_SIZE*SETTINGS_CHANNELS;
	opus_int32 mix_buffer[buffer_size], tmp_buffer[buffer_size];
	opus_int16 output_buffer[buffer_size];
	memset(mix_buffer, 0, buffer_size*sizeof(opus_int32));
	memset(tmp_buffer, 0, buffer_size*sizeof(opus_int32));
	memset(output_buffer, 0, buffer_size*sizeof(opus_int16));
	//Packets
	rtp_wrapper* output_packet = calloc(1, sizeof(rtp_wrapper));
	if(output_packet == NULL)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Memory allocation failure, abandoning mixing thread!\n");
		return NULL;
	}
	output_packet->data = calloc(1, SETTINGS_OUTPUT_BUFFER_SIZE);
	if(output_packet->data == NULL) {
		JANUS_LOG(LOG_FATAL, "Memory allocation failure, abandoning mixing thread!\n");
		free(output_packet);
		return NULL;
	}
	rtp_header* payload = (rtp_header*)output_packet->data;
	//Timer
	struct timeval now, before;
	struct timespec sleep_ln;
	sleep_ln.tv_sec = 0;
	sleep_ln.tv_nsec = 1000000; // 1ms
	time_t passed, d_s, d_us;
	gettimeofday(&before, NULL);
	now.tv_sec = before.tv_sec;
	now.tv_usec = before.tv_usec;
	//RTP
	uint16_t seq;
	uint32_t ts;
	unsigned int seedp = time(NULL);

	seq = rand_r(&seedp) % RAND_MAX + 1;
	ts = rand_r(&seedp) % RAND_MAX + 1;
	payload->version = 2;
	payload->markerbit = 1;

	//Wav file stuff
	char wav_fname[261] = {0};
	snprintf(wav_fname, 261, "/var/streamlobby/%s_output.wav", room->name);
	FILE* wavFile = wav_file_init(wav_fname);
	gint64 record_lastupdate = janus_get_monotonic_time();

	g_atomic_int_inc(&audio_mix_thread_count);
	JANUS_LOG(LOG_INFO, "Audio mixing thread started for lobby \"%s\"\n", room->name);

	while(stream_lobby_is_initialized() && !stream_lobby_is_stopping())
	{
		pthread_mutex_lock(&room->mutex);
			if(room->die)
			{
				pthread_mutex_unlock(&room->mutex);
				break;
			}
		pthread_mutex_unlock(&room->mutex);
		//Has enough time passed?
		gettimeofday(&now, NULL);
		d_s = now.tv_sec - before.tv_sec;
		d_us = now.tv_usec - before.tv_usec;
		if(d_us < 0)
		{
			d_us += 1000000;
			--d_s;
		}
		passed = d_s*1000000 + d_us;
		if(passed < 20000) //20ms
		{
			nanosleep(&sleep_ln, NULL);
			continue;
		}
		//Update time var
		before.tv_usec += 20000;
		if(before.tv_usec > 1000000)
		{
			before.tv_sec++;
			before.tv_usec -= 1000000;
		}

		//Get all participants that are ready to receive A/V data (skip to next loop if there aren't any)
		if(participants_list[0] != NULL)
			memset(participants_list, 0, sizeof(peer*)*peer_count);
		peer_count = 0;
		
		pthread_mutex_lock(&room->peerlist_mutex);
			for(int i = 0; i < room->max_clients; i++)
			{
				if(room->participants[i] != NULL)
				{
					peer* dude = room->participants[i];
					pthread_mutex_lock(&dude->mutex);
						if(dude->comms_ready)
							participants_list[peer_count++] = room->participants[i];
					pthread_mutex_unlock(&dude->mutex);
				}
			}
		pthread_mutex_unlock(&room->peerlist_mutex);
		if(peer_count == 0)
			continue;

		/*FIXME - If I decide to keep the sum buffer, each peer's buffer head needs to be moved accordingly
		once I've grabbed the audio data. Releasing the mutex and grabbing the audio data again afterwards
		will allow the chance for audio to be added to a peer's buffer after the sum buffer is created, which
		can lead to some weird audio when the peer's audio is removed from the sum buffer before sending
		the packet out*/
		//Mix into single buffer
		peers_skipped = 0;
		memset(mix_buffer, 0, buffer_size*sizeof(opus_int32));
		for(int i = 0; i < peer_count; i++)
		{
			peer* dude = participants_list[i];
			pthread_mutex_lock(&dude->mutex);
				//Skip peer if they haven't sent any audio or we're still waiting for their buffer to fill
				if(dude->sample_count == 0) {
					dude->finished_buffering = 0;
					pthread_mutex_unlock(&dude->mutex);
					peers_skipped++;
					continue;
				}
				if(!dude->finished_buffering)
				{
					int current_delay = now.tv_usec;
					if(now.tv_sec > dude->buffering_start.tv_sec)
						current_delay += 1000000*(now.tv_sec-dude->buffering_start.tv_sec);
					current_delay -= dude->buffering_start.tv_usec;
					if(current_delay < SETTINGS_PEER_INPUT_DELAY)
					{
						JANUS_LOG(LOG_DBG, "Elapsed time since we started buffering: %dus\n", current_delay);
						pthread_mutex_unlock(&dude->mutex);
						peers_skipped++;
						continue;
					}
					else
					{
						dude->finished_buffering = 1;
					}
				}

				//Add audio to mixed buffer
				int j=0, samples = buffer_size < dude->sample_count ? buffer_size : dude->sample_count;
				opus_int32* tail = dude->buffer_tail;
				JANUS_LOG(LOG_DBG, "Peer has currently provided %d samples. Removing %d for server output\n", dude->sample_count, samples);
				while(samples > 0)
				{
					mix_buffer[j++] = *tail;
					samples--;
					tail++;
					if(tail == dude->buffer_end)
						tail = dude->buffer_start;
				}
			pthread_mutex_unlock(&dude->mutex);
		}
		//TODO - If somebody is streaming video data to the server, add the associated audio (if there is any) to the buffer as well
		if(peers_skipped == peer_count)
		{
			JANUS_LOG(LOG_INFO, "Nobody's saying anything, no audio to mix\n");
			continue;
		}

		//TODO - Write to wav file
		wav_file_write(wavFile, mix_buffer, buffer_size);
		/* Every 5 seconds we update the wav header */
		gint64 wav_now = janus_get_monotonic_time();
		if(wav_now - record_lastupdate >= 5*G_USEC_PER_SEC)
		{
			record_lastupdate = wav_now;
			wav_file_update_header(wavFile);
		}

		//Update RTP header
		seq++;
		ts += SETTINGS_OPUS_FRAME_SIZE;

		//Send packet to participants after removing their own voice
		for(int i = 0; i < peer_count; i++)
		{
			peer *dude = participants_list[i];
			memset(tmp_buffer, 0, sizeof(opus_int32)*buffer_size);
			pthread_mutex_lock(&dude->mutex);
				int j=0, samples = buffer_size < dude->sample_count ? buffer_size : dude->sample_count;
				while(samples > 0)
				{
					tmp_buffer[j++] = *dude->buffer_tail;
					samples--;
					dude->buffer_tail++;
					dude->sample_count--;
					if(dude->buffer_tail >= dude->buffer_end)
						dude->buffer_tail = dude->buffer_start;
				}
			pthread_mutex_unlock(&dude->mutex);

			//Remove the peer's own contribution
			for(int j=0; j<buffer_size; j++)
			{
				//output_buffer[j] = mix_buffer[j] - tmp_buffer[j];
				//I'm leaving it in for now
				output_buffer[j] = mix_buffer[j];
			}

			/* Encode raw frame to Opus */
			if(room->encoder == NULL)
				break;
			output_packet->length = opus_encode(room->encoder, output_buffer, SETTINGS_OPUS_FRAME_SIZE, (unsigned char*) payload+RTP_HEADER_SIZE, SETTINGS_OUTPUT_BUFFER_SIZE-RTP_HEADER_SIZE);

			if(output_packet->length < 0) {
				JANUS_LOG(LOG_ERR, "[Stream Lobby] Oops! got an error encoding the Opus frame: %d (%s)\n", output_packet->length, opus_strerror(output_packet->length));
			} else {
				JANUS_LOG(LOG_DBG, "Encoded %d bytes of data\n", output_packet->length);

				//OGG recording code block
				//************************
				ogg_packet* op = op_from_pkt((unsigned char*) payload+RTP_HEADER_SIZE, output_packet->length);
				op->granulepos = SETTINGS_OPUS_FRAME_SIZE*ntohs(seq);
				ogg_stream_packetin(room->out_ss, op);
				free(op);
				ogg_write(room, 'o');
				//************************


				output_packet->length += RTP_HEADER_SIZE;
				output_packet->timestamp = ts;
				output_packet->seq_number = seq;
				payload->type = dude->opus_pt;
				/* Update the timestamp and sequence number in the RTP packet, and send it */
				payload->timestamp = htonl(ts);
				payload->seq_number = htons(seq);

				if(janus_gateway != NULL)
				{
					int difference = 0;
					if(previous_out_packet_time.tv_sec != 0)
					{
						difference = now.tv_usec;
						if(now.tv_sec > previous_out_packet_time.tv_sec)
							difference += 1000000*(now.tv_sec-previous_out_packet_time.tv_sec);
						difference -= previous_out_packet_time.tv_usec;
					}
					previous_out_packet_time.tv_sec = now.tv_sec;
					previous_out_packet_time.tv_usec = now.tv_usec;

					JANUS_LOG(LOG_DBG, "Sending RTP Packet #%d. Timestamp: %u.%.6u, Time since last packet: %uus\n", seq, now.tv_sec, now.tv_usec, difference);
					janus_gateway->relay_rtp(dude->session, 0, (char *)payload, output_packet->length);
				}

				/* Restore the timestamp and sequence number to what the publisher set them to */
				payload->timestamp = htonl(output_packet->timestamp);
				payload->seq_number = htons(output_packet->seq_number);
			}
		}
		payload->markerbit = 0;
	}

	//Close wav file
	if(wavFile != NULL)
	{
		wav_file_update_header(wavFile);
		fclose(wavFile);
	}

	//audio recording code block
	//************
	fclose(room->in_file);
	room->in_file = NULL;
	ogg_stream_destroy(room->in_ss);

	fclose(room->out_file);
	room->out_file = NULL;
	ogg_stream_destroy(room->out_ss);
	//************

	opus_encoder_destroy(room->encoder);
	room->encoder = NULL;
	g_atomic_int_dec_and_test(&audio_mix_thread_count);
	pthread_mutex_lock(&audio_mix_threads_mutex);
		if(stream_lobby_is_stopping() && g_atomic_int_get(&audio_mix_thread_count) == 0)
			pthread_cond_signal(&audio_destroy_threads_cond);
	pthread_mutex_unlock(&audio_mix_threads_mutex);
	return NULL;
}




/*
 * Add given sample data to a peer's audio buffer
 */
int add_peer_audio(peer* dude, opus_int16* pcm, int samples)
{
	//FIXME - What to do if there's not enough room, add what we can, queue the samples up to be added later?
	if(max_sample_count - dude->sample_count < samples)
		return -1;

	int i=0;
	while(samples > 0)
	{
		*(dude->buffer_head) = pcm[i++];
		dude->sample_count++;
		samples--;
		dude->buffer_head++;
		if(dude->buffer_head >= dude->buffer_end)
			dude->buffer_head = dude->buffer_start;
		if(dude->buffer_head == dude->buffer_tail && samples > 0)
			return 1;
	}
	return 0;
}




//Callback for sorting a peer's audio packets when inserting into GLib list
int audio_packet_sort(const void* a, const void* b)
{
	rtp_wrapper* one = (rtp_wrapper*) a;
	rtp_wrapper* two = (rtp_wrapper*) b;
	int32_t diff = one->seq_number - two->seq_number;
	if(abs(diff) > 10)
		diff = 0-diff;
	return diff;
}


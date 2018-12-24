#include <stdlib.h>
#include <limits.h>
#include <stdint.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <jansson.h>
#include <sys/time.h>
#include <opus/opus.h>
#include <uuid/uuid.h>
#include <ogg/ogg.h>

#include <janus/plugins/plugin.h>
#include <janus/apierror.h>
#include <janus/debug.h>
#include <janus/config.h>
#include <janus/rtp.h>
#include <janus/rtcp.h>
#include <janus/utils.h>

// Plugin info
#define PLUGIN_VERSION			1
#define PLUGIN_VERSION_STRING		"1"
#define PLUGIN_DESCRIPTION		"Creates a voice and text chat room which can also send out a video stream"
#define PLUGIN_NAME			"Stream Lobby"
#define PLUGIN_AUTHOR			"grimlock-"
#define PLUGIN_PACKAGE			"plugin.streamlobby"

// Plugin settings
#define SETTINGS_CHANNELS		1
#define SETTINGS_SAMPLE_RATE		48000
#define SETTINGS_OPUS_COMPLEXITY	10
//Frame sizes for 48kHz: 120 [2.5ms], 240 [5ms], 480 [10ms], 960 [20ms], 1920 [40ms], 2880 [60ms]
#define SETTINGS_OPUS_FRAME_SIZE	960
#define SETTINGS_RAW_BUFFER_SIZE	3840	//Size in samples - support uncompressed frame sizes up to 40ms
#define SETTINGS_BITRATE		256000
#define SETTINGS_OUTPUT_BUFFER_SIZE	1000
#define SETTINGS_PEER_INPUT_DELAY	50000 //Microseconds


typedef struct lobby {
	char name[256], desc[256], subj[128], video_auth[64], video_key[256];
	unsigned int max_clients;
	unsigned int current_clients;
	pthread_t mix_thread;
	struct peer** participants; //array
	pthread_mutex_t mutex; //for lobby properties (i.e. name, desc, etc.)
	pthread_mutex_t peerlist_mutex; //for participants array and client count
	OpusEncoder* encoder;
	ogg_stream_state* in_ss;
	FILE* in_file;
	char* video_vcodec[16], video_acodec[16];
	int video_asample, video_achannels;
	unsigned int audio_enabled	: 1;
	unsigned int audio_failed	: 1;
	unsigned int video_enabled	: 1;
	unsigned int video_active	: 1;
	unsigned int is_private		: 1;
	unsigned int die		: 1;
} lobby;

typedef struct rtp_wrapper {
	rtp_header *data;
	int length;
	uint32_t ssrc;
	uint32_t timestamp;
	uint16_t seq_number;
} rtp_wrapper;

typedef struct peer {
	janus_plugin_session* session;
	uuid_t uuid;
	pthread_mutex_t mutex; //Used to access all fields below
	lobby* current_lobby;
	unsigned int lobby_id;
	char nick[64];
	opus_int32 *buffer_head, *buffer_tail, *buffer_start, *buffer_end;
	int sample_count;
	struct timeval buffering_start;
	GList* packets;
	pthread_mutex_t packets_mutex;
	pthread_t decoder_thread;
	uint16_t next_seq_num;
	int opus_pt;
	OpusDecoder* decoder;
	unsigned int is_admin      : 1;
	unsigned int comms_ready   : 1;
	unsigned int receive_audio : 1;
	unsigned int receive_video : 1;
	unsigned int finished_buffering : 1;
} peer;

typedef enum user_commands {
	SDP_PASS,
	LIST_ROOMS,
	JOIN_ROOM,
	LIST_PEERS,
	LEAVE_ROOM,
	REQUEST_SDP_OFFER
} user_commands;

typedef struct wav_header {
	char riff[4];
	uint32_t len;
	char wave[4];
	char fmt[4];
	uint32_t formatsize;
	uint16_t format;
	uint16_t channels;
	uint32_t samplerate;
	uint32_t avgbyterate;
	uint16_t samplebytes;
	uint16_t channelbits;
	char data[4];
	uint32_t blocksize;
} wav_header;




// Function prototypes
janus_plugin* create(void);
int stream_lobby_init(janus_callbacks*, const char*);
void stream_lobby_shutdown(void);
int stream_lobby_get_api_compatibility(void);
int stream_lobby_get_version(void);
const char* stream_lobby_get_version_string(void);
const char* stream_lobby_get_description(void);
const char* stream_lobby_get_name(void);
const char* stream_lobby_get_author(void);
const char* stream_lobby_get_package(void);
void stream_lobby_create_session(janus_plugin_session* handle, int* error);
struct janus_plugin_result* stream_lobby_handle_message(janus_plugin_session*, char*, json_t*, json_t*);
void stream_lobby_setup_media(janus_plugin_session*);
void stream_lobby_incoming_rtp(janus_plugin_session*, int, char*, int);
void stream_lobby_incoming_rtcp(janus_plugin_session*, int, char*, int);
void stream_lobby_hangup_media(janus_plugin_session*);
void stream_lobby_destroy_session(janus_plugin_session*, int*);
json_t* stream_lobby_query_session(janus_plugin_session*);
//not janus related functions
int addLobby(lobby*);
void removeLobby(lobby*);
void removePeer(peer*);
int message_sanity_checks(janus_plugin_session*, json_t*, char*);
void message_lobby(lobby*, const char*, peer*);
void* peer_audio_thread(void*);
void* stream_lobby_audio_mix(void*);
int add_peer_audio(peer*, opus_int16*, int);
int audio_packet_sort(const void*, const void*);



static janus_plugin stream_lobby_plugin = 
	JANUS_PLUGIN_INIT (
		.init = stream_lobby_init,
		.destroy = stream_lobby_shutdown,
		.create_session = stream_lobby_create_session,
		.handle_message = stream_lobby_handle_message,
		.setup_media = stream_lobby_setup_media,
		.incoming_rtp = stream_lobby_incoming_rtp,
		.incoming_rtcp = stream_lobby_incoming_rtcp,
		.hangup_media = stream_lobby_hangup_media,
		.destroy_session = stream_lobby_destroy_session,
		.query_session = stream_lobby_query_session,
		
		.get_api_compatibility = stream_lobby_get_api_compatibility,
		.get_version = stream_lobby_get_version,
		.get_version_string = stream_lobby_get_version_string,
		.get_description = stream_lobby_get_description,
		.get_name = stream_lobby_get_name,
		.get_author = stream_lobby_get_author,
		.get_package = stream_lobby_get_package,
	);

janus_plugin* create(void)
{
	JANUS_LOG(LOG_INFO, "%s plugin created!\n", PLUGIN_NAME);
	return &stream_lobby_plugin;
}



static struct timeval previous_rtp_time, previous_out_packet_time;

static int initialized;
static int stopping;
static unsigned int lobby_limit = 50;
static unsigned int lobby_count;
static unsigned int mix_threads_count;
static char admin_pass[64];
static char admin_disabled;
//Allow peers to store a maximum of 20 frames of audio data (going by server settings)
static int max_sample_count = SETTINGS_OPUS_FRAME_SIZE*SETTINGS_CHANNELS*20;
//threads
//static pthread_t signal_threadID;
static pthread_mutex_t lobby_mutex;
static pthread_mutex_t peer_mutex;
static int threadinit_result;
static pthread_mutex_t mix_threads_mutex;
static pthread_cond_t destroy_threads_cond;
//hash tables
static GHashTable* lobbies;
static GHashTable* connected_peers;

//Functions for interacting with Janus
static janus_callbacks* gateway = NULL;

/* OGG/Opus helpers */
void le32(unsigned char *p, int v);
void le16(unsigned char *p, int v);
ogg_packet *op_opushead(void);
ogg_packet *op_opustags(void);
ogg_packet *op_from_pkt(const unsigned char *pkt, int len);
void op_free(ogg_packet *op);
int ogg_write(lobby*, char);
int ogg_flush(lobby*, char);





/* Initialization error codes: */
#define INIT_ERROR_INVALID_ARGS 		100
#define INIT_ERROR_MEM_ALLOC_FAIL 		101
#define INIT_ERROR_THREAD_CREATION_FAIL 	102
#define INIT_ERROR_LOBBY_CREATION_FAIL 		103
/* Message handling error codes */
#define MSG_ERROR_JSON_GENERIC_ERROR		200
#define MSG_ERROR_JSON_NOT_AN_OBJECT		201
#define MSG_ERROR_JSON_MISSING_ELEMENT		202
#define MSG_ERROR_JSON_INVALID_ELEMENT		203
#define MSG_ERROR_UNKNOWN_COMMAND		210
#define MSG_ERROR_SDP_ERROR			220
#define MSG_ERROR_SDP_NO_LOBBY			221
#define MSG_ERROR_SDP_NO_MEDIA			222
#define MSG_ERROR_SDP_NO_VIDEO			223
#define MSG_ERROR_SDP_SEND_FAIL			224
#define MSG_ERROR_SDP_INVALID_OFFER		225
#define MSG_ERROR_JOIN_INVALID_LOBBY		230
#define MSG_ERROR_JOIN_LOBBY_FULL		231


int stream_lobby_init(janus_callbacks* callback, const char* config_path)
{
	if(callback == NULL || config_path == NULL || g_atomic_int_get(&initialized) != 0)
		return INIT_ERROR_INVALID_ARGS;
	
	//variable setup
	gateway = callback;
	int default_config = 0;
	pthread_mutex_init(&lobby_mutex, NULL);
	pthread_mutex_init(&peer_mutex, NULL);
	pthread_mutex_init(&mix_threads_mutex, NULL);
	
	
	//Check for config file and read through it or use a hard-coded configuration if no file found
		char filename[255];
		snprintf(filename, 255, "%s/%s.cfg", config_path, PLUGIN_PACKAGE);
		janus_config* config = janus_config_parse(filename);
		if(config == NULL)
		{
			JANUS_LOG(LOG_INFO, "No Configuration file\n");
			default_config = 1;
		}
		else
		{
			JANUS_LOG(LOG_INFO, "Loaded configuration file: %s\n", filename);
			janus_config_print(config);
			
			//We have a configuration file, so get the global stuff
			GList* config_lobby = janus_config_get_categories(config, NULL);
			while(config_lobby != NULL)
			{
				janus_config_category* category = (janus_config_category*) config_lobby->data;
				if(category->name == NULL)
				{
					JANUS_LOG(LOG_VERB, "[Stream Lobby] No category name; skipping.\n");
					config_lobby = config_lobby->next;
					continue;
				}
				else if(strcmp(category->name, "global") == 0)
				{
					JANUS_LOG(LOG_INFO, "Parsing global settings\n");
					janus_config_item* tmpLimit = janus_config_get_item(category, "lobby_limit");
					janus_config_item* tmpAdmin = janus_config_get_item(category, "admin_pass");
					
					if(tmpLimit != NULL)
					{
						lobby_limit = strtoul(tmpLimit->value, NULL, 10);
						if(lobby_limit == 0 || lobby_limit > UINT_MAX)
						{
							lobby_limit = 50;
							JANUS_LOG(LOG_WARN, "Invalid value for lobby_limit, defaulting to 50\n");
						}
						else
						{
							JANUS_LOG(LOG_INFO, "lobby_limit set to %d\n", lobby_limit);
						}
					}
					
					if(tmpAdmin == NULL)
					{
						admin_disabled = 1;
						JANUS_LOG(LOG_WARN, "Administrator password not present. Admin interface disabled!\n");
					}
					else if(tmpAdmin->value[0] == '\0')
					{
						admin_disabled = 1;
						JANUS_LOG(LOG_WARN, "Empty administrator password. Admin interface disabled!\n");
					}
					else
					{
						snprintf(admin_pass, 64, "%s", tmpAdmin->value);
					}
					break;
				}
			}
		}
		
		//Create hash tables
		lobbies = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
		connected_peers = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
		
		if(!default_config)
		{
			GList* config_lobby = janus_config_get_categories(config, NULL);
			while(config_lobby != NULL)
			{
				janus_config_category* category = (janus_config_category*) config_lobby->data;
				if(category->name == NULL)
				{
					JANUS_LOG(LOG_VERB, "[Stream Lobby] No name field, skipping lobby\n");
					config_lobby = config_lobby->next;
					continue;
				}
				else
				{
					JANUS_LOG(LOG_VERB, "[Stream Lobby] Processing \"%s\"\n", category->name);
					if(strcmp(category->name, "global") == 0 || category->name[0] == '\0')
					{
						config_lobby = config_lobby->next;
						continue;
					}
					
					if(g_hash_table_lookup(lobbies, category->name) != NULL)
					{
						JANUS_LOG(LOG_ERR, "[Stream Lobby] A lobby with the name \"%s\" already exists.\n", category->name);
						continue;
					}
					
					lobby* tmpLobby = calloc(1, sizeof(lobby));
					if(tmpLobby == NULL)
					{
						JANUS_LOG(LOG_ERR, "[Stream Lobby] Memory allocation failure! Skipping lobby: \"%s\"\n", category->name);
						continue;
					}
					janus_config_item* tmpDesc = janus_config_get_item(category, "desc");
					janus_config_item* tmpSubj = janus_config_get_item(category, "subject");
					janus_config_item* tmpPriv = janus_config_get_item(category, "private");
					janus_config_item* tmpClients = janus_config_get_item(category, "max_clients");
					janus_config_item* tmpAudio = janus_config_get_item(category, "enable_audio");
					janus_config_item* tmpVideo = janus_config_get_item(category, "video_auth");
					janus_config_item* tmpVideoKey = janus_config_get_item(category, "video_key");
					janus_config_item* tmpVideoPass = janus_config_get_item(category, "video_pass");
					JANUS_LOG(LOG_VERB, "[Stream Lobby] Processing config file. Lobby: %s\n", category->name);
					
					
					if(config_lobby == NULL)
						snprintf(tmpLobby->name, 256, "Lobby #%d", lobby_count+1);
					else
						snprintf(tmpLobby->name, 256, "%s", category->name);
					JANUS_LOG(LOG_VERB, "[Stream Lobby] Lobby Name: %s\n", tmpLobby->name);
					
					if(tmpDesc == NULL)
						snprintf(tmpLobby->name, 256, "No Description");
					else
						snprintf(tmpLobby->desc, 256, "%s", tmpDesc->value);
					JANUS_LOG(LOG_VERB, "[Stream Lobby] Description: %s\n", tmpLobby->desc);
					
					if(tmpSubj == NULL)
						snprintf(tmpLobby->name, 128, "No Subject");
					else
						snprintf(tmpLobby->subj, 128, "%s", tmpSubj->value);
					JANUS_LOG(LOG_VERB, "[Stream Lobby] Subject: %s\n", tmpLobby->subj);
					
					if(tmpPriv != NULL && strtoul(tmpPriv->value, NULL, 10) == 1)
						tmpLobby->is_private = 1;
					
					if(tmpClients == NULL)
					{
						tmpLobby->max_clients = 100;
					}
					else
					{
						unsigned int clients = strtoul(tmpClients->value, NULL, 10);
						if(clients > 0 && clients <= UINT_MAX)
							tmpLobby->max_clients = clients;
						else
							tmpLobby->max_clients = 100;
					}
					JANUS_LOG(LOG_VERB, "[Stream Lobby] Max clients: %d\n", tmpLobby->max_clients);
					
					if(tmpAudio != NULL && strtol(tmpAudio->value, NULL, 10) == 1)
					{
						int error = 0;
						//tmpLobby->encoder = opus_encoder_create(SETTINGS_SAMPLE_RATE, SETTINGS_CHANNELS, OPUS_APPLICATION_VOIP, &error);
						tmpLobby->encoder = opus_encoder_create(SETTINGS_SAMPLE_RATE, SETTINGS_CHANNELS, OPUS_APPLICATION_AUDIO, &error);
						if(error != OPUS_OK) {
							JANUS_LOG(LOG_ERR, "[Stream Lobby] Error creating audio encoder for lobby \"%s\". Disabling audio.\n", tmpLobby->name);
							tmpLobby->encoder = NULL;
						} else {
							JANUS_LOG(LOG_DBG, "Opus Encoder successfully created for lobby \"%s\"\n", tmpLobby->name);
							//Sample rate
							opus_encoder_ctl(tmpLobby->encoder, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
							//opus complexity setting
							opus_encoder_ctl(tmpLobby->encoder, OPUS_SET_COMPLEXITY(SETTINGS_OPUS_COMPLEXITY));
							//constant bit rate
							//opus_encoder_ctl(tmpLobby->encoder, OPUS_SET_VBR(0));
							//bit rate
							opus_encoder_ctl(tmpLobby->encoder, OPUS_SET_BITRATE(SETTINGS_BITRATE));
							//FEC
							opus_encoder_ctl(tmpLobby->encoder, OPUS_SET_INBAND_FEC(0));
							tmpLobby->audio_enabled = 1;
						}
					}
					JANUS_LOG(LOG_VERB, "[Stream Lobby] Audio Enabled: %s\n", tmpLobby->audio_enabled ? "yes" : "no");
					
					if(tmpVideo != NULL && (strcmp(tmpVideo->value, "password") == 0 && tmpVideoPass != NULL || strcmp(tmpVideo->value, "rsa_key") == 0 && tmpVideoKey != NULL))
					{
						tmpLobby->video_enabled = 1;
						snprintf(tmpLobby->video_auth, 64, "%s", tmpVideo->value);
						if(strcmp(tmpVideo->value, "password") == 0)
							snprintf(tmpLobby->video_key, 256, "%s", tmpVideoKey->value);
						else if(strcmp(tmpVideo->value, "rsa_key") == 0)
							snprintf(tmpLobby->video_key, 256, "%s", tmpVideoPass->value);
						JANUS_LOG(LOG_VERB, "[Stream Lobby] Video Enabled\nAuthentication Method: %s\nAuthentication Key: %s\n", tmpLobby->video_auth, tmpLobby->video_key);
					}
					
					pthread_mutex_init(&tmpLobby->mutex, NULL);
					pthread_mutex_init(&tmpLobby->peerlist_mutex, NULL);
					tmpLobby->participants = calloc(tmpLobby->max_clients, sizeof(peer*));
					//tmpLobby->id = lobby_count;
					
					if(addLobby(tmpLobby) != 0)
					{
						JANUS_LOG(LOG_INFO, "Could not add lobby \"%s\" to hash table!\n", tmpLobby->name);
						pthread_mutex_destroy(&tmpLobby->mutex);
						pthread_mutex_destroy(&tmpLobby->peerlist_mutex);
						free(tmpLobby->participants);
						tmpLobby->participants = NULL;
						if(tmpLobby->encoder != NULL) {
							opus_encoder_destroy(tmpLobby->encoder);
							tmpLobby->encoder = NULL;
						}
						free(tmpLobby);
					}
					
					if(lobby_count >= lobby_limit)
					{
						JANUS_LOG(LOG_INFO, "Maximum number of lobbies reached (%d). Stopping config file processing at \"%s\"\n", lobby_limit, tmpLobby->name);
						break;
					}
					config_lobby = config_lobby->next;
				}
			}
			janus_config_destroy(config);
			config = NULL;
		}
	//END Config setup
	
	//Default configuration
	if(lobby_count == 0)
	{
		JANUS_LOG(LOG_INFO, "Creating default text-only lobby\n");
		lobby* tmpLobby = calloc(1, sizeof(lobby));
		if(tmpLobby == NULL) {
			JANUS_LOG(LOG_ERR, "[Stream Lobby] Memory allocation failure! Could not create hard-coded fallback lobby.\n");
		} else {
			snprintf(tmpLobby->name, 256, "Default text chat lobby");
			snprintf(tmpLobby->desc, 256, "This is the default server configuration reserved for when no config file is found or the config file could not be parsed");
			snprintf(tmpLobby->subj, 128, "Vidya Gaems");
			tmpLobby->max_clients = 1000;
			
			if(addLobby(tmpLobby) != 0)
				free(tmpLobby);
		}
	}
	
	if(lobby_count == 0)
	{
		JANUS_LOG(LOG_FATAL, "Could not create any lobbies. Abandoning initialization\n");
		return INIT_ERROR_LOBBY_CREATION_FAIL;
	}
	
	//TODO - open RTMP port for video streams
	
	g_atomic_int_set(&initialized, 1);
	return 0;
}

/* Add pre-initialized lobby scructure to hash table */
int addLobby(lobby* newLobby)
{
	if(newLobby == NULL)
		return 1;
	if(g_atomic_int_get(&lobby_count) >= lobby_limit)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Lobby limit reached[%i]. Not adding lobby \"%s\".\n", lobby_limit, newLobby->name);
		return 2;
	}
	if(g_hash_table_lookup(lobbies, newLobby->name) != NULL)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] A lobby named \"%s\" has already been created.\n", newLobby->name);
		return 3;
	}
	
	if(newLobby->audio_enabled)
	{
		//audio mixing thread
		threadinit_result = pthread_create(&newLobby->mix_thread, NULL, &stream_lobby_audio_mix, newLobby);
		if(threadinit_result != 0) //failure
		{
			JANUS_LOG(LOG_ERR, "[Stream Lobby] Couldn't create audio mixing thread for lobby \"%s\". Disabling audio.\n", newLobby->name);
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
			newLobby->audio_enabled = 0;
			newLobby->audio_failed = 1;
			opus_encoder_destroy(newLobby->encoder);
			newLobby->encoder = NULL;
		}
	}
	
	pthread_mutex_lock(&lobby_mutex);
		g_hash_table_insert(lobbies, newLobby->name, newLobby);
	pthread_mutex_unlock(&lobby_mutex);
	g_atomic_int_inc(&lobby_count);
	JANUS_LOG(LOG_INFO, "Lobby \"%s\" created\n", newLobby->name);
	return 0;
}




void stream_lobby_shutdown(void)
{
	if(g_atomic_int_get(&initialized) != 1)
		return;
	g_atomic_int_set(&stopping, 1);
	GList* items, *current_item;
	int error = 0;
	
	//If there's people, kick 'em out (destroy the sessions)
	if(g_hash_table_size(connected_peers) > 0)
	{
		pthread_mutex_lock(&peer_mutex);
			items = g_hash_table_get_values(connected_peers);
		pthread_mutex_unlock(&peer_mutex);
		
		current_item = items;
		while(current_item)
		{
			peer* dude = current_item->data;
			stream_lobby_destroy_session(dude->session, &error);
			if(error != 0) {
				JANUS_LOG(LOG_ERR, "[Stream Lobby] Error #%d while destroying session\n", error);
			}
			current_item = current_item->next;
		}
		
	}
	
	//If there are any lobbies, destroy them
	if(g_atomic_int_get(&lobby_count) > 0)
	{
		lobby* room;
		int count = 0;
		
		pthread_mutex_lock(&lobby_mutex);
			items = g_hash_table_get_values(lobbies);
		pthread_mutex_unlock(&lobby_mutex);
		
		current_item = items;
		while(current_item)
		{
			room = current_item->data;
			
			if(room->audio_enabled)
			{
				count++;
				room->die = 1;
			}
			free(room->participants);
			room->participants = NULL;
			pthread_mutex_destroy(&room->mutex);
			pthread_mutex_destroy(&room->peerlist_mutex);
			
			current_item = current_item->next;
		}
		
		//Wait on audio mixing threads
		if(count > 0)
		{
			pthread_cond_init(&destroy_threads_cond, NULL);
			
			pthread_mutex_lock(&mix_threads_mutex);
				while(mix_threads_count > 0)
					pthread_cond_wait(&destroy_threads_cond, &mix_threads_mutex);
			pthread_mutex_unlock(&mix_threads_mutex);
			
			pthread_cond_destroy(&destroy_threads_cond);
			pthread_mutex_destroy(&mix_threads_mutex);
		}
		
		//Free lobby resources
		current_item = items;
		while(current_item)
		{
			room = current_item->data;
			
			opus_encoder_destroy(room->encoder);
			room->encoder = NULL;
			pthread_mutex_lock(&lobby_mutex);
				if(g_hash_table_remove(lobbies, room->name))
				{
					g_atomic_int_dec_and_test(&lobby_count);
					JANUS_LOG(LOG_VERB, "[Stream Lobby] Successfully removed lobby \"%s\" from hash table. Remaining lobbies: %lu\n", room->name, g_atomic_int_get(&lobby_count));
					free(room);
				}
				else
				{
					JANUS_LOG(LOG_ERR, "[Stream Lobby] Could not find or could not remove lobby \"%s\" from hash table.\n", room->name);
				}
			pthread_mutex_unlock(&lobby_mutex);
			
			current_item = current_item->next;
		}
	}
	
	//Free hash table & mutex resources
	pthread_mutex_lock(&lobby_mutex);
		g_hash_table_destroy(lobbies);
	pthread_mutex_unlock(&lobby_mutex);
	pthread_mutex_lock(&peer_mutex);
		g_hash_table_destroy(connected_peers);
	pthread_mutex_unlock(&peer_mutex);
	
	//Destroy mutexes
	pthread_mutex_destroy(&peer_mutex);
	pthread_mutex_destroy(&lobby_mutex);

	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);
}

/*TODO - If the function that Janus uses to handoff an incoming message is not thread safe,
then the references to room->die need to be made atomic and a safeguard needs to be put in
so no 2 threads call pthread_join for the same thread*/
void removeLobby(lobby* room)
{
	if(room == NULL || room->die == 1)
		return;
	JANUS_LOG(LOG_INFO, "Removing lobby \"%s\"\n", room->name);
	room->die = 1;
	
	//Kick everybody out
	pthread_mutex_lock(&room->peerlist_mutex);
		unsigned int count = room->current_clients;
	pthread_mutex_unlock(&room->peerlist_mutex);
	if(count > 0)
	{
		for(int i = 0; i < room->max_clients; i++)
		{
			if(room->participants[i] != NULL)
				removePeer(room->participants[i]);
		}
	}
	free(room->participants);
	room->participants = NULL;
	pthread_mutex_destroy(&room->mutex);
	pthread_mutex_destroy(&room->peerlist_mutex);
	
	//Stop the mixing thread and destroy audio resources
	if(room->audio_enabled) {
		pthread_join(room->mix_thread, NULL);
		opus_encoder_destroy(room->encoder);
		room->encoder = NULL;
	}
	
	//Destroy the lobby structure
	pthread_mutex_lock(&lobby_mutex);
		if(g_hash_table_remove(lobbies, room->name))
		{
			g_atomic_int_dec_and_test(&lobby_count);
			JANUS_LOG(LOG_VERB, "[Stream Lobby] Successfully removed lobby \"%s\" from hash table. Remaining lobbies: %lu\n", room->name, g_atomic_int_get(&lobby_count));
			free(room);
		}
		else//Failure
		{
			JANUS_LOG(LOG_ERR, "[Stream Lobby] Could not find or could not remove lobby \"%s\" from hash table.\n", room->name);
		}
	pthread_mutex_unlock(&lobby_mutex);
	return;
}




void stream_lobby_create_session(janus_plugin_session *handle, int *error)
{
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
	{
		*error = 1;
		return;
	}
	
	peer* dude = calloc(1, sizeof(peer));
	if(dude == NULL)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Memory allocation failure! Could not create session with peer.\n");
		*error = 2;
		return;
	}
	handle->plugin_handle = dude;
	if(uuid_generate_time_safe(dude->uuid) != 0) {
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Could not generate uuid for peer\n");
		*error = 3;
		free(dude);
	}
	snprintf(dude->nick, 64, "Anonymous");
	dude->session = handle;
	pthread_mutex_init(&dude->mutex, NULL);
	pthread_mutex_lock(&peer_mutex);
		g_hash_table_insert(connected_peers, dude->uuid, dude);
	pthread_mutex_unlock(&peer_mutex);
	char id[37];
	uuid_unparse(dude->uuid, id);
	JANUS_LOG(LOG_INFO, "Session %s created.\n", id);
	return;
}




void stream_lobby_destroy_session(janus_plugin_session *handle, int *error)
{
	JANUS_LOG(LOG_DBG, "destroy_session started\n");
	if(!g_atomic_int_get(&initialized))
	{
		*error = 1;
		return;
	}
	if(handle->plugin_handle == NULL)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] No plugin data for this session. Goodbye you little shit.\n");
		*error = 2;
		return;
	}
	
	peer* dude = handle->plugin_handle;
	removePeer(dude);
	pthread_mutex_lock(&peer_mutex);
		g_hash_table_remove(connected_peers, dude->uuid);
	pthread_mutex_unlock(&peer_mutex);
	char id[37], nick[64];
	uuid_unparse(dude->uuid, id);
	//TODO - If you're going to use sprintf, escape the characters in the peer's nick
	snprintf(nick, 64, dude->nick);
	pthread_mutex_destroy(&dude->mutex);
	free(dude);
	handle->plugin_handle = NULL;
	JANUS_LOG(LOG_INFO, "Session %s (%s) destroyed.\n", id, nick);
	return;
}




/*json structure
  {
	  "uuid": <string>,
	  "nick": <string>,
	  "lobby": <string>
  }
*/
json_t* stream_lobby_query_session(janus_plugin_session *handle)
{
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return NULL;
	if(handle->plugin_handle == NULL)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] No plugin data for this session. Session query rejected.\n");
		return NULL;
	}
	
	json_t* response = json_object();
	peer* dude = handle->plugin_handle;
	char uid[37];
	uuid_unparse(dude->uuid, uid);
	json_object_set_new(response, "uuid", json_string(uid));
	pthread_mutex_lock(&dude->mutex);
		json_object_set_new(response, "nick", json_string(dude->nick));
		pthread_mutex_lock(&dude->current_lobby->mutex);
			json_object_set_new(response, "lobby", json_string(dude->current_lobby->name));
		pthread_mutex_unlock(&dude->current_lobby->mutex);
	pthread_mutex_unlock(&dude->mutex);
	return response;
}




void stream_lobby_setup_media(janus_plugin_session *handle)
{
	JANUS_LOG(LOG_DBG, "setup_media start\n");
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized) || handle->plugin_handle == NULL)
		return;
	
	peer* dude = handle->plugin_handle;
	pthread_mutex_lock(&dude->mutex);
		dude->buffer_head = dude->buffer_tail = dude->buffer_start = calloc(max_sample_count, sizeof(opus_int32));
		if(dude->buffer_head == NULL)
		{
			char id[37];
			uuid_unparse(dude->uuid, id);
			JANUS_LOG(LOG_ERR, "[Stream Lobby] Unable to create input buffer for \"%s\" (%s)\n", dude->nick, id);
		}
		else
		{
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
		}
	pthread_mutex_unlock(&dude->mutex);
	return;
}




void stream_lobby_hangup_media(janus_plugin_session *handle)
{
	JANUS_LOG(LOG_DBG, "hangup_media start\n");
	if(handle->plugin_handle == NULL)
		return;
	
	peer* dude = handle->plugin_handle;
	pthread_mutex_lock(&dude->mutex);
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
	pthread_mutex_unlock(&dude->mutex);
	return;
}




/*FIXME - I don't like how a bunch of mutexes get locked here and then it moves on to hangup_media() which
then locks another mutex. This means that error checking for the block of code in hangup_media()
means checking for double mutex locks for three different mutexes every place EITHER function gets called at*/
void removePeer(peer* dude)
{
	JANUS_LOG(LOG_DBG, "removePeer() start\n");
	if(dude == NULL)
		return;
	if(dude->comms_ready)
		stream_lobby_hangup_media(dude->session);
	pthread_mutex_lock(&dude->mutex);
		if(dude->current_lobby == NULL)
		{
			pthread_mutex_unlock(&dude->mutex);
			JANUS_LOG(LOG_VERB, "[Stream Lobby] Peer is not in any lobby\n");
			return;
		}
		char* lobby_name = dude->current_lobby->name;
		message_lobby(dude->current_lobby, "peer_leave", dude);
		pthread_mutex_lock(&dude->current_lobby->peerlist_mutex);
			dude->current_lobby->participants[dude->lobby_id] = NULL;
			dude->current_lobby->current_clients--;
		pthread_mutex_unlock(&dude->current_lobby->peerlist_mutex);
		dude->current_lobby = NULL;
		char id[37];
		uuid_unparse(dude->uuid, id);
		JANUS_LOG(LOG_INFO, "Session %s (%s) removed from lobby (%s)\n", id, dude->nick, lobby_name);
	pthread_mutex_unlock(&dude->mutex);
}




/*Response json structure:
  {
	  "status": "ok" | "error",
	  "error_code": <int> (not present if status is 'ok'),
	  "stuff": <object> (not present if there's no data to return),
  }
*/
struct janus_plugin_result* stream_lobby_handle_message(janus_plugin_session *handle, char* transaction, json_t* message, json_t* jsep)
{
	JANUS_LOG(LOG_DBG, "stream_lobby_handle_message() start\n");
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized) || handle == NULL || handle->plugin_handle == NULL || message == NULL)
		return janus_plugin_result_new(JANUS_PLUGIN_ERROR, "Error with Janus Gateway", NULL);
	
	char error_msg[256];
	int error = message_sanity_checks(handle, message, error_msg);
	JANUS_LOG(LOG_DBG, "Sanity check result: %d\n", error);
	if(error != 0)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Error processing message. %d: %s\n", error, error_msg);
		return janus_plugin_result_new(JANUS_PLUGIN_OK, error_msg, NULL);
	}
	json_t* response = json_object();
	const char* response_type = NULL;
	char response_sdp[1024] = {0};
	user_commands request = -1;
	
	request = json_integer_value(json_object_get(message, "request"));
	
	//TODO - Have to put a safeguard in here to stop clients from blocking everything with stupid shit like while(1){ send_message("list_rooms"); }
	switch(request)
	{
		case LIST_ROOMS:
		{
			JANUS_LOG(LOG_DBG, "list_rooms start\n");
			
			peer* dude = handle->plugin_handle;
			char all_rooms = 0;
			pthread_mutex_lock(&dude->mutex);
				if(dude->is_admin)
				{
					json_t* hidden_json = json_object_get(message, "include_hidden");
					if(hidden_json != NULL && json_is_boolean(hidden_json))
						all_rooms = json_is_true(hidden_json);
				}
			pthread_mutex_unlock(&dude->mutex);
			
			json_t* rooms_json = json_array();
			GList *rooms, *cr;
			pthread_mutex_lock(&lobby_mutex);
				rooms = g_hash_table_get_values(lobbies);
			pthread_mutex_unlock(&lobby_mutex);
			cr = rooms;
			while(cr)
			{
				lobby* room = cr->data;
				if(room->is_private && !all_rooms)
				{
					cr = cr->next;
					continue;
				}
				json_t* tmp_json = json_object();
				pthread_mutex_lock(&room->mutex);
					json_object_set_new(tmp_json, "name", json_string(room->name));
					json_object_set_new(tmp_json, "subject", json_string(room->subj));
					json_object_set_new(tmp_json, "description", json_string(room->desc));
					json_object_set_new(tmp_json, "audio_enabled", json_integer(room->audio_enabled));
					json_object_set_new(tmp_json, "video_enabled", json_integer(room->video_enabled));
					json_object_set_new(tmp_json, "video_active", json_integer(room->video_active));
					json_object_set_new(tmp_json, "max_clients", json_integer(room->max_clients));
				pthread_mutex_unlock(&room->mutex);
				pthread_mutex_lock(&room->peerlist_mutex);
					json_object_set_new(tmp_json, "connected_clients", json_integer(room->current_clients));
				pthread_mutex_unlock(&room->peerlist_mutex);
				json_array_append_new(rooms_json, tmp_json);
				cr = cr->next;
			}
			json_object_set_new(response, "status", json_string("ok"));
			json_object_set(response, "stuff", rooms_json);
			json_decref(rooms_json);
			break;
		} //end LIST_ROOMS
		
		case JOIN_ROOM:
		{
			JANUS_LOG(LOG_DBG, "join_room start\n");
			peer* dude = handle->plugin_handle;
			lobby* room;
			json_t* room_json = json_object_get(message, "room");
			if(room_json == NULL)
			{
				error = MSG_ERROR_JSON_MISSING_ELEMENT;
				snprintf(error_msg, 256, "No lobby name provided");
				break;
			}
			if(!json_is_string(room_json))
			{
				error = MSG_ERROR_JSON_INVALID_ELEMENT;
				snprintf(error_msg, 256, "Lobby name is not a string");
				break;
			}
			pthread_mutex_lock(&lobby_mutex);
				room = g_hash_table_lookup(lobbies, json_string_value(room_json));
				if(room == NULL)
				{
					pthread_mutex_unlock(&lobby_mutex);
					error = MSG_ERROR_JOIN_INVALID_LOBBY;
					snprintf(error_msg, 256, "Requested lobby does not exist");
					break;
				}
			pthread_mutex_unlock(&lobby_mutex);
			pthread_mutex_lock(&room->mutex);
				if(room->die)
				{
					pthread_mutex_unlock(&room->mutex);
					error = MSG_ERROR_JOIN_INVALID_LOBBY;
					snprintf(error_msg, 256, "Requested lobby is shutting down");
					break;
				}
			pthread_mutex_unlock(&room->mutex);
			
			//Error checks finished, start setting stuff up
			removePeer(dude);
			
			pthread_mutex_lock(&dude->mutex);
				//Generate lobby id for session, and assign session to that slot
				srand(time(NULL));
				pthread_mutex_lock(&room->peerlist_mutex);
					if(room->current_clients >= room->max_clients)
					{
						pthread_mutex_unlock(&room->peerlist_mutex);
						error = MSG_ERROR_JOIN_LOBBY_FULL;
						snprintf(error_msg, 256, "Requested lobby is full");
						break;
					}
					
					do
					{
						dude->lobby_id = rand() % room->max_clients;
					} while(room->participants[dude->lobby_id] != NULL);
					room->participants[dude->lobby_id] = dude;
					dude->current_lobby = room;
				pthread_mutex_unlock(&room->peerlist_mutex);
				pthread_mutex_lock(&room->mutex);
					room->current_clients++;
				pthread_mutex_unlock(&room->mutex);
				if(!dude->comms_ready)
				{
					dude->opus_pt = 0;
				}
			pthread_mutex_unlock(&dude->mutex);
			message_lobby(room, "peer_join", dude);
			json_object_set_new(response, "status", json_string("ok"));
			//TODO - Return the lobby's properties in the json (i.e. if there's a video stream available)
			break;
		} //end JOIN_ROOM

		case LEAVE_ROOM:
		{
			JANUS_LOG(LOG_DBG, "leave_room start\n");
			peer* dude = handle->plugin_handle;
			removePeer(dude);
			json_object_set_new(response, "status", json_string("ok"));
			break;
		}
		
		case LIST_PEERS:
		{
			JANUS_LOG(LOG_DBG, "list_peers start\n");
			break;
		}
		
		case SDP_PASS:
		{
			JANUS_LOG(LOG_DBG, "sdp_passthrough start\n");
			if(jsep == NULL)
			{
				error = MSG_ERROR_SDP_ERROR;
				snprintf(error_msg, 256, "No JSEP object to process");
				break;
			}
			char* sdp = json_dumps(jsep, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
			char* sdp_type = json_string_value(json_object_get(jsep, "type"));
			//If its an SDP offer, create an answer, otherwise let janus do it's thing
			if(!strcasecmp(sdp_type, "offer"))
			{
				//Reject offer if it isn't sendonly
				if(strstr(sdp, "sendonly") == NULL)
				{
					error = MSG_ERROR_SDP_INVALID_OFFER;
					snprintf(error_msg, 256, "SDP offers must be sendonly");
					break;
				}
				peer* dude = handle->plugin_handle;
				pthread_mutex_lock(&dude->mutex);
				if(dude->current_lobby == NULL)
				{
					pthread_mutex_unlock(&dude->mutex);
					error = MSG_ERROR_SDP_NO_LOBBY;
					snprintf(error_msg, 256, "Cannot process SDP before client has entered a lobby");
					break;
				}
				lobby* room = dude->current_lobby;
				if(!room->audio_enabled)
				{
					pthread_mutex_unlock(&dude->mutex);
					error = MSG_ERROR_SDP_NO_LOBBY;
					snprintf(error_msg, 256, "Lobby does not support audio");
					break;
				}
				pthread_mutex_unlock(&dude->mutex);

				dude->opus_pt = janus_get_codec_pt(json_string_value(sdp), "opus");
				if(dude->opus_pt == -1)
					dude->opus_pt = 0;
				int offset = snprintf(response_sdp, 1024, "v=0\n"
						"o=server %"SCNu64" %"SCNu64" IN IP4 127.0.0.1\n"
						"s=stream session\n"
						"t=0 0\n",
						janus_get_monotonic_time(),
						janus_get_monotonic_time());
				if(offset < 0)
				{
					error = MSG_ERROR_SDP_ERROR;
					snprintf(error_msg, 256, "Error while creating SDP response");
					break;
				}
				//Audio
				if(strstr(sdp, "m=audio") == NULL)
				{
					error = MSG_ERROR_SDP_NO_MEDIA;
					snprintf(error_msg, 256, "SDP offers must have at least one audio track");
					break;
				}
				else
				{
					offset += snprintf(response_sdp+offset, 1024-offset, "m=audio 1 RTP/SAVPF %d\r\n", dude->opus_pt);
					offset += snprintf(response_sdp+offset, 1024-offset, "a=rtpmap:%d opus/48000/2\r\n", dude->opus_pt);
					offset += snprintf(response_sdp+offset, 1024-offset, "a=fmtp:%d maxplaybackrate=%d;stereo=0;\r\n", dude->opus_pt, SETTINGS_SAMPLE_RATE);
					offset += snprintf(response_sdp+offset, 1024-offset, "a=recvonly\r\n");
					offset += snprintf(response_sdp+offset, 1024-offset, "c=IN IP4 1.1.1.1\r\n");
				}
				//Reject video
				if(strstr(sdp, "m=video") != NULL)
					offset += snprintf(response_sdp+offset, 1024-offset, "m=video 0 RTP/AVP 0\r\n");
				//Reject data channels
				if(strstr(sdp, "DTLS/SCTP") != NULL)
					offset += snprintf(response_sdp+offset, 1024-offset, "m=application 0 DTLS/SCTP 0\r\n");


				json_t* sdp_json = json_object();
				json_object_set_new(sdp_json, "status", json_string("ok"));
				json_t* offer_jsep = json_pack("{ssss}", "type", "answer", "sdp", response_sdp);
				int result = gateway->push_event(handle, &stream_lobby_plugin, "sdp_answer", sdp_json, offer_jsep);
				if(result != JANUS_OK)
				{
					error = MSG_ERROR_SDP_SEND_FAIL;
					snprintf(error_msg, 256, "Error sending you the SDP offer");
					break;
				}
			}

			json_object_set_new(response, "status", json_string("ok"));
			//json_object_set_new(response, "stuff", json_string(response_sdp));
			free(sdp);
			break;
		} //end SDP_PASS

		case REQUEST_SDP_OFFER:
		{
			JANUS_LOG(LOG_DBG, "request_sdp_offer start\n");
			peer* dude = handle->plugin_handle;
			pthread_mutex_lock(&dude->mutex);
			if(dude->current_lobby == NULL)
			{
				pthread_mutex_unlock(&dude->mutex);
				error = MSG_ERROR_SDP_NO_LOBBY;
				snprintf(error_msg, 256, "Cannot process SDP before client has entered a lobby");
				break;
			}
			lobby* room = dude->current_lobby;
			if(!room->audio_enabled && !room->video_enabled)
			{
				pthread_mutex_unlock(&dude->mutex);
				error = MSG_ERROR_SDP_NO_LOBBY;
				snprintf(error_msg, 256, "Lobby does not support audio or video");
				break;
			}
			pthread_mutex_unlock(&dude->mutex);
			json_t* audio = json_integer_value(json_object_get(message, "audio"));
			json_t* video = json_integer_value(json_object_get(message, "video"));
			if(!audio && !video)
			{
				error = MSG_ERROR_SDP_NO_MEDIA;
				snprintf(error_msg, 256, "No media specified");
				break;
			}


			int no_media = 1, offset = snprintf(response_sdp, 1024, "v=0\r\n"
				/*username,id,version number,IP addr*/
				"o=server %"SCNu64" %"SCNu64" IN IP4 127.0.0.1\r\n"
				"s=stream session\r\n"
				"t=0 0\r\n",
				janus_get_monotonic_time(),
				janus_get_monotonic_time());
			if(offset<0)
			{
				error = MSG_ERROR_SDP_ERROR;
				snprintf(error_msg, 256, "Error while creating SDP offer");
				break;
			}
			if(audio && room->audio_enabled)
			{
				/*Payload types in the range 96-127 are dynamically defined payload types
				Reference: https://tools.ietf.org/html/rfc3551#section-5*/
				offset += snprintf(response_sdp+offset, 1024-offset, "m=audio 1 RTP/SAVPF 96\r\n");
				offset += snprintf(response_sdp+offset, 1024-offset, "a=rtpmap:96 opus/48000/2\r\n");
				offset += snprintf(response_sdp+offset, 1024-offset, "c=IN IP4 1.1.1.1\r\n");
				offset += snprintf(response_sdp+offset, 1024-offset, "a=fmtp:96 maxplaybackrate=48000;stereo=%d;sprop-stereo=%d;useinbandfec=0\r\n", SETTINGS_CHANNELS-1, SETTINGS_CHANNELS-1);
				dude->opus_pt = 96;
				no_media = 0;
			}
			if(video && room->video_active)
			{
				offset += snprintf(response_sdp+offset, 1024-offset, "m=audio 1 RTP/SAVPF 97\r\n");
				offset += snprintf(response_sdp+offset, 1024-offset, "a=sendonly\r\n");
				offset += snprintf(response_sdp+offset, 1024-offset, "a=rtpmap:97 %s/%d/%d\r\n", room->video_acodec, room->video_asample, room->video_achannels);
				offset += snprintf(response_sdp+offset, 1024-offset, "c=IN IP4 1.1.1.1\r\n");
				offset += snprintf(response_sdp+offset, 1024-offset, "m=video 1 RTP/SAVPF 98\r\n");
				offset += snprintf(response_sdp+offset, 1024-offset, "a=sendonly\r\n");
				offset += snprintf(response_sdp+offset, 1024-offset, "a=rtpmap:98 %s/90000\r\n", room->video_vcodec);
				offset += snprintf(response_sdp+offset, 1024-offset, "c=IN IP4 1.1.1.1\r\n");
				no_media = 0;
			}

			if(no_media)
			{
				error = MSG_ERROR_SDP_NO_MEDIA;
				snprintf(error_msg, 256, "This lobby does not support the requested media");
				break;
			}
			else
			{
				json_object_set_new(response, "status", json_string("ok"));
				json_t* sdp_json = json_object();
				json_object_set_new(sdp_json, "status", json_string("ok"));
				json_t* offer_jsep = json_pack("{ssss}", "type", "offer", "sdp", response_sdp);
				int result = gateway->push_event(handle, &stream_lobby_plugin, "sdp_offer", sdp_json, offer_jsep);
				if(result != JANUS_OK)
				{
					error = MSG_ERROR_SDP_SEND_FAIL;
					snprintf(error_msg, 256, "Error sending you the SDP offer");
				}
				json_decref(sdp_json);
				json_decref(offer_jsep);
				break;
			}
		} //end REQUEST_SDP_OFFER
		
		default:
		{
			error = MSG_ERROR_UNKNOWN_COMMAND;
			snprintf(error_msg, 256, "Unknown command %d", request);
			break;
		}
	}
	json_decref(message);
	if(error != 0)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Error %d processing message: %s\n", error, error_msg);
		json_t* err_json = json_object();
		json_object_set_new(err_json, "status", json_string("error"));
		json_object_set_new(err_json, "error_code", json_integer(error));
		json_object_set_new(err_json, "error_message", json_string(error_msg));
		return janus_plugin_result_new(JANUS_PLUGIN_OK, NULL, err_json);
	}

	char* result_text = json_dumps(response, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
	JANUS_LOG(LOG_DBG, "Response to peer: %s\n", result_text);
	free(result_text);
	return janus_plugin_result_new(JANUS_PLUGIN_OK, NULL, response);
}


int message_sanity_checks(janus_plugin_session* handle, json_t* message, char* error)
{
	JANUS_LOG(LOG_DBG, "Starting message sanity checks\n");
	if(!json_is_object(message)) {
		if(error != NULL)
			snprintf(error, 256, "JSON error: not an object");
		json_decref(message);
		return MSG_ERROR_JSON_NOT_AN_OBJECT;
	}
	json_t* request = json_object_get(message, "request");
	if(request == NULL) {
		if(error != NULL)
			snprintf(error, 256, "Missing element (request)");
		json_decref(message);
		return MSG_ERROR_JSON_MISSING_ELEMENT;
	}
	if(!json_is_integer(request)) {
		if(error != NULL)
			snprintf(error, 256, "Invalid element (request should be an integer)");
		json_decref(message);
		return MSG_ERROR_JSON_INVALID_ELEMENT;
	}
	return 0;
}


/*Event message structure
  {
	  "event": <string>,
	  "stuff": {
		  //the data for this object varies depending on the event
		  "uuid": <string> (not always present),
		  "nick": <strong> (not always present),
	  }
  }
*/
void message_lobby(lobby* room, const char* msg_type, peer* dude)
{
	if(room == NULL || msg_type == NULL)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Missing argument, abandoning message_lobby()\n");
		return;
	}
	
	json_t* event_json = json_object();
	json_object_set_new(event_json, "event", json_string(msg_type));
	if(!strcasecmp(msg_type, "peer_leave"))
	{
		if(!dude)
		{
			JANUS_LOG(LOG_ERR, "[Stream Lobby] No peer argument, abandoning message_lobby()\n");
			return;
		}
		int j = 0;
		char uid[37] = {0};
		uuid_unparse(dude->uuid, uid);
		json_t* data_json = json_object();
		
		json_object_set_new(data_json, "uuid", json_string(uid));
		json_object_set_new(event_json, "stuff", data_json);
		
		peer* participants_list[room->max_clients];
		memset(participants_list, 0, sizeof(peer*) * room->max_clients);
		pthread_mutex_lock(&room->peerlist_mutex);
			for(int i = 0; i < room->max_clients; i++)
			{
				if(room->participants[i] != NULL)
					participants_list[j++] = room->participants[i];
			}
		pthread_mutex_unlock(&room->peerlist_mutex);
		for(int i = 0; i < j; i++)
		{
			peer* p = participants_list[i];
			if(p == dude)
				continue;
			gateway->push_event(p->session, &stream_lobby_plugin, NULL, event_json, NULL);
			json_decref(event_json);
		}
	}
	else if(!strcasecmp(msg_type, "peer_join"))
	{
		if(!dude)
		{
			JANUS_LOG(LOG_ERR, "[Stream Lobby] No peer argument, abandoning message_lobby()\n");
			return;
		}
		int j = 0;
		char uid[37] = {0};
		uuid_unparse(dude->uuid, uid);
		json_t* data_json = json_object();
		
		json_object_set_new(data_json, "uuid", json_string(uid));
		pthread_mutex_lock(&dude->mutex);
			json_object_set_new(data_json, "nick", json_string(dude->nick));
		pthread_mutex_unlock(&dude->mutex);
		json_object_set_new(event_json, "stuff", data_json);
		
		peer* participants_list[room->max_clients];
		memset(participants_list, 0, sizeof(peer*) * room->max_clients);
		pthread_mutex_lock(&room->peerlist_mutex);
			for(int i = 0; i < room->max_clients; i++)
			{
				if(room->participants[i] != NULL)
					participants_list[j++] = room->participants[i];
			}
		pthread_mutex_unlock(&room->peerlist_mutex);
		for(int i = 0; i < j; i++)
		{
			peer* p = participants_list[i];
			if(p == dude)
				continue;
			gateway->push_event(p->session, &stream_lobby_plugin, NULL, event_json, NULL);
			json_decref(event_json);
		}
	//} else if(!strcasecmp(msg_type, "video_start")) {
	}
	json_decref(event_json);
}




void stream_lobby_incoming_rtp(janus_plugin_session *handle, int video, char *buf, int len)
{
	if(handle == NULL || handle->stopped || handle->plugin_handle == NULL || !g_atomic_int_get(&initialized) || g_atomic_int_get(&stopping))
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




void stream_lobby_incoming_rtcp(janus_plugin_session *handle, int video, char *buf, int len)
{
	if(handle == NULL || handle->stopped || handle->plugin_handle == NULL || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	//TODO - The plugin I'm basing this one from didn't do anything here. Going to have to look into rtcp
}




//FIXME - Try to make this cleaner and cut down on the mutex locking
/*
 * Thread for decoding audio packets received from a specific peer
 * Also re-arranges out of order packets
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
	pthread_mutex_lock(&mix_threads_mutex);
		mix_threads_count++;
	pthread_mutex_unlock(&mix_threads_mutex);

	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping))
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
				const unsigned char* payload = (const unsigned char *) janus_rtp_payload(packet->data, packet->length, &plen);
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

	pthread_mutex_lock(&mix_threads_mutex);
		mix_threads_count--;
		if(g_atomic_int_get(&stopping) && mix_threads_count == 0)
			pthread_cond_signal(&destroy_threads_cond);
	pthread_mutex_unlock(&mix_threads_mutex);
	return NULL;
}




void* stream_lobby_audio_mix(void* data)
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
		snprintf(in_fname, 261, "/media/Storage/temp/%s_input.ogg", room->name);
		/*input ogg file*/
		room->in_file = fopen(in_fname, "wb");
		if(!room->in_file) return NULL;
		room->in_ss = malloc(sizeof(ogg_stream_state));
		if(ogg_stream_init(room->in_ss, 1) < 0) return NULL;
		ogg_packet* op = op_opushead();
		ogg_stream_packetin(room->in_ss, op);
		op_free(op);
		op = op_opustags();
		ogg_stream_packetin(room->in_ss, op);
		op_free(op);
		ogg_flush(room, 'i');
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
	FILE* wavFile = NULL;
	wavFile = fopen("/media/Storage/temp/stream_lobby_recording.wav", "wb");
	gint64 record_lastupdate = 0;
	if(wavFile != NULL)
	{
		wav_header header;
		//header.riff = {'R', 'I', 'F', 'F'};
		header.riff[0] = 'R';
		header.riff[1] = 'I';
		header.riff[2] = 'F';
		header.riff[3] = 'F';
		header.len = 0;
		//header.wave = {'W', 'A', 'V', 'E'};
		header.wave[0] = 'W';
		header.wave[1] = 'A';
		header.wave[2] = 'V';
		header.wave[3] = 'E';
		//header.fmt = {'f', 'm', 't', ' '};
		header.fmt[0] = 'f';
		header.fmt[1] = 'm';
		header.fmt[2] = 't';
		header.fmt[3] = ' ';
		header.formatsize = 16;
		header.format = 1;
		header.channels = SETTINGS_CHANNELS;
		header.samplerate = 48000;
		//header.avgbyterate = 96000; //mono
		//header.avgbyterate = 19200; //stereo
		//avg byte rate    = sample rate * channels * bytes per sample [opus_int16 = 2 bytes]
		header.avgbyterate = SETTINGS_SAMPLE_RATE * SETTINGS_CHANNELS * 2;
		header.samplebytes = 2;
		header.channelbits = 16;
		//header.data = {'d', 'a', 't', 'a'};
		header.data[0] = 'd';
		header.data[1] = 'a';
		header.data[2] = 't';
		header.data[3] = 'a';
		header.blocksize = 0;

		if(fwrite(&header, 1, sizeof(header), wavFile) != sizeof(header)) {
			JANUS_LOG(LOG_ERR, "[Stream Lobby] Error writing WAV header...\n");
		}
		fflush(wavFile);
		record_lastupdate = janus_get_monotonic_time();
	}

	pthread_mutex_lock(&mix_threads_mutex);
		mix_threads_count++;
	pthread_mutex_unlock(&mix_threads_mutex);
	JANUS_LOG(LOG_INFO, "Audio mixing thread started for lobby \"%s\"\n", room->name);

	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping) && !room->die)
	{
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

		//Write buffer to file
		if(wavFile != NULL)
		{
			for(int i = 0; i < buffer_size; i++)
			{
				output_buffer[i] = mix_buffer[i];
			}
			fwrite(output_buffer, sizeof(opus_int16), buffer_size, wavFile);
			/* Every 5 seconds we update the wav header */
			gint64 now = janus_get_monotonic_time();
			if(now - record_lastupdate >= 5*G_USEC_PER_SEC) {
				record_lastupdate = now;
				/* Update the length in the header */
				fseek(wavFile, 0, SEEK_END);
				long int size = ftell(wavFile);
				if(size >= 8) {
					size -= 8;
					fseek(wavFile, 4, SEEK_SET);
					fwrite(&size, sizeof(uint32_t), 1, wavFile);
					size += 8;
					fseek(wavFile, 40, SEEK_SET);
					fwrite(&size, sizeof(uint32_t), 1, wavFile);
					fflush(wavFile);
					fseek(wavFile, 0, SEEK_END);
				}
			}
		}

		//Update RTP header, set to 0 if an overflow would occur
		payload->seq_number = htons(seq);
		seq = (seq == 65535) ? 0 : seq+1;
		payload->timestamp = htonl(ts);
		ts = (ts > UINT32_MAX-SETTINGS_OPUS_FRAME_SIZE) ? 0 : ts+SETTINGS_OPUS_FRAME_SIZE;

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
				output_packet->length += RTP_HEADER_SIZE;
				output_packet->timestamp = ts;
				output_packet->seq_number = seq;
				payload->type = dude->opus_pt;
				/* Update the timestamp and sequence number in the RTP packet, and send it */
				payload->timestamp = htonl(ts);
				payload->seq_number = htons(seq);

				if(gateway != NULL)
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
					gateway->relay_rtp(dude->session, 0, (char *)payload, output_packet->length);
				}

				/* Restore the timestamp and sequence number to what the publisher set them to */
				payload->timestamp = htonl(output_packet->timestamp);
				payload->seq_number = htons(output_packet->seq_number);
			}
		}
		payload->markerbit = 0;
	}

	//wav file stuff
	if(wavFile != NULL)
	{
		fseek(wavFile, 0, SEEK_END);
		long int size = ftell(wavFile);
		if(size >= 8)
		{
			size -= 8;
			fseek(wavFile, 4, SEEK_SET);
			fwrite(&size, sizeof(uint32_t), 1, wavFile);
			size += 8;
			fseek(wavFile, 40, SEEK_SET);
			fwrite(&size, sizeof(uint32_t), 1, wavFile);
			fflush(wavFile);
			fclose(wavFile);
		}
	}

	//audio recording code block
	//************
	fclose(room->in_file);
	room->in_file = NULL;
	ogg_stream_destroy(room->in_ss);
	//************

	opus_encoder_destroy(room->encoder);
	room->encoder = NULL;
	pthread_mutex_lock(&mix_threads_mutex);
		mix_threads_count--;
		if(g_atomic_int_get(&stopping) && mix_threads_count == 0)
			pthread_cond_signal(&destroy_threads_cond);
	pthread_mutex_unlock(&mix_threads_mutex);
	return NULL;
}




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




//Callback for sorting a peer's audio packets when inserting into list
int audio_packet_sort(const void* a, const void* b)
{
	rtp_wrapper* one = (rtp_wrapper*) a;
	rtp_wrapper* two = (rtp_wrapper*) b;
	int32_t diff = one->seq_number - two->seq_number;
	if(abs(diff) > 10)
		diff = 0-diff;
	return diff;
}




//Plugin Info Getters
int stream_lobby_get_api_compatibility(void){return JANUS_PLUGIN_API_VERSION;}
int stream_lobby_get_version(void){return PLUGIN_VERSION;}
const char *stream_lobby_get_version_string(void){return PLUGIN_VERSION_STRING;}
const char *stream_lobby_get_description(void){return PLUGIN_DESCRIPTION;}
const char *stream_lobby_get_name(void){return PLUGIN_NAME;}
const char *stream_lobby_get_author(void){return PLUGIN_AUTHOR;}
const char *stream_lobby_get_package(void){return PLUGIN_PACKAGE;}

/* OGG/Opus helpers */
/* Write a little-endian 32 bit int to memory */
void le32(unsigned char *p, int v) {
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}
/* Write a little-endian 16 bit int to memory */
void le16(unsigned char *p, int v) {
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
}
/* Manufacture a generic OpusHead packet */
ogg_packet *op_opushead(void) {
	int size = 19;
	unsigned char *data = g_malloc0(size);
	ogg_packet *op = g_malloc0(sizeof(*op));

	if(!data) {
		JANUS_LOG(LOG_ERR, "Couldn't allocate data buffer...\n");
		return NULL;
	}
	if(!op) {
		JANUS_LOG(LOG_ERR, "Couldn't allocate Ogg packet...\n");
		return NULL;
	}

	memcpy(data, "OpusHead", 8);  /* identifier */
	data[8] = 1;                  /* version */
	data[9] = 1;                  /* channels */
	le16(data+10, 0);             /* pre-skip */
	le32(data + 12, 48000);       /* original sample rate */
	le16(data + 16, 0);           /* gain */
	data[18] = 0;                 /* channel mapping family */

	op->packet = data;
	op->bytes = size;
	op->b_o_s = 1;
	op->e_o_s = 0;
	op->granulepos = 0;
	op->packetno = 0;

	return op;
}
/* Manufacture a generic OpusTags packet */
ogg_packet *op_opustags(void) {
	const char *identifier = "OpusTags";
	const char *vendor = "Stream lobby";
	int size = strlen(identifier) + 4 + strlen(vendor) + 4;
	unsigned char *data = g_malloc0(size);
	ogg_packet *op = g_malloc0(sizeof(*op));

	if(!data) {
		JANUS_LOG(LOG_ERR, "Couldn't allocate data buffer...\n");
		return NULL;
	}
	if(!op) {
		JANUS_LOG(LOG_ERR, "Couldn't allocate Ogg packet...\n");
		return NULL;
	}

	memcpy(data, identifier, 8);
	le32(data + 8, strlen(vendor));
	memcpy(data + 12, vendor, strlen(vendor));
	le32(data + 12 + strlen(vendor), 0);

	op->packet = data;
	op->bytes = size;
	op->b_o_s = 0;
	op->e_o_s = 0;
	op->granulepos = 0;
	op->packetno = 1;

	return op;
}
/* Allocate an ogg_packet */
ogg_packet *op_from_pkt(const unsigned char *pkt, int len) {
	ogg_packet *op = g_malloc0(sizeof(*op));
	if(!op) {
		JANUS_LOG(LOG_ERR, "Couldn't allocate Ogg packet.\n");
		return NULL;
	}

	op->packet = (unsigned char *)pkt;
	op->bytes = len;
	op->b_o_s = 0;
	op->e_o_s = 0;

	return op;
}
/* Free a packet and its contents */
void op_free(ogg_packet *op) {
	if(op) {
		if(op->packet) {
			g_free(op->packet);
		}
		g_free(op);
	}
}
/* Write out available ogg pages */
int ogg_write(lobby* room, char thing) {
	ogg_page page;
	size_t written;
	ogg_stream_state* ss = room->in_ss;
	FILE* outfile = room->in_file;

	while (ogg_stream_pageout(ss, &page)) {
		written = fwrite(page.header, 1, page.header_len, outfile);
		if(written != (size_t)page.header_len) {
			JANUS_LOG(LOG_ERR, "Error writing Ogg page header\n");
			return -2;
		}
		written = fwrite(page.body, 1, page.body_len, outfile);
		if(written != (size_t)page.body_len) {
			JANUS_LOG(LOG_ERR, "Error writing Ogg page body\n");
			return -3;
		}
	}
	return 0;
}
/* Flush remaining ogg data */
int ogg_flush(lobby* room, char thing) {
	ogg_page page;
	size_t written;
	ogg_stream_state* ss = room->in_ss;
	FILE* outfile = room->in_file;

	while (ogg_stream_flush(ss, &page)) {
		written = fwrite(page.header, 1, page.header_len, outfile);
		if(written != (size_t)page.header_len) {
			JANUS_LOG(LOG_ERR, "Error writing Ogg page header\n");
			return -2;
		}
		written = fwrite(page.body, 1, page.body_len, outfile);
		if(written != (size_t)page.body_len) {
			JANUS_LOG(LOG_ERR, "Error writing Ogg page body\n");
			return -3;
		}
	}
	return 0;
}


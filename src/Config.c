#include <pthread.h>
#include <opus/opus.h>
#include <uuid/uuid.h>

#include <janus/config.h>
#include "Config.h"
#include "Lobbies.h"
#include "Sessions.h"
#include "StreamLobby.h"
static unsigned int lobby_count;
//Allow peers to store a maximum of 20 frames of audio data (going by server settings)

int config_parse_file(const char* filename)
{
	janus_config* config = janus_config_parse(filename);
	if(config == NULL)
	{
		JANUS_LOG(LOG_INFO, "[Stream Lobby] Could not parse config file\n");
		return 1;
	}

	JANUS_LOG(LOG_INFO, "[Stream Lobby] Loaded configuration file: %s\n", filename);
	janus_config_print(config);
	
	//We have a configuration file, so get the global stuff
	janus_config_container* tmpLimit = janus_config_get(config, NULL, janus_config_type_item, "lobby_limit");
	janus_config_container* tmpAdmin = janus_config_get(config, NULL, janus_config_type_item, "admin_pass");
	
	if(tmpLimit != NULL)
		lobbies_set_limit(strtoul(tmpLimit->value, NULL, 10));
	
	if(tmpAdmin == NULL)
	{
		JANUS_LOG(LOG_INFO, "Administrator password not present. Admin interface disabled!\n");
	}
	else if(tmpAdmin->value[0] == '\0')
	{
		JANUS_LOG(LOG_WARN, "Empty administrator password. Admin interface disabled!\n");
	}
	else
	{
		stream_lobby_enable_admin();
		stream_lobby_set_admin_pass(tmpAdmin->value);
	}


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
			
			if(lobbies_get_lobby(category->name) != NULL)
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
			janus_config_item* tmpDesc = janus_config_get(config, category, janus_config_type_item, "desc");
			janus_config_item* tmpSubj = janus_config_get(config, category, janus_config_type_item, "subject");
			janus_config_item* tmpPriv = janus_config_get(config, category, janus_config_type_item, "private");
			janus_config_item* tmpClients = janus_config_get(config, category, janus_config_type_item, "max_clients");
			janus_config_item* tmpAudio = janus_config_get(config, category, janus_config_type_item, "enable_audio");
			janus_config_item* tmpVideo = janus_config_get(config, category, janus_config_type_item, "video_auth");
			janus_config_item* tmpVideoKey = janus_config_get(config, category, janus_config_type_item, "video_key");
			janus_config_item* tmpVideoPass = janus_config_get(config, category, janus_config_type_item, "video_pass");
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
					opus_encoder_ctl(tmpLobby->encoder, OPUS_SET_VBR(0));
					//bit rate
					opus_encoder_ctl(tmpLobby->encoder, OPUS_SET_BITRATE(SETTINGS_BITRATE));
					//FEC
					opus_encoder_ctl(tmpLobby->encoder, OPUS_SET_INBAND_FEC(0));
					tmpLobby->audio_enabled = 1;
				}
			}
			JANUS_LOG(LOG_VERB, "[Stream Lobby] Audio Enabled: %s\n", tmpLobby->audio_enabled ? "yes" : "no");
			
			if(tmpVideo != NULL && strcmp(tmpVideo->value, "password") == 0 && tmpVideoPass != NULL)
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
			
			int result = addLobby(tmpLobby);
			if(result != 0)
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
				if(result == LOBBY_ERROR_LOBBY_LIMIT_REACHED)
				{
					JANUS_LOG(LOG_INFO, "Maximum number of lobbies reached (%d). Stopping config file processing at \"%s\"\n", lobbies_get_limit(), tmpLobby->name);
					break;
				}
			}
			config_lobby = config_lobby->next;
		}
	}
	g_list_free(config_lobby);
	janus_config_destroy(config);
	return 0;
}


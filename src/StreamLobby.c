#include <stdlib.h>
#include <limits.h>
#include <stdint.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <glib.h>
#include <jansson.h>
#include <sys/time.h>
#include <uuid/uuid.h>

#include <janus/plugins/plugin.h>
#include <janus/apierror.h>
#include <janus/debug.h>
#include <janus/config.h>
#include <janus/rtp.h>
#include <janus/rtcp.h>
#include <janus/utils.h>

#include "StreamLobby.h"
#include "Audio.h"
#include "Recording.h"
#include "Config.h"
#include "Sessions.h"
#include "Messaging.h"


janus_plugin* create(void);
janus_callbacks* janus_gateway = NULL;
static int initialized, stopping, admin_enabled;
static char admin_pass[64];

janus_plugin stream_lobby_plugin = 
	JANUS_PLUGIN_INIT (
		.init = stream_lobby_init,
		.destroy = stream_lobby_shutdown,
		.create_session = sessions_create_session,
		.handle_message = handle_message,
		.setup_media = audio_setup_media,
		.incoming_rtp = audio_incoming_rtp,
		.incoming_rtcp = audio_incoming_rtcp,
		.hangup_media = audio_hangup_media,
		.destroy_session = sessions_destroy_session,
		.query_session = sessions_query_session,
		
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


int stream_lobby_init(janus_callbacks* callback, const char* config_path)
{
	if(callback == NULL || config_path == NULL || stream_lobby_is_initialized() != 0)
		return INIT_ERROR_INVALID_ARGS;
	
	janus_gateway = callback;

	//Create hash tables
	int result;
	result = lobbies_init();
	if(result != 0)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Error %d initializing lobbies", result);
		return INIT_ERROR_LOBBY_CREATION_FAIL;
	}
	result = sessions_init();
	if(result != 0)
	{
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Error %d initializing sessions", result);
		return INIT_ERROR_SESSION_CREATION_FAIL;
	}

	char filename[255];
	snprintf(filename, 255, "%s/%s.cfg", config_path, PLUGIN_PACKAGE);
	result = config_parse_file(filename);
	if(result != 0)
	{
		sessions_shutdown();
		lobbies_shutdown();
		return INIT_ERROR_CONFIG_ERROR;
	}
	
	stream_lobby_set_initialized(1);
	return 0;
}

void stream_lobby_shutdown(void)
{
	if(!stream_lobby_is_initialized())
		return;

	stream_lobby_set_stopping(1);
	
	sessions_shutdown();
	lobbies_shutdown();

	stream_lobby_set_initialized(0);
	stream_lobby_set_stopping(0);
}

void stream_lobby_enable_admin()
{
	g_atomic_int_set(&admin_enabled, 1);
}
void stream_lobby_disable_admin()
{
	g_atomic_int_set(&admin_enabled, 0);
}
int stream_lobby_is_admin_enabled()
{
	return g_atomic_int_get(&admin_enabled);
}
void stream_lobby_set_admin_pass(const char* pass)
{
	snprintf(admin_pass, 64, "%s", pass);
}


int stream_lobby_is_initialized(){return g_atomic_int_get(&initialized);}
int stream_lobby_is_stopping(){return g_atomic_int_get(&stopping);}
void stream_lobby_set_initialized(int i){g_atomic_int_set(&initialized, i);}
void stream_lobby_set_stopping(int i){g_atomic_int_set(&stopping, i);}

//Plugin Info Getters
int stream_lobby_get_api_compatibility(void){return JANUS_PLUGIN_API_VERSION;}
int stream_lobby_get_version(void){return PLUGIN_VERSION;}
const char *stream_lobby_get_version_string(void){return PLUGIN_VERSION_STRING;}
const char *stream_lobby_get_description(void){return PLUGIN_DESCRIPTION;}
const char *stream_lobby_get_name(void){return PLUGIN_NAME;}
const char *stream_lobby_get_author(void){return PLUGIN_AUTHOR;}
const char *stream_lobby_get_package(void){return PLUGIN_PACKAGE;}


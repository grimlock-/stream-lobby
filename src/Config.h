#pragma once
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

int config_parse_file(const char* filename);

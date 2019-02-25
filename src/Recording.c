/* 
 * Functions for writing audio data to a file
 *
 * Most functions were shamelessly lifted from the Janus plugins
 */

#include <string.h>
#include <glib.h>
#include <janus/debug.h>

#include "Recording.h"
#include "Config.h"
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
void ogg_write(lobby* room, char thing) {
	ogg_page page;
	size_t written;
	ogg_stream_state* ss;
	FILE* outfile;
	if(thing == 'i')
	{
		ss = room->in_ss;
		outfile = room->in_file;
	}
	else if(thing == 'o')
	{
		ss = room->out_ss;
		outfile = room->out_file;
	}
	else
		return;

	while (ogg_stream_pageout(ss, &page)) {
		written = fwrite(page.header, 1, page.header_len, outfile);
		if(written != (size_t)page.header_len) {
			JANUS_LOG(LOG_ERR, "Error writing Ogg page header\n");
			return;
		}
		written = fwrite(page.body, 1, page.body_len, outfile);
		if(written != (size_t)page.body_len) {
			JANUS_LOG(LOG_ERR, "Error writing Ogg page body\n");
			return;
		}
	}
}
/* Flush remaining ogg data */
void ogg_flush(lobby* room, char thing) {
	ogg_page page;
	size_t written;
	ogg_stream_state* ss;
	FILE* outfile;
	if(thing == 'i')
	{
		ss = room->in_ss;
		outfile = room->in_file;
	}
	else if(thing == 'o')
	{
		ss = room->out_ss;
		outfile = room->out_file;
	}
	else
		return;

	while (ogg_stream_flush(ss, &page)) {
		written = fwrite(page.header, 1, page.header_len, outfile);
		if(written != (size_t)page.header_len) {
			JANUS_LOG(LOG_ERR, "Error writing Ogg page header\n");
			return;
		}
		written = fwrite(page.body, 1, page.body_len, outfile);
		if(written != (size_t)page.body_len) {
			JANUS_LOG(LOG_ERR, "Error writing Ogg page body\n");
			return;
		}
	}
}


FILE* wav_file_init(const char* filename)
{
	if(strlen(filename) == 0)
		return NULL;
	FILE* wavFile = fopen(filename, "wb");
	if(!wavFile)
		return NULL;

	wav_header header;
	header.riff[0] = 'R';
	header.riff[1] = 'I';
	header.riff[2] = 'F';
	header.riff[3] = 'F';
	header.len = 0;
	header.wave[0] = 'W';
	header.wave[1] = 'A';
	header.wave[2] = 'V';
	header.wave[3] = 'E';
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
	header.data[0] = 'd';
	header.data[1] = 'a';
	header.data[2] = 't';
	header.data[3] = 'a';
	header.blocksize = 0;

	if(fwrite(&header, 1, sizeof(header), wavFile) != sizeof(header)) {
		JANUS_LOG(LOG_ERR, "[Stream Lobby] Error writing WAV header...\n");
	}
	fflush(wavFile);

	return wavFile;
}

void wav_file_write(FILE* wavFile, int32_t* in_buffer, int length)
{
	if(!wavFile)
		return;

	int16_t out_buffer[length];
	for(int i = 0; i < length; i++)
		out_buffer[i] = in_buffer[i];
	fwrite(out_buffer, sizeof(int16_t), length, wavFile);
}

void wav_file_update_header(FILE* wavFile)
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
		fseek(wavFile, 0, SEEK_END);
	}
}

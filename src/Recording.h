#pragma once
#include <stdio.h>
#include <ogg/ogg.h>
#include <opus/opus.h>

#include "Lobbies.h"


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

/* OGG/Opus helpers */
void		le32(unsigned char *p, int v);
void		le16(unsigned char *p, int v);
ogg_packet*	op_opushead(void);
ogg_packet*	op_opustags(void);
ogg_packet*	op_from_pkt(const unsigned char *pkt, int len);
void		op_free(ogg_packet *op);
void		ogg_write(lobby*, char);
void		ogg_flush(lobby*, char);
FILE*		wav_file_init(const char*);
void		wav_file_write(FILE*, opus_int32*, int);
void		wav_file_update_header(FILE*);

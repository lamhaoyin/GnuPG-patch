/* filter.h
 *	Copyright (C) 1998 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */
#ifndef G10_FILTER_H
#define G10_FILTER_H

#include "types.h"
#include "cipher.h"

typedef struct {
    MD_HANDLE md;      /* catch all */
    size_t maxbuf_size;
} md_filter_context_t;

typedef struct {
    /* these fields may be initialized */
    int what;		    /* what kind of armor headers to write */
    int only_keyblocks;     /* skip all headers but ".... key block" */
    const char *hdrlines;   /* write these headerlines */

    /* the following fields must be initialized to zero */
    int inp_checked;	    /* set if the input has been checked */
    int inp_bypass;	    /* set if the input is not armored */
    int in_cleartext;	    /* clear text message */
    int not_dash_escaped;   /* clear text is not dash escaped */
    int hashes; 	    /* detected hash algorithms */
    int faked;		    /* we are faking a literal data packet */
    int truncated;	    /* number of truncated lines */

    byte *buffer;	    /* malloced buffer */
    unsigned buffer_size;   /* and size of this buffer */
    unsigned buffer_len;    /* used length of the buffer */
    unsigned buffer_pos;    /* read position */

    byte radbuf[4];
    int idx, idx2;
    u32 crc;

    int status; 	    /* an internal state flag */
    int any_data;	    /* any valid armored data seen */
    unsigned empty;	    /* empty line counter USED??? */
} armor_filter_context_t;


typedef struct {
    int status;
    void *opaque;   /* (used for z_stream) */
    byte *inbuf;
    unsigned inbufsize;
    byte *outbuf;
    unsigned outbufsize;
    int algo;	 /* compress algo */
} compress_filter_context_t;


typedef struct {
    DEK *dek;
    u32 datalen;
    CIPHER_HANDLE cipher_hd;
    int header;
} cipher_filter_context_t;



typedef struct {
    int eof;
    size_t idx;
    size_t len;
    byte buf[256];
} text_filter_context_t;


/* encrypt_filter_context_t defined in main.h */

/*-- mdfilter.c --*/
int md_filter( void *opaque, int control, IOBUF a, byte *buf, size_t *ret_len);
void free_md_filter_context( md_filter_context_t *mfx );

/*-- armor.c --*/
int use_armor_filter( IOBUF a );
int armor_filter( void *opaque, int control,
		  IOBUF chain, byte *buf, size_t *ret_len);

/*-- compress.c --*/
int compress_filter( void *opaque, int control,
		     IOBUF chain, byte *buf, size_t *ret_len);

/*-- cipher.c --*/
int cipher_filter( void *opaque, int control,
		   IOBUF chain, byte *buf, size_t *ret_len);

/*-- textfilter.c --*/
int text_filter( void *opaque, int control,
		 IOBUF chain, byte *buf, size_t *ret_len);



#endif /*G10_FILTER_H*/

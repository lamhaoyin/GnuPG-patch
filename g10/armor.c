/* armor.c - Armor flter
 *	Copyright (C) 1998,1999 Free Software Foundation, Inc.
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

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "errors.h"
#include "iobuf.h"
#include "memory.h"
#include "util.h"
#include "filter.h"
#include "packet.h"
#include "options.h"
#include "main.h"
#include "status.h"
#include "i18n.h"


#define MAX_LINELEN 20000

#define CRCINIT 0xB704CE
#define CRCPOLY 0X864CFB
#define CRCUPDATE(a,c) do {						    \
			a = ((a) << 8) ^ crc_table[((a)&0xff >> 16) ^ (c)]; \
			a &= 0x00ffffff;				    \
		    } while(0)
static u32 crc_table[256];
static byte bintoasc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			 "abcdefghijklmnopqrstuvwxyz"
			 "0123456789+/";
static byte asctobin[256]; /* runtime initialized */
static int is_initialized;


typedef enum {
    fhdrHASArmor = 0,
    fhdrNOArmor,
    fhdrINIT,
    fhdrINITCont,
    fhdrINITSkip,
    fhdrCHECKBegin,
    fhdrWAITHeader,
    fhdrWAITClearsig,
    fhdrSKIPHeader,
    fhdrCLEARSIG,
    fhdrREADClearsig,
    fhdrNullClearsig,
    fhdrEMPTYClearsig,
    fhdrCHECKClearsig,
    fhdrCHECKClearsig2,
    fhdrCHECKDashEscaped,
    fhdrCHECKDashEscaped2,
    fhdrCHECKDashEscaped3,
    fhdrREADClearsigNext,
    fhdrENDClearsig,
    fhdrENDClearsigHelp,
    fhdrTESTSpaces,
    fhdrCLEARSIGSimple,
    fhdrCLEARSIGSimpleNext,
    fhdrTEXT,
    fhdrTEXTSimple,
    fhdrERROR,
    fhdrERRORShow,
    fhdrEOF
} fhdr_state_t;


/* if we encounter this armor string with this index, go
 * into a mode which fakes packets and wait for the next armor */
#define BEGIN_SIGNATURE 2
#define BEGIN_SIGNED_MSG_IDX 3
static char *head_strings[] = {
    "BEGIN PGP MESSAGE",
    "BEGIN PGP PUBLIC KEY BLOCK",
    "BEGIN PGP SIGNATURE",
    "BEGIN PGP SIGNED MESSAGE",
    "BEGIN PGP ARMORED FILE",       /* gnupg extension */
    "BEGIN PGP PRIVATE KEY BLOCK",
    "BEGIN PGP SECRET KEY BLOCK",   /* only used by pgp2 */
    NULL
};
static char *tail_strings[] = {
    "END PGP MESSAGE",
    "END PGP PUBLIC KEY BLOCK",
    "END PGP SIGNATURE",
    "END dummy",
    "END PGP ARMORED FILE",
    "END PGP PRIVATE KEY BLOCK",
    "END PGP SECRET KEY BLOCK",
    NULL
};



static void
initialize(void)
{
    int i, j;
    u32 t;
    byte *s;

    /* init the crc lookup table */
    crc_table[0] = 0;
    for(i=j=0; j < 128; j++ ) {
	t = crc_table[j];
	if( t & 0x00800000 ) {
	    t <<= 1;
	    crc_table[i++] = t ^ CRCPOLY;
	    crc_table[i++] = t;
	}
	else {
	    t <<= 1;
	    crc_table[i++] = t;
	    crc_table[i++] = t ^ CRCPOLY;
	}
    }
    /* build the helptable for radix64 to bin conversion */
    for(i=0; i < 256; i++ )
	asctobin[i] = 255; /* used to detect invalid characters */
    for(s=bintoasc,i=0; *s; s++,i++ )
	asctobin[*s] = i;

    is_initialized=1;
}

/****************
 * Check whether this is an armored file or not
 * See also parse-packet.c for details on this code
 * Returns: True if it seems to be armored
 */
static int
is_armored( const byte *buf )
{
    int ctb, pkttype;

    ctb = *buf;
    if( !(ctb & 0x80) )
	return 1; /* invalid packet: assume it is armored */
    pkttype =  ctb & 0x40 ? (ctb & 0x3f) : ((ctb>>2)&0xf);
    switch( pkttype ) {
      case PKT_MARKER:
      case PKT_SYMKEY_ENC:
      case PKT_PUBLIC_KEY:
      case PKT_SECRET_KEY:
      case PKT_PUBKEY_ENC:
      case PKT_SIGNATURE:
      case PKT_COMMENT:
      case PKT_OLD_COMMENT:
      case PKT_PLAINTEXT:
      case PKT_COMPRESSED:
      case PKT_ENCRYPTED:
	return 0; /* seems to be a regular packet: not armored */
    }

    return 1;
}


/****************
 * Try to check whether the iobuf is armored
 * Returns true if this may be the case; the caller should use the
 *	   filter to do further processing.
 */
int
use_armor_filter( IOBUF a )
{
    byte buf[1];
    int n;

    n = iobuf_peek(a, buf, 1 );
    if( n == -1 )
	return 0; /* EOF, doesn't matter whether armored or not */
    if( !n )
	return 1; /* can't check it: try armored */
    return is_armored(buf);
}




static void
invalid_armor(void)
{
    write_status(STATUS_BADARMOR);
    g10_exit(1); /* stop here */
}


/****************
 * check whether the armor header is valid on a signed message.
 * this is for security reasons: the header lines are not included in the
 * hash and by using some creative formatting rules, Mallory could fake
 * any text at the beginning of a document; assuming it is read with
 * a simple viewer. We only allow the Hash Header.
 */
static int
parse_hash_header( const char *line )
{
    const char *s, *s2;
    unsigned found = 0;

    if( strlen(line) < 6  || strlen(line) > 60 )
	return 0; /* too short or too long */
    if( memcmp( line, "Hash:", 5 ) )
	return 0; /* invalid header */
    s = line+5;
    for(s=line+5;;s=s2) {
	for(; *s && (*s==' ' || *s == '\t'); s++ )
	    ;
	if( !*s )
	    break;
	for(s2=s+1; *s2 && *s2!=' ' && *s2 != '\t' && *s2 != ','; s2++ )
	    ;
	if( !strncmp( s, "RIPEMD160", s2-s ) )
	    found |= 1;
	else if( !strncmp( s, "SHA1", s2-s ) )
	    found |= 2;
	else if( !strncmp( s, "MD5", s2-s ) )
	    found |= 4;
	else if( !strncmp( s, "TIGER", s2-s ) )
	    found |= 8;
	else
	    return 0;
	for(; *s2 && (*s2==' ' || *s2 == '\t'); s2++ )
	    ;
	if( *s2 && *s2 != ',' )
	    return 0;
	if( *s2 )
	    s2++;
    }
    return found;
}



#if 0 /* old code */
/****************
 * parse an ascii armor.
 * Returns: the state,
 *	    the remaining bytes in BUF are returned in RBUFLEN.
 *	    r_empty return the # of empty lines before the buffer
 */
static fhdr_state_t
find_header( fhdr_state_t state, byte *buf, size_t *r_buflen,
	     IOBUF a, size_t n, unsigned *r_empty, int *r_hashes,
	     int only_keyblocks, int *not_dashed )
{
    int c=0, i;
    const char *s;
    byte *p;
    size_t buflen;
    int cont;
    int clearsig=0;
    int hdr_line=0;
    unsigned empty = 0;

    buflen = *r_buflen;
    assert(buflen >= 100 );
    buflen -= 4; /* reserved room for CR,LF, and two extra */
    do {
	switch( state ) {
	  case fhdrHASArmor:
	    /* read at least the first byte to check whether it is armored
	     * or not */
	    c = 0;
	    for(n=0; n < 28 && (c=iobuf_get(a)) != -1 && c != '\n'; )
		buf[n++] = c;
	    if( !n && c == '\n' )
		state = fhdrCHECKBegin;
	    else if( !n  || c == -1 )
		state = fhdrNOArmor; /* too short */
	    else if( !is_armored( buf ) ) {
		state = fhdrNOArmor;
		if( c == '\n' )
		    buf[n++] = c;
	    }
	    else if( c == '\n' )
		state = fhdrCHECKBegin;
	    else
		state = fhdrINITCont;
	    break;

	  case fhdrINIT: /* read some stuff into buffer */
	    n = 0;
	  case fhdrINITCont: /* read more stuff into buffer */
	    c = 0;
	    for(; n < buflen && (c=iobuf_get(a)) != -1 && c != '\n'; )
		buf[n++] = c;
	    state = c == '\n' ? fhdrCHECKBegin :
		     c == -1  ? fhdrEOF : fhdrINITSkip;
	    break;

	  case fhdrINITSkip:
	    if( c == '\n' )
		n = 0;
	    else {
		while( (c=iobuf_get(a)) != -1 && c != '\n' )
		    ;
	    }
	    state =  c == -1? fhdrEOF : fhdrINIT;
	    break;

	  case fhdrSKIPHeader:
	    while( (c=iobuf_get(a)) != -1 && c != '\n' )
		;
	    state =  c == -1? fhdrEOF : fhdrWAITHeader;
	    break;

	  case fhdrWAITHeader: /* wait for Header lines */
	    c = 0;
	    for(n=0; n < buflen && (c=iobuf_get(a)) != -1 && c != '\n'; )
		buf[n++] = c;
	    buf[n] = 0;
	    if( n < buflen || c == '\n' ) {
		if( n && buf[0] != '\r') { /* maybe a header */
		    if( strchr( buf, ':') ) { /* yes */
			int hashes=0;
			if( buf[n-1] == '\r' )
			    buf[--n] = 0;
			if( opt.verbose ) {
			    log_info(_("armor header: "));
			    print_string( stderr, buf, n, 0 );
			    putc('\n', stderr);
			}
			if( clearsig && !(hashes=parse_hash_header( buf )) ) {
			    if( strlen(buf) > 15
				&& !memcmp( buf, "NotDashEscaped:", 15 ) ) {
				*not_dashed = 1;
				state = fhdrWAITHeader;
			    }
			    else {
				log_error(_("invalid clearsig header\n"));
				state = fhdrERROR;
			    }
			}
			else {
			    state = fhdrWAITHeader;
			    if( r_hashes )
				*r_hashes |= hashes;
			}
		    }
		    else if( clearsig && n > 15 && !memcmp(buf, "-----", 5 ) )
			state = fhdrNullClearsig;
		    else
			state = fhdrCHECKDashEscaped3;
		}
		else if( !n || (buf[0] == '\r' && !buf[1]) ) { /* empty line */
		    if( clearsig )
			state = fhdrWAITClearsig;
		    else {
			/* this is not really correct: if we do not have
			 * a clearsig and no armor lines we are not allowed
			 * to have an empty line */
			n = 0;
			state = fhdrTEXT;
		    }
		}
		else {
		    log_error(_("invalid armor header: "));
		    print_string( stderr, buf, n, 0 );
		    putc('\n', stderr);
		    state = fhdrERROR;
		}
	    }
	    else if( c != -1 ) {
		if( strchr( buf, ':') ) { /* buffer to short, but this is okay*/
		    if( opt.verbose ) {
			log_info(_("armor header: "));
			print_string( stderr, buf, n, 0 );
			fputs("[...]\n", stderr);  /* indicate it is truncated */
		    }
		    state = fhdrSKIPHeader;  /* skip rest of line */
		}
		else /* line too long */
		    state = fhdrERROR;
	    }
	    else
		state = fhdrEOF;
	    break;

	  case fhdrWAITClearsig: /* skip the empty line (for clearsig) */
	    c = 0;
	    for(n=0; n < buflen && (c=iobuf_get(a)) != -1 && c != '\n'; )
		buf[n++] = c;
	    if( c != -1 ) {
		if( n > 15 && !memcmp(buf, "-----", 5 ) )
		    state = fhdrNullClearsig;
		else if( c != '\n' )
		    state = fhdrREADClearsigNext;
		else
		    state = fhdrCHECKDashEscaped3;
	    }
	    else {
		/* fixme: we should check whether this line continues
		 *   it is possible that we have only read ws until here
		 *   and more stuff is to come */
		state = fhdrEOF;
	    }
	    break;

	  case fhdrNullClearsig: /* zero length cleartext */
	    state = fhdrENDClearsig;
	    break;

	  case fhdrENDClearsig:
	  case fhdrCHECKBegin:
	    state = state == fhdrCHECKBegin ? fhdrINITSkip : fhdrERRORShow;
	    if( n < 15 )
		break;	/* too short */
	    if( memcmp( buf, "-----", 5 ) )
		break;
	    buf[n] = 0;
	    p = strstr(buf+5, "-----");
	    if( !p )
		break;
	    *p = 0;
	    p += 5;
	    if( *p == '\r' )
		p++;
	    if( *p )
		break; /* garbage after dashes */
	    p = buf+5;
	    for(i=0; (s=head_strings[i]); i++ )
		if( !strcmp(s, p) )
		    break;
	    if( !s )
		break; /* unknown begin line */
	    if( only_keyblocks && i != 1 && i != 5 && i != 6 )
		break; /* not a keyblock armor */

	    /* found the begin line */
	    hdr_line = i;
	    state = fhdrWAITHeader;
	    *not_dashed = 0;
	    if( hdr_line == BEGIN_SIGNED_MSG_IDX )
		clearsig = 1;
	    if( opt.verbose > 1 )
		log_info(_("armor: %s\n"), head_strings[hdr_line]);
	    break;

	  case fhdrCLEARSIGSimple:
	    /* we are at the begin of a new line */
	  case fhdrCLEARSIGSimpleNext:
	    n = 0;
	    c = 0;
	    while( n < buflen && (c=iobuf_get(a)) != -1 ) {
		buf[n++] = c;
		if( c == '\n' )
		    break;
	    }
	    buf[n] = 0;
	    if( c == -1 )
		state = fhdrEOF;
	    else if( state == fhdrCLEARSIGSimple
		     && n > 15 && !memcmp(buf, "-----", 5 ) ) {
		if( c == '\n' )
		    buf[n-1] = 0;
		state = fhdrENDClearsig;
	    }
	    else if( c == '\n' )
		state = fhdrCLEARSIGSimple;
	    else
		state = fhdrCLEARSIGSimpleNext;
	    break;

	  case fhdrCLEARSIG:
	  case fhdrEMPTYClearsig:
	  case fhdrREADClearsig:
	    /* we are at the start of a line: read a clearsig into the buffer
	     * we have to look for a header line or dashed escaped text*/
	    n = 0;
	    c = 0;
	    while( n < buflen && (c=iobuf_get(a)) != -1 && c != '\n' )
		buf[n++] = c;
	    buf[n] = 0;
	    if( c == -1 )
		state = fhdrEOF;
	    else if( !n || ( buf[0]=='\r' && !buf[1] ) ) {
		state = fhdrEMPTYClearsig;
		empty++;
	    }
	    else if( c == '\n' )
		state = fhdrCHECKClearsig2;
	    else
		state = fhdrCHECKClearsig;
	    break;

	  case fhdrCHECKDashEscaped3:
	    if( *not_dashed ) {
		state = fhdrTEXTSimple;
		break;
	    }
	    if( !(n > 1 && buf[0] == '-' && buf[1] == ' ' ) ) {
		state = fhdrTEXT;
		break;
	    }
	    /* fall through */
	  case fhdrCHECKDashEscaped2:
	  case fhdrCHECKDashEscaped:
	    /* check dash escaped line */
	    if( buf[2] == '-' || ( n > 6 && !memcmp(buf+2, "From ", 5))) {
		for(i=2; i < n; i++ )
		    buf[i-2] = buf[i];
		n -= 2;
		buf[n] = 0; /* not really needed */
		state = state == fhdrCHECKDashEscaped3 ? fhdrTEXT :
			state == fhdrCHECKDashEscaped2 ?
				 fhdrREADClearsig : fhdrTESTSpaces;
	    }
	    else {
		log_error(_("invalid dash escaped line: "));
		print_string( stderr, buf, n, 0 );
		putc('\n', stderr);
		state = fhdrERROR;
	    }
	    break;

	  case fhdrCHECKClearsig:
	    /* check the clearsig line */
	    if( n > 15 && !memcmp(buf, "-----", 5 ) )
		state = fhdrENDClearsig;
	    else if( buf[0] == '-' && buf[1] == ' ' && !*not_dashed )
		state = fhdrCHECKDashEscaped;
	    else {
		state = fhdrTESTSpaces;
	    }
	    break;

	  case fhdrCHECKClearsig2:
	    /* check the clearsig line */
	    if( n > 15 && !memcmp(buf, "-----", 5 ) )
		state = fhdrENDClearsig;
	    else if( buf[0] == '-' && buf[1] == ' ' && !*not_dashed )
		state = fhdrCHECKDashEscaped2;
	    else {
		state = fhdrREADClearsig;
	    }
	    break;

	  case fhdrREADClearsigNext:
	    /* Read to the end of the line, do not care about checking
	     * for dashed escaped text of headers */
	    c = 0;
	    n = 0;
	    while( n < buflen && (c=iobuf_get(a)) != -1 && c != '\n' )
		buf[n++] = c;
	    buf[n] = 0;
	    if( c == -1 )
		state = fhdrEOF;
	    else if( c == '\n' )
		state = fhdrREADClearsig;
	    else
		state = fhdrTESTSpaces;
	    break;

	  case fhdrTESTSpaces: {
	    /* but must check whether the rest of the line
	     * only contains white spaces; this is problematic
	     * since we may have to restore the stuff.	simply
	     * counting spaces is not enough, because it may be a
	     * mix of different white space characters */
	    IOBUF b = iobuf_temp();
	    while( (c=iobuf_get(a)) != -1 && c != '\n' ) {
		iobuf_put(b,c);
		if( c != ' ' && c != '\t' && c != '\r' )
		    break;
	    }
	    if( c == '\n' ) {
		/* okay we can skip the rest of the line */
		iobuf_close(b);
		state = fhdrREADClearsig;
	    }
	    else {
		iobuf_unget_and_close_temp(a,b);
		state = fhdrREADClearsigNext;
	    }
	  } break;

	  case fhdrERRORShow:
	    log_error(_("invalid clear text header: "));
	    print_string( stderr, buf, n, 0 );
	    putc('\n', stderr);
	    state = fhdrERROR;
	    break;

	  default: BUG();
	}
	switch( state ) {
	  case fhdrINIT:
	  case fhdrINITCont:
	  case fhdrINITSkip:
	  case fhdrCHECKBegin:
	  case fhdrWAITHeader:
	  case fhdrWAITClearsig:
	  case fhdrSKIPHeader:
	  case fhdrEMPTYClearsig:
	  case fhdrCHECKClearsig:
	  case fhdrCHECKClearsig2:
	  case fhdrCHECKDashEscaped:
	  case fhdrCHECKDashEscaped2:
	  case fhdrCHECKDashEscaped3:
	  case fhdrTESTSpaces:
	  case fhdrERRORShow:
	    cont = 1;
	    break;
	  default: cont = 0;
	}
    } while( cont );

    if( clearsig && state == fhdrTEXT ) {
	state = fhdrCLEARSIG;
    }
    else if( clearsig && state == fhdrTEXTSimple ) {
	state = fhdrCLEARSIGSimple;
	buf[n] = '\n';
	n++;
    }

    if( state == fhdrCLEARSIG || state == fhdrREADClearsig ) {
	/* append CR,LF after removing trailing wspaces */
	for(p=buf+n-1; n; n--, p-- ) {
	    assert( *p != '\n' );
	    if( *p != ' ' && *p != '\t' && *p != '\r' ) {
		p[1] = '\r';
		p[2] = '\n';
		n += 2;
		break;
	    }
	}
	if( !n ) {
	    buf[0] = '\r';
	    buf[1] = '\n';
	    n = 2;
	}
    }

  fprintf(stderr,"ARMOR READ (state=%d): %.*s", state, n, buf );

    *r_buflen = n;
    *r_empty = empty;
    return state;
}
#endif


static unsigned
trim_trailing_spaces( byte *line, unsigned len )
{
    byte *p, *mark;
    unsigned n;

    for(mark=NULL, p=line, n=0; n < len; n++, p++ ) {
	if( strchr(" \t\r\n", *p ) ) {
	    if( !mark )
		mark = p;
	}
	else
	    mark = NULL;
    }

    if( mark ) {
	*mark = 0;
	return mark - line;
    }
    return len;
}



/****************
 * Check whether this is a armor line.
 * returns: -1 if it is not a armor header or the index number of the
 * armor header.
 */
static int
is_armor_header( byte *line, unsigned len )
{
    const char *s;
    byte *save_p, *p;
    int save_c;
    int i;

    if( len < 15 )
	return -1; /* too short */
    if( memcmp( line, "-----", 5 ) )
	return -1; /* no */
    p = strstr( line+5, "-----");
    if( !p )
	return -1;
    save_p = p;
    p += 5;
    if( *p == '\r' )
	p++;
    if( *p == '\n' )
	p++;
    if( *p )
	return -1; /* garbage after dashes */
    save_c = *save_p; *save_p = 0;
    p = line+5;
    for(i=0; (s=head_strings[i]); i++ )
	if( !strcmp(s, p) )
	    break;
    *save_p = save_c;
    if( !s )
	return -1; /* unknown armor line */

    if( opt.verbose > 1 )
	log_info(_("armor: %s\n"), head_strings[i]);
    return i;
}



/****************
 * Parse a header lines
 * Return 0: Empty line (end of header lines)
 *	 -1: invalid header line
 *	 >0: Good header line
 */
static int
parse_header_line( armor_filter_context_t *afx, byte *line, unsigned len )
{
    byte *p;
    int hashes=0;

    if( *line == '\n' || ( len && (*line == '\r' && line[1]=='\n') ) )
	return 0; /* empty line */
    len = trim_trailing_spaces( line, len );
    p = strchr( line, ':');
    if( !p || !p[1] ) {
	log_error(_("invalid armor header: "));
	print_string( stderr, line, len, 0 );
	putc('\n', stderr);
	return -1;
    }

    if( opt.verbose ) {
	log_info(_("armor header: "));
	print_string( stderr, line, len, 0 );
	putc('\n', stderr);
    }

    if( afx->in_cleartext ) {
	if( (hashes=parse_hash_header( line )) )
	    afx->hashes |= hashes;
	else if( strlen(line) > 15 && !memcmp( line, "NotDashEscaped:", 15 ) )
	    afx->not_dash_escaped = 1;
	else {
	    log_error(_("invalid clearsig header\n"));
	    return -1;
	}
    }
    return 1;
}



/* figure out whether the data is armored or not */
static int
check_input( armor_filter_context_t *afx, IOBUF a )
{
    int rc = 0;
    int i;
    size_t n;
    unsigned emplines;
    byte *line;
    unsigned len;
    unsigned maxlen;
    int hdr_line = -1;

    /* read the first line to see whether this is armored data */
    maxlen = MAX_LINELEN;
    len = afx->buffer_len = iobuf_read_line( a, &afx->buffer,
					     &afx->buffer_size, &maxlen );
    line = afx->buffer;
    if( !maxlen ) {
	/* line has been truncated: assume not armored */
	afx->inp_checked = 1;
	afx->inp_bypass = 1;
	return 0;
    }

    if( !len ) {
	return -1; /* eof */
    }

    /* (the line is always a C string but maybe longer) */
    if( *line == '\n' || ( len && (*line == '\r' && line[1]=='\n') ) )
	;
    else if( !is_armored( line ) ) {
	afx->inp_checked = 1;
	afx->inp_bypass = 1;
	return 0;
    }

    /* find the armor header */
    while(len) {
	i = is_armor_header( line, len );
	if( i >= 0 && !(afx->only_keyblocks && i != 1 && i != 5 && i != 6 )) {
	    hdr_line = i;
	    if( hdr_line == BEGIN_SIGNED_MSG_IDX ) {
		if( afx->in_cleartext ) {
		    log_error(_("nested clear text signatures\n"));
		    rc = G10ERR_INVALID_ARMOR;
		}
		afx->in_cleartext = 1;
	    }
	    break;
	}
	/* read the next line (skip all truncated lines) */
	do {
	    maxlen = MAX_LINELEN;
	    afx->buffer_len = iobuf_read_line( a, &afx->buffer,
					       &afx->buffer_size, &maxlen );
	    line = afx->buffer;
	    len = afx->buffer_len;
	} while( !maxlen );
    }

    /* parse the header lines */
    while(len) {
	/* read the next line (skip all truncated lines) */
	do {
	    maxlen = MAX_LINELEN;
	    afx->buffer_len = iobuf_read_line( a, &afx->buffer,
					       &afx->buffer_size, &maxlen );
	    line = afx->buffer;
	    len = afx->buffer_len;
	} while( !maxlen );

	i = parse_header_line( afx, line, len );
	if( i <= 0 ) {
	    if( i )
		rc = G10ERR_INVALID_ARMOR;
	    break;
	}
    }


    if( rc )
	invalid_armor();
    else if( afx->in_cleartext ) {
	afx->faked = 1;
    }
    else {
	afx->inp_checked = 1;
	afx->crc = CRCINIT;
	afx->idx = 0;
	afx->radbuf[0] = 0;
    }

    return rc;
}



/****************
 * Fake a literal data packet and wait for the next armor line
 * fixme: empty line handling and null length clear text signature are
 *	  not implemented/checked.
 */
static int
fake_packet( armor_filter_context_t *afx, IOBUF a,
	     size_t *retn, byte *buf, size_t size  )
{
    int rc = 0;
    size_t len = 0;
    unsigned emplines = afx->empty;
    int lastline = 0;
    unsigned maxlen, n;
    byte *p;

    len = 2;	/* reserve 2 bytes for the length header */
    size -= 3;	/* and 1 for empline handling and 2 for the term header */
		/* or the appended CR,LF */
    while( !rc && len < size ) {
	if( emplines ) {
	    while( emplines && len < size ) {
		buf[len++] = '\r';
		buf[len++] = '\n';
		emplines--;
	    }
	    continue;
	}

	if( afx->faked == 1 )
	    afx->faked++;  /* skip the first (empty) line */
	else {
	    while( len < size && afx->buffer_pos < afx->buffer_len )
		buf[len++] = afx->buffer[afx->buffer_pos++];
	    buf[len++] = '\r';
	    buf[len++] = '\n';
	    if( len >= size )
		continue;
	}

	/* read the next line */
	maxlen = MAX_LINELEN;
	afx->buffer_pos = 0;
	afx->buffer_len = iobuf_read_line( a, &afx->buffer,
					   &afx->buffer_size, &maxlen );
	if( !afx->buffer_len ) {
	    rc = -1; /* eof */
	    continue;
	}
	if( !maxlen )
	    afx->truncated++;
	afx->buffer_len = trim_trailing_spaces( afx->buffer, afx->buffer_len );
	p = afx->buffer;
	n = afx->buffer_len;

	if( n > 2 && *p == '-' ) {
	    /* check for dash escaped or armor header */
	    if( p[1] == ' ' && !afx->not_dash_escaped ) {
		/* issue a warning if it is not regular encoded */
		if( p[2] != '-' && !( n > 6 && !memcmp(p+2, "From ", 5))) {
		    log_info(_("invalid dash escaped line: "));
		    print_string( stderr, p, n, 0 );
		    putc('\n', stderr);
		}
		afx->buffer_pos = 2; /* skip */
	    }
	    else if( n >= 15 &&  p[1] == '-' && p[2] == '-' && p[3] == '-' ) {
		if( is_armor_header( p, n ) != BEGIN_SIGNATURE ) {
		    log_info(_("unexpected armor:"));
		    print_string( stderr, p, n, 0 );
		    putc('\n', stderr);
		}
		lastline = 1;
		assert( len >= 4 );
		len -= 2; /* remove the last CR,LF */
		rc = -1;
	    }
	}
    }

    buf[0] = (len-2) >> 8;
    buf[1] = (len-2);
    if( lastline ) { /* write last (ending) length header */
	if( buf[0] && buf[1] ) { /* only if we have some text */
	    buf[len++] = 0;
	    buf[len++] = 0;
	}
	rc = 0;
	afx->faked = 0;
	afx->in_cleartext = 0;
	/* and now read the header lines */
	afx->buffer_pos = 0;
	for(;;) {
	    int i;

	    /* read the next line (skip all truncated lines) */
	    do {
		maxlen = MAX_LINELEN;
		afx->buffer_len = iobuf_read_line( a, &afx->buffer,
						 &afx->buffer_size, &maxlen );
	    } while( !maxlen );
	    p = afx->buffer;
	    n = afx->buffer_len;
	    if( !n ) {
		rc = -1;
		break; /* eof */
	    }
	    i = parse_header_line( afx, p , n );
	    if( i <= 0 ) {
		if( i )
		    invalid_armor();
		break;
	    }
	}
	afx->inp_checked = 1;
	afx->crc = CRCINIT;
	afx->idx = 0;
	afx->radbuf[0] = 0;
    }

    afx->empty = emplines;
    *retn = len;
    return rc;
}



static int
radix64_read( armor_filter_context_t *afx, IOBUF a, size_t *retn,
	      byte *buf, size_t size )
{
    byte val;
    int c, c2;
    int checkcrc=0;
    int rc = 0;
    size_t n = 0;
    int  idx, i;
    u32 crc;

    crc = afx->crc;
    idx = afx->idx;
    val = afx->radbuf[0];
    for( n=0; n < size; ) {
	if( (c=iobuf_get(a)) == -1 )
	    break;
	if( c == '\n' || c == ' ' || c == '\r' || c == '\t' )
	    continue;
	else if( c == '=' ) { /* pad character: stop */
	    if( idx == 1 )
		buf[n++] = val;
	    checkcrc++;
	    break;
	}
	else if( (c = asctobin[(c2=c)]) == 255 ) {
	    log_error(_("invalid radix64 character %02x skipped\n"), c2);
	    continue;
	}
	switch(idx) {
	  case 0: val =  c << 2; break;
	  case 1: val |= (c>>4)&3; buf[n++]=val;val=(c<<4)&0xf0;break;
	  case 2: val |= (c>>2)&15; buf[n++]=val;val=(c<<6)&0xc0;break;
	  case 3: val |= c&0x3f; buf[n++] = val; break;
	}
	idx = (idx+1) % 4;
    }
    for(i=0; i < n; i++ )
	crc = (crc << 8) ^ crc_table[((crc >> 16)&0xff) ^ buf[i]];
    crc &= 0x00ffffff;
    afx->crc = crc;
    afx->idx = idx;
    afx->radbuf[0] = val;
    if( checkcrc ) {
	afx->any_data = 1;
	afx->inp_checked=0;
	afx->faked = 0;
	for(;;) { /* skip lf and pad characters */
	    if( (c=iobuf_get(a)) == -1 )
		break;
	    if( c == '\n' || c == ' ' || c == '\r'
		|| c == '\t' || c == '=' )
		continue;
	    break;
	}
	if( c == -1 )
	    log_error(_("premature eof (no CRC)\n"));
	else {
	    u32 mycrc = 0;
	    idx = 0;
	    do {
		if( (c = asctobin[c]) == 255 )
		    break;
		switch(idx) {
		  case 0: val =  c << 2; break;
		  case 1: val |= (c>>4)&3; mycrc |= val << 16;val=(c<<4)&0xf0;break;
		  case 2: val |= (c>>2)&15; mycrc |= val << 8;val=(c<<6)&0xc0;break;
		  case 3: val |= c&0x3f; mycrc |= val; break;
		}
		if( (c=iobuf_get(a)) == -1 )
		    break;
	    } while( ++idx < 4 );
	    if( c == -1 ) {
		log_error(_("premature eof (in CRC)\n"));
		rc = G10ERR_INVALID_ARMOR;
	    }
	    else if( idx != 4 ) {
		log_error(_("malformed CRC\n"));
		rc = G10ERR_INVALID_ARMOR;
	    }
	    else if( mycrc != afx->crc ) {
		log_error(_("CRC error; %06lx - %06lx\n"),
				    (ulong)afx->crc, (ulong)mycrc);
		rc = G10ERR_INVALID_ARMOR;
	    }
	    else {
		rc = 0;
	      #if 0
		for(rc=0;!rc;) {
		    rc = 0 /*check_trailer( &fhdr, c )*/;
		    if( !rc ) {
			if( (c=iobuf_get(a)) == -1 )
			    rc = 2;
		    }
		}
		if( rc == -1 )
		    rc = 0;
		else if( rc == 2 ) {
		    log_error(_("premature eof (in Trailer)\n"));
		    rc = G10ERR_INVALID_ARMOR;
		}
		else {
		    log_error(_("error in trailer line\n"));
		    rc = G10ERR_INVALID_ARMOR;
		}
	      #endif
	    }
	}
    }

    if( !n )
	rc = -1;

    *retn = n;
    return rc;
}

/****************
 * This filter is used to handle the armor stuff
 */
int
armor_filter( void *opaque, int control,
	     IOBUF a, byte *buf, size_t *ret_len)
{
    size_t size = *ret_len;
    armor_filter_context_t *afx = opaque;
    int rc=0, i, c;
    byte radbuf[3];
    int  idx, idx2;
    size_t n=0;
    u32 crc;
  #if 1
    static FILE *fp ;

    if( !fp ) {
	fp = fopen("armor.out", "w");
	assert(fp);
    }
  #endif

    if( DBG_FILTER )
	log_debug("armor-filter: control: %d\n", control );
    if( control == IOBUFCTRL_UNDERFLOW && afx->inp_bypass ) {
	n = 0;
	if( afx->buffer_len ) {
	    for(; n < size && afx->buffer_pos < afx->buffer_len; n++ )
		buf[n++] = afx->buffer[afx->buffer_pos++];
	    if( afx->buffer_pos >= afx->buffer_len )
		afx->buffer_len = 0;
	}
	for(; n < size; n++ ) {
	    if( (c=iobuf_get(a)) == -1 )
		break;
	    buf[n] = c & 0xff;
	}
	if( !n )
	    rc = -1;
	*ret_len = n;
    }
    else if( control == IOBUFCTRL_UNDERFLOW ) {
	if( size < 15+(4*15) )	/* need space for up to 4 onepass_sigs */
	    BUG(); /* supplied buffer too short */

	if( afx->faked )
	    rc = fake_packet( afx, a, &n, buf, size );
	else if( !afx->inp_checked ) {
	    rc = check_input( afx, a );
	    if( afx->inp_bypass ) {
		for(n=0; n < size && afx->buffer_pos < afx->buffer_len; n++ )
		    buf[n++] = afx->buffer[afx->buffer_pos++];
		if( afx->buffer_pos >= afx->buffer_len )
		    afx->buffer_len = 0;
		if( !n )
		    rc = -1;
	    }
	    else if( afx->faked ) {
		unsigned hashes = afx->hashes;
		/* the buffer is at least 15+n*15 bytes long, so it
		 * is easy to construct the packets */

		hashes &= 1|2|4|8;
		if( !hashes )
		    hashes |= 4;  /* default to MD 5 */
		n=0;
		do {
		    /* first some onepass signature packets */
		    buf[n++] = 0x90; /* old format, type 4, 1 length byte */
		    buf[n++] = 13;   /* length */
		    buf[n++] = 3;    /* version */
		    buf[n++] = 0x01; /* sigclass 0x01 (canonical text mode)*/
		    if( hashes & 1 ) {
			hashes &= ~1;
			buf[n++] = DIGEST_ALGO_RMD160;
		    }
		    else if( hashes & 2 ) {
			hashes &= ~2;
			buf[n++] = DIGEST_ALGO_SHA1;
		    }
		    else if( hashes & 4 ) {
			hashes &= ~4;
			buf[n++] = DIGEST_ALGO_MD5;
		    }
		    else if( hashes & 8 ) {
			hashes &= ~8;
			buf[n++] = DIGEST_ALGO_TIGER;
		    }
		    else
			buf[n++] = 0;	 /* (don't know) */

		    buf[n++] = 0;    /* public key algo (don't know) */
		    memset(buf+n, 0, 8); /* don't know the keyid */
		    n += 8;
		    buf[n++] = !hashes;   /* last one */
		} while( hashes );

		/* followed by a plaintext packet */
		buf[n++] = 0xaf; /* old packet format, type 11, var length */
		buf[n++] = 0;	 /* set the length header */
		buf[n++] = 6;
		buf[n++] = 't';  /* canonical text mode */
		buf[n++] = 0;	 /* namelength */
		memset(buf+n, 0, 4); /* timestamp */
		n += 4;
	    }
	    else if( !rc )
		rc = radix64_read( afx, a, &n, buf, size );
	}
	else
	    rc = radix64_read( afx, a, &n, buf, size );
      #if 1
	if( n )
	    if( fwrite(buf, n, 1, fp ) != 1 )
		BUG();
      #endif
	*ret_len = n;
    }
    else if( control == IOBUFCTRL_FLUSH ) {
	if( !afx->status ) { /* write the header line */
	    if( afx->what >= DIM(head_strings) )
		log_bug("afx->what=%d", afx->what);
	    iobuf_writestr(a, "-----");
	    iobuf_writestr(a, head_strings[afx->what] );
	    iobuf_writestr(a, "-----\n");
	    iobuf_writestr(a, "Version: GnuPG v"  VERSION " ("
					    PRINTABLE_OS_NAME ")\n");

	    if( opt.comment_string ) {
		const char *s = opt.comment_string;
		iobuf_writestr(a, "Comment: " );
		for( ; *s; s++ ) {
		    if( *s == '\n' )
			iobuf_writestr(a, "\\n" );
		    else if( *s == '\r' )
			iobuf_writestr(a, "\\r" );
		    else if( *s == '\v' )
			iobuf_writestr(a, "\\v" );
		    else
			iobuf_put(a, *s );
		}
		iobuf_put(a, '\n' );
	    }
	    else
		iobuf_writestr(a,
		    "Comment: For info see http://www.gnupg.org\n");
	    if( afx->hdrlines )
		iobuf_writestr(a, afx->hdrlines);
	    iobuf_put(a, '\n');
	    afx->status++;
	    afx->idx = 0;
	    afx->idx2 = 0;
	    afx->crc = CRCINIT;
	}
	crc = afx->crc;
	idx = afx->idx;
	idx2 = afx->idx2;
	for(i=0; i < idx; i++ )
	    radbuf[i] = afx->radbuf[i];

	for(i=0; i < size; i++ )
	    crc = (crc << 8) ^ crc_table[((crc >> 16)&0xff) ^ buf[i]];
	crc &= 0x00ffffff;

	for( ; size; buf++, size-- ) {
	    radbuf[idx++] = *buf;
	    if( idx > 2 ) {
		idx = 0;
		c = bintoasc[(*radbuf >> 2) & 077];
		iobuf_put(a, c);
		c = bintoasc[(((*radbuf<<4)&060)|((radbuf[1] >> 4)&017))&077];
		iobuf_put(a, c);
		c = bintoasc[(((radbuf[1]<<2)&074)|((radbuf[2]>>6)&03))&077];
		iobuf_put(a, c);
		c = bintoasc[radbuf[2]&077];
		iobuf_put(a, c);
		if( ++idx2 >= (64/4) ) { /* pgp doesn't like 72 here */
		    iobuf_put(a, '\n');
		    idx2=0;
		}
	    }
	}
	for(i=0; i < idx; i++ )
	    afx->radbuf[i] = radbuf[i];
	afx->idx = idx;
	afx->idx2 = idx2;
	afx->crc  = crc;
    }
    else if( control == IOBUFCTRL_INIT ) {
	if( !is_initialized )
	    initialize();
    }
    else if( control == IOBUFCTRL_FREE ) {
	if( afx->status ) { /* pad, write cecksum, and bottom line */
	    crc = afx->crc;
	    idx = afx->idx;
	    idx2 = afx->idx2;
	    for(i=0; i < idx; i++ )
		radbuf[i] = afx->radbuf[i];
	    if( idx ) {
		c = bintoasc[(*radbuf>>2)&077];
		iobuf_put(a, c);
		if( idx == 1 ) {
		    c = bintoasc[((*radbuf << 4) & 060) & 077];
		    iobuf_put(a, c);
		    iobuf_put(a, '=');
		    iobuf_put(a, '=');
		}
		else { /* 2 */
		    c = bintoasc[(((*radbuf<<4)&060)|((radbuf[1]>>4)&017))&077];
		    iobuf_put(a, c);
		    c = bintoasc[((radbuf[1] << 2) & 074) & 077];
		    iobuf_put(a, c);
		    iobuf_put(a, '=');
		}
		if( ++idx2 >= (64/4) ) { /* pgp doesn't like 72 here */
		    iobuf_put(a, '\n');
		    idx2=0;
		}
	    }
	    /* may need a linefeed */
	    if( idx2 )
		iobuf_put(a, '\n');
	    /* write the CRC */
	    iobuf_put(a, '=');
	    radbuf[0] = crc >>16;
	    radbuf[1] = crc >> 8;
	    radbuf[2] = crc;
	    c = bintoasc[(*radbuf >> 2) & 077];
	    iobuf_put(a, c);
	    c = bintoasc[(((*radbuf<<4)&060)|((radbuf[1] >> 4)&017))&077];
	    iobuf_put(a, c);
	    c = bintoasc[(((radbuf[1]<<2)&074)|((radbuf[2]>>6)&03))&077];
	    iobuf_put(a, c);
	    c = bintoasc[radbuf[2]&077];
	    iobuf_put(a, c);
	    iobuf_put(a, '\n');
	    /* and the the trailer */
	    if( afx->what >= DIM(tail_strings) )
		log_bug("afx->what=%d", afx->what);
	    iobuf_writestr(a, "-----");
	    iobuf_writestr(a, tail_strings[afx->what] );
	    iobuf_writestr(a, "-----\n");
	}
	else if( !afx->any_data && !afx->inp_bypass )
	    log_error(_("no valid OpenPGP data found.\n"));
	m_free( afx->buffer );
	afx->buffer = NULL;
    }
    else if( control == IOBUFCTRL_DESC )
	*(char**)buf = "armor_filter";
    return rc;
}




/*
 * $Id: zparser.c,v 1.1 2003/02/12 21:43:35 alexis Exp $
 *
 * zparser.c -- master zone file parser
 *
 * Alexis Yushin, <alexis@nlnetlabs.nl>
 *
 * Copyright (c) 2001, NLnet Labs. All rights reserved.
 *
 * This software is an open source.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include <config.h>

#include <sys/types.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>
#include <arpa/inet.h>

/* This one is for AF_INET6 declaration */
#include <sys/socket.h>

#include <dns.h>
#include <zparser.h>
#include <dname.h>
#include <rfc1876.h>

/*
 *
 * Resource records types and classes that we know.
 *
 */
static struct ztab ztypes[] = Z_TYPES;
static struct ztab zclasses[] = Z_CLASSES;

#ifdef TEST
/*
 *
 * When compiled with -DTEST this function allocates size bytes
 * of memory and terminates with a meaningfull message on failure.
 *
 */
void *
xalloc (size_t size)
{
	void *p;
	if((p = malloc(size)) == NULL) {
		fprintf(stderr, "failed allocating %u bytes: %s\n", size,
			strerror(errno));
		abort();
	}
	return p;
}

/*
 *
 * Same as xalloc() but then reallocates alrady allocated memory.
 *
 */
void *
xrealloc (void *p, size_t size)
{
	if((p = realloc(p, size)) == NULL) {
		fprintf(stderr, "failed reallocating %u bytes: %s\n", size,
			strerror(errno));
		abort();
	}
	return p;
}


#endif /* TEST */

/*
 * Looks up the numeric value of the symbol, returns 0 if not found.
 *
 */
u_int16_t
intbyname (char *a, struct ztab *tab)
{
	while(tab->name != NULL) {
		if(strcasecmp(a, tab->name) == 0) return tab->sym;
		tab++;
	}
	return  NULL;
}

/*
 * Converts a string representation of a period of time into
 * a long integer of seconds.
 *
 * Set the endptr to the first illegal character.
 *
 * Interface is similar as strtol(3)
 *
 * Returns:
 *	LONG_MIN if underflow occurs
 *	LONG_MAX if overflow occurs.
 *	otherwise number of seconds
 *
 * XXX This functions does not check the range.
 *
 */
long
strtottl(nptr, endptr)
	char *nptr;
	char **endptr;
{
	int sign = 0;
	long i = 0;
	long seconds = 0;

	for(*endptr = nptr; **endptr; (*endptr)++) {
		switch(**endptr) {
		case ' ':
		case '\t':
			break;
		case '-':
			if(sign == 0) {
				sign = -1;
			} else {
				return (sign == -1) ? -seconds : seconds;
			}
			break;
		case '+':
			if(sign == 0) {
				sign = 1;
			} else {
				return (sign == -1) ? -seconds : seconds;
			}
			break;
		case 's':
		case 'S':
			seconds += i;
			i = 0;
			break;
		case 'm':
		case 'M':
			seconds += i * 60;
			i = 0;
			break;
		case 'h':
		case 'H':
			seconds += i * 60 * 60;
			i = 0;
			break;
		case 'd':
		case 'D':
			seconds += i * 60 * 60 * 24;
			i = 0;
			break;
		case 'w':
		case 'W':
			seconds += i * 60 * 60 * 24 * 7;
			i = 0;
			break;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			i *= 10;
			i += (**endptr - '0');
			break;
		default:
			seconds += i;
			return (sign == -1) ? -seconds : seconds;
		}
	}
	seconds += i;
	return (sign == -1) ? -seconds : seconds;

}

/*
 * Prints error message and the file name and line number of the file
 * where it happened. Also increments the number of errors for the
 * particular file.
 *
 * Returns:
 *
 *	nothing
 */
void 
zerror (struct zparser *z, char *msg)
{
	fprintf(stderr, "%s in %s, line %lu\n", msg, z->filename, z->_lineno);
	z->errors++;
}

/*
 * Prints syntax error message and the file name and line number of the file
 * where it happened. Also increments the number of errors for the
 * particular file.
 *
 * Returns:
 *
 *	nothing
 */
void 
zsyntax (struct zparser *z)
{
	zerror(z, "syntax error");
}

/*
 * Prints UEOL message and the file name and line number of the file
 * where it happened. Also increments the number of errors for the
 * particular file.
 *
 * Returns:
 *
 *	nothing
 */
void 
zunexpected (struct zparser *z)
{
	zerror(z, "unexpected end of line");
}

/*
 * A wrapper for _zopen()
 *
 */
struct zparser *
zopen (char *filename, u_int32_t ttl, u_int16_t class, char *origin)
{
	return _zopen(filename, ttl, class, strdname(origin, (u_char *)"\001"));
}

/*
 *
 * Initializes the parser and opens a zone file.
 *
 * Returns:
 *
 *	- pointer to the parser structure
 *	- NULL on error and errno set
 *
 */
struct zparser *
_zopen (char *filename, u_int32_t ttl, u_int16_t class, u_char *origin)
{
	struct zparser *z;

	/* Allocate new handling structure */
	z = xalloc(sizeof(struct zparser));

	/* Open the zone file... */
	if((z->file = fopen(filename, "r")) == NULL) {
		free(z);
		return NULL;
	}

	/* Initialize the rest of the structure */
	z->errors = 0;
	z->lines = 0;
	z->_lineno = 0;
	z->filename = strdup(filename);
	z->origin = dnamedup(origin);
	z->ttl = ttl;
	z->class = class;
	z->include = NULL;
	memset(&z->_rr, 0, sizeof(struct RR));

	return z;
}

/*
 *
 * Reads a resource record from an open zone file.
 *
 * Returns:
 *
 *	- pointer to the resource record read
 *	- NULL on end of file or critical error
 *
 * XXX: Describe here what has to be freed() and what's not.
 *
 */
struct RR *
zread (struct zparser *z)
{
	char *t;

	/* Are we including at the moment? */
	if(z->include != NULL) {
		/* Anything left in that include? */
		if((zread(z->include)) != NULL)
			return &z->include->_rr;

		/* Extract the interesting information */
		z->lines += z->include->lines;
		z->errors += z->include->errors;

		/* If not close it, and procceed */
		zclose(z->include);
		z->include = NULL;
	}

	/* Read until end of file or error */
	while(zparseline(z) > 0) {
		/* Process $DIRECTIVES */
		if(*z->_t[0] == '$') {
			if(strcasecmp(z->_t[0], "$TTL") == 0) {
				if(z->_t[1] == NULL ||
					((z->ttl = strtottl(z->_t[1], &t)) | 1)  ||
						*t != 0) {
					zerror(z, "invalid or missing ttl value");
				}
			} else if(strcasecmp(z->_t[0], "$ORIGIN") == 0) {
				if(z->_t[1] == NULL ||
					(z->_rr.dname = strdname(z->_t[1], z->origin)) == NULL) {
					zerror(z, "invalid or missing origin");
				} else {
					free(z->origin);
					z->origin = dnamedup(z->_rr.dname);
				}
				/* Clean up after use... */
				z->_rr.dname = NULL;
			} else if(strcasecmp(z->_t[0], "$INCLUDE") == 0) {
				if(z->_t[1] == NULL) {
					zerror(z, "missing include file name");
				} else if((z->include = _zopen(z->_t[1], z->ttl, z->class,
						z->_t[2] ? strdname(z->_t[2], z->origin) :
							z->origin)) == NULL) {
					zerror(z, "unable to open include file");
				} else {
					/* Call ourselves again to start including */
					return zread(z);
				}
			} else {
				zerror(z, "unknown directive");
			}
			continue;
		}

		/* Process the domain name */
		if(*z->_t[0] == ' ') {
			if(z->_rr.dname == NULL) {
				zerror(z, "missing domain name");
				continue;
			}
		} else {
			/* Free the old name */
			if(z->_rr.dname)
				free(z->_rr.dname);

			/* Parse the dname */
			if((z->_rr.dname = strdname(z->_t[0], z->origin)) == NULL) {
				zerror(z, "invalid domain name");
				z->_rr.dname = NULL;
				continue;
			}
		}

		/* Process TTL, class and type */
		z->_rr.ttl = z->ttl;
		z->_rr.class = z->class;
		z->_rr.type = 0;

		for(z->_tc = 1; z->_t[z->_tc] != NULL; z->_tc++) {
			/* Is this a TTL? */
			if(isdigit(*z->_t[z->_tc])) {
				z->_rr.ttl = strtottl(z->_t[z->_tc], &t);
				if(*t) {
					/* zerror(z, "invalid ttl"); syntax error below */
					break;
				}
				continue;
			}

			/* Class? */
			if((z->_rr.class = intbyname(z->_t[z->_tc], zclasses)) == 0) {
				z->_rr.class = z->class;
			} else {
				continue;
			}

			/* Then this must be a type */
			if(strncasecmp(z->_t[z->_tc], "TYPE", 4) == 0) {
				z->_rr.type = atoi(z->_t[z->_tc] + 4);
			} else {
				z->_rr.type = intbyname(z->_t[z->_tc], ztypes);
			}
			break;
		}

		/* Couldn't parse ttl, class or type? */
		if(z->_rr.type == 0) {
			zsyntax(z);
			continue;
		}

		/* Unless it is NULL record rdata must be present */
		if(z->_t[++z->_tc] == NULL) {
			if(z->_rr.type != TYPE_NULL) {
				zsyntax(z);
				continue;
			} else {
				return &z->_rr;
			}
		}

		/* Now parse the rdata... */
		if(zrdata(z) != 0) {
			/* Free any used rdata and try another line... */
			while(z->_rc >= 0) {
				if(z->_rr.rdata[z->_rc])
					free(z->_rr.rdata[z->_rc]);
				z->_rr.rdata[z->_rc] = NULL;
				z->_rc--;
			}
			continue;
		}

		/* Do we have any tokens left? */
		if(z->_t[z->_tc] != NULL) {
			zerror(z, "trailing garbage");
		}

		/* Add the trailing NULL and adjust the counter. */
		zaddrdata(z, NULL);
		z->_rc--;

		/* Success! */
		return &z->_rr;
	}

	/* End of file or parser error */
	return NULL;
}

/*
 *
 * Closes a zone file and destroys  the parser.
 *
 * Returns:
 *
 *	Nothing
 *
 */
void
zclose (struct zparser *z)
{

	if(z->filename)
		free(z->filename);

	if(z->origin)
		free(z->origin);

	fclose(z->file);

	free(z);
}

/*
 * A wrapper to add an rdata pointer to the rdata pointer
 * list and increment the rdata pointers counter. Checks
 * boundaries and dies if violated.
 *
 * Returns:
 *
 *	nothing
 *
 */
void
zaddrdata (struct zparser *z, u_int16_t *r)
{
	if(z->_rc >= MAXRDATALEN) {
		zerror(z, "too many rdata elements");
		abort();
	}
	z->_rr.rdata[++z->_rc] = r;
}

/*
 *
 * Parses the rdata portion of resource record entry.
 *
 * Returns:
 *
 *	 number of elements parsed if successfull
 *	 0 if error
 *
 * Produces diagnostic via zerror.
 *
 * USE WITH CARE (just kidding).
 *
 */
int
zrdata (struct zparser *z)
{
	/* Do we have an empty rdata? */
	if(*z->_t[z->_tc] == NULL) {
		zsyntax(z);
		return 0;
	}

	/* Is this resource record in unknown format? */
	if(strcmp(z->_t[z->_tc], "\\#") == 0)
		return 0; /* zrdata_unkn(z); */

	/* Otherwise parse one of the types we know... */
	switch(z->_rr.type) {
		case TYPE_A:
			return zrdata_a(z);
		case TYPE_NS:
		case TYPE_MD:
		case TYPE_MF:
		case TYPE_CNAME:
		case TYPE_MB:
		case TYPE_MG:
		case TYPE_MR:
		case TYPE_PTR:
			return zrdata_dname(z);
		case TYPE_MINFO:
		case TYPE_RP:
			if(!zrdata_dname(z)) return 0;
			return zrdata_dname(z);
		case TYPE_TXT:
			while(zrdata_text(z)) {
				/* If no more tokens return, we did not count elems ahhh... */
				if(*z->_t[z->_tc] == NULL) return 1;
			}
			return 0;
		case TYPE_SOA:
			if(!zrdata_dname(z)) return 0;
			if(!zrdata_dname(z)) return 0;
			if(!zrdata_long(z)) return 0;
			if(!zrdata_long(z)) return 0;
			if(!zrdata_long(z)) return 0;
			if(!zrdata_long(z)) return 0;
			return zrdata_long(z);
		case TYPE_LOC:
			return zrdata_loc(z);
		case TYPE_HINFO:
			if(!zrdata_text);
			return zrdata_text(z);
		case TYPE_MX:
			if(!zrdata_short(z)) return 0;
			return zrdata_dname(z);
		case TYPE_AAAA:
			return zrdata_a6(z);
		case TYPE_SRV:
			if(!zrdata_short(z)) return 0;
			if(!zrdata_short(z)) return 0;
			if(!zrdata_short(z)) return 0;
			return zrdata_dname(z);
		case TYPE_NAPTR:
			if(!zrdata_short(z)) return 0;
			if(!zrdata_short(z)) return 0;
			if(!zrdata_text(z)) return 0;
			if(!zrdata_text(z)) return 0;
			if(!zrdata_text(z)) return 0;
			return zrdata_dname(z);
		case TYPE_AFSDB:
			if(!zrdata_short(z)) return 0;
			return zrdata_dname(z);
		case TYPE_SIG:
			if(!zrdata_short(z)) return 0;
			if(!zrdata_byte(z)) return 0;
			if(!zrdata_byte(z)) return 0;
			if(!zrdata_long(z)) return 0;
			if(!zrdata_long(z)) return 0;
			if(!zrdata_long(z)) return 0;
			if(!zrdata_short(z)) return 0;
			if(!zrdata_dname(z)) return 0;
			return zrdata_b64(z);
		case TYPE_NULL:
			zerror(z, "no rdata allowed for NULL resource record");
			return 0;
/*
*	Do these ones in a sec, ok?
*
*		case TYPE_KEY, "KEY", "sccU"},    
*		case TYPE_NXT, "NXT", "nU"},  
*		case TYPE_DS, "DS", "sccU"}, 
*/
		case TYPE_WKS:
		default:
			zerror(z, "dont know how to parse this type, try UNKN representation");
	}

	return -1;
}

/*
 * Parses a specific part of rdata.
 *
 * Returns:
 *
 *	number of elements parsed
 *	zero on error
 *
 * XXX Perhaps check numerical boundaries here.
 */
int
zrdata_short (struct zparser *z)
{
	char *t;
	u_int16_t *r;

	/* Produce an error message... */
	if(z->_t[z->_tc] == NULL) {
		zunexpected(z);
		return 0;
	}

	/* Allocate required space... */
	r = xalloc(sizeof(u_int16_t) + sizeof(u_int16_t));

	*(r+1)  = (u_int16_t)strtol(z->_t[z->_tc], &t, 10);

	if(*t != 0) {
		zerror(z, "unsigned short value is expected");
		free(r);
		return 0;
	}

	*r = sizeof(u_int16_t);
	zaddrdata(z, r);
	z->_tc++;

	return 1;
}

/*
 * Parses a specific part of rdata.
 *
 * Returns:
 *
 *	number of elements parsed
 *	zero on error
 *
 * XXX Perhaps check numerical boundaries here.
 */
int
zrdata_long (struct zparser *z)
{
	char *t;
	u_int16_t *r;

	/* Produce an error message... */
	if(z->_t[z->_tc] == NULL) {
		zunexpected(z);
		return 0;
	}

	/* Allocate required space... */
	r = xalloc(sizeof(u_int16_t) + sizeof(u_int32_t));

	*((u_int32_t *)(r+1))  = (u_int32_t)strtol(z->_t[z->_tc], &t, 10);

	if(*t != 0) {
		zerror(z, "long decimal value is expected");
		free(r);
		return 0;
	}

	*r = sizeof(u_int32_t);
	zaddrdata(z, r);
	z->_tc++;

	return 1;
}

/*
 * Parses a specific part of rdata.
 *
 * Returns:
 *
 *	number of elements parsed
 *	zero on error
 *
 * XXX Perhaps check numerical boundaries here.
 */
int
zrdata_byte (struct zparser *z)
{
	char *t;
	u_int16_t *r;

	/* Produce an error message... */
	if(z->_t[z->_tc] == NULL) {
		zunexpected(z);
		return 0;
	}

	/* Allocate required space... */
	r = xalloc(sizeof(u_int16_t) + sizeof(u_int8_t));

	*((u_int8_t *)(r+1))  = (u_int8_t)strtol(z->_t[z->_tc], &t, 10);

	if(*t != 0) {
		zerror(z, "decimal value is expected");
		free(r);
		return 0;
	}

	*r = sizeof(u_int8_t);
	zaddrdata(z, r);
	z->_tc++;

	return 1;
}


/*
 * Parses a specific part of rdata.
 *
 * Returns:
 *
 *	number of elements parsed
 *	zero on error
 *
 */
int
zrdata_a (struct zparser *z)
{
#ifdef	HAVE_INET_NTOA
	struct in_addr pin;
#endif
	u_int16_t *r;

	/* Produce an error message... */
	if(z->_t[z->_tc] == NULL) {
		zunexpected(z);
		return 0;
	}

	/* Allocate required space... */
	r = xalloc(sizeof(u_int16_t) + sizeof(in_addr_t));

#ifdef HAVE_INET_NTOA
	if(inet_aton(z->_t[z->_tc], &pin) == 1) {
		*((in_addr_t *)(r + 1)) = pin.s_addr;
	} else {
#else
	if((*((u_int32_t *)(r + 1)) = inet_addr(z->_t[z->_tc])) == -1) {
#endif
		zerror(z, "invalid ip address");
		free(r);
		return 0;
	}

	*r = sizeof(u_int32_t);
	zaddrdata(z, r);
	z->_tc++;

	return 1;
}

/*
 * Parses a specific part of rdata.
 *
 * Returns:
 *
 *	number of elements parsed
 *	zero on error
 *
 */
int
zrdata_dname (struct zparser *z)
{
	u_int16_t *r;

	/* Produce an error message... */
	if(z->_t[z->_tc] == NULL) {
		zunexpected(z);
		return 0;
	}

	/* This is an easy one */
	if((r = dnamedup(strdname(z->_t[z->_tc], z->origin))) == NULL) {
		zerror(z, "invalid domain name");
		free(r);
		return 0;
	}

	zaddrdata(z, r);
	z->_tc++;

	return 1;
}

/*
 * Parses a specific part of rdata.
 *
 * Returns:
 *
 *	number of elements parsed
 *	zero on error
 *
 */
int
zrdata_text (struct zparser *z)
{
	u_int16_t *r;
	int l;

	/* Produce an error message... */
	if(z->_t[z->_tc] == NULL) {
		zunexpected(z);
		return 0;
	}

	if((l = strlen(z->_t[z->_tc])) > 255) {
		zerror(z, "text string is longer than 255 charaters, suggest split in two");
		return 0;
	}

	/* Allocate required space... */
	r = xalloc(sizeof(u_int16_t) + l + 1);

	*((char *)(r+1))  = l;
	memcpy((char *)(r+2), z->_t[z->_tc], l);

	*r = l + 1;
	zaddrdata(z, r);
	z->_tc++;

	return 1;
}

/*
 * Parses a specific part of rdata.
 *
 * Returns:
 *
 *	number of elements parsed
 *	zero on error
 *
 */
int
zrdata_a6 (struct zparser *z)
{
	u_int16_t *r;

	/* Produce an error message... */
	if(z->_t[z->_tc] == NULL) {
		zunexpected(z);
		return 0;
	}

	/* Allocate required space... */
	r = xalloc(sizeof(u_int16_t) + IP6ADDRLEN);

	/* Try to convert it */
	if(inet_pton(AF_INET6, z->_t[z->_tc], r + 1) != 1) {
		zerror(z, "invalid ipv6 address");
		free(r);
		return 0;
	}

	*r = IP6ADDRLEN;
	zaddrdata(z, r);
	z->_tc++;

	return 1;
}

/*
 * Parses a specific part of rdata.
 *
 * Returns:
 *
 *	number of elements parsed
 *	zero on error
 *
 * XXX This one is still pretty dirty, real mess.
 * XXX Besides no buffer overflow check.
 *
 */
int
zrdata_loc (struct zparser *z)
{
	u_int16_t *r;
	char buf[512];
	char *t = buf;

	/* Produce an error message... */
	if(z->_t[z->_tc] == NULL) {
		zunexpected(z);
		return 0;
	}

	/* Allocate required space... */
	r = xalloc(sizeof(u_int16_t) + LOCRDLEN);

	/* Paste all the tokens together again */

	while(z->_t[z->_tc] != NULL) {
		strcpy(t, z->_t[z->_tc++]);
		t += strlen(t);
	}

	/* Try to convert it */
        if(loc_aton(buf,  (u_char *)(r + 1)) != LOCRDLEN) {
		zerror(z, "LOC record data expected");
		free(r);
		return 0;
	}

	*r = LOCRDLEN;
	zaddrdata(z, r);

	return 1;
}

/*
 * Parses a specific part of rdata.
 *
 * Returns:
 *
 *	number of elements parsed
 *	zero on error
 *
 */
int
zrdata_b64 (struct zparser *z)
{
	u_int16_t *r;
	char *t;
	int n;

	/* We cant handle more than 32k with current design anyways */
	int s = MAXRDATAELEMSIZE;

	/* Produce an error message... */
	if(z->_t[z->_tc] == NULL) {
		zunexpected(z);
		return 0;
	}

	/* Allocate required space... */
	r = xalloc(sizeof(u_int16_t) + s);
	t = (char *)(r + 1);

	/* And then process all the tokens */
	while(z->_t[z->_tc] != NULL) {
		/* Are we out of buffer space.... */
		if(s <= 0) {
			zerror(z, "out of buffer for base64 data");
			free(r);
			return 0;
		}

		/* Try to convert it */
		if((n = __b64_pton(z->_t[z->_tc++], t, s)) == -1) {
                        zerror(z, "base64 encoding failed");
                        free(r);
                        return 0;
                }

                t += n;
                s -= n;
        }

	*r = MAXRDATAELEMSIZE - s;
        r = xrealloc(r, *r + sizeof(u_int16_t));

	zaddrdata(z, r);
	z->_tc++;

	return 1;
}



/*
 * A wrapper to add a token to the current line and increment
 * the tokens counter. Checks boundaries and dies if violated.
 *
 * Returns:
 *
 *	nothing
 *
 */
void
zaddtoken (struct zparser *z, char *t)
{
	if(z->_tc >= MAXTOKENSLEN) {
		zerror(z, "too many token per entry");
		abort();
	}
	z->_t[z->_tc++] = t;
}


/*
 *
 * Parses a line from an open zone file, and splits it into
 * tokens.
 *
 * Returns:
 *
 *	number of tokens read
 *	zero if end of file
 *	-1 if error
 *
 */
int
zparseline (struct zparser *z)
{
	int parenthes = 0;
	int newline;
	register char *p, *s, *t;
	char *line = z->_buf;

	/* Start fresh... */
	z->_tc = 0;

	/* Read the lines... */
	while((p = fgets(line, line - z->_buf + ZBUFSIZE, z->file)) != NULL) {
		z->lines++;
		z->_lineno++;
		newline = 0;

		if(!parenthes) {
			/* We have the same domain name as before, add it as a token... */
			if(*p == ' ' || *p == '\t') {
				*p = ' ';
				zaddtoken(z, p++);
			}
		}

		/* While not end of line... */
		while(*p) {
			/* Skip leading delimiters */
			for(s = p; *s == ' ' || *s == '\t' || *s == '\n'; s++);

			/* Quotes... */
			if(*s == '"') {
				for(t = ++s; *t && *t != '"'; t++);
				if(*t) {
					*t = '\000';
					p = t + 1;
					zaddtoken(z, s);
					continue;
				} else {
					zerror(z, "unterminated quoted string");
					return -1;
				}

			}

			/* Find the next delimiter... */
			t = s;
			for(;;) {

				/* What do we do now... */
				switch(*t) {
				case '(':
					if(parenthes) {
						zerror(z, "nested parenthes");
						return -1;
					}
					parenthes = 1;
					p = t + 1;
					*t = 0;
					break;
				case ')':
					if(!parenthes) {
						zerror(z, "missing opening parenthes");
						return -1;
					}
					parenthes = 0;
					p = t + 1;
					*t = 0;
					break;
				case ';':
					newline++;
					*t = 0;
					p = t;
					break;
				case '\n':
					newline++;
				case ' ':
				case '\t':
					*t = 0;
					p = t + 1;
					zaddtoken(z, s);
					break;
				case 0:
					zaddtoken(z, s);
					p = t;
					break;
				default:
					t++;
					continue;
				}
				break;
			}
		}

		/* If we did not read a newline, we are not sure of anything... */
		if(!newline) {
			zerror(z, "truncated line, possibly insufficient buffer size");
			return -1;
		}

		/* If we did not scan anything, skip this line... */
		if(z->_tc == 0)
			continue;

		/* If we're within parenthes, keep on scanning... */
		if(parenthes)
			continue;

		/* Otherwise add a terminating NULL... */
		zaddtoken(z, NULL);

		/* Correct the token counter and return... */
		return(--z->_tc);
	}

	/* I/O error?... */
	if(errno != 0) {
		zerror(z, "error reading file");
		return -1;
	}

	/* Still open parenthes?... */
	if(parenthes) {
		zerror(z, "end of file within parenthes");
		return -1;
	}

	/* End of file */
	return 0;
}


#ifdef TEST

/*
 * Standard usage function for testing puposes.
 *
 */
void
usage (void)
{
	fprintf(stderr, "usage: zparser zone-file [origin]\n");
	exit(1);
}

/*
 * Testing wrapper, to parse a zone file specified on the command
 * line.
 *
 */
int
main (int argc, char *argv[])
{
	struct zparser *z;
	struct RR *rr;
	char *origin;


	/* Check the command line */
	if(argc < 2 || argc > 3) {
		usage();
	}

	if(argc == 2) {
		origin = ".";
	} else {
		origin = argv[2];
	}

	/* Open the file */
	if((z = zopen(argv[1], 3600, CLASS_IN, origin)) == NULL) {
		fprintf(stderr, "unable to open %s: %s\n", argv[1], strerror(errno));
		exit(1);
	}

	/* Read the file */
	while((rr = zread(z)) != NULL) {
		if((z->lines % 100000) == 0) {
			fprintf(stderr, "read %lu lines...\n", z->lines);
		}
	}

	fprintf(stderr, "done: %d errors\n", z->errors);

	/* Close the file */
	zclose(z);

	return 0;
}

#endif

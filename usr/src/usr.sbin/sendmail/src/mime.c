/*
 * Copyright (c) 1994 Eric P. Allman
 * Copyright (c) 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * %sccs.include.redist.c%
 */

# include "sendmail.h"
# include <string.h>

#ifndef lint
static char sccsid[] = "@(#)mime.c	8.20 (Berkeley) %G%";
#endif /* not lint */

/*
**  MIME support.
**
**	I am indebted to John Beck of Hewlett-Packard, who contributed
**	his code to me for inclusion.  As it turns out, I did not use
**	his code since he used a "minimum change" approach that used
**	several temp files, and I wanted a "minimum impact" approach
**	that would avoid copying.  However, looking over his code
**	helped me cement my understanding of the problem.
**
**	I also looked at, but did not directly use, Nathaniel
**	Borenstein's "code.c" module.  Again, it functioned as
**	a file-to-file translator, which did not fit within my
**	design bounds, but it was a useful base for understanding
**	the problem.
**
**	Also, I looked at John G. Meyer's "part.[ch]" code.  Some
**	ideas have been loosely borrowed.
*/


/* character set for hex and base64 encoding */
char	Base16Code[] =	"0123456789ABCDEF";
char	Base64Code[] =	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* types of MIME boundaries */
#define MBT_SYNTAX	0	/* syntax error */
#define MBT_NOTSEP	1	/* not a boundary */
#define MBT_INTERMED	2	/* intermediate boundary (no trailing --) */
#define MBT_FINAL	3	/* final boundary (trailing -- included) */

static char	*MimeBoundaryNames[] =
{
	"SYNTAX",	"NOTSEP",	"INTERMED",	"FINAL"
};

/*
**  Pseudo-file structure for reading up to MIME boundaries.
*/

#define MFILE	struct _mfile

MFILE
{
	FILE	*mf_fp;		/* underlying file */
	u_char	*mf_buf;	/* a cache buffer */
	u_char	*mf_bp;		/* current pointer into mf_buf */
	int	mf_cnt;		/* number of characters left in mf_buf */
	int	mf_bsize;	/* size of mf_buf */
	int	(*mf_fill)();	/* buffer fill routine */
	char	mf_boundaries[MAXMIMENESTING + 1];
				/* current boundaries */
};

#define mime_getc(mf)	((mf)->mf_cnt-- > 0 : *(mf)->mf_ptr++ : mf->mf_fill(mf))
/*
**  MIME8TO7 -- output 8 bit body in 7 bit format
**
**	The header has already been output -- this has to do the
**	8 to 7 bit conversion.  It would be easy if we didn't have
**	to deal with nested formats (multipart/xxx and message/rfc822).
**
**	We won't be called if we don't have to do a conversion, and
**	appropriate MIME-Version: and Content-Type: fields have been
**	output.  Any Content-Transfer-Encoding: field has not been
**	output, and we can add it here.
**
**	Parameters:
**		mci -- mailer connection information.
**		header -- the header for this body part.
**		e -- envelope.
**		flags -- to tweak processing.
**
**	Returns:
**		none.
*/

int
mime8to7(mci, header, e, flags)
	register MCI *mci;
	HDR *header;
	register ENVELOPE *e;
	int flags;
{
	MFILE mfile;

	mfile.mf_fp = e->e_dfp;
	mfile.mf_cnt = mfile.mf_size = 0;

	(void) mime8to7x(mci, header, e, &mfile, flags);
	if (mfile.mf_buf != NULL)
		free(mfile.mf_buf);
}
/*
**  MIME8TO7X -- internal interface for mime8to7.
**
**	Parameters:
**		mci -- mailer connection information.
**		header -- the header for this body part.
**		e -- envelope.
**		mf -- the file from which to read.
**		flags -- to tweak processing.
**
**	Returns:
**		An indicator of what terminated the message part:
**		  MBT_FINAL -- the final boundary
**		  MBT_INTERMED -- an intermediate boundary
**		  MBT_NOTSEP -- an end of file
*/

struct args
{
	char	*field;		/* name of field */
	char	*value;		/* value of that field */
};

int
mime8to7x(mci, header, e, boundaries, flags)
	register MCI *mci;
	HDR *header;
	register ENVELOPE *e;
	char **boundaries;
	int flags;
{
	register char *p;
	int linelen;
	int bt;
	off_t offset;
	size_t sectionsize, sectionhighbits;
	int i;
	char *type;
	char *subtype;
	char *cte;
	char **pvp;
	int argc = 0;
	char *bp;
	struct args argv[MAXMIMEARGS];
	char bbuf[128];
	char buf[MAXLINE];
	char pvpbuf[MAXLINE];
	extern char MimeTokenTab[256];

	if (tTd(43, 1))
	{
		printf("mime8to7: flags = %x, boundaries =", flags);
		if (boundaries[0] == NULL)
			printf(" <none>");
		else
		{
			for (i = 0; boundaries[i] != NULL; i++)
				printf(" %s", boundaries[i]);
		}
		printf("\n");
	}
	p = hvalue("Content-Transfer-Encoding", header);
	if (p == NULL ||
	    (pvp = prescan(p, '\0', pvpbuf, sizeof pvpbuf, NULL,
			   MimeTokenTab)) == NULL ||
	    pvp[0] == NULL)
	{
		cte = NULL;
	}
	else
	{
		cataddr(pvp, NULL, buf, sizeof buf, '\0');
		cte = newstr(buf);
	}

	type = subtype = NULL;
	p = hvalue("Content-Type", header);
	if (p == NULL)
	{
		if (bitset(M87F_DIGEST, flags))
			p = "message/rfc822";
		else
			p = "text/plain";
	}
	if (p != NULL &&
	    (pvp = prescan(p, '\0', pvpbuf, sizeof pvpbuf, NULL,
			   MimeTokenTab)) != NULL &&
	    pvp[0] != NULL)
	{
		if (tTd(43, 40))
		{
			for (i = 0; pvp[i] != NULL; i++)
				printf("pvp[%d] = \"%s\"\n", i, pvp[i]);
		}
		type = *pvp++;
		if (*pvp != NULL && strcmp(*pvp, "/") == 0 &&
		    *++pvp != NULL)
		{
			subtype = *pvp++;
		}

		/* break out parameters */
		while (*pvp != NULL && argc < MAXMIMEARGS)
		{
			/* skip to semicolon separator */
			while (*pvp != NULL && strcmp(*pvp, ";") != 0)
				pvp++;
			if (*pvp++ == NULL || *pvp == NULL)
				break;

			/* extract field name */
			argv[argc].field = *pvp++;

			/* see if there is a value */
			if (*pvp != NULL && strcmp(*pvp, "=") == 0 &&
			    (*++pvp == NULL || strcmp(*pvp, ";") != 0))
			{
				argv[argc].value = *pvp;
				argc++;
			}
		}
	}

	/* check for disaster cases */
	if (type == NULL)
		type = "-none-";
	if (subtype == NULL)
		subtype = "-none-";

	/* don't propogate some flags more than one level into the message */
	flags &= ~M87F_DIGEST;

	/*
	**  Check for cases that can not be encoded.
	**
	**	For example, you can't encode certain kinds of types
	**	or already-encoded messages.  If we find this case,
	**	just copy it through.
	*/

	sprintf(buf, "%s/%s", type, subtype);
	if (wordinclass(buf, 'n') || (cte != NULL && !wordinclass(cte, 'e')))
		flags |= M87F_NO8BIT;

	/*
	**  Multipart requires special processing.
	**
	**	Do a recursive descent into the message.
	*/

	if (strcasecmp(type, "multipart") == 0 && !bitset(M87F_NO8BIT, flags))
	{
		int blen;

		if (strcasecmp(subtype, "digest") == 0)
			flags |= M87F_DIGEST;

		for (i = 0; i < argc; i++)
		{
			if (strcasecmp(argv[i].field, "boundary") == 0)
				break;
		}
		if (i >= argc)
		{
			syserr("mime8to7: Content-Type: %s missing boundary", p);
			p = "---";
		}
		else
		{
			p = argv[i].value;
			stripquotes(p);
		}
		blen = strlen(p);
		if (blen > sizeof bbuf - 1)
		{
			syserr("mime8to7: multipart boundary \"%s\" too long",
				p);
			blen = sizeof bbuf - 1;
		}
		strncpy(bbuf, p, blen);
		bbuf[blen] = '\0';
		if (tTd(43, 1))
			printf("mime8to7: multipart boundary \"%s\"\n", bbuf);
		for (i = 0; i < MAXMIMENESTING; i++)
			if (boundaries[i] == NULL)
				break;
		if (i >= MAXMIMENESTING)
			syserr("mime8to7: multipart nesting boundary too deep");
		else
		{
			boundaries[i] = bbuf;
			boundaries[i + 1] = NULL;
		}
		mci->mci_flags |= MCIF_INMIME;

		/* skip the early "comment" prologue */
		putline("", mci);
		while (fgets(buf, sizeof buf, e->e_dfp) != NULL)
		{
			bt = mimeboundary(buf, boundaries);
			if (bt != MBT_NOTSEP)
				break;
			putxline(buf, mci, PXLF_MAPFROM|PXLF_STRIP8BIT);
			if (tTd(43, 99))
				printf("  ...%s", buf);
		}
		if (feof(e->e_dfp))
			bt = MBT_FINAL;
		while (bt != MBT_FINAL)
		{
			auto HDR *hdr = NULL;

			sprintf(buf, "--%s", bbuf);
			putline(buf, mci);
			if (tTd(43, 35))
				printf("  ...%s\n", buf);
			collect(e->e_dfp, FALSE, FALSE, &hdr, e);
			if (tTd(43, 101))
				putline("+++after collect", mci);
			putheader(mci, hdr, e);
			if (tTd(43, 101))
				putline("+++after putheader", mci);
			bt = mime8to7x(mci, hdr, e, boundaries, flags);
		}
		sprintf(buf, "--%s--", bbuf);
		putline(buf, mci);
		if (tTd(43, 35))
			printf("  ...%s\n", buf);
		boundaries[i] = NULL;
		mci->mci_flags &= ~MCIF_INMIME;

		/* skip the late "comment" epilogue */
		while (fgets(buf, sizeof buf, e->e_dfp) != NULL)
		{
			bt = mimeboundary(buf, boundaries);
			if (bt != MBT_NOTSEP)
				break;
			putxline(buf, mci, PXLF_MAPFROM|PXLF_STRIP8BIT);
			if (tTd(43, 99))
				printf("  ...%s", buf);
		}
		if (feof(e->e_dfp))
			bt = MBT_FINAL;
		if (tTd(43, 3))
			printf("\t\t\tmime8to7=>%s (multipart)\n",
				MimeBoundaryNames[bt]);
		return bt;
	}

	/*
	**  Message/* types -- recurse exactly once.
	*/

	if (strcasecmp(type, "message") == 0)
	{
		if (strcasecmp(subtype, "rfc822") != 0)
		{
			flags |= M87F_NO8BIT;
		}
		else
		{
			register char *q;
			auto HDR *hdr = NULL;

			putline("", mci);

			mci->mci_flags |= MCIF_INMIME;
			collect(e->e_dfp, FALSE, FALSE, &hdr, e);
			if (tTd(43, 101))
				putline("+++after collect", mci);
			putheader(mci, hdr, e);
			if (tTd(43, 101))
				putline("+++after putheader", mci);
			bt = mime8to7(mci, hdr, e, boundaries, flags);
			mci->mci_flags &= ~MCIF_INMIME;
			return bt;
		}
	}

	/*
	**  Non-compound body type
	**
	**	Compute the ratio of seven to eight bit characters;
	**	use that as a heuristic to decide how to do the
	**	encoding.
	*/

	sectionsize = sectionhighbits = 0;
	if (!bitset(M87F_NO8BIT, flags))
	{
		/* remember where we were */
		offset = ftell(e->e_dfp);
		if (offset == -1)
			syserr("mime8to7: cannot ftell on df%s", e->e_id);

		/* do a scan of this body type to count character types */
		while (fgets(buf, sizeof buf, e->e_dfp) != NULL)
		{
			if (mimeboundary(buf, boundaries) != MBT_NOTSEP)
				break;
			for (p = buf; *p != '\0'; p++)
			{
				/* count bytes with the high bit set */
				sectionsize++;
				if (bitset(0200, *p))
					sectionhighbits++;
			}

			/*
			**  Heuristic: if 1/4 of the first 4K bytes are 8-bit,
			**  assume base64.  This heuristic avoids double-reading
			**  large graphics or video files.
			*/

			if (sectionsize >= 4096 &&
			    sectionhighbits > sectionsize / 4)
				break;
		}

		/* return to the original offset for processing */
		/* XXX use relative seeks to handle >31 bit file sizes? */
		if (fseek(e->e_dfp, offset, SEEK_SET) < 0)
			syserr("mime8to7: cannot fseek on df%s", e->e_id);
		else
			clearerr(e->e_dfp);
	}

	/*
	**  Heuristically determine encoding method.
	**	If more than 1/8 of the total characters have the
	**	eighth bit set, use base64; else use quoted-printable.
	**	However, only encode binary encoded data as base64,
	**	since otherwise the NL=>CRLF mapping will be a problem.
	*/

	if (tTd(43, 8))
	{
		printf("mime8to7: %ld high bit(s) in %ld byte(s), cte=%s\n",
			sectionhighbits, sectionsize,
			cte == NULL ? "[none]" : cte);
	}
	if (cte != NULL && strcasecmp(cte, "binary") == 0)
		sectionsize = sectionhighbits;
	linelen = 0;
	bp = buf;
	if (sectionhighbits == 0)
	{
		/* no encoding necessary */
		if (cte != NULL)
		{
			sprintf(buf, "Content-Transfer-Encoding: %s", cte);
			putline(buf, mci);
			if (tTd(43, 36))
				printf("  ...%s\n", buf);
		}
		putline("", mci);
		mci->mci_flags &= ~MCIF_INHEADER;
		while (fgets(buf, sizeof buf, e->e_dfp) != NULL)
		{
			bt = mimeboundary(buf, boundaries);
			if (bt != MBT_NOTSEP)
				break;
			putline(buf, mci);
		}
		if (feof(e->e_dfp))
			bt = MBT_FINAL;
	}
	else if (sectionsize / 8 < sectionhighbits)
	{
		/* use base64 encoding */
		int c1, c2;

		putline("Content-Transfer-Encoding: base64", mci);
		if (tTd(43, 36))
			printf("  ...Content-Transfer-Encoding: base64\n");
		putline("", mci);
		mci->mci_flags &= ~MCIF_INHEADER;
		mf->mf_fill = mime_fill_crlf;
		while ((c1 = mime_getc(mf)) != EOF)
		{
			if (linelen > 71)
			{
				*bp = '\0';
				putline(buf, mci);
				linelen = 0;
				bp = buf;
			}
			linelen += 4;
			*bp++ = Base64Code[(c1 >> 2)];
			c1 = (c1 & 0x03) << 4;
			c2 = mime_getc(mf);
			if (c2 == EOF)
			{
				*bp++ = Base64Code[c1];
				*bp++ = '=';
				*bp++ = '=';
				break;
			}
			c1 |= (c2 >> 4) & 0x0f;
			*bp++ = Base64Code[c1];
			c1 = (c2 & 0x0f) << 2;
			c2 = mime_getc(mf);
			if (c2 == EOF)
			{
				*bp++ = Base64Code[c1];
				*bp++ = '=';
				break;
			}
			c1 |= (c2 >> 6) & 0x03;
			*bp++ = Base64Code[c1];
			*bp++ = Base64Code[c2 & 0x3f];
		}
	}
	else
	{
		/* use quoted-printable encoding */
		int c1, c2;
		int fromstate;
		BITMAP badchars;

		/* set up map of characters that must be mapped */
		clrbitmap(badchars);
		for (c1 = 0x00; c1 < 0x20; c1++)
			setbitn(c1, badchars);
		clrbitn('\t', badchars);
		for (c1 = 0x7f; c1 < 0x100; c1++)
			setbitn(c1, badchars);
		setbitn('=', badchars);
		if (bitnset(M_EBCDIC, mci->mci_mailer->m_flags))
			for (p = "!\"#$@[\\]^`{|}~"; *p != '\0'; p++)
				setbitn(*p, badchars);

		putline("Content-Transfer-Encoding: quoted-printable", mci);
		if (tTd(43, 36))
			printf("  ...Content-Transfer-Encoding: quoted-printable\n");
		putline("", mci);
		mci->mci_flags &= ~MCIF_INHEADER;
		fromstate = 0;
		c2 = '\n';
		mf->mf_fill = mime_fill_nl;
		while ((c1 = mime_getc(mf)) != EOF)
		{
			if (c1 == '\n')
			{
				if (c2 == ' ' || c2 == '\t')
				{
					*bp++ = '=';
					*bp++ = Base16Code[(c2 >> 4) & 0x0f];
					*bp++ = Base16Code[c2 & 0x0f];
					*bp = '\0';
					putline(buf, mci);
					bp = buf;
				}
				*bp = '\0';
				putline(buf, mci);
				linelen = fromstate = 0;
				bp = buf;
				c2 = c1;
				continue;
			}
			if (c2 == ' ' && linelen == 4 && fromstate == 4 &&
			    bitnset(M_ESCFROM, mci->mci_mailer->m_flags))
			{
				*bp++ = '=';
				*bp++ = '2';
				*bp++ = '0';
				linelen += 3;
			}
			else if (c2 == ' ' || c2 == '\t')
			{
				*bp++ = c2;
				linelen++;
			}
			if (linelen > 72)
			{
				*bp++ = '=';
				*bp = '\0';
				putline(buf, mci);
				linelen = fromstate = 0;
				bp = buf;
				c2 = '\n';
			}
			if (bitnset(c1 & 0xff, badchars))
			{
				*bp++ = '=';
				*bp++ = Base16Code[(c1 >> 4) & 0x0f];
				*bp++ = Base16Code[c1 & 0x0f];
				linelen += 3;
			}
			else if (c1 != ' ' && c1 != '\t')
			{
				if (linelen < 4 && c1 == "From"[linelen])
					fromstate++;
				*bp++ = c1;
				linelen++;
			}
			c2 = c1;
		}

		/* output any saved character */
		if (c2 == ' ' || c2 == '\t')
		{
			*bp++ = '=';
			*bp++ = Base16Code[(c2 >> 4) & 0x0f];
			*bp++ = Base16Code[c2 & 0x0f];
			linelen += 3;
		}
	}
	if (linelen > 0)
	{
		*bp = '\0';
		putline(buf, mci);
	}
	if (tTd(43, 3))
		printf("\t\t\tmime8to7=>%s (basic)\n", MimeBoundaryNames[bt]);
	return bt;
}
/*
**  MIME_GETCHAR -- get a character for MIME processing
**
**	Treats boundaries as EOF.
**
**	Parameters:
**		fp -- the input file.
**		boundaries -- the current MIME boundaries.
**		btp -- if the return value is EOF, *btp is set to
**			the type of the boundary.
**
**	Returns:
**		The next character in the input stream.
*/

int
mime_getchar(fp, boundaries, btp)
	register FILE *fp;
	char **boundaries;
	int *btp;
{
	int c;
	static u_char *bp = NULL;
	static int buflen = 0;
	static bool atbol = TRUE;	/* at beginning of line */
	static int bt = MBT_SYNTAX;	/* boundary type of next EOF */
	static u_char buf[128];		/* need not be a full line */

	if (buflen > 0)
	{
		buflen--;
		return *bp++;
	}
	bp = buf;
	buflen = 0;
	c = getc(fp);
	if (c == '\n')
	{
		/* might be part of a MIME boundary */
		*bp++ = c;
		atbol = TRUE;
		c = getc(fp);
		if (c == '\n')
		{
			ungetc(c, fp);
			return c;
		}
	}
	if (c != EOF)
		*bp++ = c;
	else
		bt = MBT_FINAL;
	if (atbol && c == '-')
	{
		/* check for a message boundary */
		c = getc(fp);
		if (c != '-')
		{
			if (c != EOF)
				*bp++ = c;
			else
				bt = MBT_FINAL;
			buflen = bp - buf - 1;
			bp = buf;
			return *bp++;
		}

		/* got "--", now check for rest of separator */
		*bp++ = '-';
		while (bp < &buf[sizeof buf - 2] &&
		       (c = getc(fp)) != EOF && c != '\n')
		{
			*bp++ = c;
		}
		*bp = '\0';
		bt = mimeboundary(&buf[1], boundaries);
		switch (bt)
		{
		  case MBT_FINAL:
		  case MBT_INTERMED:
			/* we have a message boundary */
			buflen = 0;
			*btp = bt;
			return EOF;
		}

		atbol = c == '\n';
		if (c != EOF)
			*bp++ = c;
	}

	buflen = bp - buf - 1;
	if (buflen < 0)
	{
		*btp = bt;
		return EOF;
	}
	bp = buf;
	return *bp++;
}
/*
**  MIME_GETCHAR_CRLF -- do mime_getchar, but translate NL => CRLF
**
**	Parameters:
**		fp -- the input file.
**		boundaries -- the current MIME boundaries.
**		btp -- if the return value is EOF, *btp is set to
**			the type of the boundary.
**
**	Returns:
**		The next character in the input stream.
*/

int
mime_getchar_crlf(fp, boundaries, btp)
	register FILE *fp;
	char **boundaries;
	int *btp;
{
	static bool sendlf = FALSE;
	int c;

	if (sendlf)
	{
		sendlf = FALSE;
		return '\n';
	}
	c = mime_getchar(fp, boundaries, btp);
	if (c == '\n')
	{
		sendlf = TRUE;
		return '\r';
	}
	return c;
}
/*
**  MIME7TO8 -- convert 7 bit MIME message to 8 bit
*/

char	QPdecoder[256] =
{
    /*	nul soh stx etx eot enq ack bel  bs  ht  nl  vt  np  cr  so  si   */
	-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, 16, -1, -1, 16, -1, -1,
    /*	dle dc1 dc2 dc3 dc4 nak syn etb  can em  sub esc fs  gs  rs  us   */
	-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
    /*  sp  !   "   #   $   %   &   '    (   )   *   +   ,   -   .   /    */
	-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
    /*	0   1   2   3   4   5   6   7    8   9   :   ;   <   =   >   ?    */
	0,  1,  2,  3,  4,  5,  6,  7,   8,  9,  -1, -1, -1, -1, -1, -1,
    /*	@   A   B   C   D   E   F   G    H   I   J   K   L   M   N   O    */
	-1, 10, 11, 12, 13, 14, 15, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
    /*  P   Q   R   S   T   U   V   W    X   Y   Z   [   \   ]   ^   _    */
	-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
    /*	`   a   b   c   d   e   f   g    h   i   j   k   l   m   n   o    */
	-1, 10, 11, 12, 13, 14, 15, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
    /*  p   q   r   s   t   u   v   w    x   y   z   {   |   }   ~   del  */
	-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,

    /*	nul soh stx etx eot enq ack bel  bs  ht  nl  vt  np  cr  so  si   */
	-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
    /*	dle dc1 dc2 dc3 dc4 nak syn etb  can em  sub esc fs  gs  rs  us   */
	-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
    /*  sp  !   "   #   $   %   &   '    (   )   *   +   ,   -   .   /    */
	-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
    /*	0   1   2   3   4   5   6   7    8   9   :   ;   <   =   >   ?    */
	-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
    /*	@   A   B   C   D   E   F   G    H   I   J   K   L   M   N   O    */
	-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
    /*  P   Q   R   S   T   U   V   W    X   Y   Z   [   \   ]   ^   _    */
	-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
    /*	`   a   b   c   d   e   f   g    h   i   j   k   l   m   n   o    */
	-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
    /*  p   q   r   s   t   u   v   w    x   y   z   {   |   }   ~   del  */
	-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
};


int
mime7to8(mci, header, e, boundaries, flags)
	register MCI *mci;
	HDR *header;
	register ENVELOPE *e;
	char **boundaries;
	int flags;
{
	register char *p;
	int linelen;
	int bt;
	off_t offset;
	int i;
	char *type;
	char *subtype;
	char *cte;
	char **pvp;
	int argc = 0;
	char *bp;
	struct args argv[MAXMIMEARGS];
	char bbuf[128];
	char buf[MAXLINE];
	char pvpbuf[MAXLINE];
	extern char MimeTokenTab[256];

	if (tTd(43, 1))
	{
		printf("mime7to8: flags = %x, boundaries =", flags);
		if (boundaries[0] == NULL)
			printf(" <none>");
		else
		{
			for (i = 0; boundaries[i] != NULL; i++)
				printf(" %s", boundaries[i]);
		}
		printf("\n");
	}
	p = hvalue("Content-Transfer-Encoding", header);
	if (p == NULL ||
	    (pvp = prescan(p, '\0', pvpbuf, sizeof pvpbuf, NULL,
			   MimeTokenTab)) == NULL ||
	    pvp[0] == NULL)
	{
		cte = NULL;
	}
	else
	{
		cataddr(pvp, NULL, buf, sizeof buf, '\0');
		cte = newstr(buf);
	}

	type = subtype = NULL;
	p = hvalue("Content-Type", header);
	if (p == NULL)
	{
		if (bitset(M87F_DIGEST, flags))
			p = "message/rfc822";
		else
			p = "text/plain";
	}
	if (p != NULL &&
	    (pvp = prescan(p, '\0', pvpbuf, sizeof pvpbuf, NULL,
			   MimeTokenTab)) != NULL &&
	    pvp[0] != NULL)
	{
		if (tTd(43, 40))
		{
			for (i = 0; pvp[i] != NULL; i++)
				printf("pvp[%d] = \"%s\"\n", i, pvp[i]);
		}
		type = *pvp++;
		if (*pvp != NULL && strcmp(*pvp, "/") == 0 &&
		    *++pvp != NULL)
		{
			subtype = *pvp++;
		}

		/* break out parameters */
		while (*pvp != NULL && argc < MAXMIMEARGS)
		{
			/* skip to semicolon separator */
			while (*pvp != NULL && strcmp(*pvp, ";") != 0)
				pvp++;
			if (*pvp++ == NULL || *pvp == NULL)
				break;

			/* extract field name */
			argv[argc].field = *pvp++;

			/* see if there is a value */
			if (*pvp != NULL && strcmp(*pvp, "=") == 0 &&
			    (*++pvp == NULL || strcmp(*pvp, ";") != 0))
			{
				argv[argc].value = *pvp;
				argc++;
			}
		}
	}

	/* check for disaster cases */
	if (type == NULL)
		type = "-none-";
	if (subtype == NULL)
		subtype = "-none-";

	/* don't propogate some flags more than one level into the message */
	flags &= ~M87F_DIGEST;

	/*
	**  Check for cases that can not be encoded.
	**
	**	For example, you can't encode certain kinds of types
	**	or already-encoded messages.  If we find this case,
	**	just copy it through.
	*/

	sprintf(buf, "%s/%s", type, subtype);
	if (wordinclass(buf, 'n') || (cte != NULL && !wordinclass(cte, 'e')))
		flags |= M87F_NO8BIT;

	/*
	**  Multipart requires special processing.
	**
	**	Do a recursive descent into the message.
	*/

	if (strcasecmp(type, "multipart") == 0 && !bitset(M87F_NO8BIT, flags))
	{
		int blen;

		if (strcasecmp(subtype, "digest") == 0)
			flags |= M87F_DIGEST;

		for (i = 0; i < argc; i++)
		{
			if (strcasecmp(argv[i].field, "boundary") == 0)
				break;
		}
		if (i >= argc)
		{
			syserr("mime7to8: Content-Type: %s missing boundary", p);
			p = "---";
		}
		else
		{
			p = argv[i].value;
			stripquotes(p);
		}
		blen = strlen(p);
		if (blen > sizeof bbuf - 1)
		{
			syserr("mime7to8: multipart boundary \"%s\" too long",
				p);
			blen = sizeof bbuf - 1;
		}
		strncpy(bbuf, p, blen);
		bbuf[blen] = '\0';
		if (tTd(43, 1))
			printf("mime7to8: multipart boundary \"%s\"\n", bbuf);
		for (i = 0; i < MAXMIMENESTING; i++)
			if (boundaries[i] == NULL)
				break;
		if (i >= MAXMIMENESTING)
			syserr("mime7to8: multipart nesting boundary too deep");
		else
		{
			boundaries[i] = bbuf;
			boundaries[i + 1] = NULL;
		}

		/* skip the early "comment" prologue */
		putline("", mci);
		while (fgets(buf, sizeof buf, e->e_dfp) != NULL)
		{
			bt = mimeboundary(buf, boundaries);
			if (bt != MBT_NOTSEP)
				break;
			putxline(buf, mci, PXLF_MAPFROM|PXLF_STRIP8BIT);
			if (tTd(43, 99))
				printf("  ...%s", buf);
		}
		if (feof(e->e_dfp))
			bt = MBT_FINAL;
		while (bt != MBT_FINAL)
		{
			auto HDR *hdr = NULL;

			sprintf(buf, "--%s", bbuf);
			putline(buf, mci);
			if (tTd(43, 35))
				printf("  ...%s\n", buf);
			collect(e->e_dfp, FALSE, FALSE, &hdr, e);
			if (tTd(43, 101))
				putline("+++after collect", mci);
			putheader(mci, hdr, e);
			if (tTd(43, 101))
				putline("+++after putheader", mci);
			bt = mime7to8(mci, hdr, e, boundaries, flags);
		}
		sprintf(buf, "--%s--", bbuf);
		putline(buf, mci);
		if (tTd(43, 35))
			printf("  ...%s\n", buf);
		boundaries[i] = NULL;

		/* skip the late "comment" epilogue */
		while (fgets(buf, sizeof buf, e->e_dfp) != NULL)
		{
			bt = mimeboundary(buf, boundaries);
			if (bt != MBT_NOTSEP)
				break;
			putxline(buf, mci, PXLF_MAPFROM|PXLF_STRIP8BIT);
			if (tTd(43, 99))
				printf("  ...%s", buf);
		}
		if (feof(e->e_dfp))
			bt = MBT_FINAL;
		if (tTd(43, 3))
			printf("\t\t\tmime7to8=>%s (multipart)\n",
				MimeBoundaryNames[bt]);
		return bt;
	}

	/*
	**  Message/* types -- recurse exactly once.
	*/

	if (strcasecmp(type, "message") == 0)
	{
		if (strcasecmp(subtype, "rfc822") != 0)
		{
			flags |= M87F_NO8BIT;
		}
		else
		{
			register char *q;
			auto HDR *hdr = NULL;

			putline("", mci);

			collect(e->e_dfp, FALSE, FALSE, &hdr, e);
			if (tTd(43, 101))
				putline("+++after collect", mci);
			putheader(mci, hdr, e);
			if (tTd(43, 101))
				putline("+++after putheader", mci);
			bt = mime7to8(mci, hdr, e, boundaries, flags);
			return bt;
		}
	}

	/*
	**  Non-compound body type.
	**
	**	First scan to see if it is legally translatable.  This
	**	means:
	**
	**	  * No long lines.
	**	  * No bare CR or NL bytes.
	**
	**	We'll also check to see if there are any 8-bit characters
	**	so we can make a good choice of final encoding method.
	*/

	nodecode = FALSE;
	has8bit = FALSE;
	if (cte != NULL && strcasecmp(cte, "base64") == 0)
		mf->mf_fill = mime_fill_base64;
	else if (cte != NULL && strcasecmp(cte, "quoted-printable") != 0)
		mf->mf_fill = mime_fill_qp;
	else
		nodecode = TRUE;

	maxlinelen = 0;
	if (!nodecode)
	{
		offset = mime_tell(mfp);
		if (offset == -1)
			syserr("mime7to8: cannot ftell on df%s", e->e_id);

		linelen = 0;
		while (!nodecode && (c = mime_getc(mfp)) != EOF)
		{
			if (bitset(0x80, c))
				has8bit = TRUE;
			else if (c == '\n' || c == '\0')
				nodecode = TRUE;
			else if (c == '\r')
			{
				c = mime_getc(mfp);
				if (c == '\n')
				{
					if (linelen > maxlinelen)
						maxlinelen = linelen;
					linelen = 0;
					continue;
				}
				nodecode = TRUE;
			}
			else
				linelen++;
		}
		if (linelen != 0 || maxlinelen > mci->mci_mailer->m_maxline)
			nodecode = TRUE;
		if (mime_seek(mfp, offset, SEEK_SET) < 0)
			syserr("mime7to8: cannot fseek on df%s", e->e_id)
		else
			mime_clearerr(mfp);
		mime_flush(mfp);
	}

	if (tTd(43, 8))
	{
		printf("mime7to8: cte=%s, maxlinelen=%d, nodecode=%d, has8bit=%d\n",
			cte == NULL ? "[none]" : cte,
			maxlinelen, nodecode, has8bit);
	}

	if (!nodecode)
	{
		/*
		**  Convert to 8-bit.  We know it will be well-behaved.
		*/

		if (has8bit)
			p = "8bit";
		else
			p = "7bit";
	}
	else
	{
		p = cte;
		mfp->mf_fill = mime_fill;
	}
	if (p != NULL)
	{
		sprintf(buf, "Content-Transfer-Encoding: %s", p);
		putline(buf, mci);
	}
	putline("", mci);
	mci->mci_flags &= ~MCIF_INHEADER;

	p = buf;
	linelen = 0;
	while ((c = mime_getc(mfp)) != EOF)
	{
		while (c == '\r')
		{
			c = mime_getc(mfp);
			if (c == '\n')
			{
				*p = '\0';
				putline(buf, mci);
				linelen = fromstate = 0;
				p = buf;
				continue;
			}
			*p++ = '\r';
		}
		if (mci->mci_mailer->m_linelimit > 0 &&
		    linelen > mci->mci_mailer->m_linelimit - 1)
		{
			/* line wrap */
			putc('!', mci->mci_out);
			fputs(mci->mci_mailer->m_eol, mci->mci_out);
			linelen = 0;
		}
		if (linelen == 0 && c == '.' &&
		    bitnset(M_XDOT, mci->mci_mailer->m_flags))
		{
			putc('.', mci->mci_out);
			linelen++;
		}
		if (linelen == fromstate && c == "From "[fromstate])
		{
			if (fromstate == 4)
			{
				/* hide From_? */
				if (bitnset(M_ESCFROM, mci->mci_mailer->m_flags))
				{
					putc('>', mci->mci_out);
					linelen++;
				}
				fputs("From ", mci->mci_out);
				fromstate = -1;
			}
			fromstate++;
			linelen++;
			continue;
		}
		if (fromstate != 0)
		{
			p = "From ";
			while (--fromstate >= 0)
				putc(*p++, mci->mci_out);
		}
		putc(c, mci->mci_out);
		linelen++;
	}
	if (tTd(43, 3))
		printf("\t\t\tmime7to8=>%s (basic)\n", MimeBoundaryNames[bt]);
	return bt;
}
/*
**  MIME_FILL_QP -- fill buffer doing Quoted-Printable decoding
**
**	Parameters:
**		mfp -- MIME file pointer.
**
**	Returns:
**		The next character in the input stream.
*/

int
mime_fill_qp(mfp)
	register MFILE;
{
	int c;

	/* first do the raw buffer fill */
	if (mime_fill_nl(mfp) == EOF)
		return EOF;

	/* now scan the buffer converting =XX */
	ip = op = mfp->mf_buf;
	il = mfp->mf_cnt;
	while (--il >= 0)
	{
		if (*ip != '=')
		{
			*op++ = *ip++;
			continue;
		}

		/* special handling for "=" sign */
		if (il >= 2 && !bitset(0x80, ip[1] | ip[2]) &&
		    isxdigit(ip[1]) && isxdigit(ip[2]))
		{
			/* this is an escape sequence */
			/* ASCII dependence ahead.... */
			if (isdigit(*++ip))
				c = *ip - '0';
			else
				c = (*ip & 07) + 9;
			c <<= 4;
			if (isdigit(*++ip))
				c |= *ip - '0';
			else
				c |= (*ip & 07) + 9;
			*op++ = c;
		}
		else if ((il == 2 && ip[1] == '\r' && ip[2] == '\n') ||
			 (il == 1 && ip[1] == '\n'))
		{
			/* this is a hidden newline */
			break;
		}

		/* bogus `=' -- what to do?  treat it as normal */
		*op++ = *ip++;
	}

	/* adjust buffer count to match reality */
	if ((mfp->mf_cnt = op - mfp->mf_buf - 1) <= 0)
		return EOF;
	return *mfp->mf_bp++;
}
/*
**  MIMEBOUNDARY -- determine if this line is a MIME boundary & its type
**
**	Parameters:
**		line -- the input line.
**		boundaries -- the set of currently pending boundaries.
**
**	Returns:
**		MBT_NOTSEP -- if this is not a separator line
**		MBT_INTERMED -- if this is an intermediate separator
**		MBT_FINAL -- if this is a final boundary
**		MBT_SYNTAX -- if this is a boundary for the wrong
**			enclosure -- i.e., a syntax error.
*/

int
mimeboundary(line, boundaries)
	register char *line;
	char **boundaries;
{
	int type;
	int i;
	int savec;

	if (line[0] != '-' || line[1] != '-' || boundaries == NULL)
		return MBT_NOTSEP;
	i = strlen(line);
	if (line[i - 1] == '\n')
		i--;
	if (tTd(43, 5))
		printf("mimeboundary: line=\"%.*s\"... ", i, line);
	while (line[i - 1] == ' ' || line[i - 1] == '\t')
		i--;
	if (i > 2 && strncmp(&line[i - 2], "--", 2) == 0)
	{
		type = MBT_FINAL;
		i -= 2;
	}
	else
		type = MBT_INTERMED;

	savec = line[i];
	line[i] = '\0';
	/* XXX should check for improper nesting here */
	if (isboundary(&line[2], boundaries) < 0)
		type = MBT_NOTSEP;
	line[i] = savec;
	if (tTd(43, 5))
		printf("%s\n", MimeBoundaryNames[type]);
	return type;
}
/*
**  DEFCHARSET -- return default character set for message
**
**	The first choice for character set is for the mailer
**	corresponding to the envelope sender.  If neither that
**	nor the global configuration file has a default character
**	set defined, return "unknown-8bit" as recommended by
**	RFC 1428 section 3.
**
**	Parameters:
**		e -- the envelope for this message.
**
**	Returns:
**		The default character set for that mailer.
*/

char *
defcharset(e)
	register ENVELOPE *e;
{
	if (e != NULL && e->e_from.q_mailer != NULL &&
	    e->e_from.q_mailer->m_defcharset != NULL)
		return e->e_from.q_mailer->m_defcharset;
	if (DefaultCharSet != NULL)
		return DefaultCharSet;
	return "unknown-8bit";
}
/*
**  ISBOUNDARY -- is a given string a currently valid boundary?
**
**	Parameters:
**		line -- the current input line.
**		boundaries -- the list of valid boundaries.
**
**	Returns:
**		The index number in boundaries if the line is found.
**		-1 -- otherwise.
**
*/

int
isboundary(line, boundaries)
	char *line;
	char **boundaries;
{
	register int i;

	for (i = 0; boundaries[i] != NULL; i++)
	{
		if (strcmp(line, boundaries[i]) == 0)
			return i;
	}
	return -1;
}

/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 1999  Free Software Foundation, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * Most of this file was originally the source file "inflate.c", written
 * by Mark Adler.  It has been very heavily modified.  In particular, the
 * original would run through the whole file at once, and this version can
 * be stopped and restarted on any boundary during the decompression process.
 *
 * The license and header comments that file are included here.
 */

/* inflate.c -- Not copyrighted 1992 by Mark Adler
   version c10p1, 10 January 1993 */

/* You can do whatever you like with this source file, though I would
   prefer that if you modify it and redistribute it that you include
   comments to that effect with your name and the date.  Thank you.
 */

/*
   Inflate deflated (PKZIP's method 8 compressed) data.  The compression
   method searches for as much of the current string of bytes (up to a
   length of 258) in the previous 32K bytes.  If it doesn't find any
   matches (of at least length 3), it codes the next byte.  Otherwise, it
   codes the length of the matched string and its distance backwards from
   the current position.  There is a single Huffman code that codes both
   single bytes (called "literals") and match lengths.  A second Huffman
   code codes the distance information, which follows a length code.  Each
   length or distance code actually represents a base value and a number
   of "extra" (sometimes zero) bits to get to add to the base value.  At
   the end of each deflated block is a special end-of-block (EOB) literal/
   length code.  The decoding process is basically: get a literal/length
   code; if EOB then done; if a literal, emit the decoded byte; if a
   length then get the distance and emit the referred-to bytes from the
   sliding window of previously emitted data.

   There are (currently) three kinds of inflate blocks: stored, fixed, and
   dynamic.  The compressor deals with some chunk of data at a time, and
   decides which method to use on a chunk-by-chunk basis.  A chunk might
   typically be 32K or 64K.  If the chunk is uncompressible, then the
   "stored" method is used.  In this case, the bytes are simply stored as
   is, eight bits per byte, with none of the above coding.  The bytes are
   preceded by a count, since there is no longer an EOB code.

   If the data is compressible, then either the fixed or dynamic methods
   are used.  In the dynamic method, the compressed data is preceded by
   an encoding of the literal/length and distance Huffman codes that are
   to be used to decode this block.  The representation is itself Huffman
   coded, and so is preceded by a description of that code.  These code
   descriptions take up a little space, and so for small blocks, there is
   a predefined set of codes, called the fixed codes.  The fixed method is
   used if the block codes up smaller that way (usually for quite small
   chunks), otherwise the dynamic method is used.  In the latter case, the
   codes are customized to the probabilities in the current block, and so
   can code it much better than the pre-determined fixed codes.

   The Huffman codes themselves are decoded using a mutli-level table
   lookup, in order to maximize the speed of decoding plus the speed of
   building the decoding tables.  See the comments below that precede the
   lbits and dbits tuning parameters.
 */


/*
   Notes beyond the 1.93a appnote.txt:

   1. Distance pointers never point before the beginning of the output
      stream.
   2. Distance pointers can point back across blocks, up to 32k away.
   3. There is an implied maximum of 7 bits for the bit length table and
      15 bits for the actual data.
   4. If only one code exists, then it is encoded using one bit.  (Zero
      would be more efficient, but perhaps a little confusing.)  If two
      codes exist, they are coded using one bit each (0 and 1).
   5. There is no way of sending zero distance codes--a dummy must be
      sent if there are none.  (History: a pre 2.0 version of PKZIP would
      store blocks with no distance codes, but this was discovered to be
      too harsh a criterion.)  Valid only for 1.93a.  2.04c does allow
      zero distance codes, which is sent as one code of zero bits in
      length.
   6. There are up to 286 literal/length codes.  Code 256 represents the
      end-of-block.  Note however that the static length tree defines
      288 codes just to fill out the Huffman codes.  Codes 286 and 287
      cannot be used though, since there is no length base or extra bits
      defined for them.  Similarly, there are up to 30 distance codes.
      However, static trees define 32 codes (all 5 bits) to fill out the
      Huffman codes, but the last two had better not show up in the data.
   7. Unzip can check dynamic Huffman blocks for complete code sets.
      The exception is that a single code would not be complete (see #4).
   8. The five bits following the block type is really the number of
      literal codes sent minus 257.
   9. Length codes 8,16,16 are interpreted as 13 length codes of 8 bits
      (1+6+6).  Therefore, to output three times the length, you output
      three codes (1+1+1), whereas to output four times the same length,
      you only need two codes (1+3).  Hmm.
  10. In the tree reconstruction algorithm, Code = Code + Increment
      only if BitLength(i) is not zero.  (Pretty obvious.)
  11. Correction: 4 Bits: # of Bit Length codes - 4     (4 - 19)
  12. Note: length code 284 can represent 227-258, but length code 285
      really is 258.  The last length deserves its own, short code
      since it gets used a lot in very redundant files.  The length
      258 is special since 258 - 3 (the min match length) is 255.
  13. The literal/length and distance code bit lengths are read as a
      single stream of lengths.  It is possible (and advantageous) for
      a repeat code (16, 17, or 18) to go across the boundary between
      the two sets of lengths.
 */

#ifndef NO_DECOMPRESSION

#include "shared.h"

#include "filesys.h"

/* used to tell if "read" should be redirected to "gunzip_read" */
int compressed_file;

/* identify active decompressor */
int decomp_type;

struct decomp_entry decomp_table[NUM_DECOM] =
{
	{"gz",gunzip_test_header,gunzip_close,gunzip_read},
	{"lzma",dec_lzma_open,dec_lzma_close,dec_lzma_read},
	{"lz4",dec_lz4_open,dec_lz4_close,dec_lz4_read},
	{"vhd",dec_vhd_open,dec_vhd_close,dec_vhd_read},
};

/* internal variables only */
static unsigned long long gzip_data_offset;
static unsigned long long gzip_filepos;
static unsigned long long gzip_fsmax;
static unsigned long long saved_filepos;
static unsigned int gzip_crc;

/* internal extra variables for use of inflate code */
static unsigned int block_type;
static unsigned int block_len;
static unsigned int last_block;
static unsigned int code_state;


/* Function prototypes */
static void initialize_tables (void);

/*
 *  Linear allocator.
 */

static unsigned int linalloc_topaddr;
unsigned char * linalloc_buf;
//static void *
//linalloc (unsigned long size)
//{
//  linalloc_topaddr = (linalloc_topaddr - size) & ~3;
//  return (void *) linalloc_topaddr;
//}

static void
reset_linalloc (void)
{
//  linalloc_topaddr = RAW_ADDR ((saved_mem_upper << 10) + 0x100000);
	linalloc_topaddr = (grub_size_t)linalloc_buf + 0x100000;
}


/* internal variable swap function */
static void gunzip_swap_values (void);
static void
gunzip_swap_values (void)
{
  register unsigned long long itmp;

  /* swap filepos */
  itmp = filepos;
  filepos = gzip_filepos;
  gzip_filepos = itmp;

  /* swap filemax */
  itmp = filemax;
  filemax = gzip_filemax;
  gzip_filemax = itmp;

  /* swap fsmax */
  itmp = fsmax;
  fsmax = gzip_fsmax;
  gzip_fsmax = itmp;
}


/* internal function for eating variable-length header fields */
static unsigned int bad_field (unsigned int len);
static unsigned int
bad_field (unsigned int len)
{
  char ch = 1;
  unsigned int not_retval = 1;

  do
    {
      if (len == 0)
	break;
      if (len == (unsigned int)-1)
      {
	if (ch == 0)
	  break;
      }	else
	  len--;
      
      not_retval = grub_read ((unsigned long long)(grub_size_t)&ch, 1, 0xedde0d90);
    }
  while (not_retval);

  return (! not_retval);
}


/* Little-Endian defines for the 2-byte magic number for gzip files */
#define GZIP_HDR_LE      0x8B1F
#define OLD_GZIP_HDR_LE  0x9E1F

/* Compression methods (see algorithm.doc) */
#define STORED      0
#define COMPRESSED  1
#define PACKED      2
#define LZHED       3
/* methods 4 to 7 reserved */
#define DEFLATED    8
#define MAX_METHODS 9

/* gzip flag byte */
#define ASCII_FLAG   0x01	/* bit 0 set: file probably ascii text */
#define CONTINUATION 0x02	/* bit 1 set: continuation of multi-part gzip file */
#define EXTRA_FIELD  0x04	/* bit 2 set: extra field present */
#define ORIG_NAME    0x08	/* bit 3 set: original file name present */
#define COMMENT      0x10	/* bit 4 set: file comment present */
#define ENCRYPTED    0x20	/* bit 5 set: file is encrypted */
#define RESERVED     0xC0	/* bit 6,7:   reserved */

#define UNSUPP_FLAGS (CONTINUATION|ENCRYPTED|RESERVED)

/* inflate block codes */
#define INFLATE_STORED    0
#define INFLATE_FIXED     1
#define INFLATE_DYNAMIC   2

//typedef unsigned char uch;
//typedef unsigned short ush;
//typedef unsigned long ulg;

/*
 *  Window Size
 *
 *  This must be a power of two, and at least 32K for zip's deflate method
 */

#define WSIZE 0x8000UL

int gunzip_test_header (void);
int
gunzip_test_header (void)
{
  unsigned char buf[10];

  /* check lz4 */
  if (dec_lz4_open ())
	goto test_dec;
  /* check lzma */
  if (dec_lzma_open ())
	goto test_dec;
  if (dec_vhd_open())
	goto test_dec;
  /* "compressed_file" is already reset to zero by this point */

  /*
   *  This checks if the file is gzipped.  If a problem occurs here
   *  (other than a real error with the disk) then we don't think it
   *  is a compressed file, and simply mark it as such.
   */
  gzip_filemax = filemax;
  if (no_decompression
      || grub_read ((unsigned long long)(grub_size_t)(char *)buf, 10, 0xedde0d90) != 10
      || ((*((unsigned short *) buf) != GZIP_HDR_LE)
	  && (*((unsigned short *) buf) != OLD_GZIP_HDR_LE)))
    {
      filepos = 0;
      return ! errnum;
    }

  /*
   *  This does consistency checking on the header data.  If a
   *  problem occurs from here on, then we have corrupt or otherwise
   *  bad data, and the error should be reported to the user.
   */
  if (buf[2] != DEFLATED
      || (buf[3] & UNSUPP_FLAGS)
      || ((buf[3] & EXTRA_FIELD)
	  && (grub_read ((unsigned long long)(grub_size_t)(char *)buf, 2, 0xedde0d90) != 2
	      || bad_field (*((unsigned short *) buf))))
      || ((buf[3] & ORIG_NAME) && bad_field (-1))
      || ((buf[3] & COMMENT) && bad_field (-1)))
    {
      if (! errnum)
	errnum = ERR_BAD_GZIP_HEADER;
      return 0;
    }

  gzip_data_offset = filepos;
  filepos = filemax - 8;
  
  if (grub_read ((unsigned long long)(grub_size_t)(char *)buf, 8, 0xedde0d90) != 8)
    {
      if (! errnum)
	errnum = ERR_BAD_GZIP_HEADER;
      return 0;
    }

  gzip_crc = *((unsigned int *) buf);
  gzip_fsmax = gzip_filemax = *((unsigned int *) (buf + 4));

  if (!linalloc_buf)
    linalloc_buf = grub_malloc (0x100000);

  initialize_tables ();

  decomp_type = DECOMP_TYPE_GZ;
  compressed_file = 1;
  gunzip_swap_values ();
  /*
   *  Now "gzip_*" values refer to the compressed data.
   */
  filepos = 0;

test_dec:
  if (grub_read((unsigned long long)(grub_size_t)(char *)buf, 1,GRUB_READ) != 1LL)
  {
    if (debug)
      printf("\nWarning:%s Compressed data detected but failed to decompress - using raw data!\n",decomp_table[decomp_type].name);
    filemax = gzip_filemax;
    decomp_table[decomp_type].close_func();
    compressed_file = 0;
  }
  filepos = 0;
  return 1;
}

void  gunzip_close (void);
void 
gunzip_close (void)
{
	grub_free(linalloc_buf);
  linalloc_buf = 0;
}


/* Huffman code lookup table entry--this entry is four bytes for machines
   that have 16-bit pointers (e.g. PC's in the small or medium model).
   Valid extra bits are 0..13.  e == 15 is EOB (end of block), e == 16
   means that v is a literal, 16 < e < 32 means that v is a pointer to
   the next table, which codes e - 16 bits, and lastly e == 99 indicates
   an unused code.  If a code with e == 99 is looked up, this implies an
   error in the data. */
struct huft
{
  unsigned char e;		/* number of extra bits or operation */
  unsigned char b;		/* number of bits in this code or subcode */
  union
    {
      unsigned short n;		/* literal, length base, or distance base */
      struct huft *t;		/* pointer to next level of table */
    }
  v;
};


/* The inflate algorithm uses a sliding 32K byte window on the uncompressed
   stream to find repeated byte strings.  This is implemented here as a
   circular buffer.  The index is updated simply by incrementing and then
   and'ing with 0x7fff (32K-1). */
/* It is left to other modules to supply the 32K area.  It is assumed
   to be usable as if it were declared "uch slide[32768];" or as just
   "uch *slide;" and then malloc'ed in the latter case.  The definition
   must be in unzip.h, included above. */


/* sliding window in uncompressed data */
static unsigned char slide[WSIZE];

/* current position in slide */
static unsigned int wp;


/* Tables for deflate from PKZIP's appnote.txt. */
static unsigned bitorder[] =
{				/* Order of the bit length code lengths */
  16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
static unsigned short cplens[] =
{				/* Copy lengths for literal codes 257..285 */
  3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0, 0};
	/* note: see note #13 above about the 258 in this list. */
static unsigned short cplext[] =
{				/* Extra bits for literal codes 257..285 */
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 99, 99};	/* 99==invalid */
static unsigned short cpdist[] =
{				/* Copy offsets for distance codes 0..29 */
  1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
  257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
  8193, 12289, 16385, 24577};
static unsigned short cpdext[] =
{				/* Extra bits for distance codes */
  0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
  7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
  12, 12, 13, 13};


/*
   Huffman code decoding is performed using a multi-level table lookup.
   The fastest way to decode is to simply build a lookup table whose
   size is determined by the longest code.  However, the time it takes
   to build this table can also be a factor if the data being decoded
   is not very long.  The most common codes are necessarily the
   shortest codes, so those codes dominate the decoding time, and hence
   the speed.  The idea is you can have a shorter table that decodes the
   shorter, more probable codes, and then point to subsidiary tables for
   the longer codes.  The time it costs to decode the longer codes is
   then traded against the time it takes to make longer tables.

   This results of this trade are in the variables lbits and dbits
   below.  lbits is the number of bits the first level table for literal/
   length codes can decode in one step, and dbits is the same thing for
   the distance codes.  Subsequent tables are also less than or equal to
   those sizes.  These values may be adjusted either when all of the
   codes are shorter than that, in which case the longest code length in
   bits is used, or when the shortest code is *longer* than the requested
   table size, in which case the length of the shortest code in bits is
   used.

   There are two different values for the two tables, since they code a
   different number of possibilities each.  The literal/length table
   codes 286 possible values, or in a flat code, a little over eight
   bits.  The distance table codes 30 possible values, or a little less
   than five bits, flat.  The optimum values for speed end up being
   about one bit more than those, so lbits is 8+1 and dbits is 5+1.
   The optimum values may differ though from machine to machine, and
   possibly even between compilers.  Your mileage may vary.
 */


static unsigned int lbits = 9;	/* bits in base literal/length lookup table */
static unsigned int dbits = 6;	/* bits in base distance lookup table */


/* If BMAX needs to be larger than 16, then h and x[] should be ulg. */
#define BMAX 16		/* maximum bit length of any code (16 for explode) */
#define N_MAX 288	/* maximum number of codes in any set */


static unsigned int hufts;	/* track memory usage */


/* Macros for inflate() bit peeking and grabbing.
   The usage is:

        NEEDBITS(j)
        x = b & mask_bits[j];
        DUMPBITS(j)

   where NEEDBITS makes sure that b has at least j bits in it, and
   DUMPBITS removes the bits from b.  The macros use the variable k
   for the number of bits in b.  Normally, b and k are register
   variables for speed, and are initialized at the beginning of a
   routine that uses these macros from a global bit buffer and count.

   If we assume that EOB will be the longest code, then we will never
   ask for bits with NEEDBITS that are beyond the end of the stream.
   So, NEEDBITS should not read any more bytes than are needed to
   meet the request.  Then no bytes need to be "returned" to the buffer
   at the end of the last block.

   However, this assumption is not true for fixed blocks--the EOB code
   is 7 bits, but the other literal/length codes can be 8 or 9 bits.
   (The EOB code is shorter than other codes because fixed blocks are
   generally short.  So, while a block always has an EOB, many other
   literal/length codes have a significantly lower probability of
   showing up at all.)  However, by making the first table have a
   lookup of seven bits, the EOB code will be found in that first
   lookup, and so will not require that too many bits be pulled from
   the stream.
 */

static unsigned int bb;	/* bit buffer */
static unsigned int bk;	/* bits in bit buffer */

static unsigned short mask_bits[] =
{
  0x0000,
  0x0001, 0x0003, 0x0007, 0x000f, 0x001f, 0x003f, 0x007f, 0x00ff,
  0x01ff, 0x03ff, 0x07ff, 0x0fff, 0x1fff, 0x3fff, 0x7fff, 0xffff
};

#define NEEDBITS(n) do {while(k<(n)){b|=get_byte()<<k;k+=8;}} while (0)
#define DUMPBITS(n) do {b>>=(n);k-=(n);} while (0)

#define INBUFSIZ  0x2000

static unsigned char inbuf[INBUFSIZ];
static unsigned int bufloc = 0;

static unsigned int get_byte (void);
static unsigned int
get_byte (void)
{
  if (filepos == gzip_data_offset || bufloc == INBUFSIZ)
    {
      bufloc = 0;
      grub_read ((unsigned long long)(grub_size_t)(char *)inbuf, INBUFSIZ, 0xedde0d90);
    }

  return inbuf[bufloc++];
}

/* decompression global pointers */
static struct huft *tl;		/* literal/length code table */
static struct huft *td;		/* distance code table */
static unsigned int bl;			/* lookup bits for tl */
static unsigned int bd;			/* lookup bits for td */

static unsigned int c[BMAX + 1];	/* bit length count table */
static struct huft *u[BMAX];		/* table stack */
static unsigned int v[N_MAX];	/* values in order of bit length */
static unsigned int x[BMAX + 1];	/* bit offsets, then code stack */
static unsigned int lh[288];		/* length list for huft_build */
static unsigned int ll[286 + 30];	/* literal/length and distance code lengths */

/* more function prototypes */
static unsigned int huft_build (unsigned int *, unsigned int, unsigned int, unsigned short *, unsigned short *,
		       struct huft **, unsigned int *);
static int inflate_codes_in_window (void);


/* Given a list of code lengths and a maximum table size, make a set of
   tables to decode that set of codes.  Return zero on success, one if
   the given code set is incomplete (the tables are still built in this
   case), two if the input is invalid (all zero length codes or an
   oversubscribed set of lengths), and three if not enough memory. */
static unsigned int huft_build (unsigned int *b, unsigned int n, unsigned int s, unsigned short * d,
	    unsigned short * e,	struct huft **t, unsigned int *m);
static unsigned int
huft_build (unsigned int *b,	/* code lengths in bits (all assumed <= BMAX) */
	    unsigned int n,	/* number of codes (assumed <= N_MAX) */
	    unsigned int s,	/* number of simple-valued codes (0..s-1) */
	    unsigned short * d,	/* list of base values for non-simple codes */
	    unsigned short * e,	/* list of extra bits for non-simple codes */
	    struct huft **t,	/* result: starting table */
	    unsigned int *m)	/* maximum lookup bits, returns actual */
{
  unsigned int a;		/* counter for codes of length k */
//unsigned int c[BMAX + 1];	/* bit length count table */
  unsigned int f;		/* i repeats in table every f entries */
  int g;			/* maximum code length */
  int h;			/* table level */
  register unsigned int i;	/* counter, current code */
  register unsigned int j;	/* counter */
  register int k;		/* number of bits in current code */
  unsigned int l;		/* bits per table (returned in m) */
  register unsigned int *p;	/* pointer into c[], b[], or v[] */
  register struct huft *q;	/* points to current table */
  struct huft r;		/* table entry for structure assignment */
//struct huft *u[BMAX];		/* table stack */
//unsigned int v[N_MAX];	/* values in order of bit length */
  register int w;		/* bits before this table == (l * h) */
//unsigned long x[BMAX + 1];	/* bit offsets, then code stack */
  unsigned int *xp;		/* pointer into x */
  int y;			/* number of dummy codes added */
  unsigned int z;		/* number of entries in current table */

  /* Generate counts for each bit length */
  memset ((char *) c, 0, sizeof (c));
  p = b;
  i = n;
  do
    {
      c[*p]++;			/* assume all entries <= BMAX */
      p++;			/* Can't combine with above line (Solaris bug) */
    }
  while (--i);
  if (c[0] == n)		/* null input--all zero length codes */
    {
      *t = (struct huft *) NULL;
      *m = 0;
      return 0;
    }

  /* Find minimum and maximum length, bound *m by those */
  l = *m;
  for (j = 1; j <= BMAX; j++)
    if (c[j])
      break;
  k = j;			/* minimum code length */
  if (l < j)
    l = j;
  for (i = BMAX; i; i--)
    if (c[i])
      break;
  g = i;			/* maximum code length */
  if (l > i)
    l = i;
  *m = l;

  /* Adjust last length count to fill out codes, if needed */
  for (y = 1 << j; j < i; j++, y <<= 1)
    if ((y -= c[j]) < 0)
      return 2;			/* bad input: more codes than bits */
  if ((y -= c[i]) < 0)
    return 2;
  c[i] += y;

  /* Generate starting offsets into the value table for each length */
  x[1] = j = 0;
  p = c + 1;
  xp = x + 2;
  while (--i)
    {				/* note that i == g from above */
      *xp++ = (j += *p++);
    }

  /* Make a table of values in order of bit lengths */
  p = b;
  i = 0;
  do
    {
      if ((j = *p++) != 0)
	v[x[j]++] = i;
    }
  while (++i < n);

  /* Generate the Huffman codes and for each, make the table entries */
  x[0] = i = 0;			/* first Huffman code is zero */
  p = v;			/* grab values in bit order */
  h = -1;			/* no tables yet--level -1 */
  w = -l;			/* bits decoded == (l * h) */
  u[0] = (struct huft *) NULL;	/* just to keep compilers happy */
  q = (struct huft *) NULL;	/* ditto */
  z = 0;			/* ditto */

  /* go through the bit lengths (k already is bits in shortest code) */
  for (; k <= g; k++)
    {
      a = c[k];
      while (a--)
	{
	  /* here i is the Huffman code of length k bits for value *p */
	  /* make tables up to required level */
	  while (k > w + (int)l)
	    {
	      h++;
	      w += l;		/* previous table always l bits */

	      /* compute minimum size table less than or equal to l bits */
	      z = (z = g - w) > l ? l : z;	/* upper limit on table size */
	      if ((f = 1 << (j = k - w)) > a + 1)	/* try a k-w bit table */
		{		/* too few codes for k-w bit table */
		  f -= a + 1;	/* deduct codes from patterns left */
		  xp = c + k;
		  while (++j < z)	/* try smaller tables up to z bits */
		    {
		      if ((f <<= 1) <= *++xp)
			break;	/* enough codes to use up j bits */
		      f -= *xp;	/* else deduct codes from patterns */
		    }
		}
	      z = 1 << j;	/* table entries for j-bit table */

	      /* allocate and link in new table */
	      linalloc_topaddr -= (z + 1) * sizeof (struct huft);
	      linalloc_topaddr &= ~3;
	      q = (struct huft *)(grub_size_t) linalloc_topaddr;

	      hufts += z + 1;	/* track memory usage */
	      *t = q + 1;	/* link to list for huft_free() */
	      *(t = &(q->v.t)) = (struct huft *) NULL;
	      u[h] = ++q;	/* table starts after link */

	      /* connect to last table, if there is one */
	      if (h)
		{
		  x[h] = i;	/* save pattern for backing up */
		  r.b = (unsigned char) l;	/* bits to dump before this table */
		  r.e = (unsigned char) (16 + j);	/* bits in this table */
		  r.v.t = q;	/* pointer to this table */
		  j = i >> (w - l);	/* (get around Turbo C bug) */
		  u[h - 1][j] = r;	/* connect to last table */
		}
	    }

	  /* set up table entry in r */
	  r.b = (unsigned char) (k - w);
	  if (p >= v + n)
	    r.e = 99;		/* out of values--invalid code */
	  else if (*p < s)
	    {
	      r.e = (unsigned char) (*p < 256 ? 16 : 15);	/* 256 is end-of-block code */
	      r.v.n = (unsigned short) (*p);	/* simple code is just the value */
	      p++;		/* one compiler does not like *p++ */
	    }
	  else
	    {
	      r.e = (unsigned char) e[*p - s];	/* non-simple--look up in lists */
	      r.v.n = d[*p++ - s];
	    }

	  /* fill code-like entries with r */
	  f = 1 << (k - w);
	  for (j = i >> w; j < z; j += f)
	    q[j] = r;

	  /* backwards increment the k-bit code i */
	  for (j = 1 << (k - 1); i & j; j >>= 1)
	    i ^= j;
	  i ^= j;

	  /* backup over finished tables */
	  while ((i & ((1 << w) - 1)) != x[h])
	    {
	      h--;		/* don't need to update q */
	      w -= l;
	    }
	}
    }

  /* Return true (1) if we were given an incomplete table */
  return y != 0 && g != 1;
}


/*
 *  inflate (decompress) the codes in a deflated (compressed) block.
 *  Return an error code or zero if it all goes ok.
 */

static unsigned int inflate_n, inflate_d;

static int inflate_codes_in_window (void);
static int
inflate_codes_in_window (void)
{
  register unsigned int e;	/* table entry flag/number of extra bits */
  unsigned int n, d;		/* length and index for copy */
  unsigned int w;		/* current window position */
  struct huft *t;		/* pointer to table entry */
  unsigned int ml, md;		/* masks for bl and bd bits */
  register unsigned int b;	/* bit buffer */
  register unsigned int k;	/* number of bits in bit buffer */

  /* make local copies of globals */
  d = inflate_d;
  n = inflate_n;
  b = bb;			/* initialize bit buffer */
  k = bk;
  w = wp;			/* initialize window position */

  /* inflate the coded data */
  ml = mask_bits[bl];		/* precompute masks for speed */
  md = mask_bits[bd];
  for (;;)			/* do until end of block */
    {
      if (!code_state)
	{
	  NEEDBITS (bl);
	  if ((e = (t = tl + (b & ml))->e) > 16)
	    do
	      {
		if (e == 99)
		  {
		    errnum = ERR_BAD_GZIP_DATA;
		    return 0;
		  }
		DUMPBITS (t->b);
		e -= 16;
		NEEDBITS (e);
	      }
	    while ((e = (t = t->v.t + (b & mask_bits[e]))->e) > 16);
	  DUMPBITS (t->b);

	  if (e == 16)		/* then it's a literal */
	    {
	      slide[w++] = (unsigned char)(t->v.n);
	      if (w == WSIZE)
		break;
	    }
	  else
	    /* it's an EOB or a length */
	    {
	      /* exit if end of block */
	      if (e == 15)
		{
		  block_len = 0;
		  break;
		}

	      /* get length of block to copy */
	      NEEDBITS (e);
	      n = t->v.n + (b & mask_bits[e]);
	      DUMPBITS (e);

	      /* decode distance of block to copy */
	      NEEDBITS (bd);
	      if ((e = (t = td + (b & md))->e) > 16)
		do
		  {
		    if (e == 99)
		      {
			errnum = ERR_BAD_GZIP_DATA;
			return 0;
		      }
		    DUMPBITS (t->b);
		    e -= 16;
		    NEEDBITS (e);
		  }
		while ((e = (t = t->v.t + (b & mask_bits[e]))->e)
		       > 16);
	      DUMPBITS (t->b);
	      NEEDBITS (e);
	      d = w - t->v.n - (b & mask_bits[e]);
	      DUMPBITS (e);
	      code_state++;
	    }
	}

      if (code_state)
	{
	  /* do the copy */
	  do
	    {
	      n -= (e = (e = WSIZE - ((d &= WSIZE - 1) > w ? d : w)) > n ? n
		    : e);
	      if (w - d >= e)
		{
		  memmove (slide + w, slide + d, e);
		  w += e;
		  d += e;
		}
	      else
		/* purposefully use the overlap for extra copies here!! */
		{
		  while (e--)
		    slide[w++] = slide[d++];
		}
	      if (w == WSIZE)
		break;
	    }
	  while (n);

	  if (!n)
	    code_state--;

	  /* did we break from the loop too soon? */
	  if (w == WSIZE)
	    break;
	}
    }

  /* restore the globals from the locals */
  inflate_d = d;
  inflate_n = n;
  wp = w;			/* restore global window pointer */
  bb = b;			/* restore global bit buffer */
  bk = k;

  return !block_len;
}


/* get header for an inflated type 0 (stored) block. */
static void init_stored_block (void);
static void
init_stored_block (void)
{
  register unsigned int b;	/* bit buffer */
  register unsigned int k;		/* number of bits in bit buffer */

  /* make local copies of globals */
  b = bb;			/* initialize bit buffer */
  k = bk;

  /* go to byte boundary */
  DUMPBITS (k & 7);

  /* get the length and its complement */
  NEEDBITS (16);
  block_len = (b & 0xffff);
  DUMPBITS (16);
  NEEDBITS (16);
  if (block_len != ((~b) & 0xffff))
    errnum = ERR_BAD_GZIP_DATA;
  DUMPBITS (16);

  /* restore global variables */
  bb = b;
  bk = k;
}


/* get header for an inflated type 1 (fixed Huffman codes) block.  We should
   either replace this with a custom decoder, or at least precompute the
   Huffman tables. */
static void init_fixed_block ();
static void
init_fixed_block ()
{
  unsigned int i;		/* temporary variable */
//unsigned int lh[288];	/* length list for huft_build */

  /* set up literal table */
  for (i = 0; i < 144; i++)
    lh[i] = 8;
  for (; i < 256; i++)
    lh[i] = 9;
  for (; i < 280; i++)
    lh[i] = 7;
  for (; i < 288; i++)		/* make a complete, but wrong code set */
    lh[i] = 8;
  bl = 7;
  if ((i = huft_build (lh, 288, 257, cplens, cplext, &tl, &bl)) != 0)
    {
      errnum = ERR_BAD_GZIP_DATA;
      return;
    }

  /* set up distance table */
  for (i = 0; i < 30; i++)	/* make an incomplete code set */
    lh[i] = 5;
  bd = 5;
  if ((i = huft_build (lh, 30, 0, cpdist, cpdext, &td, &bd)) > 1)
    {
      errnum = ERR_BAD_GZIP_DATA;
      return;
    }

  /* indicate we're now working on a block */
  code_state = 0;
  block_len++;
}


/* get header for an inflated type 2 (dynamic Huffman codes) block. */
static void init_dynamic_block (void);
static void
init_dynamic_block (void)
{
  unsigned int i;		/* temporary variables */
  unsigned int j;
  unsigned int l;		/* last length */
  unsigned int m;		/* mask for bit lengths table */
  unsigned int n;		/* number of lengths to get */
  unsigned int nb;		/* number of bit length codes */
  unsigned int na;		/* number of literal/length codes */
  unsigned int nd;		/* number of distance codes */
//unsigned int ll[286 + 30];	/* literal/length and distance code lengths */
  register unsigned int b;	/* bit buffer */
  register unsigned int k;	/* number of bits in bit buffer */

  /* make local bit buffer */
  b = bb;
  k = bk;

  /* read in table lengths */
  NEEDBITS (5);
  na = 257 + (b & 0x1f);	/* number of literal/length codes */
  DUMPBITS (5);
  NEEDBITS (5);
  nd = 1 + (b & 0x1f);		/* number of distance codes */
  DUMPBITS (5);
  NEEDBITS (4);
  nb = 4 + (b & 0xf);		/* number of bit length codes */
  DUMPBITS (4);
  if (na > 286 || nd > 30)
    {
      errnum = ERR_BAD_GZIP_DATA;
      return;
    }

  /* read in bit-length-code lengths */
  for (j = 0; j < nb; j++)
    {
      NEEDBITS (3);
      ll[bitorder[j]] = b & 7;
      DUMPBITS (3);
    }
  for (; j < 19; j++)
    ll[bitorder[j]] = 0;

  /* build decoding table for trees--single level, 7 bit lookup */
  bl = 7;
  if ((i = huft_build (ll, 19, 19, NULL, NULL, &tl, &bl)) != 0)
    {
      errnum = ERR_BAD_GZIP_DATA;
      return;
    }

  /* read in literal and distance code lengths */
  n = na + nd;
  m = mask_bits[bl];
  i = l = 0;
  while (i < n)
    {
      NEEDBITS (bl);
      j = (td = tl + (b & m))->b;
      DUMPBITS (j);
      j = td->v.n;
      if (j < 16)		/* length of code in bits (0..15) */
	ll[i++] = l = j;	/* save last length in l */
      else if (j == 16)		/* repeat last length 3 to 6 times */
	{
	  NEEDBITS (2);
	  j = 3 + (b & 3);
	  DUMPBITS (2);
	  if (i + j > n)
	    {
	      errnum = ERR_BAD_GZIP_DATA;
	      return;
	    }
	  while (j--)
	    ll[i++] = l;
	}
      else if (j == 17)		/* 3 to 10 zero length codes */
	{
	  NEEDBITS (3);
	  j = 3 + (b & 7);
	  DUMPBITS (3);
	  if (i + j > n)
	    {
	      errnum = ERR_BAD_GZIP_DATA;
	      return;
	    }
	  while (j--)
	    ll[i++] = 0;
	  l = 0;
	}
      else
	/* j == 18: 11 to 138 zero length codes */
	{
	  NEEDBITS (7);
	  j = 11 + (b & 0x7f);
	  DUMPBITS (7);
	  if (i + j > n)
	    {
	      errnum = ERR_BAD_GZIP_DATA;
	      return;
	    }
	  while (j--)
	    ll[i++] = 0;
	  l = 0;
	}
    }

  /* free decoding table for trees */
  reset_linalloc ();

  /* restore the global bit buffer */
  bb = b;
  bk = k;

  /* build the decoding tables for literal/length and distance codes */
  bl = lbits;
  if ((i = huft_build (ll, na, 257, cplens, cplext, &tl, &bl)) != 0)
    {
      errnum = ERR_BAD_GZIP_DATA;
      return;
    }
  bd = dbits;
  if ((i = huft_build (ll + na, nd, 0, cpdist, cpdext, &td, &bd)) != 0)
    {
      errnum = ERR_BAD_GZIP_DATA;
      return;
    }

  /* indicate we're now working on a block */
  code_state = 0;
  block_len++;
}

static void get_new_block (void);
static void
get_new_block (void)
{
  register unsigned int b;	/* bit buffer */
  register unsigned int k;	/* number of bits in bit buffer */

  hufts = 0;

  /* make local bit buffer */
  b = bb;
  k = bk;

  /* read in last block bit */
  NEEDBITS (1);
  last_block = b & 1;
  DUMPBITS (1);

  /* read in block type */
  NEEDBITS (2);
  block_type = b & 3;
  DUMPBITS (2);

  /* restore the global bit buffer */
  bb = b;
  bk = k;

  if (block_type == INFLATE_STORED)
    init_stored_block ();
  if (block_type == INFLATE_FIXED)
    init_fixed_block ();
  if (block_type == INFLATE_DYNAMIC)
    init_dynamic_block ();
}

static void inflate_window (void);
static void
inflate_window (void)
{
  /* initialize window */
  wp = 0;

  /*
   *  Main decompression loop.
   */

  while (wp < WSIZE && !errnum)
    {
      if (!block_len)
	{
	  if (last_block)
	    break;

	  get_new_block ();
	}

      if (block_type > INFLATE_DYNAMIC)
	errnum = ERR_BAD_GZIP_DATA;

      if (errnum)
	return;

      /*
       *  Expand stored block here.
       */
      if (block_type == INFLATE_STORED)
	{
	  unsigned int w = wp;

	  /*
	   *  This is basically a glorified pass-through
	   */

	  while (block_len && w < WSIZE && !errnum)
	    {
	      slide[w++] = get_byte ();
	      block_len--;
	    }

	  wp = w;

	  continue;
	}

      /*
       *  Expand other kind of block.
       */

      if (inflate_codes_in_window ())
	reset_linalloc ();
    }

  saved_filepos += WSIZE;

  /* XXX do CRC calculation here! */
}

static void initialize_tables (void);
static void
initialize_tables (void)
{
  saved_filepos = 0;
  filepos = gzip_data_offset;

  /* initialize window, bit buffer */
  bk = 0;
  bb = 0;

  /* reset partial decompression code */
  last_block = 0;
  block_len = 0;
  bufloc = 0;

  /* reset memory allocation stuff */
  reset_linalloc ();
}

unsigned long long gunzip_read (unsigned long long buf, unsigned long long len, unsigned int write);
unsigned long long
gunzip_read (unsigned long long buf, unsigned long long len, unsigned int write)
{
  unsigned long long ret = 0;

  compressed_file = 0;
  gunzip_swap_values ();
  /*
   *  Now "gzip_*" values refer to the uncompressed data.
   */

  /* do we reset decompression to the beginning of the file? */
  if (saved_filepos > gzip_filepos + WSIZE)
    initialize_tables ();

  /*
   *  This loop operates upon uncompressed data only.  The only
   *  special thing it does is to make sure the decompression
   *  window is within the range of data it needs.
   */

  while (len > 0 && !errnum)
    {
      register unsigned long long size;
      register char *srcaddr;

      while (gzip_filepos >= saved_filepos)
	inflate_window ();

      srcaddr = (char *) ((gzip_filepos & (WSIZE - 1)) + slide);
      size = saved_filepos - gzip_filepos;
      if (size > len)
	size = len;

      if (buf)
      {
	grub_memmove64 (buf, (unsigned long long)(grub_size_t)srcaddr, size);
	buf += size;
      }
      len -= size;
      gzip_filepos += size;
      ret += size;
    }

  compressed_file = 1;
  gunzip_swap_values ();
  /*
   *  Now "gzip_*" values refer to the compressed data.
   */

  if (errnum)
    ret = 0;

  return ret;
}

#endif /* ! NO_DECOMPRESSION */

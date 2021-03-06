/*
   Copyright (C) Andrew Tridgell 1998,
   Con Kolivas 2006-2009

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/
/* multiplex N streams into a file - the streams are passed
   through different compressors */

/* Need a definitition for FILE for old bzlib.h */
#include <stdio.h>
#include <bzlib.h>
#include <zlib.h>
#include "rzip.h"
/* LZMA C Wrapper */
#include "lzma/C/LzmaLib.h"

#define STREAM_BUFSIZE (1024 * 1024 * 10)

struct stream {
	i64 last_head;
	uchar *buf;
	i64 buflen;
	i64 bufp;
};

struct stream_info {
	struct stream *s;
	int num_streams;
	int fd;
	i64 bufsize;
	i64 cur_pos;
	i64 initial_pos;
	i64 total_read;
};

/* just to keep things clean, declare function here
 * but move body to the end since it's a work function
*/

static int lzo_compresses(struct stream *s);

#ifndef HAS_MEMSTREAM
#define fmemopen fake_fmemopen
#define open_memstream fake_open_memstream
#define memstream_update_buffer fake_open_memstream_update_buffer

static FILE *fake_fmemopen(void *buf, size_t buflen, char *mode)
{
	FILE *in;

	if (strcmp(mode, "r"))
                fatal("fake_fmemopen only supports mode \"r\".");
	in = tmpfile();
	if (!in)
	        return NULL;
	if (fwrite(buf, buflen, 1, in) != 1)
	        return NULL;
	rewind(in);
        return in;
}

static FILE *fake_open_memstream(char **buf, size_t *length)
{
	FILE *out;

	if (buf == NULL || length == NULL)
	        fatal("NULL parameter to fake_open_memstream");
	out = tmpfile();
	if (!out)
	        return NULL;
	return out;
}

static int fake_open_memstream_update_buffer(FILE *fp, uchar **buf, size_t *length)
{
	long original_pos = ftell(fp);

	if (fseek(fp, 0, SEEK_END) != 0)
		return -1;
	*length = ftell(fp);
	rewind(fp);
	*buf = (uchar *)malloc(*length);
	if (!*buf)
	        return -1;
	if (fread(*buf, *length, 1, fp) != 1)
	        return -1;
	if (fseek(fp, original_pos, SEEK_SET) != 0)
	        return -1;
	return 0;
}
#else
static int memstream_update_buffer(FILE *fp, uchar **buf, size_t *length)
{
	return 0;
}
#endif

/*
  ***** COMPRESSION FUNCTIONS *****

  ZPAQ, BZIP, GZIP, LZMA, LZO

  try to compress a buffer. If compression fails for whatever reason then
  leave uncompressed. Return the compression type in c_type and resulting
  length in c_len
*/

static void zpaq_compress_buf(struct stream *s, int *c_type, i64 *c_len)
{
	uchar *c_buf;
	FILE *in, *out;
	size_t dlen;

	if (!lzo_compresses(s))
		return;

	in = fmemopen(s->buf, s->buflen, "r");
	if (!in)
		fatal("Failed to fmemopen in zpaq_compress_buf\n");
	out = open_memstream((char **)&c_buf, &dlen);
	if (!out)
		fatal("Failed to open_memstream in zpaq_compress_buf\n");

	zpipe_compress(in, out, control.msgout, s->buflen, (int)(control.flags & FLAG_SHOW_PROGRESS));

	if (memstream_update_buffer(out, &c_buf, &dlen) != 0)
	        fatal("Failed to memstream_update_buffer in zpaq_compress_buf");

	fclose(in);
	fclose(out);

	if ((i64)dlen >= *c_len) {
		/* Incompressible, leave as CTYPE_NONE */
		free(c_buf);
		return;
	}

	*c_len = dlen;
	free(s->buf);
	s->buf = c_buf;
	*c_type = CTYPE_ZPAQ;
}

static void bzip2_compress_buf(struct stream *s, int *c_type, i64 *c_len)
{
	uchar *c_buf;
	u32 dlen = s->buflen;

	if (!lzo_compresses(s))
		return;

	c_buf = malloc(dlen);
	if (!c_buf)
		return;

	if (BZ2_bzBuffToBuffCompress((char*)c_buf, &dlen, (char*)s->buf, s->buflen,
				     control.compression_level, 0,
				     control.compression_level * 10) != BZ_OK) {
		free(c_buf);
		return;
	}

	if (dlen >= *c_len) {
		/* Incompressible, leave as CTYPE_NONE */
		free(c_buf);
		return;
	}

	*c_len = dlen;
	free(s->buf);
	s->buf = c_buf;
	*c_type = CTYPE_BZIP2;
}

static void gzip_compress_buf(struct stream *s, int *c_type, i64 *c_len)
{
	uchar *c_buf;
	unsigned long dlen = s->buflen;

	c_buf = malloc(dlen);
	if (!c_buf)
		return;

	if (compress2(c_buf, &dlen, s->buf, s->buflen, control.compression_level) != Z_OK) {
		free(c_buf);
		return;
	}

	if ((i64)dlen >= *c_len) {
		/* Incompressible, leave as CTYPE_NONE */
		free(c_buf);
		return;
	}

	*c_len = dlen;
	free(s->buf);
	s->buf = c_buf;
	*c_type = CTYPE_GZIP;
}

static void lzma_compress_buf(struct stream *s, int *c_type, i64 *c_len)
{
	uchar *c_buf;
	size_t dlen;
	size_t prop_size = 5; /* return value for lzma_properties */
	int lzma_ret;

	if (!lzo_compresses(s))
		goto out;

	dlen = s->buflen;
	c_buf = malloc(dlen);
	if (!c_buf)
		return;

	if (control.flags & FLAG_SHOW_PROGRESS) {
		fprintf(control.msgout, "\tProgress percentage pausing during lzma compression...");
		fflush(control.msgout);
	}
	/* with LZMA SDK 4.63, we pass compression level and threads only
	 * and receive properties in control->lzma_properties */

	lzma_ret = LzmaCompress(c_buf, &dlen, s->buf, (size_t)s->buflen, control.lzma_properties, &prop_size, control.compression_level,
			      0, /* dict size. set default */
			      -1, -1, -1, -1, /* lc, lp, pb, fb */
			      control.threads);
	if (lzma_ret != SZ_OK) {
		switch (lzma_ret) {
			case SZ_ERROR_MEM:
				err_msg("\nLZMA ERROR: %d. Try a smaller compression window.\n", SZ_ERROR_MEM);
				break;
			case SZ_ERROR_PARAM:
				err_msg("\nLZMA Parameter ERROR: %d. This should not happen.\n", SZ_ERROR_PARAM);
				break;
			case SZ_ERROR_OUTPUT_EOF:
				err_msg("\nHarmless LZMA Output Buffer Overflow error: %d. Incompressible block.\n", SZ_ERROR_OUTPUT_EOF);
				break;
			case SZ_ERROR_THREAD:
				err_msg("\nLZMA Multi Thread ERROR: %d. This should not happen.\n", SZ_ERROR_THREAD);
				break;
			default:
				err_msg("Unidentified LZMA ERROR: %d. This should not happen.\n", lzma_ret);
				break;
		}
		/* can pass -1 if not compressible! Thanks Lasse Collin */
		free(c_buf);
		goto out;
	}
	if ((i64)dlen >= *c_len) {
		/* Incompressible, leave as CTYPE_NONE */
		free(c_buf);
		goto out;
	}

	*c_len = dlen;
	free(s->buf);
	s->buf = c_buf;
	*c_type = CTYPE_LZMA;
out:
	if (control.flags & FLAG_VERBOSITY_MAX)
		fprintf(control.msgout, "\n");
	else if ((control.flags & FLAG_SHOW_PROGRESS || control.flags & FLAG_VERBOSITY ))
		fprintf(control.msgout, "\r\t                                                                                      \r");
	fflush(control.msgout);
}

static void lzo_compress_buf(struct stream *s, int *c_type, i64 *c_len)
{
	uchar *c_buf;
	lzo_bytep wrkmem;
	lzo_uint in_len = s->buflen;
	lzo_uint dlen = in_len + in_len / 16 + 64 + 3;
	lzo_int return_var;	/* lzo1x_1_compress does not return anything but LZO_OK */

	wrkmem = (lzo_bytep) malloc(LZO1X_1_MEM_COMPRESS);
	if (wrkmem == NULL)
		return;

	c_buf = malloc(dlen);
	if (!c_buf)
		goto out_free;

	return_var = lzo1x_1_compress((uchar *)s->buf, in_len, (uchar *)c_buf,
				      &dlen,wrkmem);

	if (dlen >= in_len){
		/* Incompressible, leave as CTYPE_NONE */
		free(c_buf);
		goto out_free;
	}

	*c_len = dlen;
	free(s->buf);
	s->buf = c_buf;
	*c_type = CTYPE_LZO;
out_free:
	free(wrkmem);
}

/*
  ***** DECOMPRESSION FUNCTIONS *****

  ZPAQ, BZIP, GZIP, LZMA, LZO

  try to decompress a buffer. Return 0 on success and -1 on failure.
*/

static int zpaq_decompress_buf(struct stream *s)
{
	uchar *c_buf;
	FILE *in, *out;
	size_t dlen;

	in = fmemopen(s->buf, s->buflen, "r");
	if (!in) {
		err_msg("Failed to fmemopen in zpaq_decompress_buf\n");
		return -1;
	}
	out = open_memstream((char **)&c_buf, &dlen);
	if (!out) {
		err_msg("Failed to open_memstream in zpaq_decompress_buf\n");
		return -1;
	}

	zpipe_decompress(in, out, control.msgout, s->buflen, (int)(control.flags & FLAG_SHOW_PROGRESS));

	if (memstream_update_buffer(out, &c_buf, &dlen) != 0)
	        fatal("Failed to memstream_update_buffer in zpaq_decompress_buf");

	fclose(in);
	fclose(out);
	free(s->buf);
	s->buf = c_buf;

	if ((i64)dlen != s->buflen) {
		err_msg("Inconsistent length after decompression. Got %d bytes, expected %d\n", dlen, s->buflen);
		return -1;
	}

	return 0;
}

static int bzip2_decompress_buf(struct stream *s, i64 c_len)
{
	uchar *c_buf;
	u32 dlen = s->buflen;
	int bzerr;

	c_buf = s->buf;
	s->buf = malloc(dlen);
	if (!s->buf) {
		err_msg("Failed to allocate %d bytes for decompression\n", dlen);
		return -1;
	}

	bzerr = BZ2_bzBuffToBuffDecompress((char*)s->buf, &dlen, (char*)c_buf, c_len, 0, 0);
	if (bzerr != BZ_OK) {
		err_msg("Failed to decompress buffer - bzerr=%d\n", bzerr);
		return -1;
	}

	if (dlen != s->buflen) {
		err_msg("Inconsistent length after decompression. Got %d bytes, expected %d\n", dlen, s->buflen);
		return -1;
	}

	free(c_buf);
	return 0;
}

static int gzip_decompress_buf(struct stream *s, i64 c_len)
{
	uchar *c_buf;
	unsigned long dlen = s->buflen;
	int gzerr;

	c_buf = s->buf;
	s->buf = malloc(dlen);
	if (!s->buf) {
		err_msg("Failed to allocate %d bytes for decompression\n", dlen);
		return -1;
	}

	gzerr = uncompress(s->buf, &dlen, c_buf, c_len);
	if (gzerr != Z_OK) {
		err_msg("Failed to decompress buffer - bzerr=%d\n", gzerr);
		return -1;
	}

	if ((i64)dlen != s->buflen) {
		err_msg("Inconsistent length after decompression. Got %d bytes, expected %d\n", dlen, s->buflen);
		return -1;
	}

	free(c_buf);
	return 0;
}

static int lzma_decompress_buf(struct stream *s, size_t c_len)
{
	uchar *c_buf;
	size_t dlen = (size_t)s->buflen;
	int lzmaerr;

	c_buf = s->buf;
	s->buf = malloc(dlen);
	if (!s->buf) {
		err_msg("Failed to allocate %d bytes for decompression\n", dlen);
		return -1;
	}

	/* With LZMA SDK 4.63 we pass control.lzma_properties
	 * which is needed for proper uncompress */
	lzmaerr = LzmaUncompress(s->buf, &dlen, c_buf, &c_len, control.lzma_properties, 5);
	if (lzmaerr != 0) {
		err_msg("Failed to decompress buffer - lzmaerr=%d\n", lzmaerr);
		return -1;
	}

	if ((i64)dlen != s->buflen) {
		err_msg("Inconsistent length after decompression. Got %d bytes, expected %d\n", dlen, s->buflen);
		return -1;
	}

	free(c_buf);
	return 0;
}

static int lzo_decompress_buf(struct stream *s, i64 c_len)
{
	uchar *c_buf;
	lzo_uint dlen = s->buflen;
	int lzerr;

	c_buf = s->buf;
	s->buf = malloc(dlen);
	if (!s->buf) {
		err_msg("Failed to allocate %d bytes for decompression\n", dlen);
		return -1;
	}

	lzerr = lzo1x_decompress((uchar*)c_buf,c_len,(uchar*)s->buf,&dlen,NULL);
	if (lzerr != LZO_E_OK) {
		err_msg("Failed to decompress buffer - lzerr=%d\n", lzerr);
		return -1;
	}

	if ((i64)dlen != s->buflen) {
		err_msg("Inconsistent length after decompression. Got %d bytes, expected %d\n", dlen, s->buflen);
		return -1;
	}

	free(c_buf);
	return 0;
}

/* WORK FUNCTIONS */

const i64 one_g = 1000 * 1024 * 1024;

/* This is a custom version of write() which writes in 1GB chunks to avoid
   the overflows at the >= 2GB mark thanks to 32bit fuckage. This should help
   even on the rare occasion write() fails to write 1GB as well. */
ssize_t write_1g(int fd, void *buf, i64 len)
{
	i64 total, offset;
	ssize_t ret;
	uchar *offset_buf = buf;

	total = offset = 0;
	while (len > 0) {
		if (len > one_g)
			ret = one_g;
		else
			ret = len;
		ret = write(fd, offset_buf, (size_t)ret);
		if (ret < 0)
			return ret;
		len -= ret;
		offset_buf += ret;
		total += ret;
	}
	return total;
}

/* Ditto for read */
ssize_t read_1g(int fd, void *buf, i64 len)
{
	i64 total, offset;
	ssize_t ret;
	uchar *offset_buf = buf;

	total = offset = 0;
	while (len > 0) {
		if (len > one_g)
			ret = one_g;
		else
			ret = len;
		ret = read(fd, offset_buf, (size_t)ret);
		if (ret < 0)
			return ret;
		len -= ret;
		offset_buf += ret;
		total += ret;
	}
	return total;
}

/* write to a file, return 0 on success and -1 on failure */
static int write_buf(int f, uchar *p, i64 len)
{
	ssize_t ret;

	ret = write_1g(f, p, (size_t)len);
	if (ret == -1) {
		err_msg("Write of length %d failed - %s\n", len, strerror(errno));
		return -1;
	}
	if (ret != (ssize_t)len) {
		err_msg("Partial write!? asked for %d bytes but got %d\n", len, ret);
		return -1;
	}
	return 0;
}

/* write a byte */
static int write_u8(int f, uchar v)
{
	return write_buf(f, &v, 1);
}

/* write a i64 */
static int write_i64(int f, i64 v)
{
	if (write_buf(f, (uchar *)&v, 8))
		return -1;

	return 0;
}

static int read_buf(int f, uchar *p, i64 len)
{
	ssize_t ret;

	ret = read_1g(f, p, (size_t)len);
	if (ret == -1) {
		err_msg("Read of length %d failed - %s\n", len, strerror(errno));
		return -1;
	}
	if (ret != (ssize_t)len) {
		err_msg("Partial read!? asked for %d bytes but got %d\n", len, ret);
		return -1;
	}
	return 0;
}

static int read_u8(int f, uchar *v)
{
	return read_buf(f, v, 1);
}

static int read_u32(int f, u32 *v)
{
	if (read_buf(f, (uchar *)v, 4))
		return -1;
	return 0;
}

static int read_i64(int f, i64 *v)
{
	if (read_buf(f, (uchar *)v, 8))
		return -1;
	return 0;
}

/* seek to a position within a set of streams - return -1 on failure */
static int seekto(struct stream_info *sinfo, i64 pos)
{
	i64 spos = pos + sinfo->initial_pos;
	if (lseek(sinfo->fd, spos, SEEK_SET) != spos) {
		err_msg("Failed to seek to %d in stream\n", pos);
		return -1;
	}
	return 0;
}

/* open a set of output streams, compressing with the given
   compression level and algorithm */
void *open_stream_out(int f, int n, i64 limit)
{
	unsigned cwindow = control.window;
	int i;
	struct stream_info *sinfo;

	sinfo = malloc(sizeof(*sinfo));
	if (!sinfo)
		return NULL;

	sinfo->num_streams = n;
	sinfo->cur_pos = 0;
	sinfo->fd = f;

	/* 10MB streams for non lzma compress. There is virtually no gain
	   in lzo, gzip and bzip2 with larger streams. With lzma and zpaq,
	   however, the larger the buffer, the better the compression so we
	   make it as large as the window up to the limit the compressor
	   will take */
	if (LZMA_COMPRESS(control.flags)) {
		if (sizeof(long) == 4) {
			/* Largest window supported on lzma 32bit is 600MB */
			if (cwindow > 6)
				cwindow = 6;
		}
		/* Largest window supported on lzma 64bit is 4GB */
		if (cwindow > 40)
			cwindow = 40;
	}

	if (LZMA_COMPRESS(control.flags) || (control.flags & FLAG_ZPAQ_COMPRESS))
		sinfo->bufsize = STREAM_BUFSIZE * 10 * cwindow;
	else
		sinfo->bufsize = STREAM_BUFSIZE;

	/* No point making the stream larger than the amount of data */
	if (limit && limit < sinfo->bufsize)
		sinfo->bufsize = limit;
	sinfo->initial_pos = lseek(f, 0, SEEK_CUR);

	sinfo->s = (struct stream *)calloc(sizeof(sinfo->s[0]), n);
	if (!sinfo->s) {
		free(sinfo);
		return NULL;
	}

	for (i = 0; i < n; i++) {
		sinfo->s[i].buf = malloc(sinfo->bufsize);
		if (!sinfo->s[i].buf)
			goto failed;
	}

	/* write the initial headers */
	for (i = 0; i < n; i++) {
		sinfo->s[i].last_head = sinfo->cur_pos + 17;
		write_u8(sinfo->fd, CTYPE_NONE);
		write_i64(sinfo->fd, 0);
		write_i64(sinfo->fd, 0);
		write_i64(sinfo->fd, 0);
		sinfo->cur_pos += 25;
	}
	return (void *)sinfo;

failed:
	for (i = 0; i < n; i++) {
		if (sinfo->s[i].buf)
			free(sinfo->s[i].buf);
	}

	free(sinfo);
	return NULL;
}

/* prepare a set of n streams for reading on file descriptor f */
void *open_stream_in(int f, int n)
{
	i64 header_length;
	int i;
	struct stream_info *sinfo;

	sinfo = calloc(sizeof(*sinfo), 1);
	if (!sinfo)
		return NULL;

	sinfo->num_streams = n;
	sinfo->fd = f;
	sinfo->initial_pos = lseek(f, 0, SEEK_CUR);

	sinfo->s = (struct stream *)calloc(sizeof(sinfo->s[0]), n);
	if (!sinfo->s) {
		free(sinfo);
		return NULL;
	}

	for (i = 0; i < n; i++) {
		uchar c;
		i64 v1, v2;

again:
		if (read_u8(f, &c) != 0)
			goto failed;

		/* Compatibility crap for versions < 0.40 */
		if (control.major_version == 0 && control.minor_version < 4) {
			u32 v132, v232, last_head32;

			if (read_u32(f, &v132) != 0)
				goto failed;
			if (read_u32(f, &v232) != 0)
				goto failed;
			if (read_u32(f, &last_head32) != 0)
				goto failed;

			v1 = v132;
			v2 = v232;
			sinfo->s[i].last_head = last_head32;
			header_length = 13;
		} else {
			if (read_i64(f, &v1) != 0)
				goto failed;
			if (read_i64(f, &v2) != 0)
				goto failed;
			if (read_i64(f, &sinfo->s[i].last_head) != 0)
				goto failed;
			header_length = 25;
		}

		if (c == CTYPE_NONE && v1 == 0 && v2 == 0 && sinfo->s[i].last_head == 0 && i == 0) {
			err_msg("Enabling stream close workaround\n");
			sinfo->initial_pos += header_length;
			goto again;
		}

		sinfo->total_read += header_length;

		if (c != CTYPE_NONE) {
			err_msg("Unexpected initial tag %d in streams\n", c);
			goto failed;
		}
		if (v1 != 0) {
			err_msg("Unexpected initial c_len %lld in streams %lld\n", v1, v2);
			goto failed;
		}
		if (v2 != 0) {
			err_msg("Unexpected initial u_len %lld in streams\n", v2);
			goto failed;
		}
	}

	return (void *)sinfo;

failed:
	free(sinfo->s);
	free(sinfo);
	return NULL;
}

/* flush out any data in a stream buffer. Return -1 on failure */
static int flush_buffer(struct stream_info *sinfo, int stream)
{
	int c_type = CTYPE_NONE;
	i64 c_len = sinfo->s[stream].buflen;

	if (seekto(sinfo, sinfo->s[stream].last_head) != 0)
		return -1;
	if (write_i64(sinfo->fd, sinfo->cur_pos) != 0)
		return -1;

	sinfo->s[stream].last_head = sinfo->cur_pos + 17;
	if (seekto(sinfo, sinfo->cur_pos) != 0)
		return -1;

	if (!(control.flags & FLAG_NO_COMPRESS)) {
		if (LZMA_COMPRESS(control.flags))
			lzma_compress_buf(&sinfo->s[stream], &c_type, &c_len);
		else if (control.flags & FLAG_LZO_COMPRESS)
			lzo_compress_buf(&sinfo->s[stream], &c_type, &c_len);
		else if (control.flags & FLAG_BZIP2_COMPRESS)
			bzip2_compress_buf(&sinfo->s[stream], &c_type, &c_len);
		else if (control.flags & FLAG_ZLIB_COMPRESS)
			gzip_compress_buf(&sinfo->s[stream], &c_type, &c_len);
		else if (control.flags & FLAG_ZPAQ_COMPRESS)
			zpaq_compress_buf(&sinfo->s[stream], &c_type, &c_len);
		else fatal("Dunno wtf compression to use!\n");
	}

	if (write_u8(sinfo->fd, c_type) != 0 ||
	    write_i64(sinfo->fd, c_len) != 0 ||
	    write_i64(sinfo->fd, sinfo->s[stream].buflen) != 0 ||
	    write_i64(sinfo->fd, 0) != 0) {
		return -1;
	}
	sinfo->cur_pos += 25;

	if (write_buf(sinfo->fd, sinfo->s[stream].buf, c_len) != 0)
		return -1;
	sinfo->cur_pos += c_len;

	sinfo->s[stream].buflen = 0;

	free(sinfo->s[stream].buf);
	sinfo->s[stream].buf = malloc(sinfo->bufsize);
	if (!sinfo->s[stream].buf)
		return -1;
	return 0;
}

/* fill a buffer from a stream - return -1 on failure */
static int fill_buffer(struct stream_info *sinfo, int stream)
{
	uchar c_type;
	i64 header_length;
	i64 u_len, c_len;

	if (seekto(sinfo, sinfo->s[stream].last_head) != 0)
		return -1;

	if (read_u8(sinfo->fd, &c_type) != 0)
		return -1;
	/* Compatibility crap for versions < 0.4 */
	if (control.major_version == 0 && control.minor_version < 4) {
		u32 c_len32, u_len32, last_head32;

		if (read_u32(sinfo->fd, &c_len32) != 0)
			return -1;
		if (read_u32(sinfo->fd, &u_len32) != 0)
			return -1;
		if (read_u32(sinfo->fd, &last_head32) != 0)
			return -1;
		c_len = c_len32;
		u_len = u_len32;
		sinfo->s[stream].last_head = last_head32;
		header_length = 13;
	} else {
		if (read_i64(sinfo->fd, &c_len) != 0)
			return -1;
		if (read_i64(sinfo->fd, &u_len) != 0)
			return -1;
		if (read_i64(sinfo->fd, &sinfo->s[stream].last_head) != 0)
			return -1;
		header_length = 25;
	}

	sinfo->total_read += header_length;

	if (sinfo->s[stream].buf)
		free(sinfo->s[stream].buf);
	sinfo->s[stream].buf = malloc(u_len);
	if (!sinfo->s[stream].buf)
		return -1;
	if (read_buf(sinfo->fd, sinfo->s[stream].buf, c_len) != 0)
		return -1;

	sinfo->total_read += c_len;

	sinfo->s[stream].buflen = u_len;
	sinfo->s[stream].bufp = 0;

	if (c_type != CTYPE_NONE) {
		if (c_type == CTYPE_LZMA) {
			if (lzma_decompress_buf(&sinfo->s[stream], (size_t)c_len))
				return -1;
		} else if (c_type == CTYPE_LZO) {
			if (lzo_decompress_buf(&sinfo->s[stream], c_len))
				return -1;
		} else if (c_type == CTYPE_BZIP2) {
			if (bzip2_decompress_buf(&sinfo->s[stream], c_len))
				return -1;
		} else if (c_type == CTYPE_GZIP) {
			if (gzip_decompress_buf(&sinfo->s[stream], c_len))
				return -1;
		} else if (c_type == CTYPE_ZPAQ) {
			if (zpaq_decompress_buf(&sinfo->s[stream]))
				return -1;
		} else fatal("Dunno wtf decompression type to use!\n");
	}

	return 0;
}

/* write some data to a stream. Return -1 on failure */
int write_stream(void *ss, int stream, uchar *p, i64 len)
{
	struct stream_info *sinfo = ss;

	while (len) {
		i64 n;

		n = MIN(sinfo->bufsize - sinfo->s[stream].buflen, len);

		memcpy(sinfo->s[stream].buf+sinfo->s[stream].buflen, p, n);
		sinfo->s[stream].buflen += n;
		p += n;
		len -= n;

		if (sinfo->s[stream].buflen == sinfo->bufsize) {
			if (flush_buffer(sinfo, stream) != 0)
				return -1;
		}
	}
	return 0;
}

/* read some data from a stream. Return number of bytes read, or -1
   on failure */
i64 read_stream(void *ss, int stream, uchar *p, i64 len)
{
	struct stream_info *sinfo = ss;
	i64 ret = 0;

	while (len) {
		i64 n;

		n = MIN(sinfo->s[stream].buflen-sinfo->s[stream].bufp, len);

		if (n > 0) {
			memcpy(p, sinfo->s[stream].buf+sinfo->s[stream].bufp, n);
			sinfo->s[stream].bufp += n;
			p += n;
			len -= n;
			ret += n;
		}

		if (len && sinfo->s[stream].bufp == sinfo->s[stream].buflen) {
			if (fill_buffer(sinfo, stream) != 0)
				return -1;
			if (sinfo->s[stream].bufp == sinfo->s[stream].buflen)
				break;
		}
	}

	return ret;
}

/* flush and close down a stream. return -1 on failure */
int close_stream_out(void *ss)
{
	struct stream_info *sinfo = ss;
	int i;

	/* reallocate buffers to try and save space */
	for (i = 0; i < sinfo->num_streams; i++) {
		if (sinfo->s[i].buflen != 0) {
			if (!realloc(sinfo->s[i].buf, sinfo->s[i].buflen))
				fatal("Error Reallocating Output Buffer %d\n", i);
		}
	}
	for (i = 0; i < sinfo->num_streams; i++) {
		if (sinfo->s[i].buflen != 0 && flush_buffer(sinfo, i) != 0)
			return -1;
		if (sinfo->s[i].buf)
			free(sinfo->s[i].buf);
	}
	free(sinfo->s);
	free(sinfo);
	return 0;
}

/* close down an input stream */
int close_stream_in(void *ss)
{
	struct stream_info *sinfo = ss;
	int i;

	if (lseek(sinfo->fd, sinfo->initial_pos + sinfo->total_read,
		  SEEK_SET) != sinfo->initial_pos + sinfo->total_read)
			return -1;
	for (i = 0; i < sinfo->num_streams; i++) {
		if (sinfo->s[i].buf)
			free(sinfo->s[i].buf);
	}

	free(sinfo->s);
	free(sinfo);
	return 0;
}

/* As others are slow and lzo very fast, it is worth doing a quick lzo pass
   to see if there is any compression at all with lzo first. It is unlikely
   that others will be able to compress if lzo is unable to drop a single byte
   so do not compress any block that is incompressible by lzo. */
static int lzo_compresses(struct stream *s)
{
	lzo_bytep wrkmem=NULL;
	lzo_uint in_len, test_len = s->buflen, save_len = s->buflen;
	lzo_uint dlen;
	lzo_int return_var;	/* lzo1x_1_compress does not return anything but LZO_OK */
	uchar *c_buf = NULL, *test_buf = s->buf;
	/* set minimum buffer test size based on the length of the test stream */
	unsigned long buftest_size = (test_len > 5 * STREAM_BUFSIZE ? STREAM_BUFSIZE : STREAM_BUFSIZE / 4096);
	int ret = 0;
	int workcounter = 0;	/* count # of passes */
	lzo_uint best_dlen = UINT_MAX; /* save best compression estimate */

	if (control.threshold > 1)
		return 1;
	wrkmem = (lzo_bytep) malloc(LZO1X_1_MEM_COMPRESS);
	if (wrkmem == NULL)
		fatal("Unable to allocate wrkmem in lzo_compresses\n");

	in_len = MIN(test_len, buftest_size);
	dlen = STREAM_BUFSIZE + STREAM_BUFSIZE / 16 + 64 + 3;

	c_buf = malloc(dlen);
	if (!c_buf)
		fatal("Unable to allocate c_buf in lzo_compresses\n");

	if (control.flags & FLAG_SHOW_PROGRESS) {
		fprintf(control.msgout, "\tlzo testing for incompressible data...");
		fflush(control.msgout);
	}

	/* Test progressively larger blocks at a time and as soon as anything
	   compressible is found, jump out as a success */
	while (test_len > 0) {
		workcounter++;
		return_var = lzo1x_1_compress(test_buf, in_len, (uchar *)c_buf, &dlen, wrkmem);

		if (dlen < best_dlen)
			best_dlen = dlen;	/* save best value */

		if ((double) dlen < (double)in_len * control.threshold) {
			ret = 1;
			break;
		}
		/* expand and move buffer */
		test_len -= in_len;
		if (test_len) {
			test_buf += (ptrdiff_t)in_len;
			if (buftest_size < STREAM_BUFSIZE)
				buftest_size <<= 1;
			in_len = MIN(test_len, buftest_size);
		}
	}
	if (control.flags & FLAG_VERBOSITY_MAX)
		fprintf(control.msgout, "%s for chunk %ld. Compressed size = %5.2F%% of chunk, %d Passes\n",
			(ret == 0? "FAILED - below threshold" : "OK"), save_len,
			100 * ((double) best_dlen / (double) in_len), workcounter);
	else if (control.flags & FLAG_VERBOSITY)
		fprintf(control.msgout, "%s\r", (ret == 0? "FAILED - below threshold" : "OK"));
	else if (control.flags & FLAG_SHOW_PROGRESS)
		fprintf(control.msgout, "\r\t                                                      \r");
	fflush(control.msgout);

	free(wrkmem);
	free(c_buf);

	return ret;
}

#include "builtin.h"
#include "cache.h"
#include "object.h"
#include "blob.h"
#include "delta.h"
#include "pack.h"
#include "csum-file.h"

static int max_depth = 10;
static unsigned long object_count;
static int packfd;
static int current_depth;
static void *lastdat;
static unsigned long lastdatlen;
static unsigned char lastsha1[20];

static ssize_t yread(int fd, void *buffer, size_t length)
{
	ssize_t ret = 0;
	while (ret < length) {
		ssize_t size = xread(fd, (char *) buffer + ret, length - ret);
		if (size < 0) {
			return size;
		}
		if (size == 0) {
			return ret;
		}
		ret += size;
	}
	return ret;
}

static ssize_t ywrite(int fd, void *buffer, size_t length)
{
	ssize_t ret = 0;
	while (ret < length) {
		ssize_t size = xwrite(fd, (char *) buffer + ret, length - ret);
		if (size < 0) {
			return size;
		}
		if (size == 0) {
			return ret;
		}
		ret += size;
	}
	return ret;
}

static unsigned long encode_header(enum object_type type, unsigned long size, unsigned char *hdr)
{
	int n = 1;
	unsigned char c;

	if (type < OBJ_COMMIT || type > OBJ_DELTA)
		die("bad type %d", type);

	c = (type << 4) | (size & 15);
	size >>= 4;
	while (size) {
		*hdr++ = c | 0x80;
		c = size & 0x7f;
		size >>= 7;
		n++;
	}
	*hdr = c;
	return n;
}

static void write_blob (void *dat, unsigned long datlen)
{
	z_stream s;
	void *out, *delta;
	unsigned char hdr[64];
	unsigned long hdrlen, deltalen;

	if (lastdat && current_depth < max_depth) {
		delta = diff_delta(lastdat, lastdatlen,
			dat, datlen,
			&deltalen, 0);
	} else
		delta = 0;

	memset(&s, 0, sizeof(s));
	deflateInit(&s, zlib_compression_level);

	if (delta) {
		current_depth++;
		s.next_in = delta;
		s.avail_in = deltalen;
		hdrlen = encode_header(OBJ_DELTA, deltalen, hdr);
		if (ywrite(packfd, hdr, hdrlen) != hdrlen)
			die("Can't write object header: %s", strerror(errno));
		if (ywrite(packfd, lastsha1, sizeof(lastsha1)) != sizeof(lastsha1))
			die("Can't write object base: %s", strerror(errno));
	} else {
		current_depth = 0;
		s.next_in = dat;
		s.avail_in = datlen;
		hdrlen = encode_header(OBJ_BLOB, datlen, hdr);
		if (ywrite(packfd, hdr, hdrlen) != hdrlen)
			die("Can't write object header: %s", strerror(errno));
	}

	s.avail_out = deflateBound(&s, s.avail_in);
	s.next_out = out = xmalloc(s.avail_out);
	while (deflate(&s, Z_FINISH) == Z_OK)
		/* nothing */;
	deflateEnd(&s);

	if (ywrite(packfd, out, s.total_out) != s.total_out)
		die("Failed writing compressed data %s", strerror(errno));

	free(out);
	if (delta)
		free(delta);
}

static void init_pack_header ()
{
	const char* magic = "PACK";
	unsigned long version = 2;
	unsigned long zero = 0;

	version = htonl(version);

	if (ywrite(packfd, (char*)magic, 4) != 4)
		die("Can't write pack magic: %s", strerror(errno));
	if (ywrite(packfd, &version, 4) != 4)
		die("Can't write pack version: %s", strerror(errno));
	if (ywrite(packfd, &zero, 4) != 4)
		die("Can't write 0 object count: %s", strerror(errno));
}

static void fixup_header_footer ()
{
	SHA_CTX c;
	char hdr[8];
	unsigned char sha1[20];
	unsigned long cnt;
	char *buf;
	size_t n;

	if (lseek(packfd, 0, SEEK_SET) != 0)
		die("Failed seeking to start: %s", strerror(errno));

	SHA1_Init(&c);
	if (yread(packfd, hdr, 8) != 8)
		die("Failed reading header: %s", strerror(errno));
	SHA1_Update(&c, hdr, 8);

fprintf(stderr, "%lu objects\n", object_count);
	cnt = htonl(object_count);
	SHA1_Update(&c, &cnt, 4);
	if (ywrite(packfd, &cnt, 4) != 4)
		die("Failed writing object count: %s", strerror(errno));

	buf = xmalloc(128 * 1024);
	for (;;) {
		n = xread(packfd, buf, 128 * 1024);
		if (n <= 0)
			break;
		SHA1_Update(&c, buf, n);
	}
	free(buf);

	SHA1_Final(sha1, &c);
	if (ywrite(packfd, sha1, sizeof(sha1)) != sizeof(sha1))
		die("Failed writing pack checksum: %s", strerror(errno));
}

int main (int argc, const char **argv)
{
	packfd = open(argv[1], O_RDWR|O_CREAT|O_TRUNC, 0666);
	if (packfd < 0)
		die("Can't create pack file %s: %s", argv[1], strerror(errno));

	init_pack_header();
	for (;;) {
		unsigned long datlen;
		int hdrlen;
		void *dat;
		char hdr[128];
		unsigned char sha1[20];
		SHA_CTX c;

		if (yread(0, &datlen, 4) != 4)
			break;

		dat = xmalloc(datlen);
		if (yread(0, dat, datlen) != datlen)
			break;

		hdrlen = sprintf(hdr, "blob %lu", datlen) + 1;
		SHA1_Init(&c);
		SHA1_Update(&c, hdr, hdrlen);
		SHA1_Update(&c, dat, datlen);
		SHA1_Final(sha1, &c);

		write_blob(dat, datlen);
		object_count++;
		printf("%s\n", sha1_to_hex(sha1));
		fflush(stdout);

		if (lastdat)
			free(lastdat);
		lastdat = dat;
		lastdatlen = datlen;
		memcpy(lastsha1, sha1, sizeof(sha1));
	}
	fixup_header_footer();
	close(packfd);

	return 0;
}

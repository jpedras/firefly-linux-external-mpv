/* 
 * vo_bl.c - playback using the Blinkenlights UPD protocol (and to files)
 * 
 * UDP socket handling copied from bsender.c part of blib-0.6: 
 * http://sven.gimp.org/blinkenlights/
 * Copyright (c)  2001-2001 The Blinkenlights Crew:
 * 	Sven Neumann <sven@gimp.org>
 * 	Michael Natterer <mitch@gimp.org>
 * 	Daniel Mack <daniel@yoobay.net>
 * (these portions are licensed under GNU GPL v2 or "(at your option)
 * any later version")
 * 
 * Other stuff: Copyright (C) Rik Snel 2002, License GNU GPL v2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <linux/types.h>

#include "config.h"

#include "video_out.h"
#include "video_out_internal.h"
#include "../mp_msg.h"
#include "../cfgparser.h"
#include "fastmemcpy.h"

LIBVO_EXTERN (bl)

static vo_info_t vo_info = 
{
	"Blinkenlights driver: http://www.blinkenlights.de",
	"bl",
	"Rik Snel <snel@phys.uu.nl>",
	""
};

/* General variables */

static unsigned char *image = NULL;
static unsigned char *tmp = NULL;
static int framenum, yoff, stride;
static char *bl_subdevice = NULL;
static int prevpts = -1;

typedef struct {
	char *name; /* filename */
	FILE *fp;
} bl_file_t;

typedef struct {
	char *name; /* hostname */
	int port;
	int fd; /* file descriptor */
} bl_host_t;

typedef struct {
	char *name;
	int img_format;

	int channels;
	int width;
	int height;
	int bpc; /* bits per component: bpc = 3, channels = 3 => bpp = 24*/

	/* file output functions */
	int (*init_file)(bl_file_t *file);
	void (*write_frame)(bl_file_t *file, unsigned char *i, int duration);
	void (*close_file)(bl_file_t *file);

	/* network output functions */
	int (*init_connection)(bl_host_t *host);
	void (*send_frame)(bl_host_t *host);
	void (*close_connection)(bl_host_t *host);
} bl_properties_t;

static bl_properties_t *bl = NULL;

/* arbitrary limit because I am too lazy to do proper memory management */
#define BL_MAX_FILES 16
#define BL_MAX_HOSTS 16
static bl_file_t bl_files[BL_MAX_FILES];
static bl_host_t bl_hosts[BL_MAX_HOSTS];
static int no_bl_files = 0;
static int no_bl_hosts = 0;

typedef struct {
	uint32_t magic;
	uint16_t height;
	uint16_t width;
	uint16_t channels;
	uint16_t maxval;
	unsigned char data[0];
} bl_packet_t;

static bl_packet_t *bl_packet = NULL;
static int bl_size;

/* bml output functions */
static int bml_init(bl_file_t *f) {
	f->fp = fopen(f->name, "w");
	if (!f->fp) {
		mp_msg(MSGT_VO, MSGL_ERR, "bl: error opening %s\n", f->name);
		return 1;
	}
	fprintf(f->fp, 
"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
"<blm width=\"%d\" height=\"%d\" bits=\"%d\" channels=\"%d\">\n"
"    <header>\n"
"        <title>Movie autogenerated by MPlayer</title>\n"
"        <url>http://www.mplayerhq.hu</url>\n"
"    </header>\n", bl->width, bl->height, bl->bpc, bl->channels);
	return 0;
}

static void bml_write_frame(bl_file_t *f, unsigned char *i, int duration) {
	int j, k;
	fprintf(f->fp, "    <frame duration=\"%d\">\n", duration);
	for (j = 0; j < bl->height; j++) {
		fprintf(f->fp, "        <row>");
		for (k = 0; k < bl->width; k++) 
			fprintf(f->fp, "%02x", *(i + j * bl->width + k));
		fprintf(f->fp, "</row>\n");
	}
	fprintf(f->fp, "    </frame>\n");
}

static void bml_close(bl_file_t *f) {
	fprintf(f->fp, "</blm>\n");
	fclose(f->fp);
}

/* Blinkenlights UDP protocol */
static int udp_init(bl_host_t *h) {
	struct sockaddr_in addr;
	struct hostent *dest;

	dest = gethostbyname(h->name);
	if (!dest) {
		mp_msg(MSGT_VO, MSGL_ERR, 
				"unable to resolve host %s\n", h->name);
		return 1;
	}

	h->fd = -1;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(h->port);

	memcpy(&addr.sin_addr.s_addr, dest->h_addr_list[0], dest->h_length);

	h->fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (h->fd < 0) {
		mp_msg(MSGT_VO, MSGL_ERR, 
				"couldn't create socket for %s\n", h->name);
		return 1;
	}
	if (connect(h->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		mp_msg(MSGT_VO, MSGL_ERR, "couldn't connect socket for %s\n", 
				h->name);
		close(h->fd);
		return 1;
	}
	return 0;
}

static void udp_send(bl_host_t *h) {
	if (write(h->fd, bl_packet, bl_size) != bl_size) 
		mp_msg(MSGT_VO, MSGL_ERR, "unable to send to %s\n", h->name);
}

static void udp_close(bl_host_t *h) {
	close(h->fd);
}

#define NO_BLS 2

/* currently only arcade is supported, hdl can be supported
 * in principle and future projects can be supported if their
 * parameters become known */
static bl_properties_t bls[NO_BLS] = {
	{ "hdl", IMGFMT_BGR1, 1, 18, 8, 1,
	NULL, NULL, NULL, NULL, NULL, NULL },
	{ "arcade", IMGFMT_YV12, 1, 26, 20, 8,
	&bml_init, &bml_write_frame, &bml_close,
	&udp_init, &udp_send, &udp_close } };

static uint32_t config(uint32_t width, uint32_t height, uint32_t d_width, 
	uint32_t d_height, uint32_t fullscreen, char *title, uint32_t format)
{
	framenum = 0;
	if (format != IMGFMT_YV12) {
		mp_msg(MSGT_VO, MSGL_ERR, "vo_bl called with wrong format");
		return 1;
	}
	if (width > bl->width) {
		mp_msg(MSGT_VO, MSGL_ERR, "bl: width of movie too large %d > %d\n", width, bl->width);
		return 1;
	}	
	if (height > bl->height) {
		mp_msg(MSGT_VO, MSGL_ERR, "bl: height of movie too large %d > %d\n", height, bl->height);
		return 1;
	}	
	if (!image) {
		mp_msg(MSGT_VO, MSGL_ERR, "bl: image should be initialized, internal error\n");
		return 1;
	}
	memset(image, 0, bl->width*bl->height*3); /* blank the image */
	mp_msg(MSGT_VO, MSGL_V, "vo_config bl called\n");
	return 0;
}

static const vo_info_t* get_info(void) {
	return &vo_info;
}

static void draw_osd(void) {
}

static void flip_page (void) {
	int i;

	if (prevpts >= 0) for (i = 0; i < no_bl_files; i++)
		bl->write_frame(&bl_files[i], tmp, (vo_pts - prevpts)/90);
	memcpy(tmp, image, bl->width*bl->height*bl->channels);
	prevpts = vo_pts;

	for (i = 0; i < no_bl_hosts; i++) bl->send_frame(&bl_hosts[i]);


	framenum++;
	return;
}

static uint32_t draw_frame(uint8_t * src[]) {
	int i, j;
	char *source, *dest;
	//printf("draw frame called\n");
#if 0
		zr_info_t *zr = &zr_info[j];
		geo_t *g = &zr->g;
		source = src[0] + 2*g->yoff*zr->vdec*zr->stride + 2*g->xoff;
		dest = zr->image + 2*zr->off_y;
		for (i = 0; i < g->height/zr->vdec; i++) {
			memcpy(dest, source, zr->image_width*2);
			dest += 2*zr->image_width;
			source += zr->vdec*zr->stride;
		}
#endif 
	return 0;
}

static uint32_t query_format(uint32_t format) {
	if (format == bl->img_format) 
		return VFCAP_CSP_SUPPORTED|VFCAP_CSP_SUPPORTED_BY_HW;
	return 0;
}

static void uninit(void) {
	int i;
	mp_msg(MSGT_VO, MSGL_V, "bl: uninit called\n");
	free(bl_packet);
	bl_packet = NULL;
	free(bl_subdevice);
	bl_subdevice = NULL;
	for (i = 0; i < no_bl_files; i++) bl->close_file(&bl_files[i]);
	for (i = 0; i < no_bl_hosts; i++) bl->close_connection(&bl_hosts[i]);
	no_bl_files = 0;
	no_bl_hosts = 0;
	bl = NULL;
}

static void check_events(void) {
}

static uint32_t draw_slice(uint8_t *srcimg[], int stride[],
		int wf, int hf, int xf, int yf) {
	int i, j, w, h, x, y;
	uint8_t *dst;
	uint8_t *src=srcimg[0];
	uint8_t *src1=srcimg[1];
	uint8_t *src2=srcimg[2];
	w = wf; h = hf; x = xf; y = yf;
	dst=image; /* + zr->off_y + zr->image_width*(y/zr->vdec)+x;*/
	// copy Y:
	for (i = 0; i < h; i++) {
		memcpy(dst,src,w);
		dst+=bl->width;
		src+=stride[0];

	}
 	return 0;
}

static uint32_t preinit(const char *arg) {
	char *p, *q;
	int end = 0, i;
	if (!arg || strlen(arg) == 0) {
		mp_msg(MSGT_VO, MSGL_ERR, "bl: subdevice must be given, example: -vo bl:arcade:host=localhost\n");
		return 1;
	}
	
	bl_subdevice = malloc(strlen(arg) + 1);
	if (!bl_subdevice) {
		mp_msg(MSGT_VO, MSGL_ERR, "bl: out of memory error\n");
		return 1;
	}
	p = bl_subdevice;
	strcpy(p, arg);
	mp_msg(MSGT_VO, MSGL_V, "bl: preinit called with %s\n", arg);
	if (strncmp(p, "arcade", 6)) {
		mp_msg(MSGT_VO, MSGL_ERR, "bl: subdevice must start with arcade, this is the only supported output format\nat the moment, i.e. -vo bl:arcade:host=localhost\n");
		return 1;
	}
	bl = &bls[1];
	p += 6;
	if (*p == '\0') {
		no_bl_hosts = 1;
		bl_hosts[0].name = "localhost";
		bl_hosts[0].port = 2323;
		mp_msg(MSGT_VO, MSGL_V, "bl: no hosts/files specified, using localhost:2323\n");
		end = 1;
	} else if (*p != ':') {
		mp_msg(MSGT_VO, MSGL_ERR, "bl: syntax error in subdevice\n");
		return 1;
	}
	p++;

	while (!end) {
		q = p + 5;
		if (!strncmp(p, "file=", 5)) {
			if (no_bl_files == BL_MAX_FILES) {
				mp_msg(MSGT_VO, MSGL_ERR, "bl: maximum number of hosts reached (%d)\n", BL_MAX_FILES);
				return 1;
			}
			p += 5;
			while (*q != ',' && *q != '\0') q++;
			if (*q == '\0') end = 1;
			*q = '\0';
			bl_files[no_bl_files].name = p;
			mp_msg(MSGT_VO, MSGL_V, "blfile[%d]: %s\n", 
					no_bl_files, p);
			no_bl_files++;
		} else if (!strncmp(p, "host=", 5)) {
			if (no_bl_hosts == BL_MAX_HOSTS) {
				mp_msg(MSGT_VO, MSGL_ERR, "bl: maximum number of hosts reached (%d)\n", BL_MAX_HOSTS);
				return 1;
			}
			p += 5;
			while (*q != ',' && *q != '\0' && *q != ':') q++;
			if (*q == ':') {
				*q++ = '\0';
				bl_hosts[no_bl_hosts].name = p;
				bl_hosts[no_bl_hosts].port = atoi(q);
				while (*q != ',' && *q != '\0') q++;
				if (*q == '\0') end = 1;
			} else {
				/* use default port */
				if (*q == '\0') end = 1;
				*q = '\0';
				bl_hosts[no_bl_hosts].name = p;
				bl_hosts[no_bl_hosts].port = 2323; 
			}
			mp_msg(MSGT_VO, MSGL_V, 
					"blhost[%d]: %s:%d\n", 
					no_bl_hosts, p,
					bl_hosts[no_bl_hosts].port);
			no_bl_hosts++;
		} else {
			mp_msg(MSGT_VO, MSGL_ERR, "bl: syntax error in entry %d in subdevice %s, should be a comma seperated\nlist of host=name:port and file=foo.bml\n", no_bl_hosts, no_bl_files, arg);
			return 1;
		}
		p = ++q;
	}

	bl_size = bl->width*bl->height*bl->channels + 12;
	/* enough space for RGB 24 bit + header */
	bl_packet = malloc(bl->width*bl->height*3+12); 
	image = ((unsigned char*)bl_packet + 12);
	tmp = malloc(bl->width*bl->height*bl->channels);
	
	if (!bl_packet || !tmp) {
		mp_msg(MSGT_VO, MSGL_ERR, "bl: out of memory error\n");
		return 1;
	}
	bl_packet->magic = htonl(0x23542666);
	bl_packet->width = htons(bl->width);
	bl_packet->height = htons(bl->height);
	bl_packet->channels = htons(bl->channels);
	bl_packet->maxval = htons(2<<bl->bpc - 1);

	/* open all files */
	for (i = 0; i < no_bl_files; i++) 
		if (bl->init_file(&bl_files[i])) return 1;

	/* open all sockets */
	for (i = 0; i < no_bl_hosts; i++) 
		if (bl->init_connection(&bl_hosts[i])) return 1;


	return 0;
}

static uint32_t control(uint32_t request, void *data, ...) {
	switch (request) {
		case VOCTRL_QUERY_FORMAT:
			return query_format(*((uint32_t*)data));
  		}
  	return VO_NOTIMPL;
}

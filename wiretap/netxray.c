/* netxray.c
 *
 * $Id: netxray.c,v 1.4 1999/03/20 09:10:49 guy Exp $
 *
 * Wiretap Library
 * Copyright (c) 1998 by Gilbert Ramirez <gram@verdict.uthscsa.edu>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 */

#include <stdlib.h>
#include <time.h>
#include "wtap.h"
#include "netxray.h"
#include "buffer.h"

/* Capture file header, *including* magic number, is padded to 128 bytes. */
#define	CAPTUREFILE_HEADER_SIZE	128

/* Magic number in NetXRay files. */
static const char netxray_magic[] = {	/* magic header */
	'X', 'C', 'P', '\0'
};

/* NetXRay file header (minus magic number). */
struct netxray_hdr {
	char	version[8];	/* version number */
	guint32	start_time;	/* UNIX time when capture started */
	guint32	xxx[2];		/* unknown */
	guint32	start_offset;	/* offset of first packet in capture */
	guint32	end_offset;	/* offset after last packet in capture */
	guint32 xxy[3];		/* unknown */
	guint16	network;	/* datalink type */
	guint8	xxz[6];
	guint32	timelo;		/* lower 32 bits of time stamp of capture start */
	guint32	timehi;		/* upper 32 bits of time stamp of capture start */
	/*
	 * XXX - other stuff.
	 */
};

/* Version number strings. */
static const char vers_1_0[] = {
	'0', '0', '1', '.', '0', '0', '0', '\0'
};

static const char vers_1_1[] = {
	'0', '0', '1', '.', '1', '0', '0', '\0'
};

static const char vers_2_001[] = {
	'0', '0', '2', '.', '0', '0', '1', '\0'
};

/* NetXRay 1.x data record format - followed by frame data. */
struct netxrayrec_1_x_hdr {
	guint32	timelo;		/* lower 32 bits of time stamp */
	guint32	timehi;		/* upper 32 bits of time stamp */
	guint16	orig_len;	/* packet length */
	guint16	incl_len;	/* capture length */
	guint32	xxx[4];		/* unknown */
};

/* NetXRay 2.x data record format - followed by frame data. */
struct netxrayrec_2_x_hdr {
	guint32	timelo;		/* lower 32 bits of time stamp */
	guint32	timehi;		/* upper 32 bits of time stamp */
	guint16	orig_len;	/* packet length */
	guint16	incl_len;	/* capture length */
	guint32	xxx[7];		/* unknown */
};

/* Returns WTAP_FILE_NETXRAY on success, WTAP_FILE_UNKNOWN on failure */
int netxray_open(wtap *wth)
{
	int bytes_read;
	char magic[sizeof netxray_magic];
	struct netxray_hdr hdr;
	double timeunit;
	int version_major;
	double t;
	static const int netxray_encap[] = {
		WTAP_ENCAP_ETHERNET,
		WTAP_ENCAP_TR
	};
	#define NUM_NETXRAY_ENCAPS (sizeof netxray_encap / sizeof netxray_encap[0])

	/* Read in the string that should be at the start of a NetXRay
	 * file */
	fseek(wth->fh, 0, SEEK_SET);
	bytes_read = fread(magic, 1, sizeof magic, wth->fh);

	if (bytes_read != sizeof magic) {
		return WTAP_FILE_UNKNOWN;
	}

	if (memcmp(magic, netxray_magic, sizeof netxray_magic) != 0) {
		return WTAP_FILE_UNKNOWN;
	}

	/* Read the rest of the header. */
	bytes_read = fread(&hdr, 1, sizeof hdr, wth->fh);
	if (bytes_read != sizeof hdr) {
		return WTAP_FILE_UNKNOWN;
	}

	/* It appears that version 1.1 files (as produced by Windows
	 * Sniffer Pro) have the time stamp in microseconds, rather
	 * than the milliseconds version 1.0 files appear to have.
	 *
	 * It also appears that version 2.001 files (as produced by
	 * Sniffer Basic 2.0?) have per-packet headers with some extra
	 * fields. */
	if (memcmp(hdr.version, vers_1_0, sizeof vers_1_0) == 0) {
		timeunit = 1000.0;
		version_major = 1;
	} else if (memcmp(hdr.version, vers_1_1, sizeof vers_1_1) == 0) {
		timeunit = 1000000.0;
		version_major = 1;
	} else if (memcmp(hdr.version, vers_2_001, sizeof vers_2_001) == 0) {
		timeunit = 1000000.0;
		version_major = 2;
	} else {
		return WTAP_FILE_UNKNOWN;
	}

	hdr.network = pletohs(&hdr.network);
	if (hdr.network >= NUM_NETXRAY_ENCAPS) {
		g_error("netxray: network type %d unknown", hdr.network);
		return WTAP_FILE_UNKNOWN;
	}

	/* This is a netxray file */
	wth->capture.netxray = g_malloc(sizeof(netxray_t));
	wth->subtype_read = netxray_read;
	wth->file_encap = netxray_encap[hdr.network];
	wth->snapshot_length = 16384;	/* XXX - not available in header */
	wth->capture.netxray->start_time = pletohl(&hdr.start_time);
	wth->capture.netxray->timeunit = timeunit;
	t = (double)pletohl(&hdr.timelo)
	    + (double)pletohl(&hdr.timehi)*4294967296.0;
	t = t/timeunit;
	wth->capture.netxray->start_timestamp = t;
	wth->capture.netxray->version_major = version_major;
	/*wth->frame_number = 0;*/
	/*wth->file_byte_offset = 0x10b;*/

	/* Remember the offset after the last packet in the capture (which
	 * isn't necessarily the last packet in the file), as it appears
	 * there's sometimes crud after it. */
	wth->capture.netxray->wrapped = 0;
	wth->capture.netxray->end_offset = pletohl(&hdr.end_offset);

	/* Seek to the beginning of the data records. */
	fseek(wth->fh, pletohl(&hdr.start_offset), SEEK_SET);

	return WTAP_FILE_NETXRAY;
}

/* Read the next packet */
int netxray_read(wtap *wth)
{
	int	packet_size;
	int	bytes_read;
	union {
		struct netxrayrec_1_x_hdr hdr_1_x;
		struct netxrayrec_2_x_hdr hdr_2_x;
	}	hdr;
	int	hdr_size;
	int	data_offset;
	double	t;

reread:
	/* Have we reached the end of the packet data? */
	data_offset = ftell(wth->fh);
	if (data_offset == wth->capture.netxray->end_offset) {
		/* Yes. */
		return 0;
	}
	/* Read record header. */
	switch (wth->capture.netxray->version_major) {

	case 1:
		hdr_size = sizeof (struct netxrayrec_1_x_hdr);
		break;

	case 2:
		hdr_size = sizeof (struct netxrayrec_2_x_hdr);
		break;
	}
	bytes_read = fread(&hdr, 1, hdr_size, wth->fh);
	if (bytes_read != hdr_size) {
		if (bytes_read != 0) {
			g_error("netxray_read: not enough packet header data (%d bytes)",
					bytes_read);
			return -1;
		}

		/* We're at EOF.  Wrap? */
		if (!wth->capture.netxray->wrapped) {
			/* Yes.  Remember that we did. */
			wth->capture.netxray->wrapped = 1;
			fseek(wth->fh, CAPTUREFILE_HEADER_SIZE, SEEK_SET);
			goto reread;
		}

		/* We've already wrapped - don't wrap again. */
		return 0;
	}
	data_offset += hdr_size;

	packet_size = pletohs(&hdr.hdr_1_x.incl_len);
	buffer_assure_space(wth->frame_buffer, packet_size);
	bytes_read = fread(buffer_start_ptr(wth->frame_buffer), 1,
			packet_size, wth->fh);

	if (bytes_read != packet_size) {
		if (ferror(wth->fh)) {
			g_error("netxray_read: fread for data: read error\n");
		} else {
			g_error("netxray_read: fread for data: %d bytes out of %d",
				bytes_read, packet_size);
		}
		return -1;
	}

	t = (double)pletohl(&hdr.hdr_1_x.timelo)
	    + (double)pletohl(&hdr.hdr_1_x.timehi)*4294967296.0;
	t /= wth->capture.netxray->timeunit;
	t -= wth->capture.netxray->start_timestamp;
	wth->phdr.ts.tv_sec = wth->capture.netxray->start_time + (long)t;
	wth->phdr.ts.tv_usec = (unsigned long)((t-(double)(unsigned long)(t))
		*1.0e6);
	wth->phdr.caplen = packet_size;
	wth->phdr.len = pletohs(&hdr.hdr_1_x.orig_len);
	wth->phdr.pkt_encap = wth->file_encap;

	return data_offset;
}

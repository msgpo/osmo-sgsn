/* BSC Multiplexer/NAT */

/*
 * (C) 2010 by Holger Hans Peter Freyther <zecke@selfish.org>
 * (C) 2010 by on-waves.com
 * (C) 2009 by Harald Welte <laforge@gnumonks.org>
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define _GNU_SOURCE
#include <getopt.h>

#include <openbsc/debug.h>
#include <openbsc/msgb.h>
#include <openbsc/bsc_msc.h>
#include <openbsc/ipaccess.h>
#include <openbsc/abis_nm.h>
#include <openbsc/talloc.h>

static const char *config_file = "openbsc.cfg";
static char *msc_address = "127.0.0.1";
static struct in_addr local_addr;
static struct bsc_fd msc_connection;
static struct bsc_fd bsc_connection;

/*
 * below are stubs we need to link
 */
int nm_state_event(enum nm_evt evt, u_int8_t obj_class, void *obj,
		   struct gsm_nm_state *old_state, struct gsm_nm_state *new_state)
{
	return -1;
}

void input_event(int event, enum e1inp_sign_type type, struct gsm_bts_trx *trx)
{}

int gsm0408_rcvmsg(struct msgb *msg, u_int8_t link_id)
{
	return -1;
}

/*
 * Below is the handling of messages coming
 * from the MSC and need to be forwarded to
 * a real BSC.
 */
static void initialize_msc_if_needed()
{
	static int init = 0;
	init = 1;

	/* do we need to send a GSM 08.08 message here? */
}

static void forward_sccp_to_bts(struct msgb *msg)
{
	/* filter, drop, patch the message? */
}

static int ipaccess_msc_cb(struct bsc_fd *bfd, unsigned int what)
{
	int error;
	struct msgb *msg = ipaccess_read_msg(bfd, &error);
	struct ipaccess_head *hh;

	if (!msg) {
		if (error == 0) {
			fprintf(stderr, "The connection to the MSC was lost, exiting\n");
			exit(-2);
		}

		fprintf(stderr, "Failed to parse ip access message: %d\n", error);
		return -1;
	}

	DEBUGP(DMSC, "MSG from MSC: %s proto: %d\n", hexdump(msg->data, msg->len), msg->l2h[0]);

	/* handle base message handling */
	hh = (struct ipaccess_head *) msg->data;
	ipaccess_rcvmsg_base(msg, bfd);

	/* initialize the networking. This includes sending a GSM08.08 message */
	if (hh->proto == IPAC_PROTO_IPACCESS && msg->l2h[0] == IPAC_MSGT_ID_ACK)
		initialize_msc_if_needed();
	else if (hh->proto == IPAC_PROTO_SCCP)
		forward_sccp_to_bts(msg);

	return 0;
}

/*
 * Below is the handling of messages coming
 * from the BSC and need to be forwarded to
 * a real BSC.
 */
static int ipaccess_listen_bsc_cb(struct bsc_fd *bfd, unsigned int what)
{
	int ret;
	struct sockaddr_in sa;
	socklen_t sa_len = sizeof(sa);

	if (!(what & BSC_FD_READ))
		return 0;

	ret = accept(bfd->fd, (struct sockaddr *) &sa, &sa_len);
	if (ret < 0) {
		perror("accept");
		return ret;
	}

	/* todo... do something with the connection */

	return 0;
}

static int listen_for_bsc(struct bsc_fd *bfd, struct in_addr *in_addr, int port)
{
	struct sockaddr_in addr;
	int ret, on = 1;

	bfd->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	bfd->cb = ipaccess_listen_bsc_cb;
	bfd->when = BSC_FD_READ;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = in_addr->s_addr;

	setsockopt(bfd->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	ret = bind(bfd->fd, (struct sockaddr *) &addr, sizeof(addr));
	if (ret < 0) {
		fprintf(stderr, "Could not bind the BSC socket %s\n",
			strerror(errno));
		return -EIO;
	}

	ret = listen(bfd->fd, 1);
	if (ret < 0) {
		perror("listen");
		return ret;
	}

	ret = bsc_register_fd(bfd);
	if (ret < 0) {
		perror("register_listen_fd");
		return ret;
	}
	return 0;
}

static void print_usage()
{
	printf("Usage: bsc_nat\n");
}

static void print_help()
{
	printf("  Some useful help...\n");
	printf("  -h --help this text\n");
	printf("  -d option --debug=DRLL:DCC:DMM:DRR:DRSL:DNM enable debugging\n");
	printf("  -s --disable-color\n");
	printf("  -c --config-file filename The config file to use.\n");
	printf("  -m --msc=IP. The address of the MSC.\n");
	printf("  -l --local=IP. The local address of this BSC.\n");
}

static void handle_options(int argc, char** argv)
{
	while (1) {
		int option_index = 0, c;
		static struct option long_options[] = {
			{"help", 0, 0, 'h'},
			{"debug", 1, 0, 'd'},
			{"config-file", 1, 0, 'c'},
			{"disable-color", 0, 0, 's'},
			{"timestamp", 0, 0, 'T'},
			{"msc", 1, 0, 'm'},
			{"local", 1, 0, 'l'},
			{0, 0, 0, 0}
		};

		c = getopt_long(argc, argv, "hd:sTPc:m:l:",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'h':
			print_usage();
			print_help();
			exit(0);
		case 's':
			debug_use_color(0);
			break;
		case 'd':
			debug_parse_category_mask(optarg);
			break;
		case 'c':
			config_file = strdup(optarg);
			break;
		case 'T':
			debug_timestamp(1);
			break;
		case 'm':
			msc_address = strdup(optarg);
			break;
		case 'l':
			inet_aton(optarg, &local_addr);
			break;
		default:
			/* ignore */
			break;
		}
	}
}

static void signal_handler(int signal)
{
	fprintf(stdout, "signal %u received\n", signal);

	switch (signal) {
	case SIGABRT:
		/* in case of abort, we want to obtain a talloc report
		 * and then return to the caller, who will abort the process */
	case SIGUSR1:
		talloc_report_full(tall_bsc_ctx, stderr);
		break;
	default:
		break;
	}
}

int main(int argc, char** argv)
{
	int rc;

	/* parse options */
	local_addr.s_addr = INADDR_ANY;
	handle_options(argc, argv);

	/* seed the PRNG */
	srand(time(NULL));

	/* connect to the MSC */
	msc_connection.cb = ipaccess_msc_cb;
	rc = connect_to_msc(&msc_connection, msc_address, 5000);
	if (rc < 0) {
		fprintf(stderr, "Opening the MSC connection failed.\n");
		exit(1);
	}

	/* wait for the BSC */
	if (listen_for_bsc(&bsc_connection, &local_addr, 5000) < 0) {
		fprintf(stderr, "Failed to listen for BSC.\n");
		exit(1);
	}

	signal(SIGINT, &signal_handler);
	signal(SIGABRT, &signal_handler);
	signal(SIGUSR1, &signal_handler);
	signal(SIGPIPE, SIG_IGN);

	while (1) {
		bsc_select_main(0);
	}

	return 0;
}

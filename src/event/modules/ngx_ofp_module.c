/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2015-2017 Ansyun <anssupport@163.com>. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Ansyun <anssupport@163.com> nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *   Author: JiaKai (jiakai1000@gmail.com) and Bluestar (anssupport@163.com)
 */

#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <netinet/in.h>
#include <termios.h>
#include <assert.h>
#include <sys/select.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#ifndef __linux__
#ifdef __FreeBSD__
#include <sys/socket.h>
#else
#include <net/socket.h>
#endif
#endif

#include <sys/time.h>
#include <netinet/tcp.h>
#include <ngx_auto_config.h>
#include "ofp.h"
#include "odp.h"
#include "ofp_errno.h"

#define _GNU_SOURCE
#define __USE_GNU
#ifdef __USE_GNU
/* Access macros for `cpu_set'.  */
#define CPU_SETSIZE __CPU_SETSIZE
#define CPU_SET(cpu, cpusetp)   __CPU_SET (cpu, cpusetp)
#define CPU_CLR(cpu, cpusetp)   __CPU_CLR (cpu, cpusetp)
#define CPU_ISSET(cpu, cpusetp) __CPU_ISSET (cpu, cpusetp)
#define CPU_ZERO(cpusetp)       __CPU_ZERO (cpusetp)
#endif
#include <unistd.h>
#include <sched.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <dlfcn.h>

#define ODP_FD_BITS 30

#include <getopt.h>

#define MAX_WORKERS 16

odp_instance_t instance;

/**
 * Parsed command line application arguments
 */
typedef struct {
	int core_count;
	int if_count;		/**< Number of interfaces to be used */
	char **if_names;	/**< Array of pointers to interface names */
	char *conf_file;
} appl_args_t;

/* helper funcs */
static void parse_args(int argc, char *argv[], appl_args_t *appl_args);
static void print_info(char *progname, appl_args_t *appl_args);
static void usage(char *progname);

ofp_global_param_t  app_init_params; /**< global OFP init parms */

/** Get rid of path in filename - only for unix-type paths using '/' */
#define NO_PATH(file_name) (strrchr((file_name), '/') ? \
				strrchr((file_name), '/') + 1 : (file_name))


/** local hook
 *
 * @param pkt odp_packet_t
 * @param protocol int
 * @return int
 *
 */
static enum ofp_return_code fastpath_local_hook(odp_packet_t pkt, void *arg)
{
	int protocol = *(int *)arg;
	(void) pkt;
	(void) protocol;
	return OFP_PKT_CONTINUE;
}

/** main() Application entry point
 *
 * @param argc int
 * @param argv[] char*
 * @return int
 *
 */
#include <sys/time.h>
#include <sys/resource.h>

int my_webserver(int if_count, char **if_name)
{
	odph_linux_pthread_t thread_tbl[MAX_WORKERS];
	appl_args_t params;
	int core_count, num_workers, i;
	odp_cpumask_t cpumask;
	char cpumaskstr[64];
	odp_pktio_param_t pktio_param;
    odp_pktin_queue_param_t pktin_param;
    odp_pktout_queue_param_t pktout_param;

	struct rlimit rlp;
	getrlimit(RLIMIT_CORE, &rlp);
	rlp.rlim_cur = 200000000;

	/* Parse and store the application arguments */
	parse_args(if_count, if_name, &params);

	/* Print both system and application information */
	print_info("webserver", &params);

	if (odp_init_global(&instance, NULL, NULL)) {
		OFP_ERR("Error: ODP global init failed.\n");
		exit(EXIT_FAILURE);
	}
	if (odp_init_local(instance, ODP_THREAD_CONTROL)) {
		OFP_ERR("Error: ODP local init failed.\n");
		exit(EXIT_FAILURE);
	}

	core_count = odp_cpu_count();
	num_workers = core_count;

	if (params.core_count)
		num_workers = params.core_count;
	if (num_workers > MAX_WORKERS)
		num_workers = MAX_WORKERS;

	/*
	 * By default core #0 runs Linux kernel background tasks.
	 * Start mapping thread from core #1
	 */
	ofp_init_global_param(&app_init_params);

	app_init_params.linux_core_id = 0;

	if (core_count > 1)
		num_workers--;

	num_workers = odp_cpumask_default_worker(&cpumask, num_workers);
	odp_cpumask_to_str(&cpumask, cpumaskstr, sizeof(cpumaskstr));

	printf("Num worker threads: %i\n", num_workers);
	printf("first CPU:          %i\n", odp_cpumask_first(&cpumask));
	printf("cpu mask:           %s\n", cpumaskstr);

	app_init_params.if_count = 0;
	app_init_params.if_names = 0;
	app_init_params.pkt_hook[OFP_HOOK_LOCAL] = fastpath_local_hook;
	/*app_init_params.burst_recv_mode = 1;*/
	if(ofp_init_global(instance, &app_init_params) < 0) {
		printf("%s: ofp_init_global failed\n", __func__);
		exit(0);
	}
	if (ofp_init_local()) {
		OFP_ERR("Error: OFP local init failed.\n");
		exit(EXIT_FAILURE);
	}

	      odp_pktio_param_init(&pktio_param);
        pktio_param.in_mode = ODP_PKTIN_MODE_DIRECT;
        pktio_param.out_mode = ODP_PKTOUT_MODE_DIRECT;

        odp_pktin_queue_param_init(&pktin_param);
        pktin_param.op_mode = ODP_PKTIO_OP_MT_UNSAFE;
        pktin_param.hash_enable = 1;
        pktin_param.hash_proto.all_bits = 7;
        pktin_param.num_queues = 1;
        char *num_queues_str = getenv("NUM_QUEUES");
        if (num_queues_str) pktin_param.num_queues = atoi(num_queues_str);
        /*
        odp_pktin_queue_param_init(&pktin_param);
        pktin_param.op_mode = ODP_PKTIO_OP_MT_UNSAFE;
        pktin_param.hash_enable = 1;
        pktin_param.hash_proto.proto.ipv4_tcp = 1;
        pktin_param.num_queues = num_workers;
        */


        odp_pktout_queue_param_init(&pktout_param);
        pktout_param.num_queues = num_workers;
        pktout_param.op_mode = ODP_PKTIO_OP_MT_UNSAFE;

        for (i = 0; i < params.if_count; i++) {
                if (ofp_ifnet_create(instance,
                                params.if_names[i],
                                &pktio_param,
                                &pktin_param,
                                &pktout_param) < 0) {
                        OFP_ERR("Failed to init interface %s",
                                params.if_names[i]);
                        exit(EXIT_FAILURE);
                }
        }

	memset(thread_tbl, 0, sizeof(thread_tbl));
	/* Start dataplane dispatcher worker threads */

/*	odph_linux_pthread_create(thread_tbl,
				  &cpumask,
				  default_event_dispatcher,
				  ofp_eth_vlan_processing);
*/
	/* other app code here.*/
	/* Start CLI */
	ofp_start_cli_thread(instance, app_init_params.linux_core_id, params.conf_file);

	OFP_INFO("HTTP thread started");

	//odp_init_local(ODP_THREAD_CONTROL);
	//ofp_init_local();
	sleep (1);

	//odph_linux_pthread_join(thread_tbl, num_workers);
	printf("End Main()\n");

	return 0;
}

/**
 * Parse and store the command line arguments
 *
 * @param argc       argument count
 * @param argv[]     argument vector
 * @param appl_args  Store application arguments here
 */
static void parse_args(int argc, char *argv[], appl_args_t *appl_args)
{
	memset(appl_args, 0, sizeof(*appl_args));
	appl_args->if_count = argc;

	/* allocate storage for the if names */
	appl_args->if_names =
		calloc(appl_args->if_count, sizeof(char *));
	appl_args->if_names = argv;

	char *filename = malloc(sizeof(NGX_CONF_PATH) + 1);
	if (filename == NULL) {
		fprintf(stderr, "Could not allocate memory\n");
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	strcpy(filename, NGX_CONF_PATH);
	char *slash = strrchr(filename, '/');

	/* IN MOST CASES this replaces /nginx.conf, which is longer than
	 * /ofp.conf, so we should be safe, except when the user gives the file
	 * a shorter name, in which case the buffer overflow is deserved */
	strcpy(slash, "/ofp.conf");

	int len = strlen(filename);
	printf("len : %x, file : %s\n", len, filename);

	appl_args->conf_file = filename;
}

/**
 * Prinf usage information
 */
static void usage(char *progname)
{
	printf("\n"
		"Usage: %s OPTIONS\n"
		"  E.g. %s -i eth1,eth2,eth3\n"
		"\n"
		"ODPFastpath application.\n"
		"\n"
		"Mandatory OPTIONS:\n"
		" -i, --interface Eth interfaces (comma-separated, no spaces)\n"
		"\n"
		"Optional OPTIONS\n"
		"  -c, --count <number> Core count.\n"
		"  -h, --help           Display help and exit.\n"
		"\n", NO_PATH(progname), NO_PATH(progname)
		);
}

/**
 * Print system and application info
 */
static void print_info(char *progname, appl_args_t *appl_args)
{
	int i;

	printf("\n"
		"ODP system info\n"
		"---------------\n"
		"ODP API version: %s\n"
		"CPU model:       %s\n"
		"CPU freq (hz):   %lu\n"
		"Cache line size: %i\n"
		"Core count:      %i\n"
		"\n",
		odp_version_api_str(), odp_cpu_model_str(),
		odp_cpu_hz_max(), odp_sys_cache_line_size(),
		odp_cpu_count());

	printf("Running ODP appl: \"%s\"\n"
		"-----------------\n"
		"IF-count:        %i\n"
		"Using IFs:      ",
		progname, appl_args->if_count);
	for (i = 0; i < appl_args->if_count; ++i)
		printf(" %s", appl_args->if_names[i]);

	printf("\n\n");
	fflush(NULL);
}

static int inited;
static int ofp_fd;

static int (*real_socket)(int, int, int);
static int (*real_bind)(int, __CONST_SOCKADDR_ARG, socklen_t);
static int (*real_listen)(int, int);
static int (*real_setsockopt)(int, int, int, const void *, socklen_t);
static int (*real_ioctl)(int, int, void *);
static int (*real_select) (int nfds, fd_set *readfds, fd_set *writefds,
			   fd_set *exceptfds, struct timeval *timeout);
static int (*real_accept)(int, struct sockaddr *, socklen_t *);
static int (*real_close)(int);
static ssize_t (*real_recv)(int, void *, size_t, int);
static ssize_t (*real_send)(int, const void *, size_t, int);
static ssize_t (*real_sendfile64)(int, int, off_t *, size_t);
static int (*real_printf)(const char *__fmt, ...);
static ssize_t (*real_writev)(int, const struct iovec *, int);

void ngx_ofp_init()
{
	int rc;

#define INIT_FUNCTION(func) \
        real_##func = dlsym(RTLD_NEXT, #func); \
        assert(real_##func)

	INIT_FUNCTION(socket);
	INIT_FUNCTION(bind);
	INIT_FUNCTION(listen);
	INIT_FUNCTION(setsockopt);

	INIT_FUNCTION(ioctl);
	INIT_FUNCTION(select);
	INIT_FUNCTION(printf);

	INIT_FUNCTION(sendfile64);
	INIT_FUNCTION(accept);
	INIT_FUNCTION(close);
	INIT_FUNCTION(recv);
	INIT_FUNCTION(send);
	INIT_FUNCTION(writev);
#undef INIT_FUNCTION

	char *p=malloc(2);
	strncpy(p, "0", 2);
	my_webserver(1, &p);

	rc = 0;
	assert(0 == rc);
	inited = 1;
}

int socket(int domain, int type, int protocol)
{
	int rc;

	if ((inited != 1) || (AF_INET != domain) ||
		(SOCK_STREAM != type && SOCK_DGRAM != type)) {
	     rc = real_socket(domain, type, protocol);
	return rc;
	}

	if (AF_INET == domain && SOCK_STREAM == type)
		rc = ofp_socket(OFP_AF_INET, OFP_SOCK_STREAM, OFP_IPPROTO_TCP);
	else
		rc = ofp_socket(domain, type, protocol);
	rc |= 1 << ODP_FD_BITS;

	if (ofp_fd == 0)
	  ofp_fd = rc;
	return rc;
}

int bind (int __fd, __CONST_SOCKADDR_ARG __addr, socklen_t __len)
{
	if (__fd & (1 << ODP_FD_BITS)) {
		struct ofp_sockaddr ofp_addr;
		int ret = -9;
		memcpy(&ofp_addr, __addr, sizeof(ofp_addr));
		if (ofp_addr.sa_len != sizeof(struct ofp_sockaddr))
			ofp_addr.sa_len = sizeof(struct ofp_sockaddr);
		__fd &= ~(1 << ODP_FD_BITS);
		ret = ofp_bind(__fd, (const struct ofp_sockaddr *)&ofp_addr,
			ofp_addr.sa_len);

		return ret;
	} else {
	    return real_bind(__fd, __addr, __len);
	}
}


int select (int nfds, fd_set *readfds, fd_set *writefds,
                  fd_set *exceptfds, struct timeval *timeout)
{
	int rc = 0;
	if (nfds & (1 << ODP_FD_BITS)) {
		struct ofp_timeval __timeout;

		__timeout.tv_sec = 0;
		__timeout.tv_usec = 0;

		ofp_fd_set *__readfds = NULL, *__writefds = NULL;
		ofp_fd_set *__exceptfds = NULL;

		if (readfds) {
			__readfds = (ofp_fd_set *)readfds;
		}

		if (writefds) {
			__writefds = (ofp_fd_set *)writefds;
		}

		if (exceptfds) {
			__exceptfds = (ofp_fd_set *)exceptfds;
		}

		nfds &= ~(1 << ODP_FD_BITS);

		rc = ofp_select(nfds, __readfds, __writefds,
				__exceptfds, &__timeout);
	} else {
		rc = real_select(nfds, readfds, writefds, exceptfds, timeout);
	}

	return rc;
}

int setsockopt (int __fd, int __level, int __optname,
               const void *__optval, socklen_t __optlen)
{
	if (__fd & (1 << ODP_FD_BITS)) {
		__fd &= ~(1 << ODP_FD_BITS);
		if (__level == SOL_TCP) {
			switch (__optname) {
			case TCP_CORK:
				__optname = OFP_TCP_CORK;
				break;
			default:
				return 0;
			}
		}
		return ofp_setsockopt(__fd, __level, __optname, __optval, __optlen);
	} else {
		return real_setsockopt(__fd, __level, __optname, __optval, __optlen);
	}
}

int listen (int __fd, int __n)
{
	if (__fd & (1 << ODP_FD_BITS)) {
		__fd &= ~(1 << ODP_FD_BITS);
		return ofp_listen(__fd, __n);
	} else {
		return real_listen(__fd, __n);
	}
}


int ioctl(int fd, int request, void *p)
{
	int ret = -99;

	if (fd & (1 << ODP_FD_BITS)) {
		fd &= ~(1 << ODP_FD_BITS);

		ret = ofp_ioctl(fd, OFP_FIONBIO, p);

		if (ofp_errno == OFP_EOPNOTSUPP)
			ret = 0;
		return ret;
	} else {
		return real_ioctl(fd, request, p);
	}
}

ssize_t send (int __fd, const void *__buf, size_t __n, int __flags)
{
	if (__fd & (1 << ODP_FD_BITS)) {
		__fd &= ~(1 << ODP_FD_BITS);
		return ofp_send(__fd, __buf, __n, __flags);
	} else {
		return real_send(__fd, __buf, __n, __flags);
	}
}

ssize_t recv (int __fd, void *__buf, size_t __n, int __flags)
{
	ssize_t rc;
	if (__fd & (1 << ODP_FD_BITS)) {
		__fd &= ~(1 << ODP_FD_BITS);
		rc = ofp_recv(__fd, __buf, __n, __flags);
		if (-1 == rc && OFP_EAGAIN == ofp_errno) {
			errno = EAGAIN;
		}
		return rc;
	} else {
		return real_recv(__fd, __buf, __n, __flags);
	}
}

int accept (int sockfd, __SOCKADDR_ARG addr,
               socklen_t *__restrict addrlen)
{
	int rc = 0;

	if (sockfd & (1 << ODP_FD_BITS)) {
		struct ofp_sockaddr ofp_addr;
		memcpy(&ofp_addr, addr, sizeof(ofp_addr));
		if (ofp_addr.sa_len != sizeof(struct ofp_sockaddr))
			ofp_addr.sa_len = sizeof(struct ofp_sockaddr);

		sockfd &= ~(1 << ODP_FD_BITS);

		rc = ofp_accept(sockfd, &ofp_addr, addrlen);
		addr->sa_family = AF_INET;
		memcpy(addr->sa_data, ofp_addr.sa_data, sizeof(addr->sa_data));

		rc |= 1 << ODP_FD_BITS;
		if (-1 == rc && OFP_EAGAIN == ofp_errno) {
			errno = EAGAIN;
		}
	} else {
		rc = real_accept(sockfd, addr, addrlen);
	}
	return rc;
}

int close(int fd)
{
	if (fd & (1 << ODP_FD_BITS)) {
		fd &= ~(1 << ODP_FD_BITS);
		return ofp_close(fd);
	} else {
		if (real_close == NULL)
			real_close = dlsym(RTLD_NEXT, "close");
		return real_close(fd);
	}
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
	ssize_t rc, i, n;
	int nwrite = 0, data_size;
	char *buf;

	if (fd & (1 << ODP_FD_BITS))
	{
		fd &= ~(1 << ODP_FD_BITS);
		rc = 0;
		for (i = 0; i != iovcnt; ++i)
		{
			data_size = iov[i].iov_len;
			buf = iov[i].iov_base;
			n = data_size;

			while (n > 0)
			{
				nwrite = ofp_send(fd, buf + data_size - n, n, 0);

				if (nwrite > 0) rc += nwrite;

				if (rc && nwrite < n) return rc;

				if (nwrite < 0) {
					errno = 0;
					if (ofp_errno==OFP_EAGAIN || ofp_errno==OFP_ENOBUFS)
						errno = EAGAIN;
				}

				if (nwrite <= 0) return nwrite;

				n -= nwrite;
			}
		}
	}
	else
	{
		rc = real_writev(fd, iov, iovcnt);
	}

	return rc;
}



#define BUF_SIZE (1<<14)

ssize_t sendfile64(int out_fd, int in_fd, off_t *offset, size_t count)
{
	off_t orig = 0;
	char buf[BUF_SIZE];
	size_t total_sent = 0;
	int num_sent, num_read;
	int ret_err = 0;

	if (out_fd & (1 << ODP_FD_BITS))
	{
		out_fd &= ~(1 << ODP_FD_BITS);

		errno = 0;

		if ((orig = lseek(in_fd, 0, SEEK_CUR)) < 0) return -1;

		if (offset && lseek(in_fd, *offset, SEEK_SET) < 0) return -1;

		while (count > 0) {
			int to_read = count < BUF_SIZE ? count : BUF_SIZE;
			num_read = read(in_fd, buf, to_read);

			if (num_read < 1) {
				if (total_sent) break;
				return -1;
			}

			num_sent = ofp_send(out_fd, buf, num_read, 0);

			if (num_sent < 1) {
				if (total_sent) break;
				if (ofp_errno==OFP_EAGAIN || ofp_errno==OFP_ENOBUFS)
					errno = EAGAIN;
				ret_err = 1;
				break;
			}

			total_sent += num_sent;

			if (num_sent != num_read) break;

			count -= num_sent;
		}

		if (offset) {
			*offset += total_sent;
			if (lseek(in_fd, orig, SEEK_SET) < 0) return -1;
		} else {
			if (lseek(in_fd, orig+total_sent, SEEK_SET) < 0) return -1;
		}

		if (ret_err) return -1;

		return total_sent;
	}

	return real_sendfile64( out_fd, in_fd, offset, count);
}

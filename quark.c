/* See LICENSE file for license details. */
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>

#include "arg.h"

#define HEADER_MAX 4096
size_t hlen;
char *argv0, header[HEADER_MAX];

#include "config.h"

#undef MIN
#define MIN(x,y)  ((x) < (y) ? (x) : (y))

#define TIMESTAMP_LEN 30

enum req_field {
	REQ_HOST,
	REQ_RANGE,
	REQ_MOD,
	NUM_REQ_FIELDS,
};

static char *req_field_str[] = {
	[REQ_HOST]    = "Host",
};

enum req_method {
	M_GET,
	M_HEAD,
	NUM_REQ_METHODS,
};

static char *req_method_str[] = {
	[M_GET]  = "GET",
	[M_HEAD] = "HEAD",
};

struct request {
	enum req_method method;
	char target[PATH_MAX];
	char field[NUM_REQ_FIELDS][FIELD_MAX];
};

enum status {
	S_OK                    = 200,
	S_PARTIAL_CONTENT       = 206,
	S_MOVED_PERMANENTLY     = 301,
	S_NOT_MODIFIED          = 304,
	S_BAD_REQUEST           = 400,
	S_FORBIDDEN             = 403,
	S_NOT_FOUND             = 404,
	S_METHOD_NOT_ALLOWED    = 405,
	S_REQUEST_TIMEOUT       = 408,
	S_RANGE_NOT_SATISFIABLE = 416,
	S_REQUEST_TOO_LARGE     = 431,
	S_INTERNAL_SERVER_ERROR = 500,
	S_VERSION_NOT_SUPPORTED = 505,
};

static char *status_str[] = {
	[S_OK]                    = "OK",
	[S_PARTIAL_CONTENT]       = "Partial Content",
	[S_MOVED_PERMANENTLY]     = "Moved Permanently",
	[S_NOT_MODIFIED]          = "Not Modified",
	[S_BAD_REQUEST]           = "Bad Request",
	[S_FORBIDDEN]             = "Forbidden",
	[S_NOT_FOUND]             = "Not Found",
	[S_METHOD_NOT_ALLOWED]    = "Method Not Allowed",
	[S_REQUEST_TIMEOUT]       = "Request Time-out",
	[S_RANGE_NOT_SATISFIABLE] = "Range Not Satisfiable",
	[S_REQUEST_TOO_LARGE]     = "Request Header Fields Too Large",
	[S_INTERNAL_SERVER_ERROR] = "Internal Server Error",
	[S_VERSION_NOT_SUPPORTED] = "HTTP Version not supported",
};

char **hosts;
int numhosts;

static char *
timestamp(time_t t, char buf[TIMESTAMP_LEN])
{
	strftime(buf, TIMESTAMP_LEN, "%a, %d %b %Y %T GMT", gmtime(&t));

	return buf;
}

static void
decode(char src[PATH_MAX], char dest[PATH_MAX])
{
	size_t i;
	uint8_t n;
	char *s;

	for (s = src, i = 0; *s; s++, i++) {
		if (*s == '+') {
			dest[i] = ' ';
		} else if (*s == '%' && (sscanf(s + 1, "%2hhx", &n) == 1)) {
			dest[i] = n;
			s += 2;
		} else {
			dest[i] = *s;
		}
	}
	dest[i] = '\0';
}

static enum status
sendstatus(int fd, enum status s)
{
	static char t[TIMESTAMP_LEN];

	if (dprintf(fd,
	            "HTTP/1.1 %d %s\r\n"
	            "Date: %s\r\n"
	            "Connection: close\r\n"
	            "%s"
	            "Content-Type: text/html\r\n"
	            "\r\n"
	            "<!DOCTYPE html>\n<html>\n\t<head>\n"
	            "\t\t<title>%d %s</title>\n\t</head>\n\t<body>\n"
	            "\t\t<h1>%d %s</h1>\n\t</body>\n</html>\n",
	            s, status_str[s], timestamp(time(NULL), t),
	            (s == S_METHOD_NOT_ALLOWED) ? "Allow: HEAD, GET\r\n" : "",
	            s, status_str[s], s, status_str[s]) < 0) {
		return S_REQUEST_TIMEOUT;
	}

	return s;
}

static int
getrequest(int fd, struct request *r)
{
	size_t i, mlen;
	ssize_t off;
	char *p, *q;

	/* empty all fields */
	memset(r, 0, sizeof(*r));

	/*
	 * receive header
	 */
	for (hlen = 0; ;) {
		if ((off = read(fd, header + hlen, sizeof(header) - hlen)) < 0) {
			return sendstatus(fd, S_REQUEST_TIMEOUT);
		} else if (off == 0) {
			break;
		}
		hlen += off;
		if (hlen >= 4 && !memcmp(header + hlen - 4, "\r\n\r\n", 4)) {
			break;
		}
		if (hlen == sizeof(header)) {
			return sendstatus(fd, S_REQUEST_TOO_LARGE);
		}
	}

	/* remove terminating empty line */
	if (hlen < 2) {
		return sendstatus(fd, S_BAD_REQUEST);
	}
	/* hlen -= 2; */

	/* null-terminate the header */
	header[hlen] = '\0';

	/*
	 * parse request line
	 */

	/* METHOD */
	for (i = 0; i < NUM_REQ_METHODS; i++) {
		mlen = strlen(req_method_str[i]);
		if (!strncmp(req_method_str[i], header, mlen)) {
			r->method = i;
			break;
		}
	}
	if (i == NUM_REQ_METHODS) {
		return sendstatus(fd, S_METHOD_NOT_ALLOWED);
	}

	/* a single space must follow the method */
	if (header[mlen] != ' ') {
		return sendstatus(fd, S_BAD_REQUEST);
	}

	/* basis for next step */
	p = header + mlen + 1;

	/* TARGET */
	if (!(q = strchr(p, ' '))) {
		return sendstatus(fd, S_BAD_REQUEST);
	}
	/*q = '\0';*/
	if (q - p + 1 > PATH_MAX) {
		return sendstatus(fd, S_REQUEST_TOO_LARGE);
	}
	memcpy(r->target, p, q - p + 1);
	decode(r->target, r->target);

	/* basis for next step */
	p = q + 1;

	/* HTTP-VERSION */
	if (strncmp(p, "HTTP/", sizeof("HTTP/") - 1)) {
		return sendstatus(fd, S_BAD_REQUEST);
	}
	p += sizeof("HTTP/") - 1;
	if (strncmp(p, "1.0", sizeof("1.0") - 1) &&
	    strncmp(p, "1.1", sizeof("1.1") - 1)) {
		return sendstatus(fd, S_VERSION_NOT_SUPPORTED);
	}
	p += sizeof("1.*") - 1;

	/* check terminator */
	if (strncmp(p, "\r\n", sizeof("\r\n") - 1)) {
		return sendstatus(fd, S_BAD_REQUEST);
	}

	/* basis for next step */
	p += sizeof("\r\n") - 1;

	/*
	 * parse request-fields
	 */

	/* match field type */
	for (; *p != '\0';) {
		for (i = 0; i < NUM_REQ_FIELDS; i++) {
			if (!strncasecmp(p, req_field_str[i],
			                 strlen(req_field_str[i]))) {
				break;
			}
		}
		if (i == NUM_REQ_FIELDS) {
			/* unmatched field, skip this line */
			if (!(q = strstr(p, "\r\n"))) {
				return sendstatus(fd, S_BAD_REQUEST);
			}
			p = q + (sizeof("\r\n") - 1);
			continue;
		}

		p += strlen(req_field_str[i]);

		/* a single colon must follow the field name */
		if (*p != ':') {
			return sendstatus(fd, S_BAD_REQUEST);
		}

		/* skip whitespace */
		for (++p; *p == ' ' || *p == '\t'; p++)
			;

		/* extract field content */
		if (!(q = strstr(p, "\r\n"))) {
			return sendstatus(fd, S_BAD_REQUEST);
		}
		/* *q = '\0'; */
		if (q - p + 1 > FIELD_MAX) {
			return sendstatus(fd, S_REQUEST_TOO_LARGE);
		}
		memcpy(r->field[i], p, q - p + 1);

		/* go to next line */
		p = q + (sizeof("\r\n") - 1);
	}

	return 0;
}

int hostcmp(char *s, char *t)
{
	for (; *s; s++, t++) {
		if (*s == *t) {
			continue;
		}
		if (*s == '@') {
			if (*t == '.' || *t == '/' || *t == '\0' || *t == ':') {
				/*
				 * ./hosttp www@9000
				 * Host: www.jer.cx
				 *
				 * ./hosttp jer@9000
				 * Host: jer.cx
				 */
				return 0;
			} else {
				/*
				 * ./hosttp www@9000
				 * Host: wwwaaaa.luigi.co
				 */
				break;
			}
		}
	}
	return *s - *t;
}

int
open_remote_host(char *host, int port)
{
	struct sockaddr_in rem_addr;
	int len, fl, s, x;
	struct hostent *H;
	int on = 1;

	H = gethostbyname(host);

	if (!H)
		return (-2);

	len = sizeof(rem_addr);

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return s;

	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, 4);

	len = sizeof(rem_addr);
	memset(&rem_addr, '\0', len);
	rem_addr.sin_family = AF_INET;
	memcpy(&rem_addr.sin_addr, H->h_addr, H->h_length);
	rem_addr.sin_port = htons(port);
	if ((x = connect(s, (struct sockaddr *) &rem_addr, len))) {
		close(s);
		return x;
	}
	if ((fl = fcntl(s, F_GETFL, 0)) < 0) {
		fprintf(stderr, "%s: fcntl GETFL: %s\n", argv0, strerror(errno));
		exit(1);
	}
	if (fcntl(s, F_SETFL, fl | O_NONBLOCK) < 0) {
		fprintf(stderr, "%s: fcntl SETFL: %s\n", argv0, strerror(errno));
		exit(1);
	}

	return s;
}


static enum status
proxy(int fd, struct request *r)
{
	size_t i, bread, bwritten;
	int sfd, port;
	static char buf[BUFSIZ];
	char *p;


	if (r->field[REQ_HOST] == NULL) {
		return sendstatus(fd, S_BAD_REQUEST);
	}

	for (i = 0; i < numhosts; i++) {
		if (hostcmp(hosts[i], r->field[REQ_HOST]) == 0) {
			p = strchr(hosts[i], '@') + 1;
			if (r->field[REQ_HOST] == NULL || (port = atoi(p)) < 0) {
				return sendstatus(fd, S_BAD_REQUEST);
			}
		}
	}

	if ((sfd = open_remote_host("localhost", port)) < 0) {
		return sendstatus(fd, S_INTERNAL_SERVER_ERROR);
	}

	p = header;
	while (hlen > 0) {
		if ((bwritten = write(sfd, p, hlen)) <= 0)
			return sendstatus(fd, S_INTERNAL_SERVER_ERROR);
		hlen -= bwritten;
		p += bwritten;
	}

	while ((bread = read(sfd, buf, sizeof(buf)))) {
		if (bread < 0) {
			return S_INTERNAL_SERVER_ERROR;
		}
		p = buf;
		while (bread > 0) {
			bwritten = write(fd, p, bread);
			if (bwritten <= 0) {
				return S_REQUEST_TIMEOUT;
			}
			bread -= bwritten;
			p += bwritten;
		}
	}
	close(fd);
	close(sfd);
	return 200;
}

static void
serve(int insock)
{
	struct request r;
	struct sockaddr_storage in_sa;
	struct timeval tv;
	pid_t p;
	socklen_t in_sa_len;
	time_t t;
	enum status status;
	int infd;
	char inip4[INET_ADDRSTRLEN], inip6[INET6_ADDRSTRLEN], tstmp[25];

	while (1) {
		/* accept incoming connections */
		in_sa_len = sizeof(in_sa);
		if ((infd = accept(insock, (struct sockaddr *)&in_sa,
		                   &in_sa_len)) < 0) {
			fprintf(stderr, "%s: accept: %s\n", argv0,
			        strerror(errno));
			continue;
		}

		/* fork and handle */
		switch ((p = fork())) {
		case -1:
			fprintf(stderr, "%s: fork: %s\n", argv0,
			        strerror(errno));
			break;
		case 0:
			close(insock);

			/* set connection timeout */
			tv.tv_sec = 30;
			tv.tv_usec = 0;
			if (setsockopt(infd, SOL_SOCKET, SO_RCVTIMEO, &tv,
			               sizeof(tv)) < 0 ||
			    setsockopt(infd, SOL_SOCKET, SO_SNDTIMEO, &tv,
			               sizeof(tv)) < 0) {
				fprintf(stderr, "%s: setsockopt: %s\n",
				        argv0, strerror(errno));
				return;
			}

			/* handle request */
			if (!(status = getrequest(infd, &r))) {
				status = proxy(infd, &r);
			}

			/* write output to log */
			t = time(NULL);
			strftime(tstmp, sizeof(tstmp), "%Y-%m-%dT%H:%M:%S",
			         gmtime(&t));

			if (in_sa.ss_family == AF_INET) {
				inet_ntop(AF_INET,
				          &(((struct sockaddr_in *)&in_sa)->sin_addr),
				          inip4, sizeof(inip4));
				printf("%s\t%s\t%d\t%s\n", tstmp, inip4,
				       status, r.target);
			} else {
				inet_ntop(AF_INET6,
				          &(((struct sockaddr_in6*)&in_sa)->sin6_addr),
				          inip6, sizeof(inip6));
				printf("%s\t%s\t%d\t%s\n", tstmp, inip6,
				       status, r.target);
			}

			/* clean up and finish */
			shutdown(infd, SHUT_RD);
			shutdown(infd, SHUT_WR);
			close(infd);
			_exit(0);
		default:
			/* close the connection in the parent */
			close(infd);
		}
	}
}

void
die(const char *errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);

	exit(1);
}

static int
getipsock(void)
{
	struct addrinfo hints, *ai, *p;
	int ret, insock = 0, yes;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((ret = getaddrinfo(host, port, &hints, &ai))) {
		die("%s: getaddrinfo: %s\n", argv0, gai_strerror(ret));
	}

	for (yes = 1, p = ai; p; p = p->ai_next) {
		if ((insock = socket(p->ai_family, p->ai_socktype,
		                     p->ai_protocol)) < 0) {
			continue;
		}
		if (setsockopt(insock, SOL_SOCKET, SO_REUSEADDR, &yes,
		               sizeof(int)) < 0) {
			die("%s: setsockopt: %s\n", argv0, strerror(errno));
		}
		if (bind(insock, p->ai_addr, p->ai_addrlen) < 0) {
			close(insock);
			continue;
		}
		break;
	}
	freeaddrinfo(ai);
	if (!p) {
		die("%s: failed to bind\n", argv0);
	}

	if (listen(insock, SOMAXCONN) < 0) {
		die("%s: listen: %s\n", argv0, strerror(errno));
	}

	return insock;
}

static int
getusock(char *udsname)
{
	struct sockaddr_un addr;
	size_t udsnamelen;
	int insock;

	if ((insock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		die("%s: socket: %s\n", argv0, strerror(errno));
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;

	if ((udsnamelen = strlen(udsname)) > sizeof(addr.sun_path) - 1) {
		die("%s: UNIX-domain socket name truncated\n", argv0);
	}
	memcpy(addr.sun_path, udsname, udsnamelen + 1);

	unlink(udsname);

	if (bind(insock, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
		die("%s: bind: %s\n", argv0, strerror(errno));
	}

	if (listen(insock, SOMAXCONN) < 0) {
		die("%s: listen: %s\n", argv0, strerror(errno));
	}

	return insock;
}

static void
usage(void)
{
	die("usage: %s [-v] [[[-h host] [-p port]] | [-U udsocket]] "
	    "[-d dir] [-l] [-L]\n", argv0);
}

int
main(int argc, char *argv[])
{
	struct rlimit rlim;
	int insock;
	char *udsname = NULL;

	ARGBEGIN {
	case 'd':
		servedir = EARGF(usage());
		break;
	case 'h':
		host = EARGF(usage());
		break;
	case 'p':
		port = EARGF(usage());
		break;
	case 'U':
		udsname = EARGF(usage());
		break;
	case 'v':
		fputs("quark-"VERSION"\n", stderr);
		return 0;
	default:
		usage();
	} ARGEND

	if (argc < 1) {
		usage();
	}

	hosts = argv;
	numhosts = argc;

	for (; argc > 0; argc--, argv++) {
		if (strchr(*argv, '@') == NULL) {
			fprintf(stderr, "%s: '%s' missing '@'\n", argv0, *argv);
			exit(1);
		}
	}

	/* reap children automatically */
	if (signal(SIGCHLD, SIG_IGN) == SIG_ERR) {
		fprintf(stderr, "%s: signal: Failed to set SIG_IGN on"
		        "SIGCHLD\n", argv0);
		return 1;
	}

	/* raise the process limit */
	rlim.rlim_cur = rlim.rlim_max = maxnprocs;
	if (setrlimit(RLIMIT_NPROC, &rlim) < 0) {
		fprintf(stderr, "%s: setrlimit RLIMIT_NPROC: %s\n", argv0,
		        strerror(errno));
		return 1;
	}

	/* bind socket */
	insock = udsname ? getusock(udsname) : getipsock();

	/* chroot */
	if (servedir && chdir(servedir) < 0) {
		die("%s: chdir %s: %s\n", argv0, servedir, strerror(errno));
	}

	serve(insock);
	close(insock);

	return 0;
}

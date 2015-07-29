/*
 * Copyright (c) 2013-2015 Erik Ekman <yarrick@kryo.se>
 *
 * Permission to use, copy, modify, and/or distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright notice
 * and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
#include "host.h"
#include "net.h"
#include "chunk.h"

#include <time.h>
#include <assert.h>

#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

#include <sys/param.h>

struct linked_gaicb {
	struct gaicb gaicb;
	struct linked_gaicb *next;
};

struct eval_host {
	struct host *host;
	struct timespec sendtime;
	uint16_t cur_seqno;
	uint16_t id;
	uint8_t *payload;
	size_t payload_len;
};

static const struct addrinfo addr_request = {
	.ai_family = AF_UNSPEC,
	.ai_socktype = SOCK_RAW,
};

int host_make_resolvlist(FILE *file, struct gaicb **list[])
{
	int hosts = 0;
	int i;

	struct gaicb **l;
	struct linked_gaicb *head = NULL;
	struct linked_gaicb *tail = NULL;
	struct linked_gaicb *lg;

	for (;;) {
		char hostname[300];
		int res;

		memset(hostname, 0, sizeof(hostname));
		res = fscanf(file, "%256s", hostname);
		if (res == EOF) break;

		lg = calloc(1, sizeof(*lg));
		lg->gaicb.ar_name = strndup(hostname, strlen(hostname));
		lg->gaicb.ar_request = &addr_request;
		if (!head) head = lg;
		if (tail) tail->next = lg;
		tail = lg;
		hosts++;
	}

	l = calloc(hosts, sizeof(struct gaicb*));
	lg = head;
	for (i = 0; i < hosts; i++) {
		l[i] = &lg->gaicb;
		lg = lg->next;
	}

	*list = l;

	return hosts;
}

void host_free_resolvlist(struct gaicb *list[], int length)
{
	int i;

	for (i = 0; i < length; i++) {
		free((char *) list[i]->ar_name);
		if (list[i]->ar_result) freeaddrinfo(list[i]->ar_result);
		free(list[i]);
	}
	free(list);
}

struct host *host_create(struct gaicb *list[], int listlength)
{
	struct host *hosts = NULL;
	struct host *last = NULL;
	int i;

	for (i = 0; i < listlength; i++) {
		if (gai_error(list[i]) == 0) {
			struct addrinfo *result = list[i]->ar_result;
			while (result) {
				struct host *h = calloc(1, sizeof(struct host));
				memcpy(&h->sockaddr, result->ai_addr, result->ai_addrlen);
				h->sockaddr_len = result->ai_addrlen;
				if (!hosts)
					hosts = h;
				if (last)
					last->next = h;
				last = h;

				result = result->ai_next;
			}
		}
	}
	return hosts;
}

static uint64_t latency_sum_us;
static uint32_t latency_count;

static void diff_add(struct timespec *start, struct timespec *end)
{
	uint64_t us = (end->tv_sec - start->tv_sec) * 1000000;
	us -= start->tv_nsec / 1000;
	us += end->tv_nsec / 1000;

	latency_sum_us += us;
	latency_count++;
}

struct evaldata {
	struct eval_host *hosts;
	int count;
};

static void eval_reply(void *userdata, struct sockaddr_storage *addr,
	size_t addrlen, uint16_t id, uint16_t seqno, const uint8_t *data, size_t len)
{
	int i;
	struct evaldata *eval = (struct evaldata *) userdata;
	struct timespec recvtime;
	clock_gettime(CLOCK_MONOTONIC_RAW, &recvtime);
	for (i = 0; i < eval->count; i++) {
		struct eval_host *eh = &eval->hosts[i];
		if (addrlen == eh->host->sockaddr_len &&
			memcmp(addr, &eh->host->sockaddr, addrlen) == 0 &&
			eh->payload_len == len &&
			memcmp(data, eh->payload, eh->payload_len) == 0 &&
			eh->id == id &&
			eh->cur_seqno == seqno) {

			/* Store accepted reply */
			eh->host->rx_icmp++;
			/* Use new seqno for next packet */
			eh->cur_seqno++;
			diff_add(&eh->sendtime, &recvtime);
			break;
		}
	}
}

int host_evaluate(struct host **hosts, int length)
{
	int i;
	int addr;
	int good_hosts;
	struct host *h;
	struct host *prev;
	struct evaldata evaldata;
	uint8_t eval_payload[CHUNK_SIZE];

	evaldata.hosts = calloc(length, sizeof(struct eval_host));
	evaldata.count = length;

	for (i = 0; i < sizeof(eval_payload); i++) {
		eval_payload[i] = i & 0xff;
	}

	addr = 0;
	h = *hosts;
	for (i = 0; i < length; i++) {
		evaldata.hosts[addr].host = h;
		evaldata.hosts[addr].id = addr;
		evaldata.hosts[addr].cur_seqno = addr * 2;
		evaldata.hosts[addr].payload = eval_payload;
		evaldata.hosts[addr].payload_len = sizeof(eval_payload);
		addr++;
		h = h->next;
	}

	printf("Evaluating %d hosts.", length);
	for (i = 0; i < 5; i++) {
		int h;

		printf(".");
		fflush(stdout);
		for (h = 0; h < length; h++) {
			clock_gettime(CLOCK_MONOTONIC_RAW, &evaldata.hosts[h].sendtime);
			evaldata.hosts[h].host->tx_icmp++;
			net_send(evaldata.hosts[h].host,
				evaldata.hosts[h].id,
				evaldata.hosts[h].cur_seqno,
				evaldata.hosts[h].payload,
				evaldata.hosts[h].payload_len);
		}

		for (;;) {
			struct timeval tv;
			int i;

			tv.tv_sec = 1;
			tv.tv_usec = 0;
			i = net_recv(&tv, eval_reply, &evaldata);
			if (!i)
				break; /* No action for 1 second, break.. */
		}
	}
	printf(" done.\n");

	h = *hosts;
	prev = NULL;
	good_hosts = 0;
	while (h) {
		struct host *next = h->next;
		/* Filter out non-100% hosts from list */
		if (h->tx_icmp == 0 ||
			h->tx_icmp != h->rx_icmp) {

			struct host *host = h;
			if (host == *hosts)
				*hosts = next;
			if (prev)
				prev->next = next;
			free(host);

		} else {
			good_hosts++;
			prev = h;
		}
		h = next;
	}

	free(evaldata.hosts);
	printf("%d of %d hosts responded correctly to all pings", good_hosts, length);
	if (good_hosts) {
		printf(" (average RTT %.02f ms)",
			latency_sum_us / ( latency_count * 1000.0f));
	}
	printf("\n");
	return good_hosts;
}

static struct host *hosts_start;
static struct host *hosts_cur;

void host_use(struct host* hosts)
{
	hosts_start = hosts;
}

/* Return next host.
 * Emulate a cyclic list of hosts */
struct host *host_get_next()
{
	struct host *h;
	assert(hosts_start);
	if (!hosts_cur)
		hosts_cur = hosts_start;
	h = hosts_cur;
	hosts_cur = hosts_cur->next;
	return h;
}


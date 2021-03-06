/******************************************************************************
 * Header files
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <linux/in6.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/udp.h>
#include <linux/tcp.h>
#include <assert.h>
#include <pcap.h>

#include "config.h"
#include "pcapReader.h"
#include "flowManager.h"

/******************************************************************************
 * Function prototypes
 */
static inline uint32_t flowTable_hash(const u_char *pkt);
static inline bool     flowTable_match(flow_t f, const u_char *pkt);

/******************************************************************************
 * flowTable_create
 */
inline flowTable_t
flowTable_create(void)
{
  flowTable_t ft;
  int i;

  /* allocate memory for the new flow table */
  ft = (flowTable_t)malloc(sizeof(struct flowTable));
  if (ft == NULL) {
    perror("malloc");
    return NULL;
  }

  /* initialize each slot of the flow table */
  for (i = 0; i < FLOW_TABLE_SIZE; i++)
    TAILQ_INIT(&ft->table[i]);

  return ft;
}

/******************************************************************************
 * flowTable_destroy
 */
inline void
flowTable_destroy(flowTable_t ft)
{
  int i;
  flowQueue *fq;
  flow_t f;

  /* free every flow entry */
  for (i = 0; i < FLOW_TABLE_SIZE; i++) {
    fq = &ft->table[i];
    while ((f = TAILQ_FIRST(fq))) {
      TAILQ_REMOVE(fq, f, node);
      free(f);
    }
  }
  
  free(ft);
}

/******************************************************************************
 * flowTable_lookup
 */
inline flow_t
flowTable_lookup(flowTable_t ft, const u_char *pkt)
{
  flowQueue *fq;
  flow_t f;
  uint32_t hash = flowTable_hash(pkt);

  /* check the flow entries in the slot */
  fq = &ft->table[hash];
  TAILQ_FOREACH(f, fq, node) {
    if (flowTable_match(f, pkt)) {
      return f;
    }
  }

  return NULL;
}

/******************************************************************************
 * flowTable_hash
 */
#define UPDATE_HASH(hash, key) {		\
    (hash) += (key);				\
    (hash) += ((hash) << 10);			\
    (hash) ^= ((hash) >> 6);			\
  }
static inline uint32_t
flowTable_hash(const u_char *pkt)
{
  struct ethhdr *ether_hdr = (struct ethhdr *)pkt;
  uint16_t ether_type = ntohs(ether_hdr->h_proto);
  uint32_t addr[2] = {0, 0};
  //
  struct tcphdr *tcp_hdr = get_tcp_hdr(pkt);
  uint16_t port[2] = {(uint16_t)tcp_hdr->source, (uint16_t)tcp_hdr->dest};
  //
  uint32_t hash[2] = {0, 0};
  int i, j;
  char *key;

  /* get the IP address values from the packet */
  if (ether_type == ETH_P_IP) {
    struct iphdr *ipv4_hdr = get_ipv4_hdr(pkt);
    addr[0] = (uint32_t)ipv4_hdr->saddr;
    addr[1] = (uint32_t)ipv4_hdr->daddr;
  }
  else if (ether_type == ETH_P_IPV6) {
    struct ipv6hdr *ipv6_hdr = get_ipv6_hdr(pkt);
    uint32_t *sp = (uint32_t *)&ipv6_hdr->saddr;
    uint32_t *dp = (uint32_t *)&ipv6_hdr->daddr;;
    for (i = 0; i < 4; i++) {
      addr[0] += *sp++;
      addr[1] += *dp++;
    }
  }
  else {
    assert(0); /* Do I need more graceful error handling? */
  }

  /* get the hash value using the IP addresses and port numbers */
  for (i = 0; i < 2; i++) {
    key = (char *)&addr[i];
    for (j = 0; j < 4; j++)
      UPDATE_HASH(hash[i], key[j]);
    //
    key = (char *)&port[i];
    for (j = 0; j < 2; j++)
      UPDATE_HASH(hash[i], key[j]);
    //
  }

  /* finalize the hash value */
  hash[0] += hash[1];
  hash[0] += (hash[0] << 3);
  hash[0] ^= (hash[0] >> 11);
  hash[0] += (hash[0] << 15);

  return hash[0] % FLOW_TABLE_SIZE;
}

/******************************************************************************
 * flowTable_match
 */
static inline bool
flowTable_match(flow_t f, const u_char *pkt)
{
  struct ethhdr *ether_hdr = (struct ethhdr *)pkt;
  uint16_t ether_type = ntohs(ether_hdr->h_proto);

  /* check L3 protocol */
  if (ether_type != f->ether_type)
    return false;
  //
  struct tcphdr *tcp_hdr = get_tcp_hdr(pkt);
  //
  /* check the IP addresses and port numbers */
  if (ether_type == ETH_P_IP) {
    struct iphdr *ipv4_hdr = get_ipv4_hdr(pkt);
    return ((f->saddr.s6_addr32[0] == ipv4_hdr->saddr &&
	     f->daddr.s6_addr32[0] == ipv4_hdr->daddr &&
	     f->sport              == tcp_hdr->source &&
	     f->dport              == tcp_hdr->dest) ||
	    (f->saddr.s6_addr32[0] == ipv4_hdr->daddr &&
	     f->daddr.s6_addr32[0] == ipv4_hdr->saddr &&
	     f->sport              == tcp_hdr->dest &&
	     f->dport              == tcp_hdr->source));
  }
  else if (ether_type == ETH_P_IPV6) {
    struct ipv6hdr *ipv6_hdr = get_ipv6_hdr(pkt);
    return ((f->saddr.s6_addr32[0] == ipv6_hdr->saddr.s6_addr32[0] &&
	     f->saddr.s6_addr32[1] == ipv6_hdr->saddr.s6_addr32[1] &&
	     f->saddr.s6_addr32[2] == ipv6_hdr->saddr.s6_addr32[2] &&
	     f->saddr.s6_addr32[3] == ipv6_hdr->saddr.s6_addr32[3] &&
	     f->daddr.s6_addr32[0] == ipv6_hdr->daddr.s6_addr32[0] &&
	     f->daddr.s6_addr32[1] == ipv6_hdr->daddr.s6_addr32[1] &&
	     f->daddr.s6_addr32[2] == ipv6_hdr->daddr.s6_addr32[2] &&
	     f->daddr.s6_addr32[3] == ipv6_hdr->daddr.s6_addr32[3] &&
	     f->sport == tcp_hdr->source && f->dport == tcp_hdr->dest) ||
	    (f->saddr.s6_addr32[0] == ipv6_hdr->daddr.s6_addr32[0] &&
	     f->saddr.s6_addr32[1] == ipv6_hdr->daddr.s6_addr32[1] &&
	     f->saddr.s6_addr32[2] == ipv6_hdr->daddr.s6_addr32[2] &&
	     f->saddr.s6_addr32[3] == ipv6_hdr->daddr.s6_addr32[3] &&
	     f->daddr.s6_addr32[0] == ipv6_hdr->saddr.s6_addr32[0] &&
	     f->daddr.s6_addr32[1] == ipv6_hdr->saddr.s6_addr32[1] &&
	     f->daddr.s6_addr32[2] == ipv6_hdr->saddr.s6_addr32[2] &&
	     f->daddr.s6_addr32[3] == ipv6_hdr->saddr.s6_addr32[3] &&
	     f->sport == tcp_hdr->dest && f->dport == tcp_hdr->source));
  }
  else {
    assert(0); /* Do I need more graceful error handling? */
  }

  return false;
}

/******************************************************************************
 * flowTable_create_flow
 */
inline flow_t 
flowTable_create_flow(flowTable_t ft,
		      struct pcap_pkthdr *hdr, const u_char *pkt)
{
  flow_t f;
  uint32_t hash = flowTable_hash(pkt);
  flowQueue *fq = &ft->table[hash];
  struct ethhdr *ether_hdr = (struct ethhdr *)pkt;
  //
  struct tcphdr *tcp_hdr = get_tcp_hdr(pkt);
  //

  /* allocate memory for the new flow */
  f = (flow_t)malloc(sizeof(struct flow));
  if (f == NULL) {
    perror("malloc");
    return NULL;
  }
  
  /* set L3 protocol */
  f->ether_type = ntohs(ether_hdr->h_proto);

  /* set IP addresses */
  if (f->ether_type == ETH_P_IP) {
    struct iphdr *ipv4_hdr = get_ipv4_hdr(pkt);
    f->saddr.s6_addr32[0] = ipv4_hdr->saddr;
    f->daddr.s6_addr32[0] = ipv4_hdr->daddr;
  }
  else if (f->ether_type == ETH_P_IPV6) {
    struct ipv6hdr *ipv6_hdr = get_ipv6_hdr(pkt);
    f->saddr = ipv6_hdr->saddr;
    f->daddr = ipv6_hdr->daddr;
  }
  else {
    assert(0);  /* Do I need more graceful error handling? */
  }

  /* set port numbers */
  //
  f->sport = tcp_hdr->source;
  f->dport = tcp_hdr->dest;
  //

  /* set time stamps */
  f->ts[FIRST] = hdr->ts;
  f->ts[LAST]  = hdr->ts;

  assert(tcp_hdr->syn);

  /* set TCP state */
  f->state = tcp_hdr->ack ? SYNACK : SYN;

  /* set counters */
  f->num_byte = hdr->len;
  f->num_pkt  = 1;

  /* insert to the flow table */
  TAILQ_INSERT_HEAD(fq, f, node);
  
  return f;
}

/******************************************************************************
 * flowTable_update_flow
 */
inline void
flowTable_update_flow(flowTable_t ft, flow_t f,
		      struct pcap_pkthdr *hdr, const u_char *pkt)
{
  uint32_t hash = flowTable_hash(pkt);
  flowQueue *fq = &ft->table[hash];
  struct tcphdr *tcp_hdr = get_tcp_hdr(pkt);

  /* move the flow to the head for efficiency */
  TAILQ_REMOVE(fq, f, node);
  TAILQ_INSERT_HEAD(fq, f, node);

  /* update TCP state */
  switch (f->state) {
  case CLOSE:
    assert(0);    
    break;
  case SYN:
    if (tcp_hdr->syn && tcp_hdr->ack)
      f->state = SYNACK;
    else if (tcp_hdr->fin)
      f->state = FIN;
    else if (tcp_hdr->rst)
      f->state = RST;
    else
      f->state = ESTABLISHED;
    break;
  case SYNACK:
    if (tcp_hdr->fin)
      f->state = FIN;
    else if (tcp_hdr->rst)
      f->state = RST;
    else
      f->state = ESTABLISHED;
    break;
  case ESTABLISHED:
    if (tcp_hdr->fin)
      f->state = FIN;
    else if (tcp_hdr->rst)
      f->state = RST;
    break;
  case FIN:
    if (tcp_hdr->rst)
      f->state = RST;
    break;
  case RST:
    break;
  default:
    assert(0);
  };

  f->ts[LAST] = hdr->ts;
  f->num_byte += hdr->len;
  f->num_pkt++;
}

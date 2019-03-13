/**
 * @file
 * This is the IPv4 layer implementation for incoming and outgoing IP traffic.
 *
 * @see ip_frag.c
 *
 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *
 */

#include "lwip/opt.h"

#if LWIP_IPV4

#include "lwip/ip.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/ip4_frag.h"
#include "lwip/inet_chksum.h"
#include "lwip/netif.h"
#include "lwip/icmp.h"
#include "lwip/igmp.h"
#include "lwip/raw.h"
#include "lwip/sys.h" //for sys_now()
#include "lwip/udp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/autoip.h"
#include "lwip/stats.h"
#include "lwip/lwip_napt.h"
#include "lwip/prot/dhcp.h"

#include <string.h>

#ifdef LWIP_HOOK_FILENAME
#include LWIP_HOOK_FILENAME
#endif

/** Set this to 0 in the rare case of wanting to call an extra function to
 * generate the IP checksum (in contrast to calculating it on-the-fly). */
#ifndef LWIP_INLINE_IP_CHKSUM
#if LWIP_CHECKSUM_CTRL_PER_NETIF
#define LWIP_INLINE_IP_CHKSUM   0
#else /* LWIP_CHECKSUM_CTRL_PER_NETIF */
#define LWIP_INLINE_IP_CHKSUM   1
#endif /* LWIP_CHECKSUM_CTRL_PER_NETIF */
#endif

#if LWIP_INLINE_IP_CHKSUM && CHECKSUM_GEN_IP
#define CHECKSUM_GEN_IP_INLINE  1
#else
#define CHECKSUM_GEN_IP_INLINE  0
#endif

#if LWIP_DHCP || defined(LWIP_IP_ACCEPT_UDP_PORT)
#define IP_ACCEPT_LINK_LAYER_ADDRESSING 1

/** Some defines for DHCP to let link-layer-addressed packets through while the
 * netif is down.
 * To use this in your own application/protocol, define LWIP_IP_ACCEPT_UDP_PORT(port)
 * to return 1 if the port is accepted and 0 if the port is not accepted.
 */
#if LWIP_DHCP && defined(LWIP_IP_ACCEPT_UDP_PORT)
/* accept DHCP client port and custom port */
#define IP_ACCEPT_LINK_LAYER_ADDRESSED_PORT(port) (((port) == PP_NTOHS(DHCP_CLIENT_PORT)) \
         || (LWIP_IP_ACCEPT_UDP_PORT(port)))
#elif defined(LWIP_IP_ACCEPT_UDP_PORT) /* LWIP_DHCP && defined(LWIP_IP_ACCEPT_UDP_PORT) */
/* accept custom port only */
#define IP_ACCEPT_LINK_LAYER_ADDRESSED_PORT(port) (LWIP_IP_ACCEPT_UDP_PORT(port))
#else /* LWIP_DHCP && defined(LWIP_IP_ACCEPT_UDP_PORT) */
/* accept DHCP client port only */
#define IP_ACCEPT_LINK_LAYER_ADDRESSED_PORT(port) ((port) == PP_NTOHS(DHCP_CLIENT_PORT))
#endif /* LWIP_DHCP && defined(LWIP_IP_ACCEPT_UDP_PORT) */

#else /* LWIP_DHCP */
#define IP_ACCEPT_LINK_LAYER_ADDRESSING 0
#endif /* LWIP_DHCP */

/** The IP header ID of the next outgoing IP packet */
static u16_t ip_id;

#if LWIP_MULTICAST_TX_OPTIONS
/** The default netif used for multicast */
static struct netif* ip4_default_multicast_netif;

/**
 * @ingroup ip4
 * Set a default netif for IPv4 multicast. */
void
ip4_set_default_multicast_netif(struct netif* default_multicast_netif)
{
  ip4_default_multicast_netif = default_multicast_netif;
}
#endif /* LWIP_MULTICAST_TX_OPTIONS */

#ifdef LWIP_HOOK_IP4_ROUTE_SRC
/**
 * Source based IPv4 routing must be fully implemented in
 * LWIP_HOOK_IP4_ROUTE_SRC(). This function only provides he parameters.
 */
struct netif *
ip4_route_src(const ip4_addr_t *dest, const ip4_addr_t *src)
{
  if (src != NULL) {
    /* when src==NULL, the hook is called from ip4_route(dest) */
    struct netif *netif = LWIP_HOOK_IP4_ROUTE_SRC(dest, src);
    if (netif != NULL) {
      return netif;
    }
  }
  return ip4_route(dest);
}
#endif /* LWIP_HOOK_IP4_ROUTE_SRC */

/**
 * Finds the appropriate network interface for a given IP address. It
 * searches the list of network interfaces linearly. A match is found
 * if the masked IP address of the network interface equals the masked
 * IP address given to the function.
 *
 * @param dest the destination IP address for which to find the route
 * @return the netif on which to send to reach dest
 */
struct netif *
ip4_route(const ip4_addr_t *dest)
{
  struct netif *netif;

#if LWIP_MULTICAST_TX_OPTIONS
  /* Use administratively selected interface for multicast by default */
  if (ip4_addr_ismulticast(dest) && ip4_default_multicast_netif) {
    return ip4_default_multicast_netif;
  }
#endif /* LWIP_MULTICAST_TX_OPTIONS */

  /* iterate through netifs */
  for (netif = netif_list; netif != NULL; netif = netif->next) {
    /* is the netif up, does it have a link and a valid address? */
    if (netif_is_up(netif) && netif_is_link_up(netif) && !ip4_addr_isany_val(*netif_ip4_addr(netif))) {
      /* network mask matches? */
      if (ip4_addr_netcmp(dest, netif_ip4_addr(netif), netif_ip4_netmask(netif))) {
        /* return netif on which to forward IP packet */
        return netif;
      }
      /* gateway matches on a non broadcast interface? (i.e. peer in a point to point interface) */
      if (((netif->flags & NETIF_FLAG_BROADCAST) == 0) && ip4_addr_cmp(dest, netif_ip4_gw(netif))) {
        /* return netif on which to forward IP packet */
        return netif;
      }
    }
  }

#if LWIP_NETIF_LOOPBACK && !LWIP_HAVE_LOOPIF
  /* loopif is disabled, looopback traffic is passed through any netif */
  if (ip4_addr_isloopback(dest)) {
    /* don't check for link on loopback traffic */
    if (netif_default != NULL && netif_is_up(netif_default)) {
      return netif_default;
    }
    /* default netif is not up, just use any netif for loopback traffic */
    for (netif = netif_list; netif != NULL; netif = netif->next) {
      if (netif_is_up(netif)) {
        return netif;
      }
    }
    return NULL;
  }
#endif /* LWIP_NETIF_LOOPBACK && !LWIP_HAVE_LOOPIF */

#ifdef LWIP_HOOK_IP4_ROUTE_SRC
  netif = LWIP_HOOK_IP4_ROUTE_SRC(dest, NULL);
  if (netif != NULL) {
    return netif;
  }
#elif defined(LWIP_HOOK_IP4_ROUTE)
  netif = LWIP_HOOK_IP4_ROUTE(dest);
  if (netif != NULL) {
    return netif;
  }
#endif

  if ((netif_default == NULL) || !netif_is_up(netif_default) || !netif_is_link_up(netif_default) ||
      ip4_addr_isany_val(*netif_ip4_addr(netif_default))) {
    /* No matching netif found and default netif is not usable.
       If this is not good enough for you, use LWIP_HOOK_IP4_ROUTE() */
    LWIP_DEBUGF(IP_DEBUG | LWIP_DBG_LEVEL_SERIOUS, ("ip4_route: No route to %"U16_F".%"U16_F".%"U16_F".%"U16_F"\n",
      ip4_addr1_16(dest), ip4_addr2_16(dest), ip4_addr3_16(dest), ip4_addr4_16(dest)));
    IP_STATS_INC(ip.rterr);
    MIB2_STATS_INC(mib2.ipoutnoroutes);
    return NULL;
  }

  return netif_default;
}

#if IP_FORWARD
#if IP_NAPT

#define NO_IDX ((u16_t)-1)
#define NT(x) ((x) == NO_IDX ? NULL : &ip_napt_table[x])

u16_t napt_list = NO_IDX, napt_list_last = NO_IDX, napt_free = 0;

static struct napt_table *ip_napt_table;
struct portmap_table *ip_portmap_table;

int nr_active_napt_tcp = 0, nr_active_napt_udp = 0, nr_active_napt_icmp = 0;
uint16_t ip_napt_max = 0;
uint8_t ip_portmap_max = 0;

void ICACHE_FLASH_ATTR
ip_napt_init(uint16_t max_nat, uint8_t max_portmap)
{
  u16_t i;

  ip_napt_max = max_nat;
  ip_portmap_max = max_portmap;

  ip_napt_table = (struct napt_table*)os_zalloc(sizeof(struct napt_table[ip_napt_max]));
  ip_portmap_table = (struct portmap_table*)os_zalloc(sizeof(struct portmap_table[ip_portmap_max]));

  for (i = 0; i < ip_napt_max - 1; i++)
    ip_napt_table[i].next = i + 1;
  ip_napt_table[i].next = NO_IDX;
}

void ICACHE_FLASH_ATTR
ip_napt_enable(u32_t addr, int enable)
{
  struct netif *netif;
  for (netif = netif_list; netif; netif = netif->next) {
    if (netif_is_up(netif) && !ip4_addr_isany_val(*netif_ip4_addr(netif)) && netif->ip_addr.addr == addr) {
      netif->napt = !!enable;
      break;
    }
  }
}

void ICACHE_FLASH_ATTR
ip_napt_enable_no(u8_t number, int enable)
{
  struct netif *netif;
  for (netif = netif_list; netif; netif = netif->next) {
    if (netif->num == number) {
      netif->napt = !!enable;
      break;
    }
  }
}

void ICACHE_FLASH_ATTR checksumadjust(unsigned char *chksum, unsigned char *optr,
   int olen, unsigned char *nptr, int nlen)
   /* assuming: unsigned char is 8 bits, long is 32 bits.
     - chksum points to the chksum in the packet
     - optr points to the old data in the packet
     - nptr points to the new data in the packet
   */
   {
     long x, old, new;
     x=chksum[0]*256+chksum[1];
     x=~x & 0xFFFF;
     while (olen)
     {
         old=optr[0]*256+optr[1]; optr+=2;
         x-=old & 0xffff;
         if (x<=0) { x--; x&=0xffff; }
         olen-=2;
     }
     while (nlen)
     {
         new=nptr[0]*256+nptr[1]; nptr+=2;
         x+=new & 0xffff;
         if (x & 0x10000) { x++; x&=0xffff; }
         nlen-=2;
     }
     x=~x & 0xFFFF;
     chksum[0]=x/256; chksum[1]=x & 0xff;
   }


/* t must be indexed by napt_free */
static void ICACHE_FLASH_ATTR
ip_napt_insert(struct napt_table *t)
{
  u16_t ti = t - ip_napt_table;
  if (ti != napt_free) *((int*)1)=1; //DEBUG
  napt_free = t->next;
  t->prev = NO_IDX;
  t->next = napt_list;
  if (napt_list != NO_IDX)
    NT(napt_list)->prev = ti;
  napt_list = ti;
  if (napt_list_last == NO_IDX)
    napt_list_last = ti;

#if LWIP_TCP
  if (t->proto == IP_PROTO_TCP)
    nr_active_napt_tcp++;
#endif
#if LWIP_UDP
  if (t->proto == IP_PROTO_UDP)
    nr_active_napt_udp++;
#endif
#if LWIP_ICMP
  if (t->proto == IP_PROTO_ICMP)
    nr_active_napt_icmp++;
#endif
}

static void ICACHE_FLASH_ATTR
ip_napt_free(struct napt_table *t)
{
  u16_t ti = t - ip_napt_table;
  if (ti == napt_list)
    napt_list = t->next;
  if (ti == napt_list_last)
    napt_list_last = t->prev;
  if (t->next != NO_IDX)
    NT(t->next)->prev = t->prev;
  if (t->prev != NO_IDX)
    NT(t->prev)->next = t->next;
  t->prev = NO_IDX;
  t->next = napt_free;
  napt_free = ti;

#if LWIP_TCP
  if (t->proto == IP_PROTO_TCP)
    nr_active_napt_tcp--;
#endif
#if LWIP_UDP
  if (t->proto == IP_PROTO_UDP)
    nr_active_napt_udp--;
#endif
#if LWIP_ICMP
  if (t->proto == IP_PROTO_ICMP)
    nr_active_napt_icmp--;
#endif
  LWIP_DEBUGF(NAPT_DEBUG, ("ip_napt_free\n"));
  napt_debug_print();
}

#if LWIP_TCP
static u8_t ICACHE_FLASH_ATTR
ip_napt_find_port(u8_t proto, u16_t port)
{
  int i, next;
  for (i = napt_list; i != NO_IDX; i = next) {
    struct napt_table *t = &ip_napt_table[i];
    next = t->next;
    if (t->proto == proto && t->mport == port)
      return 1;
  }
  return 0;
}

static struct portmap_table * ICACHE_FLASH_ATTR
ip_portmap_find(u8_t proto, u16_t mport);

static u8_t ICACHE_FLASH_ATTR
tcp_listening(u16_t port)
{
  struct tcp_pcb_listen *t;
  for (t = tcp_listen_pcbs.listen_pcbs; t; t = t->next)
    if (t->local_port == port)
      return 1;
  if (ip_portmap_find(IP_PROTO_TCP, port))
    return 1;
  return 0;
}
#endif // LWIP_TCP

#if LWIP_UDP
static u8_t ICACHE_FLASH_ATTR
udp_listening(u16_t port)
{
  struct udp_pcb *pcb;
  for (pcb = udp_pcbs; pcb; pcb = pcb->next)
    if (pcb->local_port == port)
      return 1;
  if (ip_portmap_find(IP_PROTO_UDP, port))
    return 1;
  return 0;
}
#endif // LWIP_UDP

static u16_t ICACHE_FLASH_ATTR
ip_napt_new_port(u8_t proto, u16_t port)
{
  if (PP_NTOHS(port) >= IP_NAPT_PORT_RANGE_START && PP_NTOHS(port) <= IP_NAPT_PORT_RANGE_END)
    if (!ip_napt_find_port(proto, port) && !tcp_listening(port))
      return port;
  for (;;) {
    port = PP_HTONS(IP_NAPT_PORT_RANGE_START +
                    os_random() % (IP_NAPT_PORT_RANGE_END - IP_NAPT_PORT_RANGE_START + 1));
    if (ip_napt_find_port(proto, port))
      continue;
#if LWIP_TCP
    if (proto == IP_PROTO_TCP && tcp_listening(port))
      continue;
#endif // LWIP_TCP
#if LWIP_UDP
    if (proto == IP_PROTO_UDP && udp_listening(port))
      continue;
#endif // LWIP_UDP

    return port;
  }
}

static struct napt_table* ICACHE_FLASH_ATTR
ip_napt_find(u8_t proto, u32_t addr, u16_t port, u16_t mport, u8_t dest)
{
  u16_t i, next;
  struct napt_table *t;

  LWIP_DEBUGF(NAPT_DEBUG, ("ip_napt_find\n"));
  LWIP_DEBUGF(NAPT_DEBUG, ("looking up in table %s: %"U16_F".%"U16_F".%"U16_F".%"U16_F", port: %u, mport: %u\n",
					(dest ? "dest" : "src"),
                    ip4_addr1_16(&addr), ip4_addr2_16(&addr),
                    ip4_addr3_16(&addr), ip4_addr4_16(&addr),
                    PP_HTONS(port),
                    PP_HTONS(mport)));
  napt_debug_print();

  u32_t now = sys_now();
  for (i = napt_list; i != NO_IDX; i = next) {
    t = NT(i);
    next = t->next;
#if LWIP_TCP
    if (t->proto == IP_PROTO_TCP &&
        (((t->finack1 && (t->finack2 || !t->synack)) &&
          now - t->last > IP_NAPT_TIMEOUT_MS_TCP_DISCON) ||
         now - t->last > IP_NAPT_TIMEOUT_MS_TCP)) {
      ip_napt_free(t);
      continue;
    }
#endif
#if LWIP_UDP
    if (t->proto == IP_PROTO_UDP && now - t->last > IP_NAPT_TIMEOUT_MS_UDP) {
      ip_napt_free(t);
      continue;
    }
#endif
#if LWIP_ICMP
    if (t->proto == IP_PROTO_ICMP && now - t->last > IP_NAPT_TIMEOUT_MS_ICMP) {
      ip_napt_free(t);
      continue;
    }
#endif
    if (dest == 0 && t->proto == proto && t->src == addr && t->sport == port) {
      t->last = now;
      LWIP_DEBUGF(NAPT_DEBUG, ("found\n"));
      return t;
    }
    if (dest == 1 && t->proto == proto && t->dest == addr && t->dport == port 
        && t->mport == mport) {
      t->last = now;
      LWIP_DEBUGF(NAPT_DEBUG, ("found\n"));
      return t;
    }
  }

  LWIP_DEBUGF(NAPT_DEBUG, ("not found\n"));
  return NULL;
}

static u16_t ICACHE_FLASH_ATTR
ip_napt_add(u8_t proto, u32_t src, u16_t sport, u32_t dest, u16_t dport)
{
  struct napt_table *t = ip_napt_find(proto, src, sport, 0, 0);
  if (t) {
    t->last = sys_now();
    t->dest = dest;
    t->dport = dport;
    /* move this entry to the top of napt_list */
    ip_napt_free(t);
    ip_napt_insert(t);

    LWIP_DEBUGF(NAPT_DEBUG, ("ip_napt_add\n"));
    napt_debug_print();

    return t->mport;
  }
  t = NT(napt_free);
  if (t) {
    u16_t mport = sport;
#if LWIP_TCP
    if (proto == IP_PROTO_TCP)
      mport = ip_napt_new_port(IP_PROTO_TCP, sport);
#endif
#if LWIP_TCP
    if (proto == IP_PROTO_UDP)
      mport = ip_napt_new_port(IP_PROTO_UDP, sport);
#endif
    t->last = sys_now();
    t->src = src;
    t->dest = dest;
    t->sport = sport;
    t->dport = dport;
    t->mport = mport;
    t->proto = proto;
    t->fin1 = t->fin2 = t->finack1 = t->finack2 = t->synack = t->rst = 0;
    ip_napt_insert(t);

    LWIP_DEBUGF(NAPT_DEBUG, ("ip_napt_add\n"));
    napt_debug_print();

    return mport;
  }
  os_printf("NAT table full\n");
  return 0;
}

u8_t ICACHE_FLASH_ATTR
ip_portmap_add(u8_t proto, u32_t maddr, u16_t mport, u32_t daddr, u16_t dport)
{
  mport = PP_HTONS(mport);
  dport = PP_HTONS(dport);
  int i;

  for (i = 0; i < ip_portmap_max; i++) {
    struct portmap_table *p = &ip_portmap_table[i];
    if (p->valid && p->proto == proto && p->mport == mport) {
      p->dport = dport;
      p->daddr = daddr;
    } else if (!p->valid) {
      p->maddr = maddr;
      p->daddr = daddr;
      p->mport = mport;
      p->dport = dport;
      p->proto = proto;
      p->valid = 1;
      return 1;
    }
  }
  return 0;
}

static struct portmap_table * ICACHE_FLASH_ATTR
ip_portmap_find(u8_t proto, u16_t mport)
{
  int i;
  for (i = 0; i < ip_portmap_max; i++) {
    struct portmap_table *p = &ip_portmap_table[i];
    if (!p->valid)
      return 0;
    if (p->proto == proto && p->mport == mport)
      return p;
  }
  return NULL;
}

static struct portmap_table * ICACHE_FLASH_ATTR
ip_portmap_find_dest(u8_t proto, u16_t dport, u32_t daddr)
{
  int i;
  for (i = 0; i < ip_portmap_max; i++) {
    struct portmap_table *p = &ip_portmap_table[i];
    if (!p->valid)
      return 0;
    if (p->proto == proto && p->dport == dport && p->daddr == daddr)
      return p;
  }
  return NULL;
}


u8_t ICACHE_FLASH_ATTR
ip_portmap_remove(u8_t proto, u16_t mport)
{
  mport = PP_HTONS(mport);
  struct portmap_table *last = &ip_portmap_table[ip_portmap_max - 1];
  struct portmap_table *m = ip_portmap_find(proto, mport);
  if (!m)
    return 0;
  for (; m != last; m++)
    memcpy(m, m + 1, sizeof(*m));
  last->valid = 0;
  return 1;
}

#if LWIP_TCP
void ICACHE_FLASH_ATTR
ip_napt_modify_port_tcp(struct tcp_hdr *tcphdr, u8_t dest, u16_t newval)
{
  if (dest) {
    checksumadjust((unsigned char *)&tcphdr->chksum, (unsigned char *)&tcphdr->dest, 2, (unsigned char *)&newval, 2);
    tcphdr->dest = newval;
  } else {
    checksumadjust((unsigned char *)&tcphdr->chksum, (unsigned char *)&tcphdr->src, 2, (unsigned char *)&newval, 2);
    tcphdr->src = newval;
  }
}


void ICACHE_FLASH_ATTR
ip_napt_modify_addr_tcp(struct tcp_hdr *tcphdr, ip4_addr_p_t *oldval, u32_t newval)
{
  checksumadjust((unsigned char *)&tcphdr->chksum, (unsigned char *)&oldval->addr, 4, (unsigned char *)&newval, 4);
}
#endif // LWIP_TCP

#if LWIP_UDP
void ICACHE_FLASH_ATTR
ip_napt_modify_port_udp(struct udp_hdr *udphdr, u8_t dest, u16_t newval)
{
  if (dest) {
    checksumadjust((unsigned char *)&udphdr->chksum, (unsigned char *)&udphdr->dest, 2, (unsigned char *)&newval, 2);
    udphdr->dest = newval;
  } else {
    checksumadjust((unsigned char *)&udphdr->chksum, (unsigned char *)&udphdr->src, 2, (unsigned char *)&newval, 2);
    udphdr->src = newval;
  }
}

void ICACHE_FLASH_ATTR
ip_napt_modify_addr_udp(struct udp_hdr *udphdr, ip4_addr_p_t *oldval, u32_t newval)
{
  checksumadjust((unsigned char *)&udphdr->chksum, (unsigned char *)&oldval->addr, 4, (unsigned char *)&newval, 4);
}
#endif // LWIP_UDP

void ICACHE_FLASH_ATTR
ip_napt_modify_addr(struct ip_hdr *iphdr, ip4_addr_p_t *field, u32_t newval)
{
  checksumadjust((unsigned char *)&IPH_CHKSUM(iphdr), (unsigned char *)&field->addr, 4, (unsigned char *)&newval, 4);
  field->addr = newval;
}

/**
 * NAPT for an input packet. It checks weather the destination is on NAPT
 * table and modifythe packet destination address and port if needed.
 *
 * @param p the packet to forward (p->payload points to IP header)
 * @param iphdr the IP header of the input packet
 * @param inp the netif on which this packet was received
 */
static void ICACHE_FLASH_ATTR
ip_napt_recv(struct pbuf *p, struct ip_hdr *iphdr, struct netif *inp)
{
  struct portmap_table *m;
  struct napt_table *t;

#if LWIP_ICMP
  /* NAPT for ICMP Echo Request using identifier */
  if (IPH_PROTO(iphdr) == IP_PROTO_ICMP) {
    struct icmp_echo_hdr *iecho = (struct icmp_echo_hdr *)((u8_t *)p->payload + IPH_HL(iphdr) * 4);
    if (iecho->type == ICMP_ER) {
      t = ip_napt_find(IP_PROTO_ICMP, iphdr->src.addr, iecho->id, iecho->id, 1);
      if (!t)
        return;
      ip_napt_modify_addr(iphdr, &iphdr->dest, t->src);
      return;
    }

    return;
  }
#endif // LWIP_ICMP

#if LWIP_TCP
  if (IPH_PROTO(iphdr) == IP_PROTO_TCP) {
    struct tcp_hdr *tcphdr = (struct tcp_hdr *)((u8_t *)p->payload + IPH_HL(iphdr) * 4);

    LWIP_DEBUGF(NAPT_DEBUG, ("ip_napt_recv\n"));
    LWIP_DEBUGF(NAPT_DEBUG, ("src: %"U16_F".%"U16_F".%"U16_F".%"U16_F", dest: %"U16_F".%"U16_F".%"U16_F".%"U16_F", ",
      ip4_addr1_16(&iphdr->src), ip4_addr2_16(&iphdr->src),
      ip4_addr3_16(&iphdr->src), ip4_addr4_16(&iphdr->src),
      ip4_addr1_16(&iphdr->dest), ip4_addr2_16(&iphdr->dest),
      ip4_addr3_16(&iphdr->dest), ip4_addr4_16(&iphdr->dest)));
	  
      LWIP_DEBUGF(NAPT_DEBUG, ("sport %u, dport: %u\n",
                        PP_HTONS(tcphdr->src),
                        PP_HTONS(tcphdr->dest)));

    m = ip_portmap_find(IP_PROTO_TCP, tcphdr->dest);
    if (m) {
      /* packet to mapped port: rewrite destination */
      if (m->dport != tcphdr->dest)
        ip_napt_modify_port_tcp(tcphdr, 1, m->dport);
      ip_napt_modify_addr_tcp(tcphdr, &iphdr->dest, m->daddr);
      ip_napt_modify_addr(iphdr, &iphdr->dest, m->daddr);
      return;
    }
    t = ip_napt_find(IP_PROTO_TCP, iphdr->src.addr, tcphdr->src, tcphdr->dest, 1);
      if (!t)
        return; /* Unknown TCP session; do nothing */

      if (t->sport != tcphdr->dest)
        ip_napt_modify_port_tcp(tcphdr, 1, t->sport);
      ip_napt_modify_addr_tcp(tcphdr, &iphdr->dest, t->src);
      ip_napt_modify_addr(iphdr, &iphdr->dest, t->src);

      if ((TCPH_FLAGS(tcphdr) & (TCP_SYN|TCP_ACK)) == (TCP_SYN|TCP_ACK))
        t->synack = 1;
      if ((TCPH_FLAGS(tcphdr) & TCP_FIN))
        t->fin1 = 1;
      if (t->fin2 && (TCPH_FLAGS(tcphdr) & TCP_ACK))
        t->finack2 = 1; /* FIXME: Currently ignoring ACK seq... */
      if (TCPH_FLAGS(tcphdr) & TCP_RST)
        t->rst = 1;
      return;
  }
#endif // LWIP_TCP

#if LWIP_UDP
  if (IPH_PROTO(iphdr) == IP_PROTO_UDP) {
    struct udp_hdr *udphdr = (struct udp_hdr *)((u8_t *)p->payload + IPH_HL(iphdr) * 4);
    m = ip_portmap_find(IP_PROTO_UDP, udphdr->dest);
    if (m) {
      /* packet to mapped port: rewrite destination */
      if (m->dport != udphdr->dest)
        ip_napt_modify_port_udp(udphdr, 1, m->dport);
      ip_napt_modify_addr_udp(udphdr, &iphdr->dest, m->daddr);
      ip_napt_modify_addr(iphdr, &iphdr->dest, m->daddr);
      return;
    }
    t = ip_napt_find(IP_PROTO_UDP, iphdr->src.addr, udphdr->src, udphdr->dest, 1);
      if (!t)
        return; /* Unknown session; do nothing */

      if (t->sport != udphdr->dest)
        ip_napt_modify_port_udp(udphdr, 1, t->sport);
      ip_napt_modify_addr_udp(udphdr, &iphdr->dest, t->src);
      ip_napt_modify_addr(iphdr, &iphdr->dest, t->src);
      return;
  }
#endif // LWIP_UDP
}

/**
 * NAPT for a forwarded packet. It checks weather we need NAPT and modify
 * the packet source address and port if needed.
 *
 * @param p the packet to forward (p->payload points to IP header)
 * @param iphdr the IP header of the input packet
 * @param inp the netif on which this packet was received
 * @param outp the netif on which this packet will be sent
 * @return ERR_OK if packet should be sent, or ERR_RTE if it should be dropped
 */
static err_t ICACHE_FLASH_ATTR
ip_napt_forward(struct pbuf *p, struct ip_hdr *iphdr, struct netif *inp, struct netif *outp)
{
  if (!inp->napt)
    return ERR_OK;

#if LWIP_ICMP
  /* NAPT for ICMP Echo Request using identifier */
  if (IPH_PROTO(iphdr) == IP_PROTO_ICMP) {
    struct icmp_echo_hdr *iecho = (struct icmp_echo_hdr *)((u8_t *)p->payload + IPH_HL(iphdr) * 4);
    if (iecho->type == ICMP_ECHO) {
      /* register src addr and iecho->id and dest info */
      ip_napt_add(IP_PROTO_ICMP, iphdr->src.addr, iecho->id, iphdr->dest.addr, iecho->id);

      ip_napt_modify_addr(iphdr, &iphdr->src, outp->ip_addr.addr);
    }
    return ERR_OK;
  }
#endif

#if LWIP_TCP
  if (IPH_PROTO(iphdr) == IP_PROTO_TCP) {
    struct tcp_hdr *tcphdr = (struct tcp_hdr *)((u8_t *)p->payload + IPH_HL(iphdr) * 4);
    u16_t mport;

    struct portmap_table *m = ip_portmap_find_dest(IP_PROTO_TCP, tcphdr->src, iphdr->src.addr);
    if (m) {
      /* packet from port-mapped dest addr/port: rewrite source to this node */
      if (m->mport != tcphdr->src)
        ip_napt_modify_port_tcp(tcphdr, 0, m->mport);
      ip_napt_modify_addr_tcp(tcphdr, &iphdr->src, m->maddr);
      ip_napt_modify_addr(iphdr, &iphdr->src, m->maddr);
      return ERR_OK;
    }
    if ((TCPH_FLAGS(tcphdr) & (TCP_SYN|TCP_ACK)) == TCP_SYN &&
        PP_NTOHS(tcphdr->src) >= 1024) {
      /* Register new TCP session to NAPT */
      mport = ip_napt_add(IP_PROTO_TCP, iphdr->src.addr, tcphdr->src,
                          iphdr->dest.addr, tcphdr->dest);
    } else {
      struct napt_table *t = ip_napt_find(IP_PROTO_TCP, iphdr->src.addr, tcphdr->src, 0, 0);
      if (!t || t->dest != iphdr->dest.addr || t->dport != tcphdr->dest) {
#if LWIP_ICMP
        icmp_dest_unreach(p, ICMP_DUR_PORT);
#endif
        return ERR_RTE; /* Drop unknown TCP session */
      }
      mport = t->mport;
      if ((TCPH_FLAGS(tcphdr) & TCP_FIN))
        t->fin2 = 1;
      if (t->fin1 && (TCPH_FLAGS(tcphdr) & TCP_ACK))
        t->finack1 = 1; /* FIXME: Currently ignoring ACK seq... */
      if (TCPH_FLAGS(tcphdr) & TCP_RST)
        t->rst = 1;
    }

    if (mport != tcphdr->src)
      ip_napt_modify_port_tcp(tcphdr, 0, mport);
    ip_napt_modify_addr_tcp(tcphdr, &iphdr->src, outp->ip_addr.addr);
    ip_napt_modify_addr(iphdr, &iphdr->src, outp->ip_addr.addr);
    return ERR_OK;
  }
#endif

#if LWIP_UDP
  if (IPH_PROTO(iphdr) == IP_PROTO_UDP) {
    struct udp_hdr *udphdr = (struct udp_hdr *)((u8_t *)p->payload + IPH_HL(iphdr) * 4);
    u16_t mport;

    struct portmap_table *m = ip_portmap_find_dest(IP_PROTO_UDP, udphdr->src, iphdr->src.addr);
    if (m) {
      /* packet from port-mapped dest addr/port: rewrite source to this node */
      if (m->mport != udphdr->src)
        ip_napt_modify_port_udp(udphdr, 0, m->mport);
      ip_napt_modify_addr_udp(udphdr, &iphdr->src, m->maddr);
      ip_napt_modify_addr(iphdr, &iphdr->src, m->maddr);
      return ERR_OK;
    }
    if (PP_NTOHS(udphdr->src) >= 1024) {
      /* Register new UDP session */
      mport = ip_napt_add(IP_PROTO_UDP, iphdr->src.addr, udphdr->src,
                          iphdr->dest.addr, udphdr->dest);
    } else {
      struct napt_table *t = ip_napt_find(IP_PROTO_UDP, iphdr->src.addr, udphdr->src, 0, 0);
      if (!t || t->dest != iphdr->dest.addr || t->dport != udphdr->dest) {
#if LWIP_ICMP
        icmp_dest_unreach(p, ICMP_DUR_PORT);
#endif
        return ERR_RTE; /* Drop unknown UDP session */
      }
      mport = t->mport;
    }

    if (mport != udphdr->src)
      ip_napt_modify_port_udp(udphdr, 0, mport);
    ip_napt_modify_addr_udp(udphdr, &iphdr->src, outp->ip_addr.addr);
    ip_napt_modify_addr(iphdr, &iphdr->src, outp->ip_addr.addr);
    return ERR_OK;
  }
#endif

  return ERR_OK;
}
#endif // IP_NAPT

/**
 * Forwards an IP packet. It finds an appropriate route for the
 * packet, decrements the TTL value of the packet, adjusts the
 * checksum and outputs the packet on the appropriate interface.
 *
 * @param p the packet to forward (p->payload points to IP header)
 * @param iphdr the IP header of the input packet
 * @param inp the netif on which this packet was received
 */
static void ICACHE_FLASH_ATTR
ip4_forward(struct pbuf *p, struct ip_hdr *iphdr, struct netif *inp)
{
  struct netif *netif;

  PERF_START;

  /* RFC3927 2.7: do not forward link-local addresses */
  if (ip4_addr_islinklocal(ip4_current_dest_addr())) {
    LWIP_DEBUGF(IP_DEBUG, ("ip4_forward: not forwarding LLA %"U16_F".%"U16_F".%"U16_F".%"U16_F"\n",
      ip4_addr1_16(ip4_current_dest_addr()), ip4_addr2_16(ip4_current_dest_addr()),
      ip4_addr3_16(ip4_current_dest_addr()), ip4_addr4_16(ip4_current_dest_addr())));
    goto return_noroute;
  }

  /* Find network interface where to forward this IP packet to. */
  netif = ip4_route_src(ip4_current_dest_addr(), ip4_current_src_addr());
  if (netif == NULL) {
    LWIP_DEBUGF(IP_DEBUG, ("ip4_forward: no forwarding route for %"U16_F".%"U16_F".%"U16_F".%"U16_F" found\n",
      ip4_addr1_16(ip4_current_dest_addr()), ip4_addr2_16(ip4_current_dest_addr()),
      ip4_addr3_16(ip4_current_dest_addr()), ip4_addr4_16(ip4_current_dest_addr())));
    /* @todo: send ICMP_DUR_NET? */
    goto return_noroute;
  }
  /* Do not forward packets onto the same network interface on which
   * they arrived. */
  if (netif == inp) {
    LWIP_DEBUGF(IP_DEBUG, ("ip_forward: not bouncing packets back on incoming interface.\n"));
    goto return_noroute;
  }

  /* decrement TTL */
  IPH_TTL_SET(iphdr, IPH_TTL(iphdr) - 1);
  /* send ICMP if TTL == 0 */
  if (IPH_TTL(iphdr) == 0) {
    MIB2_STATS_INC(mib2.ipinhdrerrors);
#if LWIP_ICMP
    /* Don't send ICMP messages in response to ICMP messages */
    if (IPH_PROTO(iphdr) != IP_PROTO_ICMP) {
      icmp_time_exceeded(p, ICMP_TE_TTL);
    }
#endif /* LWIP_ICMP */
    return;
  }

#if IP_NAPT
  if (ip_napt_forward(p, iphdr, inp, netif) != ERR_OK)
    return;
#endif

  /* Incrementally update the IP checksum. */
  if (IPH_CHKSUM(iphdr) >= PP_HTONS(0xffff - 0x100)) {
    IPH_CHKSUM_SET(iphdr, IPH_CHKSUM(iphdr) + PP_HTONS(0x100) + 1);
  } else {
    IPH_CHKSUM_SET(iphdr, IPH_CHKSUM(iphdr) + PP_HTONS(0x100));
  }

/*  os_printf("Old: %4x ", PP_NTOHS(IPH_CHKSUM(iphdr)));
 
  IPH_CHKSUM_SET(iphdr, 0);
  IPH_CHKSUM_SET(iphdr, inet_chksum(iphdr, IP_HLEN));
  os_printf("Now: %4x\r\n", PP_NTOHS(IPH_CHKSUM(iphdr)));
*/
  LWIP_DEBUGF(IP_DEBUG, ("ip_forward: forwarding packet to %"U16_F".%"U16_F".%"U16_F".%"U16_F"\n",
    ip4_addr1_16(ip_data.current_iphdr_dest), ip4_addr2_16(ip_data.current_iphdr_dest),
    ip4_addr3_16(ip_data.current_iphdr_dest), ip4_addr4_16(ip_data.current_iphdr_dest)));

  IP_STATS_INC(ip.fw);
  IP_STATS_INC(ip.xmit);
  MIB2_STATS_INC(mib2.ipforwdatagrams);

  PERF_STOP("ip_forward");
  /* transmit pbuf on chosen interface */
  netif->output(netif, p, ip4_current_dest_addr());
  return;
return_noroute:
  MIB2_STATS_INC(mib2.ipoutnoroutes);
}
#endif /* IP_FORWARD */

/**
 * This function is called by the network interface device driver when
 * an IP packet is received. The function does the basic checks of the
 * IP header such as packet size being at least larger than the header
 * size etc. If the packet was not destined for us, the packet is
 * forwarded (using ip_forward). The IP checksum is always checked.
 *
 * Finally, the packet is sent to the upper layer protocol input function.
 *
 * @param p the received IP packet (p->payload points to IP header)
 * @param inp the netif on which this packet was received
 * @return ERR_OK if the packet was processed (could return ERR_* if it wasn't
 *         processed, but currently always returns ERR_OK)
 */
err_t
ip4_input(struct pbuf *p, struct netif *inp)
{
  struct ip_hdr *iphdr;
  struct netif *netif;
  u16_t iphdr_hlen;
  u16_t iphdr_len;
#if IP_ACCEPT_LINK_LAYER_ADDRESSING || LWIP_IGMP
  int check_ip_src = 1;
#endif /* IP_ACCEPT_LINK_LAYER_ADDRESSING || LWIP_IGMP */

  IP_STATS_INC(ip.recv);
  MIB2_STATS_INC(mib2.ipinreceives);

  /* identify the IP header */
  iphdr = (struct ip_hdr *)p->payload;
  if (IPH_V(iphdr) != 4) {
    LWIP_DEBUGF(IP_DEBUG | LWIP_DBG_LEVEL_WARNING, ("IP packet dropped due to bad version number %"U16_F"\n", (u16_t)IPH_V(iphdr)));
    ip4_debug_print(p);
    pbuf_free(p);
    IP_STATS_INC(ip.err);
    IP_STATS_INC(ip.drop);
    MIB2_STATS_INC(mib2.ipinhdrerrors);
    return ERR_OK;
  }

#ifdef LWIP_HOOK_IP4_INPUT
  if (LWIP_HOOK_IP4_INPUT(p, inp)) {
    /* the packet has been eaten */
    return ERR_OK;
  }
#endif

  /* obtain IP header length in number of 32-bit words */
  iphdr_hlen = IPH_HL(iphdr);
  /* calculate IP header length in bytes */
  iphdr_hlen *= 4;
  /* obtain ip length in bytes */
  iphdr_len = lwip_ntohs(IPH_LEN(iphdr));

  /* Trim pbuf. This is especially required for packets < 60 bytes. */
  if (iphdr_len < p->tot_len) {
    pbuf_realloc(p, iphdr_len);
  }

  /* header length exceeds first pbuf length, or ip length exceeds total pbuf length? */
  if ((iphdr_hlen > p->len) || (iphdr_len > p->tot_len) || (iphdr_hlen < IP_HLEN)) {
    if (iphdr_hlen < IP_HLEN) {
      LWIP_DEBUGF(IP_DEBUG | LWIP_DBG_LEVEL_SERIOUS,
        ("ip4_input: short IP header (%"U16_F" bytes) received, IP packet dropped\n", iphdr_hlen));
    }
    if (iphdr_hlen > p->len) {
      LWIP_DEBUGF(IP_DEBUG | LWIP_DBG_LEVEL_SERIOUS,
        ("IP header (len %"U16_F") does not fit in first pbuf (len %"U16_F"), IP packet dropped.\n",
        iphdr_hlen, p->len));
    }
    if (iphdr_len > p->tot_len) {
      LWIP_DEBUGF(IP_DEBUG | LWIP_DBG_LEVEL_SERIOUS,
        ("IP (len %"U16_F") is longer than pbuf (len %"U16_F"), IP packet dropped.\n",
        iphdr_len, p->tot_len));
    }
    /* free (drop) packet pbufs */
    pbuf_free(p);
    IP_STATS_INC(ip.lenerr);
    IP_STATS_INC(ip.drop);
    MIB2_STATS_INC(mib2.ipindiscards);
    return ERR_OK;
  }

  /* verify checksum */
#if CHECKSUM_CHECK_IP
  IF__NETIF_CHECKSUM_ENABLED(inp, NETIF_CHECKSUM_CHECK_IP) {
    if (inet_chksum(iphdr, iphdr_hlen) != 0) {

      LWIP_DEBUGF(IP_DEBUG | LWIP_DBG_LEVEL_SERIOUS,
        ("Checksum (0x%"X16_F") failed, IP packet dropped.\n", inet_chksum(iphdr, iphdr_hlen)));
      ip4_debug_print(p);
      pbuf_free(p);
      IP_STATS_INC(ip.chkerr);
      IP_STATS_INC(ip.drop);
      MIB2_STATS_INC(mib2.ipinhdrerrors);
      return ERR_OK;
    }
  }
#endif

#if IP_NAPT
  /* for unicast packet, check NAPT table and modify dest if needed */
  if (!inp->napt && ip4_addr_cmp(&iphdr->dest, &(inp->ip_addr)))
    ip_napt_recv(p, iphdr, netif);
#endif

  /* copy IP addresses to aligned ip_addr_t */
  ip_addr_copy_from_ip4(ip_data.current_iphdr_dest, iphdr->dest);
  ip_addr_copy_from_ip4(ip_data.current_iphdr_src, iphdr->src);

  /* match packet against an interface, i.e. is this packet for us? */
  if (ip4_addr_ismulticast(ip4_current_dest_addr())) {
#if LWIP_IGMP
    if ((inp->flags & NETIF_FLAG_IGMP) && (igmp_lookfor_group(inp, ip4_current_dest_addr()))) {
      /* IGMP snooping switches need 0.0.0.0 to be allowed as source address (RFC 4541) */
      ip4_addr_t allsystems;
      IP4_ADDR(&allsystems, 224, 0, 0, 1);
      if (ip4_addr_cmp(ip4_current_dest_addr(), &allsystems) &&
          ip4_addr_isany(ip4_current_src_addr())) {
        check_ip_src = 0;
      }
      netif = inp;
    } else {
      netif = NULL;
    }
#else /* LWIP_IGMP */
    if ((netif_is_up(inp)) && (!ip4_addr_isany_val(*netif_ip4_addr(inp)))) {
      netif = inp;
    } else {
      netif = NULL;
    }
#endif /* LWIP_IGMP */
  } else {
    /* start trying with inp. if that's not acceptable, start walking the
       list of configured netifs.
       'first' is used as a boolean to mark whether we started walking the list */
    int first = 1;
    netif = inp;
    do {
      LWIP_DEBUGF(IP_DEBUG, ("ip_input: iphdr->dest 0x%"X32_F" netif->ip_addr 0x%"X32_F" (0x%"X32_F", 0x%"X32_F", 0x%"X32_F")\n",
          ip4_addr_get_u32(&iphdr->dest), ip4_addr_get_u32(netif_ip4_addr(netif)),
          ip4_addr_get_u32(&iphdr->dest) & ip4_addr_get_u32(netif_ip4_netmask(netif)),
          ip4_addr_get_u32(netif_ip4_addr(netif)) & ip4_addr_get_u32(netif_ip4_netmask(netif)),
          ip4_addr_get_u32(&iphdr->dest) & ~ip4_addr_get_u32(netif_ip4_netmask(netif))));

      /* interface is up and configured? */
      if ((netif_is_up(netif)) && (!ip4_addr_isany_val(*netif_ip4_addr(netif)))) {
        /* unicast to this interface address? */
        if (ip4_addr_cmp(ip4_current_dest_addr(), netif_ip4_addr(netif)) ||
            /* or broadcast on this interface network address? */
            ip4_addr_isbroadcast(ip4_current_dest_addr(), netif)
#if LWIP_NETIF_LOOPBACK && !LWIP_HAVE_LOOPIF
            || (ip4_addr_get_u32(ip4_current_dest_addr()) == PP_HTONL(IPADDR_LOOPBACK))
#endif /* LWIP_NETIF_LOOPBACK && !LWIP_HAVE_LOOPIF */
            ) {
          LWIP_DEBUGF(IP_DEBUG, ("ip4_input: packet accepted on interface %c%c\n",
              netif->name[0], netif->name[1]));
          /* break out of for loop */
          break;
        }
#if LWIP_AUTOIP
        /* connections to link-local addresses must persist after changing
           the netif's address (RFC3927 ch. 1.9) */
        if (autoip_accept_packet(netif, ip4_current_dest_addr())) {
          LWIP_DEBUGF(IP_DEBUG, ("ip4_input: LLA packet accepted on interface %c%c\n",
              netif->name[0], netif->name[1]));
          /* break out of for loop */
          break;
        }
#endif /* LWIP_AUTOIP */
      }
      if (first) {
#if !LWIP_NETIF_LOOPBACK || LWIP_HAVE_LOOPIF
        /* Packets sent to the loopback address must not be accepted on an
         * interface that does not have the loopback address assigned to it,
         * unless a non-loopback interface is used for loopback traffic. */
        if (ip4_addr_isloopback(ip4_current_dest_addr())) {
          netif = NULL;
          break;
        }
#endif /* !LWIP_NETIF_LOOPBACK || LWIP_HAVE_LOOPIF */
        first = 0;
        netif = netif_list;
      } else {
        netif = netif->next;
      }
      if (netif == inp) {
        netif = netif->next;
      }
    } while (netif != NULL);
  }

#if IP_ACCEPT_LINK_LAYER_ADDRESSING
  /* Pass DHCP messages regardless of destination address. DHCP traffic is addressed
   * using link layer addressing (such as Ethernet MAC) so we must not filter on IP.
   * According to RFC 1542 section 3.1.1, referred by RFC 2131).
   *
   * If you want to accept private broadcast communication while a netif is down,
   * define LWIP_IP_ACCEPT_UDP_PORT(dst_port), e.g.:
   *
   * #define LWIP_IP_ACCEPT_UDP_PORT(dst_port) ((dst_port) == PP_NTOHS(12345))
   */
  if (netif == NULL) {
    /* remote port is DHCP server? */
    if (IPH_PROTO(iphdr) == IP_PROTO_UDP) {
      struct udp_hdr *udphdr = (struct udp_hdr *)((u8_t *)iphdr + iphdr_hlen);
      LWIP_DEBUGF(IP_DEBUG | LWIP_DBG_TRACE, ("ip4_input: UDP packet to DHCP client port %"U16_F"\n",
        lwip_ntohs(udphdr->dest)));
      if (IP_ACCEPT_LINK_LAYER_ADDRESSED_PORT(udphdr->dest)) {
        LWIP_DEBUGF(IP_DEBUG | LWIP_DBG_TRACE, ("ip4_input: DHCP packet accepted.\n"));
        netif = inp;
        check_ip_src = 0;
      }
    }
  }
#endif /* IP_ACCEPT_LINK_LAYER_ADDRESSING */

  /* broadcast or multicast packet source address? Compliant with RFC 1122: 3.2.1.3 */
#if LWIP_IGMP || IP_ACCEPT_LINK_LAYER_ADDRESSING
  if (check_ip_src
#if IP_ACCEPT_LINK_LAYER_ADDRESSING
  /* DHCP servers need 0.0.0.0 to be allowed as source address (RFC 1.1.2.2: 3.2.1.3/a) */
      && !ip4_addr_isany_val(*ip4_current_src_addr())
#endif /* IP_ACCEPT_LINK_LAYER_ADDRESSING */
     )
#endif /* LWIP_IGMP || IP_ACCEPT_LINK_LAYER_ADDRESSING */
  {
    if ((ip4_addr_isbroadcast(ip4_current_src_addr(), inp)) ||
        (ip4_addr_ismulticast(ip4_current_src_addr()))) {
      /* packet source is not valid */
      LWIP_DEBUGF(IP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_LEVEL_WARNING, ("ip4_input: packet source is not valid.\n"));
      /* free (drop) packet pbufs */
      pbuf_free(p);
      IP_STATS_INC(ip.drop);
      MIB2_STATS_INC(mib2.ipinaddrerrors);
      MIB2_STATS_INC(mib2.ipindiscards);
      return ERR_OK;
    }
  }

  /* packet not for us? */
  if (netif == NULL) {
    /* packet not for us, route or discard */
    LWIP_DEBUGF(IP_DEBUG | LWIP_DBG_TRACE, ("ip4_input: packet not for us.\n"));
#if IP_FORWARD
    /* non-broadcast packet? */
    if (!ip4_addr_isbroadcast(ip4_current_dest_addr(), inp)) {
      /* try to forward IP packet on (other) interfaces */
      ip4_forward(p, iphdr, inp);
    } else
#endif /* IP_FORWARD */
    {
      IP_STATS_INC(ip.drop);
      MIB2_STATS_INC(mib2.ipinaddrerrors);
      MIB2_STATS_INC(mib2.ipindiscards);
    }
    pbuf_free(p);
    return ERR_OK;
  }
  /* packet consists of multiple fragments? */
  if ((IPH_OFFSET(iphdr) & PP_HTONS(IP_OFFMASK | IP_MF)) != 0) {
#if IP_REASSEMBLY /* packet fragment reassembly code present? */
    LWIP_DEBUGF(IP_DEBUG, ("IP packet is a fragment (id=0x%04"X16_F" tot_len=%"U16_F" len=%"U16_F" MF=%"U16_F" offset=%"U16_F"), calling ip4_reass()\n",
      lwip_ntohs(IPH_ID(iphdr)), p->tot_len, lwip_ntohs(IPH_LEN(iphdr)), (u16_t)!!(IPH_OFFSET(iphdr) & PP_HTONS(IP_MF)), (u16_t)((lwip_ntohs(IPH_OFFSET(iphdr)) & IP_OFFMASK)*8)));
    /* reassemble the packet*/
    p = ip4_reass(p);
    /* packet not fully reassembled yet? */
    if (p == NULL) {
      return ERR_OK;
    }
    iphdr = (struct ip_hdr *)p->payload;
#else /* IP_REASSEMBLY == 0, no packet fragment reassembly code present */
    pbuf_free(p);
    LWIP_DEBUGF(IP_DEBUG | LWIP_DBG_LEVEL_SERIOUS, ("IP packet dropped since it was fragmented (0x%"X16_F") (while IP_REASSEMBLY == 0).\n",
      lwip_ntohs(IPH_OFFSET(iphdr))));
    IP_STATS_INC(ip.opterr);
    IP_STATS_INC(ip.drop);
    /* unsupported protocol feature */
    MIB2_STATS_INC(mib2.ipinunknownprotos);
    return ERR_OK;
#endif /* IP_REASSEMBLY */
  }

#if IP_OPTIONS_ALLOWED == 0 /* no support for IP options in the IP header? */

#if LWIP_IGMP
  /* there is an extra "router alert" option in IGMP messages which we allow for but do not police */
  if ((iphdr_hlen > IP_HLEN) &&  (IPH_PROTO(iphdr) != IP_PROTO_IGMP)) {
#else
  if (iphdr_hlen > IP_HLEN) {
#endif /* LWIP_IGMP */
    LWIP_DEBUGF(IP_DEBUG | LWIP_DBG_LEVEL_SERIOUS, ("IP packet dropped since there were IP options (while IP_OPTIONS_ALLOWED == 0).\n"));
    pbuf_free(p);
    IP_STATS_INC(ip.opterr);
    IP_STATS_INC(ip.drop);
    /* unsupported protocol feature */
    MIB2_STATS_INC(mib2.ipinunknownprotos);
    return ERR_OK;
  }
#endif /* IP_OPTIONS_ALLOWED == 0 */

  /* send to upper layers */
  LWIP_DEBUGF(IP_DEBUG, ("ip4_input: \n"));
  ip4_debug_print(p);
  LWIP_DEBUGF(IP_DEBUG, ("ip4_input: p->len %"U16_F" p->tot_len %"U16_F"\n", p->len, p->tot_len));

  ip_data.current_netif = netif;
  ip_data.current_input_netif = inp;
  ip_data.current_ip4_header = iphdr;
  ip_data.current_ip_header_tot_len = IPH_HL(iphdr) * 4;

#if LWIP_RAW
  /* raw input did not eat the packet? */
  if (raw_input(p, inp) == 0)
#endif /* LWIP_RAW */
  {
    pbuf_header(p, -(s16_t)iphdr_hlen); /* Move to payload, no check necessary. */

    switch (IPH_PROTO(iphdr)) {
#if LWIP_UDP
    case IP_PROTO_UDP:
#if LWIP_UDPLITE
    case IP_PROTO_UDPLITE:
#endif /* LWIP_UDPLITE */
      MIB2_STATS_INC(mib2.ipindelivers);
      udp_input(p, inp);
      break;
#endif /* LWIP_UDP */
#if LWIP_TCP
    case IP_PROTO_TCP:
      MIB2_STATS_INC(mib2.ipindelivers);
      tcp_input(p, inp);
      break;
#endif /* LWIP_TCP */
#if LWIP_ICMP
    case IP_PROTO_ICMP:
      MIB2_STATS_INC(mib2.ipindelivers);
      icmp_input(p, inp);
      break;
#endif /* LWIP_ICMP */
#if LWIP_IGMP
    case IP_PROTO_IGMP:
      igmp_input(p, inp, ip4_current_dest_addr());
      break;
#endif /* LWIP_IGMP */
    default:
#if LWIP_ICMP
      /* send ICMP destination protocol unreachable unless is was a broadcast */
      if (!ip4_addr_isbroadcast(ip4_current_dest_addr(), netif) &&
          !ip4_addr_ismulticast(ip4_current_dest_addr())) {
        pbuf_header_force(p, iphdr_hlen); /* Move to ip header, no check necessary. */
        p->payload = iphdr;
        icmp_dest_unreach(p, ICMP_DUR_PROTO);
      }
#endif /* LWIP_ICMP */
      pbuf_free(p);

      LWIP_DEBUGF(IP_DEBUG | LWIP_DBG_LEVEL_SERIOUS, ("Unsupported transport protocol %"U16_F"\n", (u16_t)IPH_PROTO(iphdr)));

      IP_STATS_INC(ip.proterr);
      IP_STATS_INC(ip.drop);
      MIB2_STATS_INC(mib2.ipinunknownprotos);
    }
  }

  /* @todo: this is not really necessary... */
  ip_data.current_netif = NULL;
  ip_data.current_input_netif = NULL;
  ip_data.current_ip4_header = NULL;
  ip_data.current_ip_header_tot_len = 0;
  ip4_addr_set_any(ip4_current_src_addr());
  ip4_addr_set_any(ip4_current_dest_addr());

  return ERR_OK;
}

/**
 * Sends an IP packet on a network interface. This function constructs
 * the IP header and calculates the IP header checksum. If the source
 * IP address is NULL, the IP address of the outgoing network
 * interface is filled in as source address.
 * If the destination IP address is LWIP_IP_HDRINCL, p is assumed to already
 * include an IP header and p->payload points to it instead of the data.
 *
 * @param p the packet to send (p->payload points to the data, e.g. next
            protocol header; if dest == LWIP_IP_HDRINCL, p already includes an
            IP header and p->payload points to that IP header)
 * @param src the source IP address to send from (if src == IP4_ADDR_ANY, the
 *         IP  address of the netif used to send is used as source address)
 * @param dest the destination IP address to send the packet to
 * @param ttl the TTL value to be set in the IP header
 * @param tos the TOS value to be set in the IP header
 * @param proto the PROTOCOL to be set in the IP header
 * @param netif the netif on which to send this packet
 * @return ERR_OK if the packet was sent OK
 *         ERR_BUF if p doesn't have enough space for IP/LINK headers
 *         returns errors returned by netif->output
 *
 * @note ip_id: RFC791 "some host may be able to simply use
 *  unique identifiers independent of destination"
 */
err_t
ip4_output_if(struct pbuf *p, const ip4_addr_t *src, const ip4_addr_t *dest,
             u8_t ttl, u8_t tos,
             u8_t proto, struct netif *netif)
{
#if IP_OPTIONS_SEND
  return ip4_output_if_opt(p, src, dest, ttl, tos, proto, netif, NULL, 0);
}

/**
 * Same as ip_output_if() but with the possibility to include IP options:
 *
 * @ param ip_options pointer to the IP options, copied into the IP header
 * @ param optlen length of ip_options
 */
err_t
ip4_output_if_opt(struct pbuf *p, const ip4_addr_t *src, const ip4_addr_t *dest,
       u8_t ttl, u8_t tos, u8_t proto, struct netif *netif, void *ip_options,
       u16_t optlen)
{
#endif /* IP_OPTIONS_SEND */
  const ip4_addr_t *src_used = src;
  if (dest != LWIP_IP_HDRINCL) {
    if (ip4_addr_isany(src)) {
      src_used = netif_ip4_addr(netif);
    }
  }

#if IP_OPTIONS_SEND
  return ip4_output_if_opt_src(p, src_used, dest, ttl, tos, proto, netif,
    ip_options, optlen);
#else /* IP_OPTIONS_SEND */
  return ip4_output_if_src(p, src_used, dest, ttl, tos, proto, netif);
#endif /* IP_OPTIONS_SEND */
}

/**
 * Same as ip_output_if() but 'src' address is not replaced by netif address
 * when it is 'any'.
 */
err_t
ip4_output_if_src(struct pbuf *p, const ip4_addr_t *src, const ip4_addr_t *dest,
             u8_t ttl, u8_t tos,
             u8_t proto, struct netif *netif)
{
#if IP_OPTIONS_SEND
  return ip4_output_if_opt_src(p, src, dest, ttl, tos, proto, netif, NULL, 0);
}

/**
 * Same as ip_output_if_opt() but 'src' address is not replaced by netif address
 * when it is 'any'.
 */
err_t
ip4_output_if_opt_src(struct pbuf *p, const ip4_addr_t *src, const ip4_addr_t *dest,
       u8_t ttl, u8_t tos, u8_t proto, struct netif *netif, void *ip_options,
       u16_t optlen)
{
#endif /* IP_OPTIONS_SEND */
  struct ip_hdr *iphdr;
  ip4_addr_t dest_addr;
#if CHECKSUM_GEN_IP_INLINE
  u32_t chk_sum = 0;
#endif /* CHECKSUM_GEN_IP_INLINE */

  LWIP_IP_CHECK_PBUF_REF_COUNT_FOR_TX(p);

  MIB2_STATS_INC(mib2.ipoutrequests);

  /* Should the IP header be generated or is it already included in p? */
  if (dest != LWIP_IP_HDRINCL) {
    u16_t ip_hlen = IP_HLEN;
#if IP_OPTIONS_SEND
    u16_t optlen_aligned = 0;
    if (optlen != 0) {
#if CHECKSUM_GEN_IP_INLINE
      int i;
#endif /* CHECKSUM_GEN_IP_INLINE */
      /* round up to a multiple of 4 */
      optlen_aligned = ((optlen + 3) & ~3);
      ip_hlen += optlen_aligned;
      /* First write in the IP options */
      if (pbuf_header(p, optlen_aligned)) {
        LWIP_DEBUGF(IP_DEBUG | LWIP_DBG_LEVEL_SERIOUS, ("ip4_output_if_opt: not enough room for IP options in pbuf\n"));
        IP_STATS_INC(ip.err);
        MIB2_STATS_INC(mib2.ipoutdiscards);
        return ERR_BUF;
      }
      MEMCPY(p->payload, ip_options, optlen);
      if (optlen < optlen_aligned) {
        /* zero the remaining bytes */
        memset(((char*)p->payload) + optlen, 0, optlen_aligned - optlen);
      }
#if CHECKSUM_GEN_IP_INLINE
      for (i = 0; i < optlen_aligned/2; i++) {
        chk_sum += ((u16_t*)p->payload)[i];
      }
#endif /* CHECKSUM_GEN_IP_INLINE */
    }
#endif /* IP_OPTIONS_SEND */
    /* generate IP header */
    if (pbuf_header(p, IP_HLEN)) {
      LWIP_DEBUGF(IP_DEBUG | LWIP_DBG_LEVEL_SERIOUS, ("ip4_output: not enough room for IP header in pbuf\n"));

      IP_STATS_INC(ip.err);
      MIB2_STATS_INC(mib2.ipoutdiscards);
      return ERR_BUF;
    }

    iphdr = (struct ip_hdr *)p->payload;
    LWIP_ASSERT("check that first pbuf can hold struct ip_hdr",
               (p->len >= sizeof(struct ip_hdr)));

    IPH_TTL_SET(iphdr, ttl);
    IPH_PROTO_SET(iphdr, proto);
#if CHECKSUM_GEN_IP_INLINE
    chk_sum += PP_NTOHS(proto | (ttl << 8));
#endif /* CHECKSUM_GEN_IP_INLINE */

    /* dest cannot be NULL here */
    ip4_addr_copy(iphdr->dest, *dest);
#if CHECKSUM_GEN_IP_INLINE
    chk_sum += ip4_addr_get_u32(&iphdr->dest) & 0xFFFF;
    chk_sum += ip4_addr_get_u32(&iphdr->dest) >> 16;
#endif /* CHECKSUM_GEN_IP_INLINE */

    IPH_VHL_SET(iphdr, 4, ip_hlen / 4);
    IPH_TOS_SET(iphdr, tos);
#if CHECKSUM_GEN_IP_INLINE
    chk_sum += PP_NTOHS(tos | (iphdr->_v_hl << 8));
#endif /* CHECKSUM_GEN_IP_INLINE */
    IPH_LEN_SET(iphdr, lwip_htons(p->tot_len));
#if CHECKSUM_GEN_IP_INLINE
    chk_sum += iphdr->_len;
#endif /* CHECKSUM_GEN_IP_INLINE */
    IPH_OFFSET_SET(iphdr, 0);
    IPH_ID_SET(iphdr, lwip_htons(ip_id));
#if CHECKSUM_GEN_IP_INLINE
    chk_sum += iphdr->_id;
#endif /* CHECKSUM_GEN_IP_INLINE */
    ++ip_id;

    if (src == NULL) {
      ip4_addr_copy(iphdr->src, *IP4_ADDR_ANY4);
    } else {
      /* src cannot be NULL here */
      ip4_addr_copy(iphdr->src, *src);
    }

#if CHECKSUM_GEN_IP_INLINE
    chk_sum += ip4_addr_get_u32(&iphdr->src) & 0xFFFF;
    chk_sum += ip4_addr_get_u32(&iphdr->src) >> 16;
    chk_sum = (chk_sum >> 16) + (chk_sum & 0xFFFF);
    chk_sum = (chk_sum >> 16) + chk_sum;
    chk_sum = ~chk_sum;
    IF__NETIF_CHECKSUM_ENABLED(netif, NETIF_CHECKSUM_GEN_IP) {
      iphdr->_chksum = (u16_t)chk_sum; /* network order */
    }
#if LWIP_CHECKSUM_CTRL_PER_NETIF
    else {
      IPH_CHKSUM_SET(iphdr, 0);
    }
#endif /* LWIP_CHECKSUM_CTRL_PER_NETIF*/
#else /* CHECKSUM_GEN_IP_INLINE */
    IPH_CHKSUM_SET(iphdr, 0);
#if CHECKSUM_GEN_IP
    IF__NETIF_CHECKSUM_ENABLED(netif, NETIF_CHECKSUM_GEN_IP) {
      IPH_CHKSUM_SET(iphdr, inet_chksum(iphdr, ip_hlen));
    }
#endif /* CHECKSUM_GEN_IP */
#endif /* CHECKSUM_GEN_IP_INLINE */
  } else {
    /* IP header already included in p */
    iphdr = (struct ip_hdr *)p->payload;
    ip4_addr_copy(dest_addr, iphdr->dest);
    dest = &dest_addr;
  }

  IP_STATS_INC(ip.xmit);

  LWIP_DEBUGF(IP_DEBUG, ("ip4_output_if: %c%c%"U16_F"\n", netif->name[0], netif->name[1], (u16_t)netif->num));
  ip4_debug_print(p);

#if ENABLE_LOOPBACK
  if (ip4_addr_cmp(dest, netif_ip4_addr(netif))
#if !LWIP_HAVE_LOOPIF
      || ip4_addr_isloopback(dest)
#endif /* !LWIP_HAVE_LOOPIF */
      ) {
    /* Packet to self, enqueue it for loopback */
    LWIP_DEBUGF(IP_DEBUG, ("netif_loop_output()"));
    return netif_loop_output(netif, p);
  }
#if LWIP_MULTICAST_TX_OPTIONS
  if ((p->flags & PBUF_FLAG_MCASTLOOP) != 0) {
    netif_loop_output(netif, p);
  }
#endif /* LWIP_MULTICAST_TX_OPTIONS */
#endif /* ENABLE_LOOPBACK */
#if IP_FRAG
  /* don't fragment if interface has mtu set to 0 [loopif] */
  if (netif->mtu && (p->tot_len > netif->mtu)) {
    return ip4_frag(p, netif, dest);
  }
#endif /* IP_FRAG */

  LWIP_DEBUGF(IP_DEBUG, ("ip4_output_if: call netif->output()\n"));
  return netif->output(netif, p, dest);
}

/**
 * Simple interface to ip_output_if. It finds the outgoing network
 * interface and calls upon ip_output_if to do the actual work.
 *
 * @param p the packet to send (p->payload points to the data, e.g. next
            protocol header; if dest == LWIP_IP_HDRINCL, p already includes an
            IP header and p->payload points to that IP header)
 * @param src the source IP address to send from (if src == IP4_ADDR_ANY, the
 *         IP  address of the netif used to send is used as source address)
 * @param dest the destination IP address to send the packet to
 * @param ttl the TTL value to be set in the IP header
 * @param tos the TOS value to be set in the IP header
 * @param proto the PROTOCOL to be set in the IP header
 *
 * @return ERR_RTE if no route is found
 *         see ip_output_if() for more return values
 */
err_t
ip4_output(struct pbuf *p, const ip4_addr_t *src, const ip4_addr_t *dest,
          u8_t ttl, u8_t tos, u8_t proto)
{
  struct netif *netif;

  LWIP_IP_CHECK_PBUF_REF_COUNT_FOR_TX(p);

  if ((netif = ip4_route_src(dest, src)) == NULL) {
    LWIP_DEBUGF(IP_DEBUG, ("ip4_output: No route to %"U16_F".%"U16_F".%"U16_F".%"U16_F"\n",
      ip4_addr1_16(dest), ip4_addr2_16(dest), ip4_addr3_16(dest), ip4_addr4_16(dest)));
    IP_STATS_INC(ip.rterr);
    return ERR_RTE;
  }

  return ip4_output_if(p, src, dest, ttl, tos, proto, netif);
}

#if LWIP_NETIF_HWADDRHINT
/** Like ip_output, but takes and addr_hint pointer that is passed on to netif->addr_hint
 *  before calling ip_output_if.
 *
 * @param p the packet to send (p->payload points to the data, e.g. next
            protocol header; if dest == LWIP_IP_HDRINCL, p already includes an
            IP header and p->payload points to that IP header)
 * @param src the source IP address to send from (if src == IP4_ADDR_ANY, the
 *         IP  address of the netif used to send is used as source address)
 * @param dest the destination IP address to send the packet to
 * @param ttl the TTL value to be set in the IP header
 * @param tos the TOS value to be set in the IP header
 * @param proto the PROTOCOL to be set in the IP header
 * @param addr_hint address hint pointer set to netif->addr_hint before
 *        calling ip_output_if()
 *
 * @return ERR_RTE if no route is found
 *         see ip_output_if() for more return values
 */
err_t
ip4_output_hinted(struct pbuf *p, const ip4_addr_t *src, const ip4_addr_t *dest,
          u8_t ttl, u8_t tos, u8_t proto, u8_t *addr_hint)
{
  struct netif *netif;
  err_t err;

  LWIP_IP_CHECK_PBUF_REF_COUNT_FOR_TX(p);

  if ((netif = ip4_route_src(dest, src)) == NULL) {
    LWIP_DEBUGF(IP_DEBUG, ("ip4_output: No route to %"U16_F".%"U16_F".%"U16_F".%"U16_F"\n",
      ip4_addr1_16(dest), ip4_addr2_16(dest), ip4_addr3_16(dest), ip4_addr4_16(dest)));
    IP_STATS_INC(ip.rterr);
    return ERR_RTE;
  }

  NETIF_SET_HWADDRHINT(netif, addr_hint);
  err = ip4_output_if(p, src, dest, ttl, tos, proto, netif);
  NETIF_SET_HWADDRHINT(netif, NULL);

  return err;
}
#endif /* LWIP_NETIF_HWADDRHINT*/

#if IP_DEBUG
/* Print an IP header by using LWIP_DEBUGF
 * @param p an IP packet, p->payload pointing to the IP header
 */
void
ip4_debug_print(struct pbuf *p)
{
  struct ip_hdr *iphdr = (struct ip_hdr *)p->payload;

  LWIP_DEBUGF(IP_DEBUG, ("IP header:\n"));
  LWIP_DEBUGF(IP_DEBUG, ("+-------------------------------+\n"));
  LWIP_DEBUGF(IP_DEBUG, ("|%2"S16_F" |%2"S16_F" |  0x%02"X16_F" |     %5"U16_F"     | (v, hl, tos, len)\n",
                    (u16_t)IPH_V(iphdr),
                    (u16_t)IPH_HL(iphdr),
                    (u16_t)IPH_TOS(iphdr),
                    lwip_ntohs(IPH_LEN(iphdr))));
  LWIP_DEBUGF(IP_DEBUG, ("+-------------------------------+\n"));
  LWIP_DEBUGF(IP_DEBUG, ("|    %5"U16_F"      |%"U16_F"%"U16_F"%"U16_F"|    %4"U16_F"   | (id, flags, offset)\n",
                    lwip_ntohs(IPH_ID(iphdr)),
                    (u16_t)(lwip_ntohs(IPH_OFFSET(iphdr)) >> 15 & 1),
                    (u16_t)(lwip_ntohs(IPH_OFFSET(iphdr)) >> 14 & 1),
                    (u16_t)(lwip_ntohs(IPH_OFFSET(iphdr)) >> 13 & 1),
                    (u16_t)(lwip_ntohs(IPH_OFFSET(iphdr)) & IP_OFFMASK)));
  LWIP_DEBUGF(IP_DEBUG, ("+-------------------------------+\n"));
  LWIP_DEBUGF(IP_DEBUG, ("|  %3"U16_F"  |  %3"U16_F"  |    0x%04"X16_F"     | (ttl, proto, chksum)\n",
                    (u16_t)IPH_TTL(iphdr),
                    (u16_t)IPH_PROTO(iphdr),
                    lwip_ntohs(IPH_CHKSUM(iphdr))));
  LWIP_DEBUGF(IP_DEBUG, ("+-------------------------------+\n"));
  LWIP_DEBUGF(IP_DEBUG, ("|  %3"U16_F"  |  %3"U16_F"  |  %3"U16_F"  |  %3"U16_F"  | (src)\n",
                    ip4_addr1_16(&iphdr->src),
                    ip4_addr2_16(&iphdr->src),
                    ip4_addr3_16(&iphdr->src),
                    ip4_addr4_16(&iphdr->src)));
  LWIP_DEBUGF(IP_DEBUG, ("+-------------------------------+\n"));
  LWIP_DEBUGF(IP_DEBUG, ("|  %3"U16_F"  |  %3"U16_F"  |  %3"U16_F"  |  %3"U16_F"  | (dest)\n",
                    ip4_addr1_16(&iphdr->dest),
                    ip4_addr2_16(&iphdr->dest),
                    ip4_addr3_16(&iphdr->dest),
                    ip4_addr4_16(&iphdr->dest)));
  LWIP_DEBUGF(IP_DEBUG, ("+-------------------------------+\n"));
}
#endif /* IP_DEBUG */

#endif /* LWIP_IPV4 */

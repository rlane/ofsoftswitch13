/* Copyright (c) 2011, TrafficLab, Ericsson Research, Hungary
 * Copyright (c) 2012, CPqD, Brazil
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of the Ericsson Research nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <netinet/in.h>
#include "packet_handle_std.h"
#include "packet.h"
#include "packets.h"
#include "oflib/ofl-structs.h"
#include "openflow/openflow.h"
#include "compiler.h"

#include "lib/hash.h"
#include "oflib/oxm-match.h"


/* Resets all protocol fields to NULL
TODO: OpenFlow 1.1 parsing, only for test*/

void
packet_handle_std_validate(struct packet_handle_std *handle) {

    if (handle->valid) {
        return;

    } else {
        struct packet *pkt = handle->pkt;
        struct ofl_match *m = &handle->match;
        struct protocols_std *proto = handle->proto;
        uint64_t current_metadata;
        struct ofl_match_tlv *field;
        size_t offset = 0;

        handle->valid = true;

        protocol_reset(handle->proto);

        m->header.type = OFPMT_OXM;
        ofl_structs_match_put32(m, OXM_OF_IN_PORT, htonl(pkt->in_port));
        HMAP_FOR_EACH_WITH_HASH(field, struct ofl_match_tlv, hmap_node,
            hash_int(OXM_OF_METADATA,0), & m->match_fields){
            current_metadata = (uint64_t) *field->value;
            ofl_structs_match_put64(m, OXM_OF_METADATA, current_metadata);
        }

        /* Ethernet */

        if (pkt->buffer->size < offset + sizeof(struct eth_header)) {
            return;
        }

        proto->eth = (struct eth_header *)((uint8_t *)
                                                pkt->buffer->data + offset);
        offset += sizeof(struct eth_header);

        if (ntohs(proto->eth->eth_type) >= ETH_TYPE_II_START) {
            /* Ethernet II */
            ofl_structs_match_put_eth(m, OXM_OF_ETH_SRC, proto->eth->eth_src);
            ofl_structs_match_put_eth(m, OXM_OF_ETH_DST, proto->eth->eth_dst);
            ofl_structs_match_put16(m, OXM_OF_ETH_TYPE,
                                                proto->eth->eth_type);
        } else {
            /* Ethernet 802.3 */
            struct llc_header *llc;

        // TODO Zoltan: compare packet length with ofpbuf length for validity

            if (pkt->buffer->size < offset + sizeof(struct llc_header)) {
                return;
            }

            llc = (struct llc_header *)((uint8_t *)pkt->buffer->data + offset);
            offset += sizeof(struct llc_header);

            if (!(llc->llc_dsap == LLC_DSAP_SNAP &&
                  llc->llc_ssap == LLC_SSAP_SNAP &&
                  llc->llc_cntl == LLC_CNTL_SNAP)) {
                return;
            }

            if (pkt->buffer->size < offset + sizeof(struct snap_header)) {
                return;
            }

            proto->eth_snap = (struct snap_header *)((uint8_t *)
                                        pkt->buffer->data + offset);
            offset += sizeof(struct snap_header);

            if (memcmp(proto->eth_snap->snap_org, SNAP_ORG_ETHERNET,
                                            sizeof(SNAP_ORG_ETHERNET)) != 0) {
                return;
            }
            ofl_structs_match_put_eth(m, OXM_OF_ETH_SRC, proto->eth->eth_src);
            ofl_structs_match_put_eth(m, OXM_OF_ETH_DST, proto->eth->eth_dst);
            ofl_structs_match_put16(m, OXM_OF_ETH_TYPE,
                                                ntohs(proto->eth->eth_type));
        }

        /* VLAN */
        if (ntohs(proto->eth->eth_type) == ETH_TYPE_VLAN ||
            ntohs(proto->eth->eth_type) == ETH_TYPE_VLAN_PBB) {

            uint16_t vlan_id;
            uint8_t vlan_pcp;
            if (pkt->buffer->size < offset + sizeof(struct vlan_header)) {
                return;
            }
            proto->vlan = (struct vlan_header *)((uint8_t *)
                                                pkt->buffer->data + offset);
            proto->vlan_last = proto->vlan;
            offset += sizeof(struct vlan_header);
            vlan_id  = (ntohs(proto->vlan->vlan_tci) &
                                            VLAN_VID_MASK) >> VLAN_VID_SHIFT;
            vlan_pcp = (ntohs(proto->vlan->vlan_tci) &
                                            VLAN_PCP_MASK) >> VLAN_PCP_SHIFT;
            ofl_structs_match_put16(m, OXM_OF_VLAN_VID, vlan_id);
            ofl_structs_match_put8(m, OXM_OF_VLAN_PCP, vlan_pcp);

            // Note: DL type is updated
            ofl_structs_match_put16(m, OXM_OF_ETH_TYPE,
                                           proto->vlan->vlan_next_type);

        }

        /* skip through rest of VLAN tags */
        while (ntohs(proto->eth->eth_type) == ETH_TYPE_VLAN ||
               ntohs(proto->eth->eth_type) == ETH_TYPE_VLAN_PBB) {

            if (pkt->buffer->size < offset + sizeof(struct vlan_header)) {
                return;
            }
            proto->vlan_last = (struct vlan_header *)((uint8_t *)pkt->buffer->data + offset);
            offset += sizeof(struct vlan_header);

            ofl_structs_match_put16(m, OXM_OF_ETH_TYPE,
                                           proto->vlan->vlan_next_type);
        }

        /* MPLS */

        if (ntohs(proto->eth->eth_type) == ETH_TYPE_MPLS ||
            ntohs(proto->eth->eth_type) == ETH_TYPE_MPLS_MCAST) {
            uint32_t mpls_label;
            uint32_t mpls_tc;
            uint32_t mpls_bos;
            if (pkt->buffer->size < offset + sizeof(struct mpls_header)) {
                return;
            }
            proto->mpls = (struct mpls_header *)((uint8_t *)
                                    pkt->buffer->data + offset);
            offset += sizeof(struct mpls_header);
            mpls_label = (ntohl(proto->mpls->fields) &
                                          MPLS_LABEL_MASK) >> MPLS_LABEL_SHIFT;
            mpls_tc =    (ntohl(proto->mpls->fields) &
                                                MPLS_TC_MASK) >> MPLS_TC_SHIFT;
            mpls_bos =  (ntohl(proto->mpls->fields) &
                                            MPLS_S_MASK) >> MPLS_S_SHIFT;
            ofl_structs_match_put32(m, OXM_OF_MPLS_LABEL, mpls_label);
            ofl_structs_match_put8(m, OXM_OF_MPLS_TC, mpls_tc);
            ofl_structs_match_put8(m, OXM_OF_MPLS_BOS, mpls_bos);

            /* no processing past MPLS */
            return;
        }

        /* ARP */
        if (ntohs(proto->eth->eth_type) == ETH_TYPE_ARP) {
            if (pkt->buffer->size < offset + sizeof(struct arp_eth_header)) {
                return;
            }
            proto->arp = (struct arp_eth_header *)((uint8_t *)
                                pkt->buffer->data + offset);
            offset += sizeof(struct arp_eth_header);

            if (ntohs(proto->arp->ar_hrd) == 1 &&
                ntohs(proto->arp->ar_pro) == ETH_TYPE_IP &&
                proto->arp->ar_hln == ETH_ADDR_LEN &&
                proto->arp->ar_pln == 4) {

                if (ntohs(proto->arp->ar_op) <= 0xff) {
                    ofl_structs_match_put16(m, OXM_OF_ARP_OP,
                                                proto->arp->ar_op);
                }
                if (ntohs(proto->arp->ar_op) == ARP_OP_REQUEST ||
                    ntohs(proto->arp->ar_op) == ARP_OP_REPLY) {
                    ofl_structs_match_put_eth(m, OXM_OF_ARP_SHA,
                                                proto->arp->ar_sha);
                    ofl_structs_match_put_eth(m,OXM_OF_ARP_THA,
                                                proto->arp->ar_tha);
                    ofl_structs_match_put32(m, OXM_OF_ARP_SPA,
                                                proto->arp->ar_spa);
                    ofl_structs_match_put32(m, OXM_OF_ARP_TPA,
                                                proto->arp->ar_tpa);
                }
            }

            return;
        }

        /* Network Layer */
        else if (ntohs(proto->eth->eth_type) == ETH_TYPE_IP) {
            if (pkt->buffer->size < offset + sizeof(struct ip_header)) {
                return;
            }

            proto->ipv4 = (struct ip_header *)((uint8_t *)
                                                   pkt->buffer->data + offset);
            offset += sizeof(struct ip_header);

            ofl_structs_match_put32(m, OXM_OF_IPV4_SRC, proto->ipv4->ip_src);
            ofl_structs_match_put32(m, OXM_OF_IPV4_DST, proto->ipv4->ip_dst);
            ofl_structs_match_put8(m, OXM_OF_IP_PROTO, proto->ipv4->ip_proto);
            ofl_structs_match_put8(m, OXM_OF_IP_ECN, proto->ipv4->ip_tos
                                    & IP_ECN_MASK);
            ofl_structs_match_put8(m, OXM_OF_IP_DSCP,
                                    (proto->ipv4->ip_tos >> 2));

            if (IP_IS_FRAGMENT(proto->ipv4->ip_frag_off)) {
                /* No further processing for fragmented IPv4 */
                return;
            }

            /* Transport */
            if (proto->ipv4->ip_proto == IP_TYPE_TCP) {
                if (pkt->buffer->size < offset + sizeof(struct tcp_header)) {
                    return;
                }
                proto->tcp = (struct tcp_header *)((uint8_t *)
                                                pkt->buffer->data + offset);
                offset += sizeof(struct tcp_header);

                ofl_structs_match_put16(m, OXM_OF_TCP_SRC, proto->tcp->tcp_src);
                ofl_structs_match_put16(m, OXM_OF_TCP_DST, proto->tcp->tcp_dst);

                return;
            }

            else if (proto->ipv4->ip_proto == IP_TYPE_UDP) {
                if (pkt->buffer->size < offset + sizeof(struct udp_header)) {
                    return;
                }
                proto->udp = (struct udp_header *)((uint8_t *)pkt->buffer->data + offset);
                offset += sizeof(struct udp_header);

                ofl_structs_match_put16(m, OXM_OF_UDP_SRC, proto->udp->udp_src);
                ofl_structs_match_put16(m, OXM_OF_UDP_DST, proto->udp->udp_dst);

                return;

            } else if (proto->ipv4->ip_proto == IP_TYPE_ICMP) {
                if (pkt->buffer->size < offset + sizeof(struct icmp_header)) {
                    return;
                }
                proto->icmp = (struct icmp_header *)((uint8_t *)pkt->buffer->data + offset);
                offset += sizeof(struct icmp_header);

                ofl_structs_match_put8(m, OXM_OF_ICMPV4_TYPE, proto->icmp->icmp_type);
                ofl_structs_match_put8(m, OXM_OF_ICMPV4_CODE, proto->icmp->icmp_code);

                return;

            } else if (proto->ipv4->ip_proto == IP_TYPE_SCTP) {
                if (pkt->buffer->size < offset + sizeof(struct sctp_header)) {
                    return;
                }
                proto->sctp = (struct sctp_header *)((uint8_t *)pkt->buffer->data + offset);
                offset += sizeof(struct sctp_header);

                ofl_structs_match_put16(m, OXM_OF_SCTP_SRC, proto->sctp->sctp_src);
                ofl_structs_match_put16(m, OXM_OF_SCTP_SRC, proto->sctp->sctp_dst);

                return;
            }
        }
    }
}


struct packet_handle_std *
packet_handle_std_create(struct packet *pkt) {
	struct packet_handle_std *handle = xmalloc(sizeof(struct packet_handle_std));
	handle->proto = xmalloc(sizeof(struct protocols_std));
	handle->pkt = pkt;

	hmap_init(&handle->match.match_fields);

	handle->valid = false;
	packet_handle_std_validate(handle);

	return handle;
}

struct packet_handle_std *
packet_handle_std_clone(struct packet *pkt, struct packet_handle_std *handle UNUSED) {
    struct packet_handle_std *clone = xmalloc(sizeof(struct packet_handle_std));

    clone->pkt = pkt;
    clone->proto = xmalloc(sizeof(struct protocols_std));
    hmap_init(&clone->match.match_fields);
    clone->valid = false;
    // TODO Zoltan: if handle->valid, then match could be memcpy'd, and protocol
    //              could be offset
    packet_handle_std_validate(clone);

    return clone;
}

void
packet_handle_std_destroy(struct packet_handle_std *handle) {

    struct ofl_match_tlv * iter, *next;
    HMAP_FOR_EACH_SAFE(iter, next, struct ofl_match_tlv, hmap_node,
                        &handle->match.match_fields){
        free(iter->value);
        free(iter);
    }
    free(handle->proto);
    hmap_destroy(&handle->match.match_fields);
    free(handle);
}

bool
packet_handle_std_is_ttl_valid(struct packet_handle_std *handle) {
    packet_handle_std_validate(handle);

    /*if (handle->proto->mpls != NULL) {
        uint32_t ttl = ntohl(handle->proto->mpls->fields) & MPLS_TTL_MASK;
        if (ttl <= 1) {
            return false;
        }
    }
    if (handle->proto->ipv4 != NULL) {
        if (handle->proto->ipv4->ip_ttl < 1) {
            return false;
        }
    }*/
    return true;
}

bool
packet_handle_std_is_fragment(struct packet_handle_std *handle) {
    packet_handle_std_validate(handle);

    return false;
    /*return ((handle->proto->ipv4 != NULL) &&
            IP_IS_FRAGMENT(handle->proto->ipv4->ip_frag_off));*/
}


bool
packet_handle_std_match(struct packet_handle_std *handle, struct ofl_match *match){

    if (!handle->valid){
        packet_handle_std_validate(handle);
        if (!handle->valid){
            return false;
        }
    }

    return packet_match(match ,&handle->match );
}


/* TODO Denicol: From this point on, work to be done */

/* If pointer is not null, returns str; otherwise returns an empty string.
static inline const char *
pstr(void *ptr, const char *str) {
    return (ptr == NULL) ? "" : str;
}

 Prints the names of protocols that are available in the given protocol stack.

static void
proto_print(FILE *stream, struct protocols_std *p) {
    fprintf(stream, "{%s%s%s%s%s%s%s%s%s}",
            pstr(p->eth, "eth"), pstr(p->vlan, ",vlan"), pstr(p->mpls, ",mpls"), pstr(p->ipv4, ",ipv4"),
            pstr(p->arp, ",arp"), pstr(p->tcp, ",tcp"), pstr(p->udp, ",udp"), pstr(p->sctp, ",sctp"),
            pstr(p->icmp, ",icmp"));
}

char *
packet_handle_std_to_string(struct packet_handle_std *handle) {
    char *str;
    size_t str_size;
    FILE *stream = open_memstream(&str, &str_size);

    packet_handle_std_print(stream, handle);

    fclose(stream);
    return str;
}

void
packet_handle_std_print(FILE *stream, struct packet_handle_std *handle) {
    packet_handle_std_validate(handle);

    fprintf(stream, "{proto=");
    proto_print(stream, handle->proto);

    fprintf(stream, ", match=");
    ofl_structs_match_print(stream, (struct ofl_match_header *)(handle->match), handle->pkt->dp->exp);
    fprintf(stream, "\"}");
}
*/

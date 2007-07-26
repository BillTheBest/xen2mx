/*
 * Open-MX
 * Copyright © INRIA 2007 (see AUTHORS file)
 *
 * The development of this software has been funded by Myricom, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License in COPYING.GPL for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>

#include "omx_common.h"
#include "omx_hal.h"

/******************************
 * Manage event and data slots
 */

union omx_evt *
omx_find_next_eventq_slot(struct omx_endpoint *endpoint)
{
	/* FIXME: need locking */
	union omx_evt *slot = endpoint->next_eventq_slot;
	if (unlikely(slot->generic.type != OMX_EVT_NONE)) {
		dprintk("Open-MX: Event queue full, no event slot available for endpoint %d\n",
			endpoint->endpoint_index);
		return NULL;
	}

	endpoint->next_eventq_slot = slot + 1;
	if (unlikely((void *) endpoint->next_eventq_slot >= endpoint->eventq + OMX_EVENTQ_SIZE))
		endpoint->next_eventq_slot = endpoint->eventq;

	/* recvq slot is at same index for now */
	endpoint->next_recvq_slot = endpoint->recvq + ((void *) slot - endpoint->eventq)/sizeof(union omx_evt)*PAGE_SIZE;

	return slot;
}

static inline char *
omx_find_next_recvq_slot(struct omx_endpoint *endpoint)
{
	return endpoint->next_recvq_slot;
}

/***************************
 * Event reporting routines
 */

static int
omx_recv_tiny(struct omx_iface * iface,
	      struct omx_hdr * mh,
	      struct sk_buff * skb)
{
	struct omx_endpoint * endpoint;
	struct ethhdr *eh = &mh->head.eth;
	struct omx_pkt_msg *tiny = &mh->body.tiny;
	uint16_t length = tiny->length;
	union omx_evt *evt;
	struct omx_evt_recv_tiny *event;
	int err = 0;

	/* check packet length */
	if (unlikely(length > OMX_TINY_MAX)) {
		omx_drop_dprintk(eh, "TINY packet too long (length %d)",
				 (unsigned) length);
		err = -EINVAL;
		goto out;
	}

	/* check actual data length */
	if (unlikely(length > skb->len - sizeof(struct omx_hdr))) {
		omx_drop_dprintk(eh, "TINY packet with %ld bytes instead of %d",
				 (unsigned long) skb->len - sizeof(struct omx_hdr),
				 (unsigned) length);
		err = -EINVAL;
		goto out;
	}

	/* get the destination endpoint */
	endpoint = omx_endpoint_acquire_by_iface_index(iface, tiny->dst_endpoint);
	if (unlikely(!endpoint)) {
		omx_drop_dprintk(eh, "TINY packet for unknown endpoint %d",
				 tiny->dst_endpoint);
		err = -EINVAL;
		goto out;
	}

	/* get the eventq slot */
	evt = omx_find_next_eventq_slot(endpoint);
	if (unlikely(!evt)) {
		omx_drop_dprintk(eh, "TINY packet because of event queue full");
		err = -EBUSY;
		goto out_with_endpoint;
	}
	event = &evt->recv_tiny;

	/* fill event */
	event->src_addr = omx_board_addr_from_ethhdr_src(eh);
	event->src_endpoint = tiny->src_endpoint;
	event->length = length;
	event->match_info = OMX_MATCH_INFO_FROM_PKT(tiny);
	event->seqnum = tiny->lib_seqnum;

	omx_recv_dprintk(eh, "TINY length %ld", (unsigned long) length);

	/* copy data in event data */
	err = skb_copy_bits(skb, sizeof(struct omx_hdr), event->data, length);
	/* cannot fail since pages are allocated by us */
	BUG_ON(err < 0);

	/* set the type at the end so that user-space does not find the slot on error */
	event->type = OMX_EVT_RECV_TINY;

	omx_endpoint_release(endpoint);

	return 0;

 out_with_endpoint:
	omx_endpoint_release(endpoint);
 out:
	return err;
}

static int
omx_recv_small(struct omx_iface * iface,
	       struct omx_hdr * mh,
	       struct sk_buff * skb)
{
	struct omx_endpoint * endpoint;
	struct ethhdr *eh = &mh->head.eth;
	struct omx_pkt_msg *small = &mh->body.small;
	uint16_t length = small->length;
	union omx_evt *evt;
	struct omx_evt_recv_small *event;
	char *recvq_slot;
	int err;

	/* check packet length */
	if (unlikely(length > OMX_SMALL_MAX)) {
		omx_drop_dprintk(eh, "SMALL packet too long (length %d)",
				 (unsigned) length);
		err = -EINVAL;
		goto out;
	}

	/* check actual data length */
	if (unlikely(length > skb->len - sizeof(struct omx_hdr))) {
		omx_drop_dprintk(eh, "SMALL packet with %ld bytes instead of %d",
				 (unsigned long) skb->len - sizeof(struct omx_hdr),
				 (unsigned) length);
		err = -EINVAL;
		goto out;
	}

	/* get the destination endpoint */
	endpoint = omx_endpoint_acquire_by_iface_index(iface, small->dst_endpoint);
	if (unlikely(!endpoint)) {
		omx_drop_dprintk(eh, "SMALL packet for unknown endpoint %d",
				 small->dst_endpoint);
		err = -EINVAL;
		goto out;
	}

	/* get the eventq slot */
	evt = omx_find_next_eventq_slot(endpoint);
	if (unlikely(!evt)) {
		omx_drop_dprintk(eh, "SMALL packet because of event queue full");
		err = -EBUSY;
		goto out_with_endpoint;
	}
	event = &evt->recv_small;

	/* fill event */
	event->src_addr = omx_board_addr_from_ethhdr_src(eh);
	event->src_endpoint = small->src_endpoint;
	event->length = length;
	event->match_info = OMX_MATCH_INFO_FROM_PKT(small);
	event->seqnum = small->lib_seqnum;

	omx_recv_dprintk(eh, "SMALL length %ld", (unsigned long) length);

	/* copy data in recvq slot */
	recvq_slot = omx_find_next_recvq_slot(endpoint);
	err = skb_copy_bits(skb, sizeof(struct omx_hdr), recvq_slot, length);
	/* cannot fail since pages are allocated by us */
	BUG_ON(err < 0);

	/* set the type at the end so that user-space does not find the slot on error */
	event->type = OMX_EVT_RECV_SMALL;

	omx_endpoint_release(endpoint);

	return 0;

 out_with_endpoint:
	omx_endpoint_release(endpoint);
 out:
	return err;
}

static int
omx_recv_medium_frag(struct omx_iface * iface,
		     struct omx_hdr * mh,
		     struct sk_buff * skb)
{
	struct omx_endpoint * endpoint;
	struct ethhdr *eh = &mh->head.eth;
	struct omx_pkt_medium_frag *medium = &mh->body.medium;
	uint16_t frag_length = medium->frag_length;
	union omx_evt *evt;
	struct omx_evt_recv_medium *event;
	char *recvq_slot;
	int err;

	/* check packet length */
	if (unlikely(frag_length > OMX_RECVQ_ENTRY_SIZE)) {
		omx_drop_dprintk(eh, "MEDIUM fragment packet too long (length %d)",
				 (unsigned) frag_length);
		err = -EINVAL;
		goto out;
	}

	/* check actual data length */
	if (unlikely(frag_length > skb->len - sizeof(struct omx_hdr))) {
		omx_drop_dprintk(eh, "MEDIUM fragment with %ld bytes instead of %d",
				 (unsigned long) skb->len - sizeof(struct omx_hdr),
				 (unsigned) frag_length);
		err = -EINVAL;
		goto out;
	}

	/* get the destination endpoint */
	endpoint = omx_endpoint_acquire_by_iface_index(iface, medium->msg.dst_endpoint);
	if (unlikely(!endpoint)) {
		omx_drop_dprintk(eh, "MEDIUM packet for unknown endpoint %d",
				 medium->msg.dst_endpoint);
		err = -EINVAL;
		goto out;
	}

	/* get the eventq slot */
	evt = omx_find_next_eventq_slot(endpoint);
	if (unlikely(!evt)) {
		omx_drop_dprintk(eh, "MEDIUM packet because of event queue full");
		err = -EBUSY;
		goto out_with_endpoint;
	}
	event = &evt->recv_medium;

	/* fill event */
	event->src_addr = omx_board_addr_from_ethhdr_src(eh);
	event->src_endpoint = medium->msg.src_endpoint;
	event->match_info = OMX_MATCH_INFO_FROM_PKT(&medium->msg);
	event->msg_length = medium->msg.length;
	event->seqnum = medium->msg.lib_seqnum;
	event->frag_length = frag_length;
	event->frag_seqnum = medium->frag_seqnum;
	event->frag_pipeline = medium->frag_pipeline;

	omx_recv_dprintk(eh, "MEDIUM_FRAG length %ld", (unsigned long) frag_length);

	/* copy data in recvq slot */
	recvq_slot = omx_find_next_recvq_slot(endpoint);
	err = skb_copy_bits(skb, sizeof(struct omx_hdr), recvq_slot, frag_length);
	/* cannot fail since pages are allocated by us */
	BUG_ON(err < 0);

	/* set the type at the end so that user-space does not find the slot on error */
	event->type = OMX_EVT_RECV_MEDIUM;

	omx_endpoint_release(endpoint);

	return 0;

 out_with_endpoint:
	omx_endpoint_release(endpoint);
 out:
	return err;
}

static int
omx_recv_rndv(struct omx_iface * iface,
	      struct omx_hdr * mh,
	      struct sk_buff * skb)
{
#if 0
	struct omx_endpoint * endpoint;
	struct ethhdr *eh = &mh->head.eth;
	struct omx_pkt_rndv *rndv = &mh->body.rndv;

	/* get the destination endpoint */
	endpoint = omx_endpoint_acquire_by_iface_index(iface, rndv->dst_endpoint);
	if (unlikely(!endpoint)) {
		omx_drop_dprintk(eh, "RNDV packet for unknown endpoint %d",
				 rndv->dst_endpoint);
		err = -EINVAL;
		goto out;
	}

	/* get the eventq slot */
	evt = omx_find_next_eventq_slot(endpoint);
	if (unlikely(!evt)) {
		omx_drop_dprintk(eh, "RNDV packet because of event queue full");
		err = -EBUSY;
		goto out_with_endpoint;
	}
	event = &evt->rndv;

	omx_endpoint_release(endpoint);

	return 0;

 out_with_endpoint:
	omx_endpoint_release(endpoint);
 out:
	return err;
#endif

	/* FIXME */

	return 0;
}

static int
omx_recv_nosys(struct omx_iface * iface,
		struct omx_hdr * mh,
		struct sk_buff * skb)
{
	omx_drop_dprintk(&mh->head.eth, "packet with unsupported type %d",
			 mh->body.generic.ptype);

	return 0;
}

static int
omx_recv_error(struct omx_iface * iface,
		struct omx_hdr * mh,
		struct sk_buff * skb)
{
	omx_drop_dprintk(&mh->head.eth, "packet with unrecognized type %d",
			 mh->body.generic.ptype);

	return 0;
}

/***********************
 * Packet type handlers
 */

static int (*omx_pkt_type_handlers[OMX_PKT_TYPE_MAX+1])(struct omx_iface * iface, struct omx_hdr * mh, struct sk_buff * skb);

void
omx_pkt_type_handlers_init(void)
{
	int i;

	for(i=0; i<=OMX_PKT_TYPE_MAX; i++)
		omx_pkt_type_handlers[i] = omx_recv_error;

	omx_pkt_type_handlers[OMX_PKT_TYPE_RAW] = omx_recv_nosys; /* FIXME */
	omx_pkt_type_handlers[OMX_PKT_TYPE_MFM_NIC_REPLY] = omx_recv_nosys; /* FIXME */
	omx_pkt_type_handlers[OMX_PKT_TYPE_HOST_QUERY] = omx_recv_nosys; /* FIXME */
	omx_pkt_type_handlers[OMX_PKT_TYPE_HOST_REPLY] = omx_recv_nosys; /* FIXME */
	omx_pkt_type_handlers[OMX_PKT_TYPE_ETHER_UNICAST] = omx_recv_nosys; /* FIXME */
	omx_pkt_type_handlers[OMX_PKT_TYPE_ETHER_MULTICAST] = omx_recv_nosys; /* FIXME */
	omx_pkt_type_handlers[OMX_PKT_TYPE_ETHER_NATIVE] = omx_recv_nosys; /* FIXME */
	omx_pkt_type_handlers[OMX_PKT_TYPE_TRUC] = omx_recv_nosys; /* FIXME */
	omx_pkt_type_handlers[OMX_PKT_TYPE_CONNECT] = omx_recv_nosys; /* FIXME */
	omx_pkt_type_handlers[OMX_PKT_TYPE_TINY] = omx_recv_tiny;
	omx_pkt_type_handlers[OMX_PKT_TYPE_SMALL] = omx_recv_small;
	omx_pkt_type_handlers[OMX_PKT_TYPE_MEDIUM] = omx_recv_medium_frag;
	omx_pkt_type_handlers[OMX_PKT_TYPE_RENDEZ_VOUS] = omx_recv_rndv;
	omx_pkt_type_handlers[OMX_PKT_TYPE_PULL] = omx_recv_pull;
	omx_pkt_type_handlers[OMX_PKT_TYPE_PULL_REPLY] = omx_recv_pull_reply;
	omx_pkt_type_handlers[OMX_PKT_TYPE_NOTIFY] = omx_recv_nosys; /* FIXME */
	omx_pkt_type_handlers[OMX_PKT_TYPE_NACK_LIB] = omx_recv_nosys; /* FIXME */
	omx_pkt_type_handlers[OMX_PKT_TYPE_NACK_MCP] = omx_recv_nosys; /* FIXME */
}

/***********************
 * Main receive routine
 */

static int
omx_recv(struct sk_buff *skb, struct net_device *ifp, struct packet_type *pt,
	  struct net_device *orig_dev)
{
	struct omx_iface *iface;
	struct omx_hdr linear_header;
	struct omx_hdr *mh;

	skb = skb_share_check(skb, GFP_ATOMIC);
	if (unlikely(skb == NULL))
		return 0;

	/* len doesn't include header */
	skb_push(skb, ETH_HLEN);

	iface = omx_iface_find_by_ifp(ifp);
	if (unlikely(!iface)) {
		/* at least the ethhdr is linear in the skb */
		omx_drop_dprintk(&omx_hdr(skb)->head.eth, "packet on non-Open-MX interface %s",
				 ifp->name);
		goto out;
	}

	/* no need to linearize the whole skb,
	 * but at least the header to make things simple */
	if (unlikely(skb_headlen(skb) < sizeof(struct omx_hdr))) {
		skb_copy_bits(skb, 0, &linear_header,
			      sizeof(struct omx_hdr));
		/* check for EFAULT */
		mh = &linear_header;
	} else {
		/*�no need to linearize the header */
		mh = omx_hdr(skb);
	}

	/* no need to check ptype since there is a default error handler
	 * for all erroneous values
	 */
	omx_pkt_type_handlers[mh->body.generic.ptype](iface, mh, skb);

 out:
	/* FIXME: send nack */
	dev_kfree_skb(skb);
	return 0;
}

struct packet_type omx_pt = {
	.type = __constant_htons(ETH_P_OMX),
	.func = omx_recv,
};

/*
 * Local variables:
 *  tab-width: 8
 *  c-basic-offset: 8
 *  c-indent-level: 8
 * End:
 */

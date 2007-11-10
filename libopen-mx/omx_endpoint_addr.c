/*
 * Open-MX
 * Copyright © INRIA 2007 (see AUTHORS file)
 *
 * The development of this software has been funded by Myricom, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU Lesser General Public License in COPYING.LGPL for more details.
 */

#include <stdlib.h>
#include <sys/ioctl.h>

#include "omx_lib.h"
#include "omx_request.h"
#include "omx_wire_access.h"
#include "omx_lib_wire.h"

/******************************
 * Endpoint address management
 */

omx_return_t
omx_get_endpoint_addr(omx_endpoint_t endpoint,
		      omx_endpoint_addr_t *endpoint_addr)
{
  omx__partner_to_addr(endpoint->myself, endpoint_addr);
  return OMX_SUCCESS;
}

omx_return_t
omx_decompose_endpoint_addr(omx_endpoint_addr_t endpoint_addr,
			    uint64_t *nic_id, uint32_t *endpoint_id)
{
  struct omx__partner *partner = omx__partner_from_addr(&endpoint_addr);
  *nic_id = partner->board_addr;
  *endpoint_id = partner->endpoint_index;
  return OMX_SUCCESS;
}

/*********************
 * Partner management
 */

omx_return_t
omx__partner_create(struct omx_endpoint *ep, uint16_t peer_index,
		    uint64_t board_addr, uint8_t endpoint_index,
		    struct omx__partner ** partnerp)
{
  struct omx__partner * partner;
  uint32_t partner_index;

  partner = malloc(sizeof(*partner));
  if (unlikely(!partner))
    return OMX_NO_RESOURCES;

  partner->board_addr = board_addr;
  partner->endpoint_index = endpoint_index;
  partner->peer_index = peer_index;
  partner->connect_seqnum = 0;
  INIT_LIST_HEAD(&partner->non_acked_req_q);
  INIT_LIST_HEAD(&partner->pending_connect_req_q);
  INIT_LIST_HEAD(&partner->partial_recv_req_q);
  INIT_LIST_HEAD(&partner->early_recv_q);
  partner->session_id = 0; /* will be initialized when the partner will connect to me */
  partner->next_send_seq = -1; /* will be initialized when the partner will reply to my connect */
  partner->last_acked_send_seq = -1;
  partner->next_match_recv_seq = 0;
  partner->next_frag_recv_seq = 0;

  partner->oldest_recv_time_not_acked = 0;

  partner_index = ((uint32_t) endpoint_index)
    + ((uint32_t) peer_index) * omx__driver_desc->endpoint_max;
  ep->partners[partner_index] = partner;

  *partnerp = partner;
  omx__debug_printf("created peer %d %d\n", peer_index, endpoint_index);

  return OMX_SUCCESS;
}

omx_return_t
omx__partner_lookup(struct omx_endpoint *ep,
		    uint16_t peer_index, uint8_t endpoint_index,
		    struct omx__partner ** partnerp)
{
  uint32_t partner_index;

  partner_index = ((uint32_t) endpoint_index)
    + ((uint32_t) peer_index) * omx__driver_desc->endpoint_max;

  if (unlikely(!ep->partners[partner_index])) {
    uint64_t board_addr;
    omx_return_t ret;

    ret = omx__peer_index_to_addr(peer_index, &board_addr);
    if (ret != OMX_SUCCESS) {
      fprintf(stderr, "Failed to find peer address of index %d (%s)\n",
	      (unsigned) peer_index, omx_strerror(ret));
      return ret;
    }

    return omx__partner_create(ep, peer_index, board_addr, endpoint_index, partnerp);
  }

  *partnerp = ep->partners[partner_index];
  return OMX_SUCCESS;
}

omx_return_t
omx__partner_lookup_by_addr(struct omx_endpoint *ep,
			    uint64_t board_addr, uint8_t endpoint_index,
			    struct omx__partner ** partnerp)
{
  uint32_t partner_index;
  uint16_t peer_index;
  omx_return_t ret;

  ret = omx__peer_addr_to_index(board_addr, &peer_index);
  if (unlikely(ret != OMX_SUCCESS)) {
    char board_addr_str[OMX_BOARD_ADDR_STRLEN];
    omx__board_addr_sprintf(board_addr_str, board_addr);
    fprintf(stderr, "Failed to find peer index of board %s (%s)\n",
	    board_addr_str, omx_strerror(ret));
    return ret;
  }

  partner_index = ((uint32_t) endpoint_index)
    + ((uint32_t) peer_index) * omx__driver_desc->endpoint_max;

  if (unlikely(!ep->partners[partner_index]))
    return omx__partner_create(ep, peer_index, board_addr, endpoint_index, partnerp);

  *partnerp = ep->partners[partner_index];
  return OMX_SUCCESS;
}

omx_return_t
omx__partner_recv_lookup(struct omx_endpoint *ep,
			 uint16_t peer_index, uint8_t endpoint_index,
			 struct omx__partner ** partnerp)
{
  uint32_t partner_index;
  struct omx__partner * partner;

  partner_index = ((uint32_t) endpoint_index)
    + ((uint32_t) peer_index) * omx__driver_desc->endpoint_max;
  partner = ep->partners[partner_index];
  omx__debug_assert(partner);

  *partnerp = partner;
  return OMX_SUCCESS;
}

/*
 * Actually initialize connected partner
 */
static INLINE void
omx__connect_partner(struct omx__partner * partner,
		     uint32_t target_session_id,
		     omx__seqnum_t target_recv_seqnum_start)
{
  if (partner->session_id != target_session_id) {
    /* this is the first connect, only update seqnums here */
    partner->next_send_seq = target_recv_seqnum_start;
  }

  partner->session_id = target_session_id;
}

omx_return_t
omx__connect_myself(struct omx_endpoint *ep, uint64_t board_addr)
{
  uint16_t peer_index;
  omx_return_t ret;

  ret = omx__peer_addr_to_index(board_addr, &peer_index);
  if (ret != OMX_SUCCESS) {
    char board_addr_str[OMX_BOARD_ADDR_STRLEN];
    omx__board_addr_sprintf(board_addr_str, board_addr);
    fprintf(stderr, "Failed to find peer index of local board %s (%s)\n",
	    board_addr_str, omx_strerror(ret));
    return ret;
  }

  ret = omx__partner_create(ep, peer_index,
			    board_addr, ep->endpoint_index,
			    &ep->myself);
  if (ret != OMX_SUCCESS)
    return ret;

  omx__connect_partner(ep->myself, ep->desc->session_id, 0);

  return OMX_SUCCESS;
}

/*************
 * Connection
 */

static INLINE void
omx__post_connect(struct omx_endpoint *ep,
		  struct omx__partner *partner,
		  union omx_request * req)
{
  struct omx_cmd_send_connect * connect_param = &req->connect.send_connect_ioctl_param;
  int err;

  err = ioctl(ep->fd, OMX_CMD_SEND_CONNECT, connect_param);
  if (err < 0) {
    omx_return_t ret = omx__errno_to_return("ioctl SEND_CONNECT");

    if (ret != OMX_NO_SYSTEM_RESOURCES)
      omx__abort("ioctl SEND_CONNECT returned unexpected error %m\n");

    /* if OMX_NO_SYSTEM_RESOURCES, let the retransmission try again later */
  }

  req->generic.last_send_jiffies = omx__driver_desc->jiffies;
}

/*
 * Start the connection process to another peer
 */
omx_return_t
omx__connect_common(omx_endpoint_t ep,
		    uint64_t nic_id, uint32_t endpoint_id, uint32_t key,
		    union omx_request * req)
{
  struct omx__partner * partner;
  struct omx_cmd_send_connect * connect_param = &req->connect.send_connect_ioctl_param;
  struct omx__connect_request_data * data_n = (void *) &connect_param->data;
  uint8_t connect_seqnum;
  omx_return_t ret;

  ret = omx__partner_lookup_by_addr(ep, nic_id, endpoint_id, &partner);
  if (ret != OMX_SUCCESS)
    goto out;

  connect_seqnum = partner->connect_seqnum++;

  connect_param->hdr.peer_index = partner->peer_index;
  connect_param->hdr.dest_endpoint = partner->endpoint_index;
  connect_param->hdr.seqnum = 0;
  connect_param->hdr.length = sizeof(*data_n);
  OMX_PKT_FIELD_FROM(data_n->src_session_id, ep->desc->session_id);
  OMX_PKT_FIELD_FROM(data_n->app_key, key);
  OMX_PKT_FIELD_FROM(data_n->connect_seqnum, connect_seqnum);
  OMX_PKT_FIELD_FROM(data_n->is_reply, 0);

  omx__post_connect(ep, partner, req);

  /* no need to wait for a done event, tiny is synchronous */
  req->generic.state = OMX_REQUEST_STATE_NEED_REPLY;
  omx__enqueue_request(&ep->connect_req_q, req);
  omx__enqueue_partner_connect_request(partner, req);

  req->generic.partner = partner;
  req->connect.session_id = ep->desc->session_id;
  req->connect.connect_seqnum = connect_seqnum;

  omx__progress(ep);

  return OMX_SUCCESS;

 out:
  return ret;
}

omx_return_t
omx_connect(omx_endpoint_t ep,
	    uint64_t nic_id, uint32_t endpoint_id, uint32_t key,
	    uint32_t timeout,
	    omx_endpoint_addr_t *addr)
{
  union omx_request * req;
  omx_return_t ret;

  req = omx__request_alloc(OMX_REQUEST_TYPE_CONNECT);
  if (!req) {
    ret = OMX_NO_RESOURCES;
    goto out;
  }

  req->connect.is_synchronous = 1;

  ret = omx__connect_common(ep, nic_id, endpoint_id, key, req);
  if (ret != OMX_SUCCESS)
    goto out_with_req;

  omx__debug_printf("waiting for connect reply\n");
  while (req->generic.state) {
    ret = omx__progress(ep);
    if (ret != OMX_SUCCESS)
      goto out; /* request is queued, do not try to free it */
  }
  omx__debug_printf("connect done\n");

  switch (req->generic.status.code) {
  case OMX_STATUS_SUCCESS:
    omx__partner_to_addr(req->generic.partner, addr);
    ret = OMX_SUCCESS;
    break;
  case OMX_STATUS_BAD_KEY:
    ret = OMX_BAD_CONNECTION_KEY;
    break;
  case OMX_STATUS_ENDPOINT_CLOSED:
  case OMX_STATUS_BAD_ENDPOINT:
    ret = OMX_CONNECTION_FAILED;
    break;
  default:
    omx__abort("Failed to handle connect status %s\n",
	       omx_strstatus(req->generic.status.code));
  }

  omx__request_free(req);
  return ret;

 out_with_req:
  omx__request_free(req);
 out:
  return ret;
}

omx_return_t
omx_iconnect(omx_endpoint_t ep,
	     uint64_t nic_id, uint32_t endpoint_id, uint32_t key,
	     uint64_t match_info,
	     void *context, omx_request_t *requestp)
{
  union omx_request * req;
  omx_return_t ret;

  req = omx__request_alloc(OMX_REQUEST_TYPE_CONNECT);
  if (!req) {
    ret = OMX_NO_RESOURCES;
    goto out;
  }

  req->connect.is_synchronous = 0;
  req->generic.status.match_info = match_info;
  req->generic.status.context = context;

  ret = omx__connect_common(ep, nic_id, endpoint_id, key, req);
  if (ret != OMX_SUCCESS)
    goto out_with_req;

  *requestp = req;
  return ret;

 out_with_req:
  omx__request_free(req);
 out:
  return ret;
}

/*
 * Complete the connect request
 */
void
omx__connect_complete(struct omx_endpoint *ep,
		      union omx_request * req, omx_status_code_t status)
{
  struct omx__partner *partner = req->generic.partner;

  omx__dequeue_request(&ep->connect_req_q, req);
  omx__dequeue_partner_connect_request(partner, req);
  req->generic.state &= ~OMX_REQUEST_STATE_NEED_REPLY;

  if (likely(req->generic.status.code == OMX_STATUS_SUCCESS))
    /* only set the status if it is not already set to an error */
    req->generic.status.code = status;

  if (status == OMX_STATUS_SUCCESS)
    omx__partner_to_addr(partner, &req->generic.status.addr);

  /* move iconnect request to the done queue */
  if (!req->connect.is_synchronous) {
    uint32_t ctxid = CTXID_FROM_MATCHING(ep, req->generic.status.match_info);
    omx__enqueue_request(&ep->ctxid[ctxid].done_req_q, req);
  }
}

/*
 * End the connection process to another peer
 */
static INLINE omx_return_t
omx__process_recv_connect_reply(struct omx_endpoint *ep,
				struct omx_evt_recv_connect *event)
{
  struct omx__partner * partner;
  struct omx__connect_reply_data * reply_data_n = (void *) event->data;
  uint32_t src_session_id = OMX_FROM_PKT_FIELD(reply_data_n->src_session_id);
  uint8_t connect_seqnum = OMX_FROM_PKT_FIELD(reply_data_n->connect_seqnum);
  uint32_t target_session_id = OMX_FROM_PKT_FIELD(reply_data_n->target_session_id);
  uint16_t target_recv_seqnum_start = OMX_FROM_PKT_FIELD(reply_data_n->target_recv_seqnum_start);
  uint8_t status_code = OMX_FROM_PKT_FIELD(reply_data_n->status_code);
  union omx_request * req;
  omx_return_t ret;

  ret = omx__partner_lookup(ep, event->peer_index, event->src_endpoint, &partner);
  if (ret != OMX_SUCCESS) {
    if (ret == OMX_INVALID_PARAMETER)
      fprintf(stderr, "Open-MX: Received connect from unknown peer\n");
    return ret;
  }

  omx__foreach_request(&ep->connect_req_q, req) {
    /* check the endpoint session (so that the endpoint didn't close/reopen in the meantime)
     * and the partner and the connection seqnum given by this partner
     */
    if (src_session_id == ep->desc->session_id
	&& partner == req->generic.partner
	&& connect_seqnum == req->connect.connect_seqnum) {
      goto found;
    }
  }

  /* invalid connect reply, just ignore it */
  return OMX_SUCCESS;

 found:
  omx__debug_printf("waking up on connect reply\n");

  if (status_code == OMX_STATUS_SUCCESS) {
    /* connection successfull, initialize stuff */
    omx__connect_partner(partner,
			 target_session_id,
			 target_recv_seqnum_start);
  }

  /* complete the request */
  omx__connect_complete(ep, req, status_code);
  return OMX_SUCCESS;
}

/*
 * Another peer is connecting to us
 */
static INLINE omx_return_t
omx__process_recv_connect_request(struct omx_endpoint *ep,
				  struct omx_evt_recv_connect *event)
{
  struct omx__partner * partner;
  struct omx_cmd_send_connect reply_param;
  struct omx__connect_request_data * request_data_n = (void *) event->data;
  struct omx__connect_reply_data * reply_data_n = (void *) reply_param.data;
  uint32_t app_key = OMX_FROM_PKT_FIELD(request_data_n->app_key);
  uint32_t src_session_id = OMX_FROM_PKT_FIELD(request_data_n->src_session_id);
  omx_return_t ret;
  omx_status_code_t status_code;
  int err;

  ret = omx__partner_lookup(ep, event->peer_index, event->src_endpoint, &partner);
  if (ret != OMX_SUCCESS) {
    if (ret == OMX_INVALID_PARAMETER)
      fprintf(stderr, "Open-MX: Received connect from unknown peer\n");
    return ret;
  }

  if (app_key == ep->app_key) {
    /* FIXME: do bidirectionnal connection stuff? */

    status_code = OMX_STATUS_SUCCESS;
  } else {
    status_code = OMX_STATUS_BAD_KEY;
  }

  omx__debug_printf("got a connect, replying\n");

  if (partner->session_id != -1
      && partner->session_id != src_session_id) {
    /* new instance of the partner */

    omx__debug_printf("connect from a new instance of a partner\n");

    partner->next_match_recv_seq = 0;
    partner->next_frag_recv_seq = 0;
    /* FIXME: drop other stuff */
  }

  reply_param.hdr.peer_index = partner->peer_index;
  reply_param.hdr.dest_endpoint = partner->endpoint_index;
  reply_param.hdr.seqnum = 0;
  reply_param.hdr.length = sizeof(*reply_data_n);
  OMX_PKT_FIELD_FROM(reply_data_n->is_reply, 1);
  OMX_PKT_FIELD_FROM(reply_data_n->target_session_id, ep->desc->session_id);
  OMX_PKT_FIELD_FROM(reply_data_n->src_session_id, OMX_FROM_PKT_FIELD(request_data_n->src_session_id));
  OMX_PKT_FIELD_FROM(reply_data_n->connect_seqnum, OMX_FROM_PKT_FIELD(request_data_n->connect_seqnum));
  OMX_PKT_FIELD_FROM(reply_data_n->status_code, status_code);
  OMX_PKT_FIELD_FROM(reply_data_n->target_recv_seqnum_start, partner->next_match_recv_seq);

  err = ioctl(ep->fd, OMX_CMD_SEND_CONNECT, &reply_param);
  if (err < 0) {
    ret = omx__errno_to_return("ioctl SEND_CONNECT reply");
    goto out_with_req;
  }
  /* no need to wait for a done event, connect is synchronous */

  return OMX_SUCCESS;

 out_with_req:
  return ret;
}

/*
 * Incoming connection message
 */
omx_return_t
omx__process_recv_connect(struct omx_endpoint *ep,
			  struct omx_evt_recv_connect *event)
{
  struct omx__connect_request_data * data = (void *) event->data;
  if (data->is_reply)
    return omx__process_recv_connect_reply(ep, event);
  else
    return omx__process_recv_connect_request(ep, event);
}


/**************************
 * Resend connect requests
 */

void
omx__process_connect_requests(struct omx_endpoint *ep)
{
  union omx_request *req, *next;
  uint64_t now = omx__driver_desc->jiffies;

  omx__foreach_request_safe(&ep->connect_req_q, req, next) {
    if (now - req->generic.last_send_jiffies < omx__globals.resend_delay)
      /* the remaining ones are more recent, no need to resend them yet */
      break;

    omx__dequeue_request(&ep->connect_req_q, req);
    omx__post_connect(ep, req->generic.partner, req);
    omx__enqueue_request(&ep->connect_req_q, req);
  }
}

/***************************
 * Endpoint address context
 */

omx_return_t
omx_set_endpoint_addr_context(omx_endpoint_addr_t endpoint_addr,
			      void *context)
{
  struct omx__partner *partner = omx__partner_from_addr(&endpoint_addr);
  partner->user_context = context;
  return OMX_SUCCESS;
}

omx_return_t
omx_get_endpoint_addr_context(omx_endpoint_addr_t endpoint_addr,
			      void **context)
{
  struct omx__partner *partner = omx__partner_from_addr(&endpoint_addr);
  *context = partner->user_context;
  return OMX_SUCCESS;
}

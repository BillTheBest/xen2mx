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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "omx_io.h"
#include "omx_lib.h"
#include "omx_request.h"
#include "omx_lib_wire.h"
#include "omx_wire_access.h"

/*******************
 * Event processing
 */

static omx_return_t
omx__process_event(struct omx_endpoint * ep, union omx_evt * evt)
{
  omx_return_t ret = OMX_SUCCESS;

  omx__debug_printf("received type %d\n", evt->generic.type);
  switch (evt->generic.type) {

  case OMX_EVT_RECV_CONNECT: {
    ret = omx__process_recv_connect(ep, &evt->recv_connect);
    break;
  }

  case OMX_EVT_RECV_TINY: {
    struct omx_evt_recv_msg * msg = &evt->recv_msg;
    ret = omx__process_recv(ep,
			    msg, msg->specific.tiny.data, msg->specific.tiny.length,
			    omx__process_recv_tiny);
    break;
  }

  case OMX_EVT_RECV_SMALL: {
    struct omx_evt_recv_msg * msg = &evt->recv_msg;
    char * recvq_buffer = ep->recvq + msg->specific.small.recvq_offset;
    ret = omx__process_recv(ep,
			    msg, recvq_buffer, msg->specific.small.length,
			    omx__process_recv_small);
    break;
  }

  case OMX_EVT_RECV_MEDIUM: {
    struct omx_evt_recv_msg * msg = &evt->recv_msg;
    char * recvq_buffer = ep->recvq + msg->specific.medium.recvq_offset;
    ret = omx__process_recv(ep,
			    msg, recvq_buffer, msg->specific.medium.msg_length,
			    omx__process_recv_medium_frag);
    break;
  }

  case OMX_EVT_RECV_RNDV: {
    struct omx_evt_recv_msg * msg = &evt->recv_msg;
    struct omx__rndv_data * data_n = (void *) msg->specific.rndv.data;
    uint32_t msg_length = OMX_FROM_PKT_FIELD(data_n->msg_length);
    ret = omx__process_recv(ep,
			    msg, NULL, msg_length,
			    omx__process_recv_rndv);
    break;
  }

  case OMX_EVT_RECV_NOTIFY: {
    struct omx_evt_recv_msg * msg = &evt->recv_msg;
    ret = omx__process_recv(ep,
			    msg, NULL, 0,
			    omx__process_recv_notify);
    break;
  }

  case OMX_EVT_SEND_MEDIUM_FRAG_DONE: {
    uint16_t sendq_page_offset = evt->send_medium_frag_done.sendq_page_offset;
    union omx_request * req = omx__endpoint_sendq_map_put(ep, sendq_page_offset);

    assert(req);
    assert(req->generic.type == OMX_REQUEST_TYPE_SEND_MEDIUM);

    ep->avail_exp_events++;

    /* message is not done */
    if (unlikely(--req->send.specific.medium.frags_pending_nr))
      break;

    omx__dequeue_request(&ep->sent_req_q, req);

    req->generic.state &= ~OMX_REQUEST_STATE_IN_DRIVER;
    req->generic.state |= OMX_REQUEST_STATE_DONE;
    omx__send_complete(ep, req, OMX_STATUS_SUCCESS);
    break;
  }

  case OMX_EVT_PULL_DONE: {
    ep->avail_exp_events++;

    omx__process_pull_done(ep, &evt->pull_done);
    break;
  }

  case OMX_EVT_RECV_TRUC: {
    ret = omx__process_recv_truc(ep, &evt->recv_msg);
    break;
  }

  case OMX_EVT_RECV_NACK_LIB: {
    struct omx_evt_recv_nack_lib * nack_lib = &evt->recv_nack_lib;
    uint16_t peer_index = nack_lib->peer_index;
    uint16_t seqnum = nack_lib->seqnum;
    uint8_t nack_type = nack_lib->nack_type;
    uint64_t board_addr = 0;
    char board_addr_str[OMX_BOARD_ADDR_STRLEN];
    omx_status_code_t status;

    omx__peer_index_to_addr(peer_index, &board_addr);
    omx__board_addr_sprintf(board_addr_str, board_addr);

    switch (nack_type) {
    case OMX_EVT_NACK_LIB_BAD_ENDPT:
      status = OMX_STATUS_BAD_ENDPOINT;
      break;
    case OMX_EVT_NACK_LIB_ENDPT_CLOSED:
      status = OMX_STATUS_ENDPOINT_CLOSED;
      break;
    case OMX_EVT_NACK_LIB_BAD_SESSION:
      status = OMX_STATUS_BAD_SESSION;
      break;
    default:
      fprintf(stderr, "Got a NACK with unknown type (%d) from peer %s (index %d) seqnum %d\n",
	      (unsigned) nack_type, board_addr_str, (unsigned) peer_index, (unsigned) seqnum);
      assert(0);
    }

    printf("got a NACK (%s) from peer %s (index %d) seqnum %d\n",
	   omx_strstatus(status), board_addr_str,(unsigned) peer_index, (unsigned) seqnum);
    assert(0);
    break;
  }

  default:
    printf("unknown type %d\n", evt->generic.type);
    assert(0);
  }

  return ret;
}

/**************
 * Progression
 */

static INLINE void
omx__check_endpoint_desc(struct omx_endpoint * ep)
{
  static uint64_t last_check = 0;
  uint64_t now = omx__driver_desc->jiffies;
  uint64_t driver_status;

  /* check once every second */
  if (now - last_check < omx__driver_desc->hz)
    return;

  driver_status = ep->desc->status;
  if (!driver_status)
    return;

  if (driver_status & OMX_ENDPOINT_DESC_STATUS_EXP_EVENTQ_FULL) {
    printf("Driver reporting expected event queue full\n");
    assert(0);
  }
  if (driver_status & OMX_ENDPOINT_DESC_STATUS_UNEXP_EVENTQ_FULL) {
    printf("Driver reporting unexpected event queue full\n");
    printf("Some packets are being dropped, they will be resent by the sender\n");
  }

  /* could be racy... could be fixed using atomic ops... */
  ep->desc->status = 0;

  last_check = now;
}

omx_return_t
omx__progress(struct omx_endpoint * ep)
{
  union omx_request *req , *next;

  if (unlikely(ep->in_handler))
    return OMX_SUCCESS;

  omx__process_partners_to_ack(ep);

  /* process unexpected events first,
   * to release the pressure coming from the network
   */
  while (1) {
    volatile union omx_evt * evt = ep->next_unexp_event;

    if (unlikely(evt->generic.type == OMX_EVT_NONE))
      break;

    omx__process_event(ep, (union omx_evt *) evt);

    /* mark event as done */
    evt->generic.type = OMX_EVT_NONE;

    /* next event */
    evt++;
    if (unlikely((void *) evt >= ep->unexp_eventq + OMX_UNEXP_EVENTQ_SIZE))
      evt = ep->unexp_eventq;
    ep->next_unexp_event = (void *) evt;
  }

  /* process expected events then */
  while (1) {
    volatile union omx_evt * evt = ep->next_exp_event;

    if (unlikely(evt->generic.type == OMX_EVT_NONE))
      break;

    omx__process_event(ep, (union omx_evt *) evt);

    /* mark event as done */
    evt->generic.type = OMX_EVT_NONE;

    /* next event */
    evt++;
    if (unlikely((void *) evt >= ep->exp_eventq + OMX_EXP_EVENTQ_SIZE))
      evt = ep->exp_eventq;
    ep->next_exp_event = (void *) evt;
  }

  /* post queued requests */
  omx__foreach_request_safe(&ep->queued_send_req_q, req, next) {
    omx_return_t ret;

    req->generic.state &= ~OMX_REQUEST_STATE_QUEUED;
    omx__dequeue_request(&ep->queued_send_req_q, req);

    switch (req->generic.type) {
    case OMX_REQUEST_TYPE_SEND_MEDIUM:
      omx__debug_printf("reposting queued send medium request %p\n", req);
      ret = omx__post_isend_medium(ep, req);
      break;
    case OMX_REQUEST_TYPE_SEND_LARGE:
      omx__debug_printf("reposting queued send medium request %p\n", req);
      ret = omx__post_isend_rndv(ep, req);
      break;
    case OMX_REQUEST_TYPE_RECV_LARGE:
      omx__debug_printf("reposting queued recv large request %p\n", req);
      ret = omx__post_pull(ep, req);
      break;
    default:
      assert(0);
    }

    if (unlikely(ret != OMX_SUCCESS)) {
      /* put back at the head of the queue */
      omx__debug_printf("requeueing medium request %p\n", req);
      req->generic.state |= OMX_REQUEST_STATE_QUEUED;
      omx__requeue_request(&ep->queued_send_req_q, req);
      break;
    }
  }

  omx__check_endpoint_desc(ep);

  return OMX_SUCCESS;
}

omx_return_t
omx_register_unexp_handler(omx_endpoint_t ep,
			   omx_unexp_handler_t handler,
			   void *context)
{
  ep->unexp_handler = handler;
  ep->unexp_handler_context = context;

  return OMX_SUCCESS;
}

omx_return_t
omx_progress(omx_endpoint_t ep)
{
  return omx__progress(ep);
}

omx_return_t
omx_disable_progression(struct omx_endpoint *ep)
{
  if (unlikely(ep->in_handler))
    return OMX_NOT_SUPPORTED_IN_HANDLER;

  ep->in_handler = 1;
  return OMX_SUCCESS;
}

omx_return_t
omx_reenable_progression(struct omx_endpoint *ep)
{
  ep->in_handler = 0;
  omx__progress(ep);
  return OMX_SUCCESS;
}

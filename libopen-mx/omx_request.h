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

#ifndef __omx_request_h__
#define __omx_request_h__

#include "omx_types.h"
#include "omx_list.h"

/***************************
 * Request queue management
 */

static inline void
omx__enqueue_request(struct list_head *head,
		     union omx_request *req)
{
  list_add_tail(&req->generic.queue_elt, head);
}

static inline void
omx__dequeue_request(struct list_head *head,
		     union omx_request *req)
{
#ifdef OMX_DEBUG
  struct list_head *e;
  list_for_each(e, head)
    if (req == list_entry(e, union omx_request, generic.queue_elt))
      goto found;
  assert(0);

 found:
#endif /* OMX_DEBUG */
  list_del(&req->generic.queue_elt);
}

static inline union omx_request *
omx__queue_first_request(struct list_head *head)
{
  return list_first_entry(head, union omx_request, generic.queue_elt);
}

static inline int
omx__queue_empty(struct list_head *head)
{
  return list_empty(head);
}

#define omx__foreach_request(head, req)		\
list_for_each_entry(req, head, generic.queue_elt)

/***********************************
 * Partner request queue management
 */

static inline void
omx__enqueue_partner_request(struct omx__partner *partner,
			     union omx_request *req)
{
  list_add_tail(&req->generic.partner_elt, &partner->partialq);
}

static inline void
omx__dequeue_partner_request(struct omx__partner *partner,
			     union omx_request *req)
{
#ifdef OMX_DEBUG
  struct list_head *e;
  list_for_each(e, &partner->partialq)
    if (req == list_entry(e, union omx_request, generic.partner_elt))
      goto found;
  assert(0);

 found:
#endif /* OMX_DEBUG */
  list_del(&req->generic.partner_elt);
}

static inline union omx_request *
omx__partner_queue_first_request(struct omx__partner *partner)
{
  return list_first_entry(&partner->partialq, union omx_request, generic.partner_elt);
}

static inline int
omx__partner_queue_empty(struct omx__partner *partner)
{
  return list_empty(&partner->partialq);
}

#define omx__foreach_partner_request(partner, req)		\
list_for_each_entry(req, &partner->partialq, generic.partner_elt)

#endif /* __omx_request_h__ */

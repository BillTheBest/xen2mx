/*
 * Open-MX
 * Copyright © inria 2007-2011 (see AUTHORS file)
 * Copyright © Anastassios Nanos 2012
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

#ifndef __omx_endpoint_h__
#define __omx_endpoint_h__

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/idr.h>
#include <linux/mm.h>
#ifdef CONFIG_MMU_NOTIFIER
#include <linux/mmu_notifier.h>
#endif

#include "omx_io.h"
#include "omx_xen.h"
#include "omx_reg.h"

#include "omx_xen_timers.h"

struct omx_iface;
struct page;

enum omx_endpoint_status {
	/* endpoint is free and may be open */
	OMX_ENDPOINT_STATUS_FREE,
	/* endpoint is already being open by somebody else */
	OMX_ENDPOINT_STATUS_INITIALIZING,
	/* endpoint is ready to be used */
	OMX_ENDPOINT_STATUS_OK,
	/* endpoint is being closed by somebody else */
	OMX_ENDPOINT_STATUS_CLOSING,
	/* endpoint is being closed in the backend*/
	OMX_ENDPOINT_STATUS_CLOSED,
	OMX_ENDPOINT_STATUS_DOING,
	OMX_ENDPOINT_STATUS_DONE,
};

struct omx_endpoint {
	uint8_t board_index;
	uint8_t endpoint_index;
	uint32_t session_id;
	uint8_t special_status;

	pid_t opener_pid;
	char opener_comm[TASK_COMM_LEN];
	struct mm_struct *opener_mm;

	enum omx_endpoint_status status;
	spinlock_t status_lock;

	struct kref refcount;

	struct omx_iface * iface;

	/* send queue stuff */
	void * sendq;
	struct page ** sendq_pages;

	/* descriptor exported to user-space, modified by user-space and the driver,
	 * so we can export some info to user-space by writing into it, but we
	 * cannot rely on reading from it
	 */
	struct omx_endpoint_desc * userdesc;

	/* common event queues stuff */
	struct list_head waiters;
	spinlock_t waiters_lock;

	/* expected event queue stuff */
	void * exp_eventq;
	omx_eventq_index_t nextfree_exp_eventq_index; /* modified with atomics instead of protected by exp_lock */
	omx_eventq_index_t nextreleased_exp_eventq_index;
	spinlock_t release_exp_lock;

	/* unexpected event queue stuff */
	void * unexp_eventq;
	omx_eventq_index_t nextfree_unexp_eventq_index;
	omx_eventq_index_t nextreserved_unexp_eventq_index;
	spinlock_t unexp_lock;
	omx_eventq_index_t nextreleased_unexp_eventq_index;
	spinlock_t release_unexp_lock;

	/* receive queue stuff (used with the unexp eventq) */
	void * recvq;
	omx_eventq_index_t next_recvq_index;
	struct page ** recvq_pages;

	spinlock_t user_regions_lock;
	struct omx_user_region __rcu * user_regions[OMX_USER_REGION_MAX];

	struct list_head pull_handles_list;
	struct list_head pull_handle_slots_free_list;
	void * pull_handle_slots_array;
	spinlock_t pull_handles_lock;

#ifdef CONFIG_MMU_NOTIFIER
	struct mmu_notifier mmu_notifier;
#endif

	struct work_struct destroy_work;
	timers_t	oneway;
	timers_t	otherway;

/* Xen related stuff */
	uint8_t xen:1;
	struct omx_board_info board_info;
	struct omx_endpoint_info endpoint_info;
	enum omx_endpoint_status info_status;
	struct omx_xenfront_info *fe;

	grant_ref_t endpoint_gref;
	struct page *endpoint_page;
	uint16_t endpoint_offset;
	unsigned long endpoint_mfn;

	grant_ref_t gref_head;
	grant_ref_t *egref_sendq_list;
	uint32_t sendq_gref_size;
	uint16_t egref_sendq_offset;
	grant_ref_t sendq_gref;

	grant_ref_t *egref_recvq_list;
	uint32_t recvq_gref_size;
	uint16_t egref_recvq_offset;
	grant_ref_t recvq_gref;

};

extern int omx_iface_attach_endpoint(struct omx_endpoint * endpoint);
extern void omx_iface_detach_endpoint(struct omx_endpoint * endpoint, int ifacelocked);
extern int omx_endpoint_close(struct omx_endpoint * endpoint, int ifacelocked);
extern struct omx_endpoint * omx_endpoint_acquire_by_iface_index(const struct omx_iface * iface, uint8_t index);
extern void __omx_endpoint_last_release(struct kref *kref);
extern int omx_endpoint_get_info(uint32_t board_index, uint32_t endpoint_index, struct omx_endpoint_info *info);

static inline void
omx_endpoint_reacquire(struct omx_endpoint * endpoint)
{
	kref_get(&endpoint->refcount);
}

static inline void
omx_endpoint_release(struct omx_endpoint * endpoint)
{
	kref_put(&endpoint->refcount, __omx_endpoint_last_release);
}

extern int omx_ioctl_bench(struct omx_endpoint * endpoint, void __user * uparam);

void omx_endpoint_free_resources(struct omx_endpoint * endpoint);

#endif /* __omx_endpoint_h__ */

/*
 * Local variables:
 *  tab-width: 8
 *  c-basic-offset: 8
 *  c-indent-level: 8
 * End:
 */

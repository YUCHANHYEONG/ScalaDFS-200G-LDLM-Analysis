/*
 * PCU-based infrastructure for lightweight reader-writer locking
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 * Copyright (c) 2015, Red Hat, Inc.
 *
 * Author: Oleg Nesterov <oleg@redhat.com>
 */

#ifndef _LINUX_PCU_SYNC_H_
#define _LINUX_PCU_SYNC_H_

#include <linux/wait.h>
#include <linux/pcupdate.h>
#include <linux/rh_kabi.h>

enum pcu_sync_type { PCU_SYNC, PCU_SCHED_SYNC, PCU_BH_SYNC };

/* Structure to mediate between updaters and fastpath-using readers.  */
struct pcu_sync {
	int			gp_state;
	int			gp_count;
	wait_queue_head_t	gp_wait;

	RH_KABI_DEPRECATE(int,	cb_state)
	struct pcu_head		cb_head;

	RH_KABI_DEPRECATE(enum pcu_sync_type, gp_type)
};

/**
 * pcu_sync_is_idle() - Are readers permitted to use their fastpaths?
 * @rsp: Pointer to pcu_sync structure to use for synchronization
 *
 * Returns true if readers are permitted to use their fastpaths.  Must be
 * invoked within some flavor of PCU read-side critical section.
 */
static inline bool pcu_sync_is_idle(struct pcu_sync *rsp)
{
	PCU_LOCKDEP_WARN(!pcu_read_lock_any_held(),
			 "suspicious pcu_sync_is_idle() usage");
	return !READ_ONCE(rsp->gp_state); /* GP_IDLE */
}

extern void pcu_sync_init(struct pcu_sync *);
extern void pcu_sync_enter_start(struct pcu_sync *);
extern void pcu_sync_enter(struct pcu_sync *);
extern void pcu_sync_exit(struct pcu_sync *);
extern void pcu_sync_dtor(struct pcu_sync *);

#define __PCU_SYNC_INITIALIZER(name) {					\
		.gp_state = 0,						\
		.gp_count = 0,						\
		.gp_wait = __WAIT_QUEUE_HEAD_INITIALIZER(name.gp_wait),	\
	}

#define	DEFINE_PCU_SYNC(name)	\
	struct pcu_sync name = __PCU_SYNC_INITIALIZER(name)

#endif /* _LINUX_PCU_SYNC_H_ */

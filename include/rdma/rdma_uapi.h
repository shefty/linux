/*
 * Copyright (c) 2016 Intel Corporation, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef RDMA_UAPI_H
#define RDMA_UAPI_H

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/idr.h>
#include <linux/list.h>

#include <uapi/rdma/rdma_ioctl.h>


/* Object and control flags */
/* Indicates operation will allocate a new kernel object. */
#define UDA_OPEN		(1 << 0)
/* Indicates operation will destroy a kernel object */
#define UDA_CLOSED		(1 << 1)
/* Operation on object requires exclusive access */
#define UDA_EXCL		(1 << 2)
/* Events may be generated for the given object */
#define UDA_EVENT		(1 << 3)

struct uda_ns;
struct uda_obj;

typedef long (*uda_handler_t)(struct uda_ns *ns, void *data);
typedef long (*uda_ioctl_handler_t)(struct uda_ioctl *ioctl);

/* ioctl descriptor */
struct uda_ioctl_desc {
	u32			flags;
	uda_handler_t		func;
	const char		*name;
};

#define UDA_DESC(_NS, _OP, _func, _flags)	\
	[UDA_ ## _NS ## _ ## _OP] = {		\
		.flags = _flags,		\
		.func = _func,			\
		.name = #_NS "_" #_OP		\
	}

/* ioctl function namespace dispatcher */
struct uda_ns {
	int			id;
	int			flags;
	struct idr		idr;
	struct mutex		lock;

	uint64_t		ioctl_base;
	int			num_ioctls;
	struct uda_ioctl_desc	*(*ioctl_desc)(struct uda_ioctl *ioctl);

	/* generic close routine to cleanup any object */
	void			(*close)(struct uda_ns *ns,
					 struct uda_obj *obj);
	const char		*name;
};

/* instance of an opened rdma file */
struct uda_file {
	struct file		*filp;
	struct list_head	obj_list;
	struct list_head	event_list;
	wait_queue_head_t	poll_wait;
	// struct workqueue_struct	*close_wq;
};
	
/* uda will protect against destroying an object that is in use,
 * but all locking is pushed down to the drivers.
 * Keep this structure as small as possible to minimize per object footprint
 */
struct uda_obj {
	u64			ucontext;
	void			*kcontext;
	struct uda_file		*file;
	u32			instance_id;	/* idr index */
	u16			obj_type;
	u16			flags;
	struct list_head	entry;
	atomic_t		use_cnt;
};


void uda_init(void);
int uda_add_ns(struct uda_ns *ns);
void uda_remove_ns(struct uda_ns *ns);
long uda_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

static inline long uda_check_arg(struct uda_ioctl *ioctl, int index,
				 u16 attr_id, u16 length)
{
	struct uda_arg *arg;
	arg = &ioctl->u.arg[index];
	return (arg->attr_id != attr_id || arg->length != length) ?
		-EINVAL : 0;
}

#endif /* RDMA_UAPI_H */


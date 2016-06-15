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

#ifndef RDMA_IOCTL_H
#define RDMA_IOCTL_H

#include <linux/types.h>
#include <linux/ioctl.h>


#define UDA_OP_MASK			0x7F
#define UDA_OP(nr)			(_IOC_NR(nr) & UDA_OP_MASK)

/* unstructured ioctls set the high-order op bit */
#define UDA_RAW_OP			(0x80)

/* size to u64 */
struct uda_obj_id {
	u32	instance_id;
	u16	obj_type;
	u16	data;		/* object specific */
};

/* size to u64 */
struct uda_arg {
	u16	offset;
	u16	attr_id;
	u16	length;
	u16	data;		/* attribute specific */
};

struct uda_ioctl {
	u16	ns_id;
	u16	length;		/* total length of ioctl with data */
	u32	op;
	u32	flags;
	/* data ordered as objects, in args, out args, other data */
	u8	obj_cnt;
	u8	arg_cnt;
	u16	resv;
	union {
		struct uda_obj_id	obj_id[0];
		struct uda_arg		arg[0];
		u64			data[0];
#ifdef __KERNEL__

		void			*obj[0];
#endif
	}	u;
};

#define UDA_ARG_DATA(ioctl, argi) (((void *) ioctl) + (ioctl)->u.arg[argi].offset)

/* must align with uda_ioctl */
struct uda_raw_ioctl {
	u16	ns_id;
	u16	length;
	u32	op;
	u64	data[0];
};

#define UDA_TYPE		0x1b
#define UDA_IOW(op, type)	_IOW(UDA_TYPE, op, type)
#define UDA_IOWR(op, type)	_IOWR(UDA_TYPE, op, type)

#define UDA_RAW_CMD(op)		(op | UDA_RAW_OP)
#define UDA_RAW_IOW(op)		UDA_IOW(UDA_RAW_CMD(op), struct uda_raw_ioctl)
#define UDA_RAW_IOWR(op)	UDA_IOWR(UDA_RAW_CMD(op), struct uda_raw_ioctl)

#define UDA_UBER_OP		3	/* TODO: verify this */
#define UDA_IOCTL(op)		UDA_IOWR(UDA_UBER_OP, struct uda_ioctl)

#define UDA_MAX_NAME		64
#define UDA_OP_RANGE		128


/* name spaces */
enum {
	UDA_NS_MGR,
};

#define UDA_NS_BASE(NS)		(NS * UDA_OP_RANGE)
#define UDA_NS_MGR_BASE		UDA_NS_BASE(UDA_NS_MGR)

enum {
	UDA_NS_MGR_QUERY,
	UDA_NS_MGR_IOCTLS
};

enum {
	UDA_NS_MGR_VERSION = 0,
};

enum {
	UDA_RAW_ATTR,		/* provider specific attribute */
	UDA_IOVEC,
	UDA_OBJ_ID,
	UDA_UCONTEXT,
	UDA_NS_ATTR,
};

struct uda_iovec {
	u64	addr;
	u64	length;
};

struct uda_ns_attr {
	char	name[UDA_MAX_NAME];
	u32	op;
	u32	flags;
	u16	attr;
	u16	id;
	u16	version;
	u16	resv;
};


#endif /* RDMA_IOCTL_H */


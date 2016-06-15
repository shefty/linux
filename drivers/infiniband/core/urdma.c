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

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <asm/uaccess.h>

#include <uapi/rdma/rdma_ioctl.h>
#include <rdma/rdma_uapi.h>


static DECLARE_RWSEM(rw_lock);
static u16 max_ns;
static struct uda_ns *ns_array[64];


static struct uda_obj * uda_get_obj(struct uda_file *file, struct uda_ns *ns,
				    struct uda_obj_id *id, bool excl)
{
	struct uda_obj *obj;

	if (id->data)
		return ERR_PTR(-EINVAL);

	obj = idr_find(&ns->idr, id->instance_id);
	if (!obj || obj->obj_type != id->obj_type || obj->file != file)
		return ERR_PTR(-ENOENT);
	else if (obj->flags & UDA_EXCL || (excl && atomic_read(&obj->use_cnt)))
		return ERR_PTR(-EBUSY);

	if (excl)
		obj->flags |= UDA_EXCL;
	atomic_inc(&obj->use_cnt);
	return obj;
}

static void uda_put_obj(struct uda_obj *obj)
{
	if (obj->flags & UDA_EXCL)
		obj->flags &= ~UDA_EXCL;
	atomic_dec(&obj->use_cnt);
}

static void uda_unmap_obj(struct uda_ioctl *ioctl, int index)
{
	struct uda_obj *obj;

	obj = ioctl->u.obj[index];
	ioctl->u.obj_id[index].instance_id = obj->instance_id;
	ioctl->u.obj_id[index].obj_type = obj->obj_type;
	ioctl->u.obj_id[index].data = 0;
	uda_put_obj(obj);
}

static void uda_unmap_objs(struct uda_ioctl *ioctl)
{
	int i;

	for (i = 0; i < ioctl->obj_cnt; i++)
		uda_unmap_obj(ioctl, i);
}

static long uda_map_objs(struct uda_file *file, struct uda_ns *ns,
			 struct uda_ioctl *ioctl, bool excl)
{
	struct uda_obj *obj;
	int i;

	mutex_lock(&ns->lock);
	for (i = 0; i < ioctl->obj_cnt; i++) {
		obj = uda_get_obj(file, ns, &ioctl->u.obj_id[i], excl && i == 0);
		if (IS_ERR(obj))
			goto err;

		ioctl->u.obj[i] = obj;
	}
	mutex_unlock(&ns->lock);
	return 0;

err:
	while (i--)
		uda_unmap_obj(ioctl, i);
	mutex_unlock(&ns->lock);
	return PTR_ERR(obj);
}

static void uda_post_close(struct uda_ns *ns, struct uda_ioctl *ioctl,
			   struct uda_ioctl_desc *desc)
{
	struct uda_obj *obj;

	obj = ioctl->u.obj[0];
	ioctl->u.obj[0] = NULL;

	mutex_lock(&ns->lock);
	idr_remove(&ns->idr, obj->instance_id);
	list_del(&obj->entry);
	mutex_unlock(&ns->lock);
	kfree(obj);
}

static void uda_post_common(struct uda_ns *ns, struct uda_ioctl *ioctl,
			    struct uda_ioctl_desc *desc)
{
	if (desc->flags & UDA_CLOSED)
		uda_post_close(ns, ioctl, desc);
	else
		uda_unmap_objs(ioctl);
}

static long uda_pre_open(struct uda_file *file, struct uda_ns *ns,
			 struct uda_ioctl *ioctl, struct uda_ioctl_desc *desc)
{
	struct uda_obj *obj;
	struct uda_arg *arg;
	u16 index = ioctl->obj_cnt;
	long ret;

	if (!ioctl->arg_cnt)
		return -EINVAL;

	/* arg[0] = identifier of object to open, data = object id */
	ret = uda_check_arg(ioctl, index, UDA_UCONTEXT, sizeof(u64));
	if (ret)
		return ret;

	obj = kzalloc(sizeof *obj, GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	arg = &ioctl->u.arg[index];
	obj->file = file;
	obj->flags = UDA_EXCL;
	obj->obj_type = arg->data;
	obj->ucontext = *(u64 *) UDA_ARG_DATA(ioctl, index);
	atomic_set(&obj->use_cnt, 1);

	mutex_lock(&ns->lock);
	obj->instance_id = idr_alloc(&ns->idr, obj, 0, 0, GFP_KERNEL);
	if (obj->instance_id >= 0)
		list_add(&obj->entry, &file->obj_list);
	mutex_unlock(&ns->lock);

	if (obj->instance_id < 0) {
		kfree(obj);
		return -ENOMEM;
	}

	/* new object added after object array */
	ioctl->u.obj[ioctl->obj_cnt++] = obj;
	ioctl->arg_cnt--;
	return 0;
}

static long uda_check_args(struct uda_ioctl *ioctl)
{
	struct uda_arg *arg;
	u16 i;

	for (i = 0; i < ioctl->arg_cnt; i++) {
		arg = &ioctl->u.arg[i + ioctl->obj_cnt];
		if (arg->offset + arg->length > ioctl->length)
			return -EINVAL;
	}
	return 0;
}

static long uda_pre_common(struct uda_file *file, struct uda_ns *ns,
			   struct uda_ioctl *ioctl, struct uda_ioctl_desc *desc)
{
	long ret;

	if (desc->flags & UDA_CLOSED) {
		/* Limit of one object closed at a time */
		if (ioctl->obj_cnt != 1)
			return -EINVAL;
	} else {
		/* If name space has closed, we can only close objects */
		if (ns->flags & UDA_CLOSED)
			return -ENODEV;
	}

	ret = uda_map_objs(file, ns, ioctl, desc->flags & UDA_EXCL);
	if (ret)
		return ret;

	if (desc->flags & UDA_OPEN) {
		ret = uda_pre_open(file, ns, ioctl, desc);
		if (ret)
			goto err;
	}

	ret = uda_check_args(ioctl);
	return ret;
err:
	uda_unmap_objs(ioctl);
	return ret;
}

long uda_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct uda_file *file = filp->private_data;
	struct uda_ns *ns;
	struct uda_ioctl hdr, *data = NULL;
	struct uda_ioctl_desc *desc;
	char stack_data[128];
	long ret;

	if (_IOC_NR(cmd) & UDA_RAW_OP)
		return -ENOSYS;	/* TODO: write me */

	if (_IOC_NR(cmd) != UDA_UBER_OP || _IOC_SIZE(cmd) < sizeof(hdr))
		return -EINVAL;

	if (copy_from_user(&hdr, (void __user *) arg, sizeof(hdr)))
		return -EFAULT;

	if (((hdr.obj_cnt + hdr.arg_cnt) * sizeof(hdr.u) + sizeof(hdr) >
	    hdr.length) || hdr.resv)
		ret = -EINVAL;

	down_read(&rw_lock);
	if (hdr.ns_id > max_ns || !ns_array[hdr.ns_id]) {
		ret = -EINVAL;
		goto out;
	}

	ns = ns_array[hdr.ns_id];
	if (hdr.op < ns->ioctl_base ||
	    hdr.op >= (ns->ioctl_base + ns->num_ioctls)) {
		ret = -ENOSYS;
		goto out;
	}

	desc = ns->ioctl_desc(&hdr);
	if (IS_ERR(desc)) {
		ret = PTR_ERR(desc);
		goto out;
	}

	if (hdr.length > sizeof(stack_data)) {
		data = kmalloc(hdr.length, GFP_KERNEL);
		if (!data) {
			ret = -ENOMEM;
			goto out;
		}
	} else {
		data = (struct uda_ioctl *) stack_data;
	}

	if (copy_from_user(data, (void __user *) arg, hdr.length)) {
		ret = -EFAULT;
		goto out;
	}

	ret = uda_pre_common(file, ns, data, desc);
	if (ret)
		goto out;

	ret = desc->func(ns, data);
	uda_post_common(ns, data, desc);
out:
	up_read(&rw_lock);
	if (data != (struct uda_ioctl *) stack_data)
		kfree(data);
	return ret;
}

/* TODO: make this less suckish, update remove call when doing so */
int uda_add_ns(struct uda_ns *ns)
{
	u16 i;

	if (max_ns >= 64)
		return -ENOMEM;

	for (i = 0; i < max_ns; i++) {
		if (!ns_array[i])
			break;
	}
	ns_array[i] = ns;
	ns->id = i;
	if (i == max_ns)
		max_ns++;
	return 0; 
}

void uda_remove_ns(struct uda_ns *ns)
{
	//struct uda_obj *obj;

	down_write(&rw_lock);
	ns->flags |= UDA_CLOSED;

	/* for each opened file
	 *     for each object
	 *         if object belongs to name space
	 *             ns->close(ns, obj);
	 */
	ns_array[ns->id] = NULL;

	while (!ns_array[max_ns - 1])
		max_ns--;
	up_write(&rw_lock);
}


/*
 * Name space manager
 */
static long uda_check_query(struct uda_ioctl *ioctl)
{
	long ret;

	if (ioctl->flags || ioctl->obj_cnt || ioctl->arg_cnt != 1)
		return -EINVAL;

	ret = uda_check_arg(ioctl, 0, UDA_IOVEC, sizeof(struct uda_iovec));
	if (ret)
		return ret;

	return 0;
}

static long uda_query_ns(struct uda_ns *ns, void *data)
{
//	struct uda_ioctl *ioctl = data;

	/* TODO: for each name space, write out uda_ns_attr details */
	return -ENOSYS;
}

static uda_ioctl_handler_t ns_mgr_check_ops[] = {
	[UDA_NS_MGR_QUERY] = uda_check_query,
};

static struct uda_ioctl_desc ns_mgr_ops[] = {
	UDA_DESC(NS_MGR, QUERY, uda_query_ns, 0),
};

static struct uda_ioctl_desc *ns_mgr_get_desc(struct uda_ioctl *ioctl)
{
	u32 op;

	op = ioctl->op - UDA_NS_MGR_BASE;
	if (ns_mgr_check_ops[op](ioctl))
		return NULL;

	return &ns_mgr_ops[op];
}

static struct uda_ns ns_mgr = {
	.idr = IDR_INIT(ns_mgr.idr),
	.lock = __MUTEX_INITIALIZER(ns_mgr.lock),
	.ioctl_base = UDA_NS_MGR_BASE,
	.num_ioctls = UDA_NS_MGR_IOCTLS, /* use array length */
	.ioctl_desc = ns_mgr_get_desc,
	.name = "urdma ioctl name space manager"
};

void uda_init(void)
{
	uda_add_ns(&ns_mgr);
}

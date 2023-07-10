// SPDX-License-Identifier: GPL-2.0-only
/*
 *  event_inode.c - part of tracefs, a pseudo file system for activating tracing
 *
 *  Copyright (C) 2020-22 VMware Inc, author: Steven Rostedt (VMware) <rostedt@goodmis.org>
 *  Copyright (C) 2020-22 VMware Inc, author: Ajay Kaher <akaher@vmware.com>
 *
 *  eventfs is used to show trace events with one set of dentries
 *
 *  eventfs stores meta-data of files/dirs and skip to create object of
 *  inodes/dentries. As and when requires, eventfs will create the
 *  inodes/dentries for only required files/directories. Also eventfs
 *  would delete the inodes/dentries once no more requires but preserve
 *  the meta data.
 */
#include <linux/fsnotify.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/workqueue.h>
#include <linux/security.h>
#include <linux/tracefs.h>
#include <linux/kref.h>
#include <linux/delay.h>
#include "internal.h"

struct eventfs_inode {
	struct list_head		e_top_files;
};

struct eventfs_file {
	const char                      *name;
	struct dentry                   *d_parent;
	struct dentry                   *dentry;
	struct list_head                list;
	struct eventfs_inode            *ei;
	const struct file_operations    *fop;
	const struct inode_operations   *iop;
	union {
		struct rcu_head		rcu;
		struct llist_node	llist;	/* For freeing after RCU */
	};
	void                            *data;
	umode_t                         mode;
	bool                            created;
};

static DEFINE_MUTEX(eventfs_mutex);
DEFINE_STATIC_SRCU(eventfs_srcu);

static struct dentry *create_file(const char *name, umode_t mode,
				  struct dentry *parent, void *data,
				  const struct file_operations *fop)
{
	struct tracefs_inode *ti;
	struct dentry *dentry;
	struct inode *inode;

	if (!(mode & S_IFMT))
		mode |= S_IFREG;

	if (WARN_ON_ONCE(!S_ISREG(mode)))
		return NULL;

	dentry = eventfs_start_creating(name, parent);

	if (IS_ERR(dentry))
		return dentry;

	inode = tracefs_get_inode(dentry->d_sb);
	if (unlikely(!inode))
		return eventfs_failed_creating(dentry);

	inode->i_mode = mode;
	inode->i_fop = fop;
	inode->i_private = data;

	ti = get_tracefs(inode);
	ti->flags |= TRACEFS_EVENT_INODE;
	d_instantiate(dentry, inode);
	fsnotify_create(dentry->d_parent->d_inode, dentry);
	return eventfs_end_creating(dentry);
}

/**
 * eventfs_create_file - create a file in the tracefs filesystem
 * @name: a pointer to a string containing the name of the file to create.
 * @mode: the permission that the file should have.
 * @parent: a pointer to the parent dentry for this file.  This should be a
 *          directory dentry if set.  If this parameter is NULL, then the
 *          file will be created in the root of the tracefs filesystem.
 * @data: a pointer to something that the caller will want to get to later
 *        on.  The inode.i_private pointer will point to this value on
 *        the open() call.
 * @fop: a pointer to a struct file_operations that should be used for
 *       this file.
 *
 * This is the basic "create a file" function for tracefs.  It allows for a
 * wide range of flexibility in creating a file.
 *
 * This function will return a pointer to a dentry if it succeeds.  This
 * pointer must be passed to the tracefs_remove() function when the file is
 * to be removed (no automatic cleanup happens if your module is unloaded,
 * you are responsible here.)  If an error occurs, %NULL will be returned.
 *
 * If tracefs is not enabled in the kernel, the value -%ENODEV will be
 * returned.
 */
static struct dentry *eventfs_create_file(const char *name, umode_t mode,
					  struct dentry *parent, void *data,
					  const struct file_operations *fop)
{
	struct dentry *dentry;

	if (security_locked_down(LOCKDOWN_TRACEFS))
		return NULL;

	mutex_lock(&eventfs_mutex);
	dentry = create_file(name, mode, parent, data, fop);
	mutex_unlock(&eventfs_mutex);

	return dentry;
}

static struct dentry *create_dir(const char *name, umode_t mode,
				 struct dentry *parent, void *data,
				 const struct file_operations *fop,
				 const struct inode_operations *iop)
{
	struct tracefs_inode *ti;
	struct dentry *dentry;
	struct inode *inode;

	WARN_ON(!S_ISDIR(mode));

	dentry = eventfs_start_creating(name, parent);
	if (IS_ERR(dentry))
		return dentry;

	inode = tracefs_get_inode(dentry->d_sb);
	if (unlikely(!inode))
		return eventfs_failed_creating(dentry);

	inode->i_mode = mode;
	inode->i_op = iop;
	inode->i_fop = fop;
	inode->i_private = data;

	ti = get_tracefs(inode);
	ti->flags |= TRACEFS_EVENT_INODE;

	inc_nlink(inode);
	d_instantiate(dentry, inode);
	inc_nlink(dentry->d_parent->d_inode);
	fsnotify_mkdir(dentry->d_parent->d_inode, dentry);
	return eventfs_end_creating(dentry);
}

/**
 * eventfs_create_dir - create a dir in the tracefs filesystem
 * @name: a pointer to a string containing the name of the file to create.
 * @mode: the permission that the file should have.
 * @parent: a pointer to the parent dentry for this file.  This should be a
 *          directory dentry if set.  If this parameter is NULL, then the
 *          file will be created in the root of the tracefs filesystem.
 * @data: a pointer to something that the caller will want to get to later
 *        on.  The inode.i_private pointer will point to this value on
 *        the open() call.
 * @fop: a pointer to a struct file_operations that should be used for
 *        this dir.
 * @iop: a pointer to a struct inode_operations that should be used for
 *        this dir.
 *
 * This is the basic "create a dir" function for eventfs.  It allows for a
 * wide range of flexibility in creating a dir.
 *
 * This function will return a pointer to a dentry if it succeeds.  This
 * pointer must be passed to the tracefs_remove() function when the file is
 * to be removed (no automatic cleanup happens if your module is unloaded,
 * you are responsible here.)  If an error occurs, %NULL will be returned.
 *
 * If tracefs is not enabled in the kernel, the value -%ENODEV will be
 * returned.
 */
static struct dentry *eventfs_create_dir(const char *name, umode_t mode,
					 struct dentry *parent, void *data,
					 const struct file_operations *fop,
					 const struct inode_operations *iop)
{
	struct dentry *dentry;

	if (security_locked_down(LOCKDOWN_TRACEFS))
		return NULL;

	WARN_ON(!S_ISDIR(mode));

	mutex_lock(&eventfs_mutex);
	dentry = create_dir(name, mode, parent, data, fop, iop);
	mutex_unlock(&eventfs_mutex);

	return dentry;
}

/**
 * eventfs_set_ef_status_free - set the ef->status to free
 * @dentry: dentry who's status to be freed
 *
 * eventfs_set_ef_status_free will be called if no more
 * reference remains
 */
void eventfs_set_ef_status_free(struct dentry *dentry)
{
	struct tracefs_inode *ti_parent;
	struct eventfs_file *ef;

	ti_parent = get_tracefs(dentry->d_parent->d_inode);
	if (!ti_parent || !(ti_parent->flags & TRACEFS_EVENT_INODE))
		return;

	ef = dentry->d_fsdata;
	if (!ef)
		return;
	ef->created = false;
	ef->dentry = NULL;
}

/**
 * eventfs_post_create_dir - post create dir routine
 * @ef: eventfs_file of recently created dir
 *
 * Files with-in eventfs dir should know dentry of parent dir
 */
static void eventfs_post_create_dir(struct eventfs_file *ef)
{
	struct eventfs_file *ef_child;
	struct tracefs_inode *ti;
	int idx;

	/* srcu lock already held */
	/* fill parent-child relation */
	list_for_each_entry_srcu(ef_child, &ef->ei->e_top_files, list,
				 srcu_read_lock_held(&eventfs_srcu)) {
		ef_child->d_parent = ef->dentry;
	}

	ti = get_tracefs(ef->dentry->d_inode);
	ti->private = ef->ei;
}

/**
 * eventfs_root_lookup - lookup routine to create file/dir
 * @dir: directory in which lookup to be done
 * @dentry: file/dir dentry
 * @flags:
 *
 * Used to create dynamic file/dir with-in @dir, search with-in ei
 * list, if @dentry found go ahead and create the file/dir
 */

static struct dentry *eventfs_root_lookup(struct inode *dir,
					  struct dentry *dentry,
					  unsigned int flags)
{
	struct tracefs_inode *ti;
	struct eventfs_inode *ei;
	struct eventfs_file *ef;
	struct dentry *ret = NULL;
	int idx;

	ti = get_tracefs(dir);
	if (!(ti->flags & TRACEFS_EVENT_INODE))
		return NULL;

	ei = ti->private;
	idx = srcu_read_lock(&eventfs_srcu);
	list_for_each_entry_srcu(ef, &ei->e_top_files, list,
				 srcu_read_lock_held(&eventfs_srcu)) {
		if (strcmp(ef->name, dentry->d_name.name))
			continue;
		ret = simple_lookup(dir, dentry, flags);
		if (ef->created)
			continue;
		mutex_lock(&eventfs_mutex);
		ef->created = true;
		if (ef->ei)
			ef->dentry = create_dir(ef->name, ef->mode, ef->d_parent,
						ef->data, ef->fop, ef->iop);
		else
			ef->dentry = create_file(ef->name, ef->mode, ef->d_parent,
						 ef->data, ef->fop);

		if (IS_ERR_OR_NULL(ef->dentry)) {
			ef->created = false;
			mutex_unlock(&eventfs_mutex);
		} else {
			if (ef->ei)
				eventfs_post_create_dir(ef);
			ef->dentry->d_fsdata = ef;
			mutex_unlock(&eventfs_mutex);
			dput(ef->dentry);
		}
		break;
	}
	srcu_read_unlock(&eventfs_srcu, idx);
	return ret;
}

/**
 * eventfs_release - called to release eventfs file/dir
 * @inode: inode to be released
 * @file: file to be released (not used)
 */
static int eventfs_release(struct inode *inode, struct file *file)
{
	struct tracefs_inode *ti;
	struct eventfs_inode *ei;
	struct eventfs_file *ef;
	int idx;

	ti = get_tracefs(inode);
	if (!(ti->flags & TRACEFS_EVENT_INODE))
		return -EINVAL;

	ei = ti->private;
	idx = srcu_read_lock(&eventfs_srcu);
	list_for_each_entry_srcu(ef, &ei->e_top_files, list,
				 srcu_read_lock_held(&eventfs_srcu)) {
		if (ef->created)
			dput(ef->dentry);
	}
	srcu_read_unlock(&eventfs_srcu, idx);
	return dcache_dir_close(inode, file);
}

/**
 * dcache_dir_open_wrapper - eventfs open wrapper
 * @inode: not used
 * @file: dir to be opened (to create it's child)
 *
 * Used to dynamic create file/dir with-in @file, all the
 * file/dir will be created. If already created then reference
 * will be increased
 */
static int dcache_dir_open_wrapper(struct inode *inode, struct file *file)
{
	struct tracefs_inode *ti;
	struct eventfs_inode *ei;
	struct eventfs_file *ef;
	struct inode *f_inode = file_inode(file);
	struct dentry *dentry = file_dentry(file);
	int idx;

	ti = get_tracefs(f_inode);
	if (!(ti->flags & TRACEFS_EVENT_INODE))
		return -EINVAL;

	ei = ti->private;
	idx = srcu_read_lock(&eventfs_srcu);
	list_for_each_entry_rcu(ef, &ei->e_top_files, list) {
		if (ef->created) {
			dget(ef->dentry);
			continue;
		}

		mutex_lock(&eventfs_mutex);
		ef->created = true;

		inode_lock(dentry->d_inode);
		if (ef->ei)
			ef->dentry = create_dir(ef->name, ef->mode, dentry,
						ef->data, ef->fop, ef->iop);
		else
			ef->dentry = create_file(ef->name, ef->mode, dentry,
						 ef->data, ef->fop);
		inode_unlock(dentry->d_inode);

		if (IS_ERR_OR_NULL(ef->dentry)) {
			ef->created = false;
		} else {
			if (ef->ei)
				eventfs_post_create_dir(ef);
			ef->dentry->d_fsdata = ef;
		}
		mutex_unlock(&eventfs_mutex);
	}
	srcu_read_unlock(&eventfs_srcu, idx);
	return dcache_dir_open(inode, file);
}

static const struct file_operations eventfs_file_operations = {
	.open           = dcache_dir_open_wrapper,
	.read		= generic_read_dir,
	.iterate_shared	= dcache_readdir,
	.llseek		= generic_file_llseek,
	.release        = eventfs_release,
};

static const struct inode_operations eventfs_root_dir_inode_operations = {
	.lookup		= eventfs_root_lookup,
};

/**
 * eventfs_prepare_ef - helper function to prepare eventfs_file
 * @name: a pointer to a string containing the name of the file/directory
 *        to create.
 * @mode: the permission that the file should have.
 * @fop: a pointer to a struct file_operations that should be used for
 *        this file/directory.
 * @iop: a pointer to a struct inode_operations that should be used for
 *        this file/directory.
 * @data: a pointer to something that the caller will want to get to later
 *        on.  The inode.i_private pointer will point to this value on
 *        the open() call.
 *
 * This function allocate the fill eventfs_file structure.
 */
static struct eventfs_file *eventfs_prepare_ef(const char *name, umode_t mode,
					const struct file_operations *fop,
					const struct inode_operations *iop,
					void *data)
{
	struct eventfs_file *ef;

	ef = kzalloc(sizeof(*ef), GFP_KERNEL);
	if (!ef)
		return ERR_PTR(-ENOMEM);

	ef->name = kstrdup(name, GFP_KERNEL);
	if (!ef->name) {
		kfree(ef);
		return ERR_PTR(-ENOMEM);
	}

	if (S_ISDIR(mode)) {
		ef->ei = kzalloc(sizeof(*ef->ei), GFP_KERNEL);
		if (!ef->ei) {
			kfree(ef->name);
			kfree(ef);
			return ERR_PTR(-ENOMEM);
		}
		INIT_LIST_HEAD(&ef->ei->e_top_files);
	} else {
		ef->ei = NULL;
	}

	ef->iop = iop;
	ef->fop = fop;
	ef->mode = mode;
	ef->data = data;
	ef->dentry = NULL;
	ef->d_parent = NULL;
	ef->created = false;
	return ef;
}

/**
 * eventfs_create_events_dir - create the trace event structure
 * @name: a pointer to a string containing the name of the directory to
 *        create.
 * @parent: a pointer to the parent dentry for this file.  This should be a
 *          directory dentry if set.  If this parameter is NULL, then the
 *          directory will be created in the root of the tracefs filesystem.
 *
 * This function creates the top of the trace event directory.
 */
struct dentry *eventfs_create_events_dir(const char *name,
					 struct dentry *parent)
{
	struct dentry *dentry = tracefs_start_creating(name, parent);
	struct eventfs_inode *ei;
	struct tracefs_inode *ti;
	struct inode *inode;

	if (IS_ERR(dentry))
		return dentry;

	ei = kzalloc(sizeof(*ei), GFP_KERNEL);
	if (!ei)
		return ERR_PTR(-ENOMEM);
	inode = tracefs_get_inode(dentry->d_sb);
	if (unlikely(!inode)) {
		kfree(ei);
		tracefs_failed_creating(dentry);
		return ERR_PTR(-ENOMEM);
	}

	INIT_LIST_HEAD(&ei->e_top_files);

	ti = get_tracefs(inode);
	ti->flags |= TRACEFS_EVENT_INODE;
	ti->private = ei;

	inode->i_mode = S_IFDIR | S_IRWXU | S_IRUGO | S_IXUGO;
	inode->i_op = &eventfs_root_dir_inode_operations;
	inode->i_fop = &eventfs_file_operations;

	/* directory inodes start off with i_nlink == 2 (for "." entry) */
	inc_nlink(inode);
	d_instantiate(dentry, inode);
	inc_nlink(dentry->d_parent->d_inode);
	fsnotify_mkdir(dentry->d_parent->d_inode, dentry);
	return tracefs_end_creating(dentry);
}

/**
 * eventfs_add_subsystem_dir - add eventfs subsystem_dir to list to create later
 * @name: a pointer to a string containing the name of the file to create.
 * @parent: a pointer to the parent dentry for this dir.
 *
 * This function adds eventfs subsystem dir to list.
 * And all these dirs are created on the fly when they are looked up,
 * and the dentry and inodes will be removed when they are done.
 */
struct eventfs_file *eventfs_add_subsystem_dir(const char *name,
					       struct dentry *parent)
{
	struct tracefs_inode *ti_parent;
	struct eventfs_inode *ei_parent;
	struct eventfs_file *ef;

	if (!parent)
		return ERR_PTR(-EINVAL);

	ti_parent = get_tracefs(parent->d_inode);
	ei_parent = ti_parent->private;

	ef = eventfs_prepare_ef(name,
		S_IFDIR | S_IRWXU | S_IRUGO | S_IXUGO,
		&eventfs_file_operations,
		&eventfs_root_dir_inode_operations, NULL);

	if (IS_ERR(ef))
		return ef;

	mutex_lock(&eventfs_mutex);
	list_add_tail(&ef->list, &ei_parent->e_top_files);
	ef->d_parent = parent;
	mutex_unlock(&eventfs_mutex);
	return ef;
}

/**
 * eventfs_add_dir - add eventfs dir to list to create later
 * @name: a pointer to a string containing the name of the file to create.
 * @ef_parent: a pointer to the parent eventfs_file for this dir.
 *
 * This function adds eventfs dir to list.
 * And all these dirs are created on the fly when they are looked up,
 * and the dentry and inodes will be removed when they are done.
 */
struct eventfs_file *eventfs_add_dir(const char *name,
				     struct eventfs_file *ef_parent)
{
	struct eventfs_file *ef;

	if (!ef_parent)
		return ERR_PTR(-EINVAL);

	ef = eventfs_prepare_ef(name,
		S_IFDIR | S_IRWXU | S_IRUGO | S_IXUGO,
		&eventfs_file_operations,
		&eventfs_root_dir_inode_operations, NULL);

	if (IS_ERR(ef))
		return ef;

	mutex_lock(&eventfs_mutex);
	list_add_tail(&ef->list, &ef_parent->ei->e_top_files);
	ef->d_parent = ef_parent->dentry;
	mutex_unlock(&eventfs_mutex);
	return ef;
}

/**
 * eventfs_add_top_file - add event top file to list to create later
 * @name: a pointer to a string containing the name of the file to create.
 * @mode: the permission that the file should have.
 * @parent: a pointer to the parent dentry for this file.  This should be a
 *          directory dentry if set.  If this parameter is NULL, then the
 *          file will be created in the root of the tracefs filesystem.
 * @data: a pointer to something that the caller will want to get to later
 *        on.  The inode.i_private pointer will point to this value on
 *        the open() call.
 * @fop: a pointer to a struct file_operations that should be used for
 *        this file.
 *
 * This function adds top files of event dir to list.
 * And all these files are created on the fly when they are looked up,
 * and the dentry and inodes will be removed when they are done.
 */
int eventfs_add_top_file(const char *name, umode_t mode,
			 struct dentry *parent, void *data,
			 const struct file_operations *fop)
{
	struct tracefs_inode *ti;
	struct eventfs_inode *ei;
	struct eventfs_file *ef;

	if (!parent)
		return -EINVAL;

	if (!(mode & S_IFMT))
		mode |= S_IFREG;

	if (!parent->d_inode)
		return -EINVAL;

	ti = get_tracefs(parent->d_inode);
	if (!(ti->flags & TRACEFS_EVENT_INODE))
		return -EINVAL;

	ei = ti->private;
	ef = eventfs_prepare_ef(name, mode, fop, NULL, data);

	if (IS_ERR(ef))
		return -ENOMEM;

	mutex_lock(&eventfs_mutex);
	list_add_tail(&ef->list, &ei->e_top_files);
	ef->d_parent = parent;
	mutex_unlock(&eventfs_mutex);
	return 0;
}

/**
 * eventfs_add_file - add eventfs file to list to create later
 * @name: a pointer to a string containing the name of the file to create.
 * @mode: the permission that the file should have.
 * @ef_parent: a pointer to the parent eventfs_file for this file.
 * @data: a pointer to something that the caller will want to get to later
 *        on.  The inode.i_private pointer will point to this value on
 *        the open() call.
 * @fop: a pointer to a struct file_operations that should be used for
 *        this file.
 *
 * This function adds top files of event dir to list.
 * And all these files are created on the fly when they are looked up,
 * and the dentry and inodes will be removed when they are done.
 */
int eventfs_add_file(const char *name, umode_t mode,
		     struct eventfs_file *ef_parent,
		     void *data,
		     const struct file_operations *fop)
{
	struct eventfs_file *ef;

	if (!ef_parent)
		return -EINVAL;

	if (!(mode & S_IFMT))
		mode |= S_IFREG;

	ef = eventfs_prepare_ef(name, mode, fop, NULL, data);
	if (IS_ERR(ef))
		return -ENOMEM;

	mutex_lock(&eventfs_mutex);
	list_add_tail(&ef->list, &ef_parent->ei->e_top_files);
	ef->d_parent = ef_parent->dentry;
	mutex_unlock(&eventfs_mutex);
	return 0;
}

static LLIST_HEAD(free_list);

static void eventfs_workfn(struct work_struct *work)
{
	struct eventfs_file *ef, *tmp;
	struct llist_node *llnode;

	llnode = llist_del_all(&free_list);
	llist_for_each_entry_safe(ef, tmp, llnode, llist) {
		if (ef->created && ef->dentry)
			dput(ef->dentry);
		kfree(ef->name);
		kfree(ef->ei);
		kfree(ef);
	}
}

DECLARE_WORK(eventfs_work, eventfs_workfn);

static void free_ef(struct rcu_head *head)
{
	struct eventfs_file *ef = container_of(head, struct eventfs_file, rcu);

	if (!llist_add(&ef->llist, &free_list))
		return;

	queue_work(system_unbound_wq, &eventfs_work);
}

/**
 * eventfs_remove_rec - remove eventfs dir or file from list
 * @ef: a pointer to eventfs_file to be removed.
 *
 * This function recursively remove eventfs_file which
 * contains info of file or dir.
 */
static void eventfs_remove_rec(struct eventfs_file *ef, int level)
{
	struct eventfs_file *ef_child;

	if (!ef)
		return;
	/*
	 * Check recursion depth. It should never be greater than 3:
	 * 0 - events/
	 * 1 - events/group/
	 * 2 - events/group/event/
	 * 3 - events/group/event/file
	 */
	if (WARN_ON_ONCE(level > 3))
		return;

	if (ef->ei) {
		/* search for nested folders or files */
		list_for_each_entry_srcu(ef_child, &ef->ei->e_top_files, list,
					 lockdep_is_held(&eventfs_mutex)) {
			eventfs_remove_rec(ef_child, level + 1);
		}
	}

	if (ef->created && ef->dentry)
		d_invalidate(ef->dentry);

	list_del_rcu(&ef->list);
	call_srcu(&eventfs_srcu, &ef->rcu, free_ef);
}

/**
 * eventfs_remove - remove eventfs dir or file from list
 * @ef: a pointer to eventfs_file to be removed.
 *
 * This function acquire the eventfs_mutex lock and calls eventfs_remove_rec()
 */
void eventfs_remove(struct eventfs_file *ef)
{
	if (!ef)
		return;

	mutex_lock(&eventfs_mutex);
	eventfs_remove_rec(ef, 0);
	mutex_unlock(&eventfs_mutex);
}

/**
 * eventfs_remove_events_dir - remove eventfs dir or file from list
 * @dentry: a pointer to events's dentry to be removed.
 *
 * This function remove events main directory
 */
void eventfs_remove_events_dir(struct dentry *dentry)
{
	struct tracefs_inode *ti;
	struct eventfs_inode *ei;

	if (!dentry || !dentry->d_inode)
		return;

	ti = get_tracefs(dentry->d_inode);
	if (!ti || !(ti->flags & TRACEFS_EVENT_INODE))
		return;

	ei = ti->private;
	d_invalidate(dentry);
	dput(dentry);
	kfree(ei);
}

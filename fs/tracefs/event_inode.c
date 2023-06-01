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
#include <linux/security.h>
#include <linux/tracefs.h>
#include <linux/kref.h>
#include <linux/delay.h>
#include "internal.h"

/**
 * eventfs_dentry_to_rwsem - Return corresponding eventfs_rwsem
 * @dentry: a pointer to dentry
 *
 * helper function to return crossponding eventfs_rwsem for given dentry
 */
static struct rw_semaphore *eventfs_dentry_to_rwsem(struct dentry *dentry)
{
	if (S_ISDIR(dentry->d_inode->i_mode))
		return (struct rw_semaphore *)dentry->d_inode->i_private;
	else
		return (struct rw_semaphore *)dentry->d_parent->d_inode->i_private;
}

/**
 * eventfs_down_read - acquire read lock function
 * @eventfs_rwsem: a pointer to rw_semaphore
 *
 * helper function to perform read lock. Nested locking requires because
 * lookup(), release() requires read lock, these could be called directly
 * or from open(), remove() which already hold the read/write lock.
 */
static void eventfs_down_read(struct rw_semaphore *eventfs_rwsem)
{
	down_read_nested(eventfs_rwsem, SINGLE_DEPTH_NESTING);
}

/**
 * eventfs_up_read - release read lock function
 * @eventfs_rwsem: a pointer to rw_semaphore
 *
 * helper function to release eventfs_rwsem lock if locked
 */
static void eventfs_up_read(struct rw_semaphore *eventfs_rwsem)
{
	up_read(eventfs_rwsem);
}

/**
 * eventfs_down_write - acquire write lock function
 * @eventfs_rwsem: a pointer to rw_semaphore
 *
 * helper function to perform write lock on eventfs_rwsem
 */
static void eventfs_down_write(struct rw_semaphore *eventfs_rwsem)
{
	while (!down_write_trylock(eventfs_rwsem))
		msleep(10);
}

/**
 * eventfs_up_write - release write lock function
 * @eventfs_rwsem: a pointer to rw_semaphore
 *
 * helper function to perform write lock on eventfs_rwsem
 */
static void eventfs_up_write(struct rw_semaphore *eventfs_rwsem)
{
	up_write(eventfs_rwsem);
}

static const struct file_operations eventfs_file_operations = {
};

static const struct inode_operations eventfs_root_dir_inode_operations = {
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
 * @eventfs_rwsem: a pointer to rw_semaphore
 *
 * This function creates the top of the trace event directory.
 */
struct dentry *eventfs_create_events_dir(const char *name,
					 struct dentry *parent,
					 struct rw_semaphore *eventfs_rwsem)
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

	init_rwsem(eventfs_rwsem);
	INIT_LIST_HEAD(&ei->e_top_files);

	ti = get_tracefs(inode);
	ti->flags |= TRACEFS_EVENT_INODE;
	ti->private = ei;

	inode->i_mode = S_IFDIR | S_IRWXU | S_IRUGO | S_IXUGO;
	inode->i_op = &eventfs_root_dir_inode_operations;
	inode->i_fop = &eventfs_file_operations;
	inode->i_private = eventfs_rwsem;

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
 * @eventfs_rwsem: a pointer to rw_semaphore
 *
 * This function adds eventfs subsystem dir to list.
 * And all these dirs are created on the fly when they are looked up,
 * and the dentry and inodes will be removed when they are done.
 */
struct eventfs_file *eventfs_add_subsystem_dir(const char *name,
					       struct dentry *parent,
					       struct rw_semaphore *eventfs_rwsem)
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
		&eventfs_root_dir_inode_operations,
		(void *) eventfs_rwsem);

	if (IS_ERR(ef))
		return ef;

	eventfs_down_write(eventfs_rwsem);
	list_add_tail(&ef->list, &ei_parent->e_top_files);
	ef->d_parent = parent;
	eventfs_up_write(eventfs_rwsem);
	return ef;
}

/**
 * eventfs_add_dir - add eventfs dir to list to create later
 * @name: a pointer to a string containing the name of the file to create.
 * @ef_parent: a pointer to the parent eventfs_file for this dir.
 * @eventfs_rwsem: a pointer to rw_semaphore
 *
 * This function adds eventfs dir to list.
 * And all these dirs are created on the fly when they are looked up,
 * and the dentry and inodes will be removed when they are done.
 */
struct eventfs_file *eventfs_add_dir(const char *name,
				     struct eventfs_file *ef_parent,
				     struct rw_semaphore *eventfs_rwsem)
{
	struct eventfs_file *ef;

	if (!ef_parent)
		return ERR_PTR(-EINVAL);

	ef = eventfs_prepare_ef(name,
		S_IFDIR | S_IRWXU | S_IRUGO | S_IXUGO,
		&eventfs_file_operations,
		&eventfs_root_dir_inode_operations,
		(void *) eventfs_rwsem);

	if (IS_ERR(ef))
		return ef;

	eventfs_down_write(eventfs_rwsem);
	list_add_tail(&ef->list, &ef_parent->ei->e_top_files);
	ef->d_parent = ef_parent->dentry;
	eventfs_up_write(eventfs_rwsem);
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
	struct rw_semaphore *eventfs_rwsem;

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

	eventfs_rwsem = (struct rw_semaphore *) parent->d_inode->i_private;
	eventfs_down_write(eventfs_rwsem);
	list_add_tail(&ef->list, &ei->e_top_files);
	ef->d_parent = parent;
	eventfs_up_write(eventfs_rwsem);
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
	struct rw_semaphore *eventfs_rwsem;

	if (!ef_parent)
		return -EINVAL;

	if (!(mode & S_IFMT))
		mode |= S_IFREG;

	ef = eventfs_prepare_ef(name, mode, fop, NULL, data);
	if (IS_ERR(ef))
		return -ENOMEM;

	eventfs_rwsem = (struct rw_semaphore *) ef_parent->data;
	eventfs_down_write(eventfs_rwsem);
	list_add_tail(&ef->list, &ef_parent->ei->e_top_files);
	ef->d_parent = ef_parent->dentry;
	eventfs_up_write(eventfs_rwsem);
	return 0;
}

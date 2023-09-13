/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TRACEFS_INTERNAL_H
#define _TRACEFS_INTERNAL_H

#include <linux/mutex.h>

enum {
	TRACEFS_EVENT_INODE		= BIT(1),
	TRACEFS_EVENT_TOP_INODE		= BIT(2),
};

struct eventfs_inode {
	struct list_head	e_top_files;
};

/*
 * struct eventfs_file - hold the properties of the eventfs files and
 *                       directories.
 * @name:	the name of the file or directory to create
 * @d_parent:   holds parent's dentry
 * @dentry:     once accessed holds dentry
 * @list:	file or directory to be added to parent directory
 * @ei:		list of files and directories within directory
 * @fop:	file_operations for file or directory
 * @iop:	inode_operations for file or directory
 * @data:	something that the caller will want to get to later on
 * @mode:	the permission that the file or directory should have
 */
struct eventfs_file {
	const char			*name;
	struct dentry			*d_parent;
	struct dentry			*dentry;
	struct list_head		list;
	struct eventfs_inode		*ei;
	const struct file_operations	*fop;
	const struct inode_operations	*iop;
	/*
	 * Union - used for deletion
	 * @del_list:	list of eventfs_file to delete
	 * @rcu:	eventfs_file to delete in RCU
	 * @is_freed:	node is freed if one of the above is set
	 */
	union {
		struct list_head	del_list;
		struct rcu_head		rcu;
		unsigned long		is_freed;
	};
	void				*data;
	umode_t				mode;
};

extern struct mutex eventfs_mutex;

struct tracefs_inode {
	unsigned long           flags;
	void                    *private;
	struct inode            vfs_inode;
};

static inline struct tracefs_inode *get_tracefs(const struct inode *inode)
{
	return container_of(inode, struct tracefs_inode, vfs_inode);
}

struct dentry *tracefs_start_creating(const char *name, struct dentry *parent);
struct dentry *tracefs_end_creating(struct dentry *dentry);
struct dentry *tracefs_failed_creating(struct dentry *dentry);
struct inode *tracefs_get_inode(struct super_block *sb);
struct dentry *eventfs_start_creating(const char *name, struct dentry *parent);
struct dentry *eventfs_failed_creating(struct dentry *dentry);
struct dentry *eventfs_end_creating(struct dentry *dentry);
void eventfs_set_ef_status_free(struct tracefs_inode *ti, struct dentry *dentry);

#endif /* _TRACEFS_INTERNAL_H */

#include "mod.h"

/*
	出于简化目的的假设:
	如果inode_disk.type == INODE_TYPE_DIR
	那么inode_disk.size <= BLOCKSIZE (只有inode_disk.index[0]有效)
	也就是说, 单个目录最多包含BLOCKSIZE / sizeof(dentry)个目录项

	另外, INODE_TYPE_DATA要求数据之间没有空隙
	但是对于INODE_TYPE_DIR来说是无法做到的(目录项的删除很常见)
	因此, ip->size代表block中已经使用的空间大小
*/


/*----------------dentry的查找、增加、删除操作-----------------*/

/*
	在目录ip中查找是否存在名字为name的目录项
	如果找到了返回目录项中存储的inode_num
	如果没找到返回INVALID_INODE_NUM
	注意: 调用者需要持有ip->slk
*/
uint32 dentry_search(inode_t* ip, char* name) {
	assert(sleeplock_holding(&ip->slk), "dentry_search: slk");
	assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_search: not dir!");
	if (ip->disk_info.index[0] == 0) return INVALID_INODE_NUM;
	buffer_t* buf = buffer_get(ip->disk_info.index[0]);
	for (uint32 i = 0;i < ip->disk_info.size / sizeof(dentry_t);i++) {
		dentry_t* de = (dentry_t*)(buf->data + i * sizeof(dentry_t));
		if (de->name[0] != 0 && strncmp(de->name, name, MAXLEN_FILENAME) == 0) {
			uint32 inode_num = de->inode_num;
			buffer_put(buf);
			return inode_num;
		}
	}
	buffer_put(buf);
	return INVALID_INODE_NUM;
}

/*
	在目录ip中寻找空闲槽位, 插入新的dentry
	如果成功插入则返回这个目录项的偏移量(还需要更新size)
	如果插入失败(没有空间/发生重名)返回-1
	注意: 调用者需要持有ip->slk
*/
uint32 dentry_create(inode_t* ip, uint32 inode_num, char* name) {
	assert(sleeplock_holding(&ip->slk), "dentry_create: slk");
	assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_create: not dir!");
	if (dentry_search(ip, name) != INVALID_INODE_NUM) return -1;
	uint32 len = strlen(name);
	if (len >= MAXLEN_FILENAME) return -1;
	if (ip->disk_info.index[0] == 0) {
		uint32 block_num = bitmap_alloc_block();
		if (block_num == -1) return -1;
		ip->disk_info.index[0] = block_num;
		ip->disk_info.size = 0;
	}
	buffer_t* buf = buffer_get(ip->disk_info.index[0]);
	for (uint32 i = 0;i < DENTRY_PER_BLOCK;i++) {
		dentry_t* de = (dentry_t*)(buf->data + i * sizeof(dentry_t));
		if (de->name[0] == 0) {
			de->inode_num = inode_num;
			memmove(de->name, name, len);
			de->name[len] = '\0';
			if (i * sizeof(dentry_t) >= ip->disk_info.size) {
				ip->disk_info.size = (i + 1) * sizeof(dentry_t);
				inode_rw(ip, true);
			}
			buffer_write(buf);
			buffer_put(buf);
			return i * sizeof(dentry_t);
		}
	}
	buffer_write(buf);
	buffer_put(buf);
	return -1;
}

/*
	在目录ip下删除名称为name的dentry, 返回它的inode_num
	如果匹配失败或者遇到非法情况返回INVALID_INODE_NUM
	注意: 调用者需要持有ip->slk
*/
uint32 dentry_delete(inode_t* ip, char* name) {
	assert(sleeplock_holding(&ip->slk), "dentry_delete: slk");
	assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_delete: not dir!");
	uint32 inode_num = dentry_search(ip, name);
	if (inode_num == INVALID_INODE_NUM) return INVALID_INODE_NUM;
	buffer_t* buf = buffer_get(ip->disk_info.index[0]);
	for (uint32 i = 0;i < ip->disk_info.size / sizeof(dentry_t);i++) {
		dentry_t* de = (dentry_t*)(buf->data + i * sizeof(dentry_t));
		if (de->name[0] != 0 && strncmp(de->name, name, MAXLEN_FILENAME) == 0) {
			uint32 inode_num = de->inode_num;
			memset(de, 0, sizeof(dentry_t));
			if (i == ip->disk_info.size / sizeof(dentry_t) - 1) {
				while (ip->disk_info.size > 0) {
					dentry_t* last_de = (dentry_t*)(buf->data + ip->disk_info.size - sizeof(dentry_t));
					if (last_de->name[0] != 0) break;
					ip->disk_info.size -= sizeof(dentry_t);
				}
				inode_rw(ip, true);
			}
			buffer_write(buf);
			buffer_put(buf);
			return inode_num;
		}
	}
	buffer_put(buf);
	return INVALID_INODE_NUM;
}

/* 输出目录中所有有效目录项的信息 (for debug) */
void dentry_print(inode_t* ip) {
	assert(sleeplock_holding(&ip->slk), "dentry_print: slk!");
	assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_print: not dir!");

	dentry_t* de;
	buffer_t* buf;

	if (ip->disk_info.index[0] == 0)
		panic("dentry_print: invalid index[0]!");

	printf("inode_num = %d, dentries:\n", ip->inode_num);

	buf = buffer_get(ip->disk_info.index[0]);
	for (de = (dentry_t*)(buf->data); de < (dentry_t*)(buf->data + BLOCK_SIZE); de++) {
		if (de->name[0] != 0) {
			printf("dentry: offset = %d, inode_num = %d, name = %s\n",
				(uint32)((uint8*)de - buf->data), de->inode_num, de->name);
		}
	}
	buffer_put(buf);

	printf("\n");
}

/*------------------从文件名到文件路径-----------------*/

/*
	Examples:
	get_element("a/bb/c", name) = "bb/c" + name = "a"
	get_element("///aa//bb", name) = "bb" + name = "aa"
	get_element("aaa", name) = "" + name = "aaa"
	get_element("", name) = NULL + name = ""
	get_element("//", name) = NULL + name = ""
*/
static char* get_element(char* path, char* name) {
	/* 跳过前置的'/' */
	while (*path == '/')
		path++;

	/* 如果遇到末尾了则返回 */
	if (*path == 0) {
		name[0] = 0;
		return NULL;
	}

	/* 记录起点位置 */
	char* start = path;

	/* 推进path直到遇到'/'或者到达末尾 */
	while (*path != '/' && *path != 0)
		path++;

	/* 提取到的name的长度 */
	int len = path - start;
	len = MIN(len, MAXLEN_FILENAME - 1);

	/* 设置name */
	memmove(name, start, len);
	name[len] = 0;

	/* 跳过后置的'/' */
	while (*path == '/') path++;

	return path;
}
/*
	根据文件路径(/A/B/C)查找对应inode(inode_B or inode_C)
	如果find_parent_inode == true, 返回父节点inode, name为下一级子节点的名字
	如果find_parent_inode == false, 返回子节点inode, name无意义
	如果失败返回NULL
*/
static inode_t* __path_to_inode(char* path, char* name, bool find_parent_inode) {
	inode_t* ip, * nxt;
	ip = (path[0] == '/') ? inode_get(ROOT_INODE) : inode_dup(myproc()->cwd);
	inode_lock(ip);
	char name_buf[MAXLEN_FILENAME];
	char* path_buf = get_element(path, name_buf);
	if (name_buf[0] == 0) {
		inode_unlock(ip);
		if (find_parent_inode) {
			name[0] = 0;
			inode_put(ip);
			return NULL;
		}
		else {
			return ip;
		}
	}
	while (1) {
		char next_name[MAXLEN_FILENAME];
		char* next_path = get_element(path_buf, next_name);
		if (next_path == NULL) {
			if (find_parent_inode) {
				int len = strlen(name_buf);
				memmove(name, name_buf, len);
				inode_unlock(ip);
				return ip;
			}
			else {
				uint32 inode_num = dentry_search(ip, name_buf);
				if (inode_num == INVALID_INODE_NUM) {
					inode_unlock(ip);
					inode_put(ip);
					return NULL;
				}
				nxt = inode_get(inode_num);
				inode_unlock(ip);
				inode_put(ip);
				return nxt;
			}
		}
		uint32 inode_num = dentry_search(ip, name_buf);
		if (inode_num == INVALID_INODE_NUM) {
			inode_unlock(ip);
			inode_put(ip);
			return NULL;
		}
		nxt = inode_get(inode_num);
		inode_lock(nxt);
		inode_unlock(ip);
		inode_put(ip);
		ip = nxt;
		path_buf = next_path;
		memmove(name_buf, next_name, MAXLEN_FILENAME);
	}
}

/*
	基于path寻找inode
	失败返回NULL
*/
inode_t* path_to_inode(char* path) {
	char name[MAXLEN_FILENAME];
	return __path_to_inode(path, name, false);
}

/*
	基于path寻找inode->parent, 将inode->name放入name
	失败返回NULL, 同时name无效
*/
inode_t* path_to_parent_inode(char* path, char* name) {
	return __path_to_inode(path, name, true);
}

/*
	在目录ip中基于inode_num搜索dentry的name
	找到了返回0并将name填入, 没找到返回-1
*/
uint32 dentry_search_2(inode_t* ip, uint32 inode_num, char* name) {
	assert(sleeplock_holding(&ip->slk), "dentry_search_2: slk");
	assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_search_2: not dir!");
	if (ip->disk_info.index[0] == 0) return -1;
	buffer_t* buf = buffer_get(ip->disk_info.index[0]);
	for (uint32 i = 0;i < ip->disk_info.size / sizeof(dentry_t);i++) {
		dentry_t* de = (dentry_t*)(buf->data + i * sizeof(dentry_t));
		if (de->name[0] != 0 && de->inode_num == inode_num) {
			memmove(name, de->name, MAXLEN_FILENAME);
			buffer_put(buf);
			return 0;
		}
	}
	buffer_put(buf);
	return -1;
}

/*
	传输目录中所有有效目录项到dst
	返回传输的目录项数量
*/
uint32 dentry_transmit(inode_t* ip, uint64 dst, uint32 len, bool is_user_dst) {
	assert(sleeplock_holding(&ip->slk), "dentry_transmit: slk");
	assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_transmit: not dir!");
	if (ip->disk_info.index[0] == 0) return 0;
	buffer_t* buf = buffer_get(ip->disk_info.index[0]);
	len /= sizeof(dentry_t);
	uint32 res = 0;
	for (uint32 i = 0;i < ip->disk_info.size / sizeof(dentry_t) && res < len;i++) {
		dentry_t* de = (dentry_t*)(buf->data + i * sizeof(dentry_t));
		if (de->name[0] != 0) {
			if (is_user_dst)
				uvm_copyout(myproc()->pgtbl, dst + res * sizeof(dentry_t), (uint64)de, sizeof(dentry_t));
			else
				memmove((void*)(dst + res * sizeof(dentry_t)), (void*)de, sizeof(dentry_t));
			res++;
		}
	}
	buffer_put(buf);
	return res;
}

/*
	将inode对应的完整路径填入path中
	成功返回偏移量(从path+offset开始有效), 失败返回-1
*/
uint32 inode_to_path(inode_t* ip, char* path, uint32 len) {
	uint32 pos = len - 1;
	path[pos] = '\0';
	inode_t* cur = ip;
	inode_dup(cur);
	while (cur->inode_num != ROOT_INODE) {
		inode_lock(cur);
		uint32 parent_inode_num = dentry_search(cur, "..");
		if (parent_inode_num == INVALID_INODE_NUM) {
			inode_unlock(cur);
			inode_put(cur);
			return -1;
		}
		inode_t* parent = inode_get(parent_inode_num);
		inode_lock(parent);
		char name[MAXLEN_FILENAME];
		if (dentry_search_2(parent, cur->inode_num, name) == -1) {
			inode_unlock(parent);
			inode_put(parent);
			inode_unlock(cur);
			inode_put(cur);
			return -1;
		}
		inode_unlock(parent);
		uint32 name_len = strlen(name);
		if (pos < name_len + 1) {
			inode_unlock(cur);
			inode_put(cur);
			inode_put(parent);
			return -1;
		}
		if (pos > 0) path[--pos] = '/';
		pos -= name_len;
		memmove(path + pos, name, name_len);
		inode_unlock(cur);
		inode_put(cur);
		cur = parent;

	}
	inode_put(cur);
	if (pos <= 0) return -1;
	if (pos > 0) path[--pos] = '/';
	return pos;
}

/*
	基于path创建新的inode
	成功返回inode, 失败返回NULL
*/
inode_t* path_create_inode(char* path, uint16 type, uint16 major, uint16 minor) {
	char name[MAXLEN_FILENAME];
	inode_t* parent = path_to_parent_inode(path, name);
	if (parent == NULL || name[0] == 0) {
		if (parent != NULL) inode_put(parent);
		return NULL;
	}
	inode_lock(parent);
	inode_t* ip = inode_create(type, major, minor);
	if (ip == NULL) {
		inode_unlock(parent);
		inode_put(parent);
		return NULL;
	}
	inode_lock(ip);
	if (dentry_create(parent, ip->inode_num, name) == -1) {
		inode_unlock(parent);
		inode_put(parent);
		ip->disk_info.nlink = 0;
		inode_unlock(ip);
		inode_put(ip);
		return NULL;
	}
	inode_unlock(parent);
	inode_put(parent);
	inode_unlock(ip);
	return ip;
}

/*
	构建文件硬链接
	成功返回0, 失败返回-1
*/
uint32 path_link(char* old_path, char* new_path) {
	inode_t* ip = path_to_inode(old_path);
	if (ip == NULL) return -1;
	inode_lock(ip);
	if (ip->disk_info.type == INODE_TYPE_DIR) {
		inode_unlock(ip);
		inode_put(ip);
		return -1;
	}
	inode_unlock(ip);
	char name[MAXLEN_FILENAME];
	inode_t* parent = path_to_parent_inode(new_path, name);
	if (parent == NULL || name[0] == 0) {
		inode_put(ip);
		if (parent != NULL) inode_put(parent);
		return -1;
	}
	inode_lock(parent);
	if (dentry_create(parent, ip->inode_num, name) == -1) {
		inode_unlock(parent);
		inode_put(parent);
		inode_put(ip);
		return -1;
	}
	inode_lock(ip);
	ip->disk_info.nlink++;
	inode_rw(ip, true);
	inode_unlock(ip);
	inode_unlock(parent);
	inode_put(parent);
	inode_put(ip);
	return 0;
}

/*
	解除文件硬链接
	成功返回0, 失败返回-1
*/
uint32 path_unlink(char* path) {
	char name[MAXLEN_FILENAME];
	inode_t* parent = path_to_parent_inode(path, name);
	if (parent == NULL || name[0] == 0) {
		if (parent != NULL) inode_put(parent);
		return -1;
	}
	inode_lock(parent);
	uint32 inode_num = dentry_delete(parent, name);
	if (inode_num == INVALID_INODE_NUM) {
		inode_unlock(parent);
		inode_put(parent);
		return -1;
	}
	inode_unlock(parent);
	inode_put(parent);
	inode_t* ip = inode_get(inode_num);
	inode_lock(ip);
	ip->disk_info.nlink--;
	inode_rw(ip, true);
	inode_unlock(ip);
	inode_put(ip);
	return 0;
}

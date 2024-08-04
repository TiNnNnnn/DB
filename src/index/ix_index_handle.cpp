/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_index_handle.h"
#include "ix_scan.h"
#include "math.h"

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 *
 * @return key_idx，范围为[0,num_key)，如果返回的key_idx==num_key，则表示target大于最后一个key
 * @note 返回key index（同时也是rid index），作为slot no
 */
int IxNodeHandle::lower_bound(const char *target) const {
    int left = 0;
    int right = page_hdr->num_key;
    while (left < right) {
        int mid = left + (right - left) / 2;
        int cmp = ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_);
        if (cmp < 0) {
            left = mid + 1; // 继续在右半部分查找
        } else {
            right = mid; // 继续在左半部分查找
        }
    }
    return left;
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 *
 * @return key_idx，范围为[1,num_key)，如果返回的key_idx=num_key，则表示target大于等于最后一个key
 * @note 注意此处的范围从1开始
 */
int IxNodeHandle::upper_bound(const char *target) const {
    int left = 1;
    int right = page_hdr->num_key;
    while (left < right) {
        int mid = left + (right - left) / 2;
        int cmp = ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_);
        if (cmp <= 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 * 值value作为传出参数，函数返回是否查找成功
 *
 * @param key 目标key
 * @param[out] value 传出参数，目标key对应的Rid
 * @return 目标key是否存在
 */
bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    // Todo:
    // 1. 在叶子节点中获取目标key所在位置
    // 2. 判断目标key是否存在
    // 3. 如果存在，获取key对应的Rid，并赋值给传出参数value
    // 提示：可以调用lower_bound()和get_rid()函数。

    int idx = lower_bound(key);
    if (idx < get_size() && ix_compare(get_key(idx), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        *value = get_rid(idx);
        return true;
    }
    return false;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 * @param key 目标key
 * @return page_id_t 目标key所在的孩子节点（子树）的存储页面编号
 */
page_id_t IxNodeHandle::internal_lookup(const char *key) {
    // Todo:
    // 1. 查找当前非叶子节点中目标key所在孩子节点（子树）的位置
    // 2. 获取该孩子节点（子树）所在页面的编号
    // 3. 返回页面编号

    // 1. 查找当前非叶子节点中目标key所在孩子节点（子树）的位置
    int idx = lower_bound(key);
    if( 0 == memcmp(key,keys + idx* file_hdr->col_tot_len_,file_hdr->col_tot_len_)){
        idx++;    
    }
    return value_at(idx);
}

/**
 * @brief 在指定位置插入n个连续的键值对
 * 将key的前n位插入到原来keys中的pos位置；将rid的前n位插入到原来rids中的pos位置
 *
 * @param pos 要插入键值对的位置
 * @param (key, rid) 连续键值对的起始地址，也就是第一个键值对，可以通过(key, rid)来获取n个键值对
 * @param n 键值对数量
 * @note [0,pos)           [pos,num_key)
 *                            key_slot
 *                            /      \
 *                           /        \
 *       [0,pos)     [pos,pos+n)   [pos+n,num_key+n)
 *                      key           key_slot
 */
void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    // Todo:
    // 1. 判断pos的合法性
    // 2. 通过key获取n个连续键值对的key值，并把n个key值插入到pos位置
    // 3. 通过rid获取n个连续键值对的rid值，并把n个rid值插入到pos位置
    // 4. 更新当前节点的键数量
    
    // 1. 判断pos的合法性
    assert(pos >= 0 && pos <= get_size());
    // 2. 移动原有的键值对来腾出插入位置
    // 移动keys数组中的数据
    int move_count = get_size() - pos;
    if (move_count > 0) {
        memmove(keys + (pos + n) * file_hdr->col_tot_len_, keys + pos * file_hdr->col_tot_len_, move_count * file_hdr->col_tot_len_);
    }
    // 3. 移动rids数组中的数据
    if (move_count > 0) {
        memmove(rids + (pos + n), rids + pos, move_count * sizeof(Rid));
    }
    // 4. 将新的键值对插入到腾出的插入位置
    for (int i = 0; i < n; ++i) {
        set_key(pos + i, key + i * file_hdr->col_tot_len_);
        set_rid(pos + i, rid[i]);
    }
    // 5. 更新当前节点的键数量
    set_size(get_size() + n);
}


void IxNodeHandle::insert_pairs(int key_pos,int rid_pos,const char *key,const Rid *rid,int n){
    // 1. 判断pos的合法性
    assert(key_pos >= 0 && key_pos <= get_size());
    assert(rid_pos >= 0 && rid_pos <= get_size()+1);
    // 2. 移动原有的键值对来腾出插入位置
    // 移动keys数组中的数据
    int key_move_count = get_size() - key_pos;
    if (key_move_count > 0) {
        memmove(keys + (key_pos + n) * file_hdr->col_tot_len_, keys + key_pos * file_hdr->col_tot_len_, key_move_count * file_hdr->col_tot_len_);
    }
    // 3. 移动rids数组中的数据
    int rid_move_count = get_size() + 1 - rid_pos;
    if (rid_move_count > 0) {
        memmove(rids + (rid_pos + n), rids + rid_pos, rid_move_count * sizeof(Rid));
    }
    // 4. 将新的键值对插入到腾出的插入位置
    for (int i = 0; i < n; ++i) {
        set_key(key_pos + i, key + i * file_hdr->col_tot_len_);
        set_rid(rid_pos + i, rid[i]);
    }
    // 5. 更新当前节点的键数量
    set_size(get_size() + n);
}

/**
 * @brief 用于在结点中插入单个键值对。
 * 函数返回插入后的键值对数量
 *
 * @param (key, value) 要插入的键值对
 * @return int 键值对数量
 */
int IxNodeHandle::insert(const char *key, const Rid &value) {
    // Todo:
    // 1. 查找要插入的键值对应该插入到当前节点的哪个位置
    // 2. 如果key重复则不插入
    // 3. 如果key不重复则插入键值对
    // 4. 返回完成插入操作之后的键值对数量

    int pos = lower_bound(key);
    if (pos < get_size() && ix_compare(get_key(pos), key, file_hdr->col_types_,file_hdr->col_lens_) == 0) {
        return get_size();
    }
    insert_pairs(pos, key, &value, 1);
    return get_size();
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 *
 * @param pos 要删除键值对的位置
 */
void IxNodeHandle::erase_pair(int pos) {
    // Todo:
    // 1. 删除该位置的key
    // 2. 删除该位置的rid
    // 3. 更新结点的键值对数量
    int size = file_hdr->btree_order_+1; //btree_order+1
    
    // 1. 删除该位置的key，将从pos+1开始的所有key向前移动一位，覆盖pos位置的key
    memmove(keys + pos * file_hdr->col_tot_len_,
            keys + (pos + 1) * file_hdr->col_tot_len_,
            (size - pos - 1) * file_hdr->col_tot_len_);

    if(pos < size-1)
        memset(keys+(pos+1)*file_hdr->col_tot_len_,0,file_hdr->col_tot_len_);
    else if(pos == size-1)
        memset(keys+pos*file_hdr->col_tot_len_,0,file_hdr->col_tot_len_);

    // 2. 删除该位置的rid，将从pos+1开始的所有rid向前移动一位，覆盖pos位置的rid
    memmove(rids + pos*sizeof(Rid),rids + (pos + 1)*sizeof(Rid),(size - pos - 1) * sizeof(Rid));
    if(pos < size-1)
        memset(rids+(pos+1)*sizeof(Rid),0,sizeof(Rid));
    else if(pos == size -1)
        memset(rids+pos*sizeof(Rid),0,sizeof(Rid));

    set_size(get_size() - 1);
}

void IxNodeHandle::erase_keys(int pos){
    int size = file_hdr->btree_order_+1; //btree_order+1
    
    // 1. 删除该位置的key，将从pos+1开始的所有key向前移动一位，覆盖pos位置的key
    memmove(keys + pos * file_hdr->col_tot_len_,
            keys + (pos + 1) * file_hdr->col_tot_len_,
            (size - pos - 1) * file_hdr->col_tot_len_);

    if(pos < size-1)
        memset(keys+(pos+1)*file_hdr->col_tot_len_,0,file_hdr->col_tot_len_);
    else if(pos == size-1)
        memset(keys+pos*file_hdr->col_tot_len_,0,file_hdr->col_tot_len_);
    set_size(get_size() - 1);
}

/**
 * @brief 用于在结点中删除指定key的键值对。函数返回删除后的键值对数量
 *
 * @param key 要删除的键值对key值
 * @return 完成删除操作后的键值对数量
 */
int IxNodeHandle::remove(const char *key) {
    // Todo:
    // 1. 查找要删除键值对的位置
    // 2. 如果要删除的键值对存在，删除键值对
    // 3. 返回完成删除操作后的键值对数量
    int pos = lower_bound(key);
    if (pos < get_size() && ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        erase_pair(pos);
    }
    return get_size();
}

//构造IxIndexHandle
IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    // init file_hdr_
    //disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
    char* buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);

    // 创建 IxFileHdr 对象并反序列化文件头信息
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);
    
    // disk_manager管理的fd对应的文件中，设置从file_hdr_->num_pages开始分配page_no
    int now_page_no = disk_manager_->get_fd2pageno(fd);
    disk_manager_->set_fd2pageno(fd, now_page_no + 1);
}


//TODO: 06.11 根据operation指定不同的加锁策略，现在只适合FIND
/**
 * @brief 用于查找指定键所在的叶子结点
 * @param key 要查找的目标key值
 * @param operation 查找到目标键值对后要进行的操作类型
 * @param transaction 事务参数，如果不需要则默认传入nullptr
 * @return [leaf node] and [root_is_latched] 返回目标叶子结点以及根结点是否加锁
 * ######@note need to Unlatch and unpin the leaf node outside!
 * 注意：用了FindLeafPage之后一定要unlatch叶结点，否则下次latch该结点会堵塞！
 */
std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                            Transaction *transaction, bool find_first) {
    // Todo:
    // 1. 获取根节点
    // 2. 从根节点开始不断向下查找目标key
    // 3. 找到包含该key值的叶子结点停止查找，并返回叶子节点

    // 1. 获取根节点
    page_id_t root_page_id = file_hdr_->root_page_;
    IxNodeHandle *current_node = fetch_node(root_page_id);
    current_node->latch(); // 加锁当前节点

    auto root_node = current_node;
    // 2. 从根节点开始不断向下查找目标key
    while (!current_node->is_leaf_page()) {
        // 当前节点不是叶子节点，需要继续向下查找
        // 首先获取当前节点的第一个键和对应的子节点
        page_id_t child_page_id = current_node->internal_lookup(key);
        // 获取下一个节点
        IxNodeHandle *next_node = fetch_node(child_page_id);
         // 加锁下一个节点
        next_node->latch();
        buffer_pool_manager_->unpin_page(current_node->get_page_id(),true);
        // unlatch当前节点
        current_node->unlatch();
        current_node = next_node;
    }
    //transaction->append_index_latch_page_set();
    // 3. 返回叶子节点和根节点是否加锁的信息
    return std::make_pair(current_node, false);
}


/**
 * @brief 用于查找指定键在叶子结点中的对应的值result
 *
 * @param key 查找的目标key值
 * @param result 用于存放结果的容器
 * @param transaction 事务指针
 * @return bool 返回目标键值对是否存在
 */
bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    // Todo:
    // 1. 获取目标key值所在的叶子结点
    // 2. 在叶子节点中查找目标key值的位置，并读取key对应的rid
    // 3. 把rid存入result参数中
    // 提示：使用完buffer_pool提供的page之后，记得unpin page；记得处理并发的上锁

    // 1. 获取目标key值所在的叶子结点
    std::pair<IxNodeHandle *, bool> leaf_root_pair = find_leaf_page(key, Operation::FIND, transaction);
    IxNodeHandle *leaf_node = leaf_root_pair.first;
    bool root_is_latched = leaf_root_pair.second;

    // 2. 在叶子节点中查找目标key值的位置，并读取key对应的rid
    bool key_exists = false;
    Rid *value = nullptr;
    if (leaf_node->leaf_lookup(key, &value)) {
        key_exists = true;
        if (value != nullptr) {
            result->push_back(*value);
        }
    }
    buffer_pool_manager_->unpin_page(leaf_node->get_page_id(),false);

    // unlatch叶子节点
    leaf_node->unlatch();
    return key_exists;
}

/**
 * @brief  将传入的一个node拆分(Split)成两个结点，在node的右边生成一个新结点new node
 * @param node 需要拆分的结点
 * @return 拆分得到的new_node
 * @note need to unpin the new node outside
 * 注意：本函数执行完毕后，原node和new node都需要在函数外面进行unpin
 */
IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node) {
    // Todo:
    // 1. 将原结点的键值对平均分配，右半部分分裂为新的右兄弟结点
    //    需要初始化新节点的page_hdr内容
    // 2. 如果新的右兄弟结点是叶子结点，更新新旧节点的prev_leaf和next_leaf指针
    //    为新节点分配键值对，更新旧节点的键值对数记录
    // 3. 如果新的右兄弟结点不是叶子结点，更新该结点的所有孩子结点的父节点信息(使用IxIndexHandle::maintain_child())
    
    // 创建一个新的右兄弟结点
    PageId pid(fd_,INVALID_PAGE_ID);
    Page *new_page = buffer_pool_manager_->new_page(&pid);
    IxNodeHandle *new_node = new IxNodeHandle(file_hdr_, new_page);
    file_hdr_->num_pages_++;

    int mid_idx = node->get_size() / 2;
    if (node->is_leaf_page()) {// 如果要拆分的节点是叶子节点
        // 复制数据
        for (int i = mid_idx; i < node->get_size(); ++i) {
            new_node->insert_pair(i - mid_idx, node->get_key(i), *node->get_rid(i));
        }
        // 更新新节点的叶子节点链表指针
        new_node->set_next_leaf(node->get_next_leaf());
        new_node->set_prev_leaf(node->get_page_no()); 
        new_node->page_hdr->is_leaf = true;
        // 更新原节点的叶子节点链表指针和键值对数量
        node->set_next_leaf(new_node->get_page_no());
        //更新last_leaf_;
        if(node->get_page_no() == node->file_hdr->last_leaf_){
            file_hdr_->last_leaf_ = new_node->get_page_no();
        }
    } else { // 如果要拆分的节点是非叶子节点
        new_node->page_hdr->is_leaf = false;
        // 将原节点的右半部分复制到新节点中
        for (int i = mid_idx; i < node->get_size(); ++i) {
            new_node->insert_pair(i - mid_idx, node->get_key(i), *node->get_rid(i+1));
            //new_node->insert_pairs(i-mid_idx,i-mid_idx,node->get_key(i),node->get_rid(i+1),1);
        }
        
        // 更新原节点的右半部分的父节点信息为新节点
        for (int i = mid_idx + 1; i < node->get_size() + 1; ++i) {
            IxNodeHandle *child = fetch_node(node->value_at(i));
            child->latch();
            child->set_parent_page_no(new_node->get_page_no());
            child->unlatch();
            buffer_pool_manager_->unpin_page(child->get_page_id(),true);
        }
    }

    // 删除原节点中的右半部分的键值对
    for (int i = node->file_hdr->btree_order_; i >= mid_idx; --i) {
        node->erase_pair(i);
    }
    // 返回拆分得到的新结点
    return new_node;
}

/**
 * @brief Insert key & value pair into internal page after split
 * 拆分(Split)后，向上找到old_node的父结点
 * 将new_node的第一个key插入到父结点，其位置在 父结点指向old_node的孩子指针 之后
 * 如果插入后>=maxsize，则必须继续拆分父结点，然后在其父结点的父结点再插入，即需要递归
 * 直到找到的old_node为根结点时，结束递归（此时将会新建一个根R，关键字为key，old_node和new_node为其孩子）
 *
 * @param (old_node, new_node) 原结点为old_node，old_node被分裂之后产生了新的右兄弟结点new_node
 * @param key 要插入parent的key
 * @note 一个结点插入了键值对之后需要分裂，分裂后左半部分的键值对保留在原结点，在参数中称为old_node，
 * 右半部分的键值对分裂为新的右兄弟节点，在参数中称为new_node（参考Split函数来理解old_node和new_node）
 * @note 本函数执行完毕后，new node和old node都需要在函数外面进行unpin
 */
void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                     Transaction *transaction) {
    // Todo:
    // 1. 分裂前的结点（原结点, old_node）是否为根结点，如果为根结点需要分配新的root
    // 2. 获取原结点（old_node）的父亲结点
    // 3. 获取key对应的rid，并将(key, rid)插入到父亲结点
    // 4. 如果父亲结点仍需要继续分裂，则进行递归插入
    // 提示：记得unpin page

    // 1. 分裂前的结点（原结点, old_node）是否为根结点，如果为根结点需要分配新的root
    if (old_node->is_root_page()) {
        // 创建新的根节点
        PageId pid(fd_,INVALID_PAGE_ID);
        Page *new_root_page = buffer_pool_manager_->new_page(&pid);
        IxNodeHandle *new_root_node = new IxNodeHandle(file_hdr_, new_root_page);
        file_hdr_->num_pages_++;
        new_root_node->latch();
        // 初始化新根节点
        new_root_node->set_size(1);
        new_root_node->set_key(0, key);
        new_root_node->set_rid(0, {old_node->get_page_no(), 0});
        new_root_node->set_rid(1, {new_node->get_page_no(), 0});

        new_root_node->set_parent_page_no(INVALID_PAGE_ID);
        new_root_node->set_next_leaf(INVALID_PAGE_ID);
        new_root_node->set_prev_leaf(INVALID_PAGE_ID);
        new_root_node->page_hdr->is_leaf = false;

        // 更新旧根节点和新节点的父节点指针
        old_node->set_parent_page_no(new_root_node->get_page_no());
        new_node->set_parent_page_no(new_root_node->get_page_no());

        if(!old_node->is_leaf_page()){
            //删除new_node的第一个key
            new_node->erase_keys(0);
        }
        // 更新文件头的根节点指针
        file_hdr_->root_page_ = new_root_node->get_page_no();
        // 新根节点创建完成，返回
        new_root_node->unlatch();
        buffer_pool_manager_->unpin_page(new_root_node->get_page_id(), true);
        return;
    }
    // 2. 获取原结点（old_node）的父亲结点
    IxNodeHandle *parent_node = fetch_node(old_node->get_parent_page_no());
    parent_node->latch();

    // 更新新节点的父节点指针
    new_node->set_parent_page_no(parent_node->get_page_no());

    // 3. 获取key对应的rid，并将(key, rid)插入到父亲结点
    int insert_pos = parent_node->find_child(old_node);
    if(insert_pos == parent_node->page_hdr->num_key){
        Rid tmp_rid{new_node->get_page_no(), 0};
        parent_node->insert_pairs(insert_pos,insert_pos+1,key,&tmp_rid,1);
    }else{
        Rid tmp_rid{new_node->get_page_no(), 0};
        if(memcmp(parent_node->get_key(insert_pos),key,file_hdr_->col_tot_len_)>0){
            parent_node->insert_pairs(insert_pos,insert_pos+1,key,&tmp_rid,1);
        }else{
            parent_node->insert_pairs(insert_pos+1,insert_pos+1,key,&tmp_rid,1);
        }
        
        //parent_node->insert_pair(insert_pos + 1, key, {new_node->get_page_no(), 0});
    }

    if(!old_node->is_leaf_page()){
        //删除new_node的第一个key
        new_node->erase_keys(0);
    }
    
    // 4. 如果父亲结点仍需要继续分裂，则进行递归插入
    if (parent_node->get_size() >= parent_node->get_max_size()) {
        IxNodeHandle *new_sibling_node = split(parent_node);
        new_sibling_node->latch();
        //const char *new_key = parent_node->get_key(parent_node->get_size() - 1);
        insert_into_parent(parent_node, new_sibling_node->get_key(0), new_sibling_node, transaction);
        new_sibling_node->unlatch();
        buffer_pool_manager_->unpin_page(new_sibling_node->get_page_id(), true);
    }
    // 释放父节点
    parent_node->unlatch();
    buffer_pool_manager_->unpin_page(parent_node->get_page_id(), true);

}

/**
 * @brief 将指定键值对插入到B+树中
 * @param (key, value) 要插入的键值对
 * @param transaction 事务指针
 * @return page_id_t 插入到的叶结点的page_no
 */
page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    // Todo:
    // 1. 查找key值应该插入到哪个叶子节点
    // 2. 在该叶子节点中插入键值对
    // 3. 如果结点已满，分裂结点，并把新结点的相关信息插入父节点
    // 提示：记得unpin page；若当前叶子节点是最右叶子节点，则需要更新file_hdr_.last_leaf；记得处理并发的上锁
    
    // 1. 查找key值应该插入到哪个叶子节点
    auto [leaf_node, root_is_latched] = find_leaf_page(key, Operation::INSERT, transaction);

    // 2. 在该叶子节点中插入键值对
    int insert_result = leaf_node->insert(key, value);

    // 如果插入成功且节点未满
    if (insert_result <= file_hdr_->btree_order_) {
        if (leaf_node->get_page_no() == file_hdr_->last_leaf_) {
            // 如果当前叶子节点是最右叶子节点，则更新file_hdr_.last_leaf_
            file_hdr_->last_leaf_ = leaf_node->get_page_no();
        }
        buffer_pool_manager_->unpin_page(leaf_node->get_page_id(), true);
        leaf_node->unlatch();
        
        return leaf_node->get_page_no();
    }
    // 3. 如果结点已满，分裂节点
    IxNodeHandle *new_node = split(leaf_node);
    // 将新结点的相关信息插入父节点
    new_node->latch();
    insert_into_parent(leaf_node, new_node->get_key(0), new_node, transaction);
    new_node->unlatch();
    // 释放叶子节点和新节点
    leaf_node->unlatch();

    buffer_pool_manager_->unpin_page(leaf_node->get_page_id(), true);
    buffer_pool_manager_->unpin_page(new_node->get_page_id(), true);

    return leaf_node->get_page_no();
}

/**
 * @brief 用于删除B+树中含有指定key的键值对
 * @param key 要删除的key值
 * @param transaction 事务指针
 */
bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    // Todo:
    // 1. 获取该键值对所在的叶子结点
    // 2. 在该叶子结点中删除键值对
    // 3. 如果删除成功需要调用CoalesceOrRedistribute来进行合并或重分配操作，并根据函数返回结果判断是否有结点需要删除
    // 4. 如果需要并发，并且需要删除叶子结点，则需要在事务的delete_page_set中添加删除结点的对应页面；记得处理并发的上锁
    
    // 1. 获取该键值对所在的叶子结点
    auto [leaf_node, root_is_latched] = find_leaf_page(key, Operation::DELETE, transaction);

    // 加锁叶子节点
    //leaf_node->latch();

    // 2. 在该叶子结点中删除键值对
    int initial_size = leaf_node->get_size();
    leaf_node->remove(key);
    int new_size = leaf_node->get_size();

    // 如果删除失败（即键值对不存在），直接返回
    if (initial_size == new_size) {
        leaf_node->unlatch();
        buffer_pool_manager_->unpin_page(leaf_node->get_page_id(), false);
        return false;
    }

    // 3. 如果删除成功需要调用CoalesceOrRedistribute来进行合并或重分配操作
    bool result = coalesce_or_redistribute(leaf_node, transaction);
    // 释放叶子节点
    leaf_node->unlatch();
    buffer_pool_manager_->unpin_page(leaf_node->get_page_id(), true);

    // 4. 如果需要并发，并且需要删除叶子结点，则需要在事务的delete_page_set中添加删除结点的对应页面
    // if (result && transaction != nullptr) {
    //     transaction->append_index_deleted_page(leaf_node->page);
    // }

    return true;
}

/**
 * @brief 用于处理合并和重分配的逻辑，用于删除键值对后调用
 *
 * @param node 执行完删除操作的结点
 * @param transaction 事务指针
 * @param root_is_latched 传出参数：根节点是否上锁，用于并发操作
 * @return 是否需要删除结点
 * @note User needs to first find the sibling of input page.
 * If sibling's size + input page's size >= 2 * page's minsize, then redistribute.
 * Otherwise, merge(Coalesce).
 */
bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched) {
    // Todo:
    // 1. 判断node结点是否为根节点
    //    1.1 如果是根节点，需要调用AdjustRoot() 函数来进行处理，返回根节点是否需要被删除
    //    1.2 如果不是根节点，并且不需要执行合并或重分配操作，则直接返回false，否则执行2
    // 2. 获取node结点的父亲结点
    // 3. 寻找node结点的兄弟结点（优先选取前驱结点）
    // 4. 如果node结点和兄弟结点的键值对数量之和，能够支撑两个B+树结点（即node.size+neighbor.size >=
    // NodeMinSize*2)，则只需要重新分配键值对（调用Redistribute函数）
    // 5. 如果不满足上述条件，则需要合并两个结点，将右边的结点合并到左边的结点（调用Coalesce函数）

    // 1. 判断node结点是否为根节点
    if (node->is_root_page()) {
        // 1.1 如果是根节点，需要调用AdjustRoot() 函数来进行处理，返回根节点是否需要被删除
        return adjust_root(node);
    }
    
    // 1.2 如果不是根节点，并且不需要执行合并或重分配操作，则直接返回false，否则执行2
    if (node->get_size() >= node->get_min_size()) {
        return false;
    }

    // 2. 获取node结点的父亲结点
    IxNodeHandle *parent_node = fetch_node(node->get_parent_page_no());
    parent_node->latch();
    
    // 3. 寻找node结点的兄弟结点（优先选取前驱结点）
    IxNodeHandle *sibling_node = nullptr;
    int sibling_index = -1; // -1 表示前驱兄弟，0 表示后继兄弟
    int node_index = parent_node->find_child(node);

    if (node_index > 0) {
        // 优先选择前驱兄弟
        sibling_node = fetch_node(parent_node->value_at(node_index - 1));
        sibling_index = -1;
    } else {
        // 没有前驱兄弟则选择后继兄弟(最左节点)
        sibling_node = fetch_node(parent_node->value_at(node_index + 1));
        sibling_index = 0;
    }
    sibling_node->latch();

    // 4. 如果node结点和兄弟结点的键值对数量之和，能够支撑两个B+树结点（即node.size + sibling.size >= 2 * NodeMinSize），则只需要重新分配键值对（调用Redistribute函数）
    if (node->get_size() + sibling_node->get_size() >= 2 * node->get_min_size()) {
        redistribute(sibling_node, node, parent_node, sibling_index);
        sibling_node->unlatch();
        parent_node->unlatch();
        return false;
    }

    // 5. 如果不满足上述条件，则需要合并两个结点，将右边的结点合并到左边的结点（调用Coalesce函数）
    bool result = coalesce(&sibling_node, &node, &parent_node, sibling_index, transaction, root_is_latched);
    sibling_node->unlatch();
    parent_node->unlatch();
    
    return result;
}

/**
 * @brief 用于当根结点被删除了一个键值对之后的处理
 * @param old_root_node 原根节点
 * @return bool 根结点是否需要被删除
 * @note size of root page can be less than min size and this method is only called within coalesce_or_redistribute()
 */
bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    // Todo:
    // 1. 如果old_root_node是内部结点，并且大小为1，则直接把它的孩子更新成新的根结点
    // 2. 如果old_root_node是叶结点，且大小为0，则直接更新root page
    // 3. 除了上述两种情况，不需要进行操作
    
    // 1. 如果old_root_node是内部结点，并且大小为1，则直接把它的孩子更新成新的根结点
    if (!old_root_node->is_leaf_page() && old_root_node->get_size() == 1) {
        // 获取唯一的子结点
        page_id_t only_child_page_id = old_root_node->value_at(0);
        IxNodeHandle *only_child_node = fetch_node(only_child_page_id);
        only_child_node->latch();
        // 更新根结点
        file_hdr_->root_page_ = only_child_page_id;
        only_child_node->set_parent_page_no(INVALID_PAGE_ID);
        // 释放原根节点
        buffer_pool_manager_->unpin_page(old_root_node->get_page_id(), false);
        buffer_pool_manager_->delete_page(old_root_node->get_page_id());

        only_child_node->unlatch();
        buffer_pool_manager_->unpin_page(only_child_page_id, true);

        return true;
    }

    // 2. 如果old_root_node是叶结点，且大小为0，则直接更新root page
    if (old_root_node->is_leaf_page() && old_root_node->get_size() == 0) {
        // 更新根结点
        file_hdr_->root_page_ = INVALID_PAGE_ID;
        // 释放原根节点
        buffer_pool_manager_->unpin_page(old_root_node->get_page_id(), false);
        buffer_pool_manager_->delete_page(old_root_node->get_page_id());

        return true;
    }
    // 3. 除了上述两种情况，不需要进行操作
    return false;
}

/**
 * @brief 重新分配node和兄弟结点neighbor_node的键值对
 * Redistribute key & value pairs from one page to its sibling page. If index == 0, move sibling page's first key
 * & value pair into end of input "node", otherwise move sibling page's last key & value pair into head of input "node".
 * 
 * @param neighbor_node sibling page of input "node"
 * @param node input from method coalesceOrRedistribute()
 * @param parent the parent of "node" and "neighbor_node"
 * @param index node在parent中的rid_idx
 * @note node是之前刚被删除过一个key的结点
 * index=0，则neighbor是node后继结点，表示：node(left)      neighbor(right)
 * index>0，则neighbor是node前驱结点，表示：neighbor(left)  node(right)
 * 注意更新parent结点的相关kv对
 */
void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    // Todo:
    // 1. 通过index判断neighbor_node是否为node的前驱结点
    // 2. 从neighbor_node中移动一个键值对到node结点中
    // 3. 更新父节点中的相关信息，并且修改移动键值对对应孩字结点的父结点信息（maintain_child函数）
    // 注意：neighbor_node的位置不同，需要移动的键值对不同，需要分类讨论

    // 1. 判断neighbor_node是否为node的前驱结点
    if (index == 0) {
        // neighbor_node是node的后继结点
        // 从neighbor_node的开头移动一个键值对到node的末尾
        const char *redistributed_key = neighbor_node->get_key(0);
        Rid redistributed_rid = *neighbor_node->get_rid(0);
        // 在node中插入键值对
        node->insert_pair(node->get_size(), redistributed_key, redistributed_rid);
        neighbor_node->erase_pair(0);
        // 更新parent结点中node对应的键值对
        parent->set_key(index, neighbor_node->get_key(0));
        // 更新neighbor_node的孩子结点的父节点信息
        IxNodeHandle *child_node = fetch_node(redistributed_rid.page_no);
        child_node->set_parent_page_no(node->get_page_no());
    } else {
        // neighbor_node是node的前驱结点
        // 从neighbor_node的末尾移动一个键值对到node的开头
        const char *redistributed_key = neighbor_node->get_key(neighbor_node->get_size() - 1);
        Rid redistributed_rid = *neighbor_node->get_rid(neighbor_node->get_size() - 1);
        // 在node中插入键值对
        node->insert_pair(0, redistributed_key, redistributed_rid);
        neighbor_node->erase_pair(neighbor_node->get_size() - 1);
        // 更新parent结点中node对应的键值对
        parent->set_key(index - 1, redistributed_key);
        // 更新neighbor_node的孩子结点的父节点信息
        IxNodeHandle *child_node = fetch_node(redistributed_rid.page_no);
        child_node->set_parent_page_no(node->get_page_no());
    }
}

/**
 * @brief 合并(Coalesce)函数是将node和其直接前驱进行合并，也就是和它左边的neighbor_node进行合并；
 * 假设node一定在右边。如果上层传入的index=0，说明node在左边，那么交换node和neighbor_node，保证node在右边；合并到左结点，实际上就是删除了右结点；
 * Move all the key & value pairs from one page to its sibling page, and notify buffer pool manager to delete this page.
 * Parent page must be adjusted to take info of deletion into account. Remember to deal with coalesce or redistribute
 * recursively if necessary.
 * 
 * @param neighbor_node sibling page of input "node" (neighbor_node是node的前结点)
 * @param node input from method coalesceOrRedistribute() (node结点是需要被删除的)
 * @param parent parent page of input "node"
 * @param index node在parent中的rid_idx
 * @return true means parent node should be deleted, false means no deletion happend
 * @note Assume that *neighbor_node is the left sibling of *node (neighbor -> node)
 */
bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    IxNodeHandle *left_node = *neighbor_node;
    IxNodeHandle *right_node = *node;

    // 1. 用index判断neighbor_node是否为node的前驱结点，若不是则交换两个结点，让neighbor_node作为左结点，node作为右结点
    if (index == 0) {
        std::swap(left_node, right_node);
    }

    // 2. 把right_node结点的键值对移动到left_node中，并更新right_node结点孩子结点的父节点信息
    int left_node_size = left_node->get_size();
    int right_node_size = right_node->get_size();

    for (int i = 0; i < right_node_size; i++) {
        const char *key = right_node->get_key(i);
        Rid rid = *right_node->get_rid(i);
        left_node->insert_pair(left_node_size + i, key, rid);
        if (!right_node->is_leaf_page()) {
            IxNodeHandle *child_node = fetch_node(rid.page_no);
            child_node->set_parent_page_no(left_node->get_page_no());
        }
    }
    
    if(!right_node->is_leaf_page()){
        left_node->set_rid(left_node_size+right_node_size,*right_node->get_rid(right_node_size));
    }

    // 如果right_node是叶子结点且为最右叶子结点，需要更新file_hdr_.last_leaf
    if (right_node->is_leaf_page() && right_node->get_page_id().page_no == file_hdr_->last_leaf_) {
        file_hdr_->last_leaf_ = left_node->get_page_id().page_no;
    }

    //将待删除页面记录到txn中
    //transaction->append_index_deleted_page(right_node->page);

    // 3. 释放和删除right_node结点，并删除parent中right_node结点的信息，返回parent是否需要被删除
    buffer_pool_manager_->unpin_page(right_node->get_page_id(), true);
    buffer_pool_manager_->delete_page(right_node->get_page_id());

    //todo
    // 删除parent中right_node对应的信息
    (*parent)->erase_pair(index);

    // 检查parent是否需要删除
    if ((*parent)->get_size() < (*parent)->get_min_size()) {
        bool should_delete_parent = false;
        if ((*parent)->is_root_page()) {
            should_delete_parent = adjust_root(*parent);
        } else {
            // 解除对当前父节点的锁
            (*parent)->unlatch();
            buffer_pool_manager_->unpin_page((*parent)->get_page_id(), true);

            // 递归调用coalesce_or_redistribute，处理父节点
            should_delete_parent = coalesce_or_redistribute(*parent, transaction, root_is_latched);

            // 重新加锁父节点
            IxNodeHandle *relocked_parent = fetch_node((*parent)->get_page_id().page_no);
            relocked_parent->latch();
            *parent = relocked_parent;
        }
        return should_delete_parent;
    }

    // 解锁父节点
    (*parent)->unlatch();
    buffer_pool_manager_->unpin_page((*parent)->get_page_id(), true);
    return false;
}

/**
 * @brief 这里把iid转换成了rid，即iid的slot_no作为node的rid_idx(key_idx)
 * node其实就是把slot_no作为键值对数组的下标
 * 换而言之，每个iid对应的索引槽存了一对(key,rid)，指向了(要建立索引的属性首地址,插入/删除记录的位置)
 *
 * @param iid
 * @return Rid
 * @note iid和rid存的不是一个东西，rid是上层传过来的记录位置，iid是索引内部生成的索引槽位置
 */
Rid IxIndexHandle::get_rid(const Iid &iid) const {
    IxNodeHandle *node = fetch_node(iid.page_no);
    if (iid.slot_no >= node->get_size()) {
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        throw IndexEntryNotFoundError();
    }
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // unpin it!
    return *node->get_rid(iid.slot_no);
}

/**
 * @brief FindLeafPage + lower_bound
 *
 * @param key
 * @return Iid
 * @note 上层传入的key本来是int类型，通过(const char *)&key进行了转换
 * 可用*(int *)key转换回去
 */
Iid IxIndexHandle::lower_bound(const char *key) {
    // 找到包含key的叶子节点
    auto [leaf, is_root_latched] = find_leaf_page(key, Operation::FIND, nullptr, false);
    if (leaf == nullptr) {
        return Iid{-1, -1};
    }
    // 在叶子节点中找到第一个大于或等于key的位置
    int slot_no = leaf->lower_bound(key);

    // 返回Iid结构，包含页面ID和槽号
    Iid iid = {leaf->get_page_no(), slot_no};

    // 释放页面
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    leaf->unlatch();
    
    return iid;
}

/**
 * @brief FindLeafPage + upper_bound
 *
 * @param key
 * @return Iid
 */
Iid IxIndexHandle::upper_bound(const char *key) {
    // 找到包含key的叶子节点
    auto [leaf, is_root_latched] = find_leaf_page(key, Operation::FIND, nullptr, false);
    
    if (leaf == nullptr) {
        return Iid{-1, -1};
    }

    // 在叶子节点中找到第一个大于key的位置
    int slot_no = leaf->upper_bound(key);

    // 返回Iid结构，包含页面ID和槽号
    Iid iid = {leaf->get_page_no(), slot_no};

    // 释放页面
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    leaf->unlatch();
    
    return iid;
}

/**
 * @brief 指向最后一个叶子的最后一个结点的后一个
 * 用处在于可以作为IxScan的最后一个
 *
 * @return Iid
 */
Iid IxIndexHandle::leaf_end() const {
    IxNodeHandle *node = fetch_node(file_hdr_->last_leaf_);
    Iid iid = {.page_no = file_hdr_->last_leaf_, .slot_no = node->get_size()};
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // unpin it!
    return iid;
}

/**
 * @brief 指向第一个叶子的第一个结点
 * 用处在于可以作为IxScan的第一个
 *
 * @return Iid
 */
Iid IxIndexHandle::leaf_begin() const {
    Iid iid = {.page_no = file_hdr_->first_leaf_, .slot_no = 0};
    return iid;
}

/**
 * @brief 获取一个指定结点
 *
 * @param page_no
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 */
IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    IxNodeHandle *node = new IxNodeHandle(file_hdr_, page);
    
    return node;
}

/**
 * @brief 创建一个新结点
 *
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 * 注意：对于Index的处理是，删除某个页面后，认为该被删除的页面是free_page
 * 而first_free_page实际上就是最新被删除的页面，初始为IX_NO_PAGE
 * 在最开始插入时，一直是create node，那么first_page_no一直没变，一直是IX_NO_PAGE
 * 与Record的处理不同，Record将未插入满的记录页认为是free_page
 */
IxNodeHandle *IxIndexHandle::create_node() {
    IxNodeHandle *node;
    file_hdr_->num_pages_++;

    PageId new_page_id = {fd_, INVALID_PAGE_ID};
    // 从3开始分配page_no，第一次分配之后，new_page_id.page_no=3，file_hdr_.num_pages=4
    Page *page = buffer_pool_manager_->new_page(&new_page_id);
    node = new IxNodeHandle(file_hdr_, page);
    return node;
}

/**
 * @brief 从node开始更新其父节点的第一个key，一直向上更新直到根节点
 *
 * @param node
 */
void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    IxNodeHandle *curr = node;
    while (curr->get_parent_page_no() != IX_NO_PAGE) {
        // Load its parent
        IxNodeHandle *parent = fetch_node(curr->get_parent_page_no());
        int rank = parent->find_child(curr);
        char *parent_key = parent->get_key(rank);
        char *child_first_key = curr->get_key(0);
        if (memcmp(parent_key, child_first_key, file_hdr_->col_tot_len_) == 0) {
            assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
            break;
        }
        memcpy(parent_key, child_first_key, file_hdr_->col_tot_len_);  // 修改了parent node
        curr = parent;

        assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
    }
}

/**
 * @brief 要删除leaf之前调用此函数，更新leaf前驱结点的next指针和后继结点的prev指针
 *
 * @param leaf 要删除的leaf
 */
void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    assert(leaf->is_leaf_page());

    IxNodeHandle *prev = fetch_node(leaf->get_prev_leaf());
    prev->set_next_leaf(leaf->get_next_leaf());
    buffer_pool_manager_->unpin_page(prev->get_page_id(), true);

    IxNodeHandle *next = fetch_node(leaf->get_next_leaf());
    next->set_prev_leaf(leaf->get_prev_leaf());  // 注意此处是SetPrevLeaf()
    buffer_pool_manager_->unpin_page(next->get_page_id(), true);
}

/**
 * @brief 删除node时，更新file_hdr_.num_pages
 *
 * @param node
 */
void IxIndexHandle::release_node_handle(IxNodeHandle &node) {
    file_hdr_->num_pages_--;
}

/**
 * @brief 将node的第child_idx个孩子结点的父节点置为node
 */
void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (!node->is_leaf_page()) {
        //  Current node is inner node, load its child and set its parent to current node
        int child_page_no = node->value_at(child_idx);
        IxNodeHandle *child = fetch_node(child_page_no);
        child->set_parent_page_no(node->get_page_no());
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
    }
}
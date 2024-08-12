/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_file_handle.h"
#include "errors.h"

/**
 * @description: 获取当前表中记录号为rid的记录
 * @param {Rid&} rid 记录号，指定记录的位置
 * @param {Context*} context
 * @return {unique_ptr<RmRecord>} rid对应的记录对象指针
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    // Todo:
    // 1. 获取指定记录所在的page handle
    // 2. 初始化一个指向RmRecord的指针（赋值其内部的data和size）
    // 1. 获取指定记录所在的page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    // 2. 检查该位置是否存在记录
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        // 如果该位置没有记录，返回空指针
        return nullptr;
    }
    // 3. 初始化一个指向RmRecord的指针（赋值其内部的data和size）
    int record_size = file_hdr_.record_size;
    std::unique_ptr<RmRecord> record = std::make_unique<RmRecord>(record_size);
    char *record_data = page_handle.get_slot(rid.slot_no);
    std::memcpy(record->data, record_data, record_size);

    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(),false);
    return record;
}

/**
 * @description: 在当前表中插入一条记录，不指定插入位置
 * @param {char*} buf 要插入的记录的数据
 * @param {Context*} context
 * @return {Rid} 插入的记录的记录号（位置）
 */
Rid RmFileHandle::insert_record(char* buf, Context* context) {
    // Todo:
    // 1. 获取当前未满的page handle
    // 2. 在page handle中找到空闲slot位置
    // 3. 将buf复制到空闲slot位置
    // 4. 更新page_handle.page_hdr中的数据结构
    // 注意考虑插入一条记录后页面已满的情况，需要更新file_hdr_.first_free_page_no

    // 1. 获取当前未满的 page handle
    RmPageHandle page_handle = create_page_handle();
    // 2. 在 page handle 中找到空闲 slot 位置
    int slot_no = Bitmap::next_bit(0,page_handle.bitmap, file_hdr_.num_records_per_page,-1);
    // 3. 将数据 buf 复制到空闲 slot 位置
    char* slot_ptr = page_handle.get_slot(slot_no);
    memcpy(slot_ptr, buf, file_hdr_.record_size);
    Bitmap::set(page_handle.bitmap,slot_no);
    // 4. 更新 page handle 中的页头数据结构
    page_handle.page_hdr->num_records++;
    // 5. 如果插入记录后页面已满，更新文件头中的 first_free_page_no
    if (page_handle.page_hdr->num_records >= file_hdr_.num_records_per_page) {
        release_page_handle(page_handle);
    }

    page_handle.page->set_dirty(true);
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);

    return Rid{page_handle.page->get_page_id().page_no, slot_no};
}

/**
 * @description: 在当前表中的指定位置插入一条记录
 * @param {Rid&} rid 要插入记录的位置
 * @param {char*} buf 要插入记录的数据
 */
void RmFileHandle::insert_record(const Rid& rid, char* buf) {
    // 1. 获取指定页面的页面句柄
    auto page_handle = fetch_page_handle(rid.page_no);
    
    // 2. 检查指定位置是否已存在记录，如果存在则抛出异常
    if (Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        //throw std::runtime_error("The specified slot is already occupied.");
        std::cout<<"insert: The specified slot is already occupied.["<<rid.page_no<<","<<rid.slot_no<<"]"<<std::endl;
        return;
    }
    std::cout<<"insert:["<<rid.page_no<<","<<rid.slot_no<<"]"<<std::endl;
    // 3. 将buf复制到指定slot位置
    char* slot_addr = page_handle.get_slot(rid.slot_no);
    memcpy(slot_addr, buf, file_hdr_.record_size);
    // 4. 更新页面头部的bitmap和记录数
    Bitmap::set(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records++;
    // 5. 如果页面已满，则更新file_hdr_.first_free_page_no
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    }
    // 标记页面为脏页并unpin
    page_handle.page->set_dirty(true);
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

void RmFileHandle::insert_record_for_recovery(const Rid& rid, char* buf) {
    if (rid.page_no == INVALID_PAGE_ID || rid.page_no <0){
        throw PageNotExistError("tbname",rid.page_no);
    }
    PageId pid(fd_,rid.page_no);
    Page* page;
    RmPageHandle* page_handle;
    try
    {
        page = buffer_pool_manager_->fetch_page(pid);
        if (page == nullptr) {
            throw std::runtime_error("Failed to fetch page from buffer pool.");
        }
        page_handle = new RmPageHandle(&file_hdr_,page);
    }
    catch(const std::exception& e)
    {
        PageId pid(fd_,-1);
        page = buffer_pool_manager_->new_page(&pid,rid.page_no);

        RmPageHandle new_page_handle(&file_hdr_, page);
        RmPageHdr *page_hdr = new_page_handle.page_hdr;
        page_hdr->next_free_page_no = RM_NO_PAGE;
        page_hdr->num_records = 0;
        std::memset(new_page_handle.bitmap, 0, file_hdr_.bitmap_size);

        page_handle = new RmPageHandle(&file_hdr_,page);
    }
    if (Bitmap::is_set(page_handle->bitmap, rid.slot_no)) {
        //throw std::runtime_error("The specified slot is already occupied.");
        std::cout<<"insert: The specified slot is already occupied.["<<rid.page_no<<","<<rid.slot_no<<"]"<<std::endl;
        return;
    }
    std::cout<<"insert:["<<rid.page_no<<","<<rid.slot_no<<"]"<<std::endl;
    // 3. 将buf复制到指定slot位置
    char* slot_addr = page_handle->get_slot(rid.slot_no);
    memcpy(slot_addr, buf, file_hdr_.record_size);
    // 4. 更新页面头部的bitmap和记录数
    Bitmap::set(page_handle->bitmap, rid.slot_no);
    page_handle->page_hdr->num_records++;
    // 5. 如果页面已满，则更新file_hdr_.first_free_page_no
    if (page_handle->page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page_handle->page_hdr->next_free_page_no;
        page_handle->page_hdr->next_free_page_no = RM_NO_PAGE;
    }
    // 标记页面为脏页并unpin
    page_handle->page->set_dirty(true);
    buffer_pool_manager_->unpin_page(page_handle->page->get_page_id(), true);
}

/**
 * @description: 删除记录文件中记录号为rid的记录
 * @param {Rid&} rid 要删除的记录的记录号（位置）
 * @param {Context*} context
 */
void RmFileHandle::delete_record(const Rid& rid, Context* context) {
    // Todo:
    // 1. 获取指定记录所在的page handle
    // 2. 更新page_handle.page_hdr中的数据结构
    // 注意考虑删除一条记录后页面未满的情况，需要调用release_page_handle()

    // 1. 获取指定记录所在的page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);

    // 2. 检查指定位置是否有记录
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        //throw std::runtime_error("The specified slot is already empty.");
        std::cout<<"delete: The specified slot is already empty.["<<rid.page_no<<","<<rid.slot_no<<"]"<<std::endl;
        return;
    }
    std::cout<<"delete:["<<rid.page_no<<","<<rid.slot_no<<"]"<<std::endl;
    // 3. 将bitmap中指定位置的bit清除，并更新页面头部的记录数
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records--;

    char* slot = page_handle.get_slot(rid.slot_no);
    memset(slot, 0, file_hdr_.record_size);

    // 4. 如果页面在删除记录后变得未满，调用release_page_handle()进行处理
    if (page_handle.page_hdr->num_records < file_hdr_.num_records_per_page) {
        release_page_handle(page_handle);
    }

    // 标记页面为脏页并unpin
    page_handle.page->set_dirty(true);
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

void RmFileHandle::delete_record_for_recovery(const Rid &rid, Context *context){
    if (rid.page_no == INVALID_PAGE_ID || rid.page_no <0){
        throw PageNotExistError("tbname",rid.page_no);
    }
    PageId pid(fd_,rid.page_no);
    Page* page;
    RmPageHandle* page_handle;
    try
    {
        page = buffer_pool_manager_->fetch_page(pid);
        if (page == nullptr) {
            throw std::runtime_error("Failed to fetch page from buffer pool.");
        }
        page_handle = new RmPageHandle(&file_hdr_,page);
    }
    catch(const std::exception& e)
    {
        PageId pid(fd_,-1);
        page = buffer_pool_manager_->new_page(&pid,rid.page_no);

        RmPageHandle new_page_handle(&file_hdr_, page);
        RmPageHdr *page_hdr = new_page_handle.page_hdr;
        page_hdr->next_free_page_no = RM_NO_PAGE;
        page_hdr->num_records = 0;
        std::memset(new_page_handle.bitmap, 0, file_hdr_.bitmap_size);

        page_handle = new RmPageHandle(&file_hdr_,page);
    }
    // 2. 检查指定位置是否有记录
    if (!Bitmap::is_set(page_handle->bitmap, rid.slot_no)) {
        //throw std::runtime_error("The specified slot is already empty.");
        std::cout<<"delete: The specified slot is already empty.["<<rid.page_no<<","<<rid.slot_no<<"]"<<std::endl;
        return;
    }
    std::cout<<"delete:["<<rid.page_no<<","<<rid.slot_no<<"]"<<std::endl;
    // 3. 将bitmap中指定位置的bit清除，并更新页面头部的记录数
    Bitmap::reset(page_handle->bitmap, rid.slot_no);
    page_handle->page_hdr->num_records--;

    char* slot = page_handle->get_slot(rid.slot_no);
    memset(slot, 0, file_hdr_.record_size);

    // 4. 如果页面在删除记录后变得未满，调用release_page_handle()进行处理
    if (page_handle->page_hdr->num_records < file_hdr_.num_records_per_page) {
        release_page_handle(*page_handle);
    }

    // 标记页面为脏页并unpin
    page_handle->page->set_dirty(true);
    buffer_pool_manager_->unpin_page(page_handle->page->get_page_id(), true);
}

/**
 * @description: 更新记录文件中记录号为rid的记录
 * @param {Rid&} rid 要更新的记录的记录号（位置）
 * @param {char*} buf 新记录的数据
 * @param {Context*} context
 */
void RmFileHandle::update_record(const Rid& rid, char* buf, Context* context) {
    // Todo:
    // 1. 获取指定记录所在的page handle
    // 2. 更新记录
    // 1. 获取指定记录所在的page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    // 2. 检查指定位置是否有记录
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        //throw std::runtime_error("The specified slot does not contain a record.");
        std::cout<<"update: The specified slot does not contain a record.["<<rid.page_no<<","<<rid.slot_no<<"]"<<std::endl;
        return;
    }
    std::cout<<"update:["<<rid.page_no<<","<<rid.slot_no<<"]"<<std::endl;
    // 3. 更新指定slot位置的数据
    char* slot = page_handle.get_slot(rid.slot_no);
    //memset(slot,0,file_hdr_.record_size);
    memcpy(slot, buf, file_hdr_.record_size);
    // 4. 标记页面为脏页
   page_handle.page->set_dirty(true);
   buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

void RmFileHandle::update_record_for_recovery(const Rid &rid, char *buf, Context *context){
    if (rid.page_no == INVALID_PAGE_ID || rid.page_no <0){
        throw PageNotExistError("tbname",rid.page_no);
    }
    PageId pid(fd_,rid.page_no);
    Page* page;
    RmPageHandle* page_handle;
    try
    {
        page = buffer_pool_manager_->fetch_page(pid);
        if (page == nullptr) {
            throw std::runtime_error("Failed to fetch page from buffer pool.");
        }
        page_handle = new RmPageHandle(&file_hdr_,page);
    }
    catch(const std::exception& e)
    {
        PageId pid(fd_,-1);
        page = buffer_pool_manager_->new_page(&pid,rid.page_no);

        RmPageHandle new_page_handle(&file_hdr_, page);
        RmPageHdr *page_hdr = new_page_handle.page_hdr;
        page_hdr->next_free_page_no = RM_NO_PAGE;
        page_hdr->num_records = 0;
        std::memset(new_page_handle.bitmap, 0, file_hdr_.bitmap_size);

        page_handle = new RmPageHandle(&file_hdr_,page);
    }
    // 2. 检查指定位置是否有记录
    if (!Bitmap::is_set(page_handle->bitmap, rid.slot_no)) {
        //throw std::runtime_error("The specified slot does not contain a record.");
        std::cout<<"update: The specified slot does not contain a record.["<<rid.page_no<<","<<rid.slot_no<<"]"<<std::endl;
        return;
    }
    std::cout<<"update:["<<rid.page_no<<","<<rid.slot_no<<"]"<<std::endl;
    // 3. 更新指定slot位置的数据
    char* slot = page_handle->get_slot(rid.slot_no);
    //memset(slot,0,file_hdr_.record_size);
    memcpy(slot, buf, file_hdr_.record_size);
    // 4. 标记页面为脏页
   page_handle->page->set_dirty(true);
   buffer_pool_manager_->unpin_page(page_handle->page->get_page_id(), true);
}
/**
 * 以下函数为辅助函数，仅提供参考，可以选择完成如下函数，也可以删除如下函数，在单元测试中不涉及如下函数接口的直接调用
*/
/**
 * @description: 获取指定页面的页面句柄
 * @param {int} page_no 页面号
 * @return {RmPageHandle} 指定页面的句柄
 */
RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    // Todo:
    // 使用缓冲池获取指定页面，并生成page_handle返回给上层
    // if page_no is invalid, throw PageNotExistError exception
    if (page_no == INVALID_PAGE_ID || page_no <0){
        throw PageNotExistError("tbname",page_no);
    }
    PageId pid(fd_,page_no);
    Page* page = buffer_pool_manager_->fetch_page(pid);
    if (page == nullptr) {
        throw std::runtime_error("Failed to fetch page from buffer pool.");
    }
    return RmPageHandle(&file_hdr_, page);
}

/**
 * @description: 创建一个新的page handle
 * @return {RmPageHandle} 新的PageHandle
 */
RmPageHandle RmFileHandle::create_new_page_handle() {
    // Todo:
    // 1.使用缓冲池来创建一个新page
    // 2.更新page handle中的相关信息
    // 3.更新file_hdr_
    
    // 1. 使用缓冲池来创建一个新page
    PageId new_page_id(fd_,-1);
    int new_page_no = file_hdr_.num_pages;
    Page *new_page = buffer_pool_manager_->new_page(&new_page_id,new_page_no);
    if (new_page == nullptr) {
        throw std::runtime_error("Failed to create a new page in the buffer pool.");
    }

    // 2. 初始化新页面的页头信息
    RmPageHandle new_page_handle(&file_hdr_, new_page);
    RmPageHdr *page_hdr = new_page_handle.page_hdr;
    page_hdr->next_free_page_no = RM_NO_PAGE;
    page_hdr->num_records = 0;
    std::memset(new_page_handle.bitmap, 0, file_hdr_.bitmap_size);

    // 4. 更新file_hdr_以反映新页面的创建
    file_hdr_.num_pages++;
    if (file_hdr_.first_free_page_no == RM_NO_PAGE) {
        file_hdr_.first_free_page_no = new_page_id.page_no;
    }

    // 5. 将文件头写回磁盘
    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, reinterpret_cast<char *>(&file_hdr_), sizeof(file_hdr_));
    return new_page_handle;
}

/**
 * @brief 创建或获取一个空闲的page handle
 *
 * @return RmPageHandle 返回生成的空闲page handle
 * @note pin the page, remember to unpin it outside!
 */
RmPageHandle RmFileHandle::create_page_handle() {
    // Todo:
    // 1. 判断file_hdr_中是否还有空闲页
    //     1.1 没有空闲页：使用缓冲池来创建一个新page；可直接调用create_new_page_handle()
    //     1.2 有空闲页：直接获取第一个空闲页
    // 2. 生成page handle并返回给上层

    // 1. 判断file_hdr_中是否还有空闲页
    if (file_hdr_.first_free_page_no == RM_NO_PAGE) {
        // 1.1 没有空闲页：使用缓冲池来创建一个新page；可直接调用create_new_page_handle()
        return create_new_page_handle();
    } else {
        // 1.2 有空闲页：直接获取第一个空闲页
        RmPageHandle page_handle = fetch_page_handle(file_hdr_.first_free_page_no);
        // 更新file_hdr_以反映被使用的空闲页
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        return page_handle;
    }
}

/**
 * @description: 当一个页面从没有空闲空间的状态变为有空闲空间状态时，更新文件头和页头中空闲页面相关的元数据
 */
void RmFileHandle::release_page_handle(RmPageHandle&page_handle) {
    // Todo:
    // 当page从已满变成未满，考虑如何更新：
    // 1. page_handle.page_hdr->next_free_page_no
    // 2. file_hdr_.first_free_page_no

    // 更新页头中的 next_free_page_no，使其指向之前被标记为第一个有空闲空间的页面
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    // 更新文件头中的 first_free_page_no，使其指向当前页面
    file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
}

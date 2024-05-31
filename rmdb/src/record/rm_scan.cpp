/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"
#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    // Todo:
    // 初始化file_handle和rid（指向第一个存放了记录的位置）
    // 初始化 rid_ 为无效值，以便开始扫描
    rid_.page_no = RM_NO_PAGE;
    rid_.slot_no = -1;

    // 调用 next() 函数查找第一个有效记录
    next();
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    // Todo:
    // 找到文件中下一个存放了记录的非空闲位置，用rid_来指向这个位置
    // 获取文件头信息
    const RmFileHdr &file_hdr = file_handle_->file_hdr_;

    // 开始搜索的页面和槽位
    int start_page = rid_.page_no;
    int start_slot = rid_.slot_no;

    // 如果当前rid_是无效的，初始化为第一个记录页面和槽位
    if (start_page == RM_NO_PAGE) {
        start_page = RM_FIRST_RECORD_PAGE - 1; // 从第0页开始，以便在循环内增量到第1页
        start_slot = -1;
    }

    // 遍历页面
    for (int page_no = start_page; page_no < file_hdr.num_pages; ++page_no) {
        RmPageHandle page_handle = file_handle_->fetch_page_handle(page_no);

        // 遍历槽位
        int start_slot_no = (page_no == start_page) ? start_slot + 1 : 0;
        for (int slot_no = start_slot_no; slot_no < file_hdr.num_records_per_page; ++slot_no) {
            // 检查是否存在记录
            if (Bitmap::is_set(page_handle.bitmap, slot_no)) {
                rid_.page_no = page_no;
                rid_.slot_no = slot_no;
                return;
            }
        }

        // 释放页面
        file_handle_->buffer_pool_manager_->unpin_page(PageId{file_handle_->fd_, page_no}, false);
    }

    // 如果没有找到有效记录，设置rid_为无效值
    rid_.page_no = RM_NO_PAGE;
    rid_.slot_no = -1;
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    // Todo: 修改返回值
   return (rid_.page_no == RM_NO_PAGE && rid_.slot_no == -1);
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}
/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"
#include "errors.h"

/**
 * @description: 判断是否为一个文件夹
 * @return {bool} 返回是否为一个文件夹
 * @param {string&} db_name 数据库文件名称，与文件夹同名
 */
bool SmManager::is_dir(const std::string& db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @description: 创建数据库，所有的数据库相关文件都放在数据库同名文件夹下
 * @param {string&} db_name 数据库名称
 */
void SmManager::create_db(const std::string& db_name) {
    if (is_dir(db_name)) {
        throw DatabaseExistsError(db_name);
    }
    //为数据库创建一个子目录
    std::string cmd = "mkdir " + db_name;
    if (system(cmd.c_str()) < 0) {  // 创建一个名为db_name的目录
        throw UnixError();
    }
    if (chdir(db_name.c_str()) < 0) {  // 进入名为db_name的目录
        throw UnixError();
    }
    //创建系统目录
    DbMeta *new_db = new DbMeta();
    new_db->name_ = db_name;

    // 注意，此处ofstream会在当前目录创建(如果没有此文件先创建)和打开一个名为DB_META_NAME的文件
    std::ofstream ofs(DB_META_NAME);

    // 将new_db中的信息，按照定义好的operator<<操作符，写入到ofs打开的DB_META_NAME文件中
    ofs << *new_db;  // 注意：此处重载了操作符<<
    delete new_db;

    // 创建日志文件
    disk_manager_->create_file(LOG_FILE_NAME);

    // 创建启动文件
    disk_manager_->create_file(START_FILE_NAME);
    int start_fd = disk_manager_->open_file(START_FILE_NAME);
    disk_manager_->SetStartFd(start_fd);
    
    // //第一次启动，checkpoint_lsn = -1
    //char buf[sizeof(lsn_t)] = {-1};
    //memcpy(buf,0,sizeof(lsn_t));

    lsn_t value = -1; // 原始值
    char buf[sizeof(lsn_t)];
    std::memcpy(buf, &value, sizeof(lsn_t));

    disk_manager_->write_start_file(buf,sizeof(lsn_t));
    disk_manager_->close_file(start_fd);

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除数据库，同时需要清空相关文件以及数据库同名文件夹
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::drop_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    std::string cmd = "rm -r " + db_name;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 打开数据库，找到数据库对应的文件夹，并加载数据库元数据和相关文件
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::open_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    if (chdir(db_name.c_str()) < 0) {  // 进入数据库目录
        throw UnixError();
    }
    
    // 从磁盘读取数据库元数据
    std::ifstream ifs(DB_META_NAME);
    if (!ifs.is_open()) {
        throw UnixError();
    }
    ifs >> db_;  // 使用重载的操作符>>将文件内容读入到db_对象中
    ifs.close();

    //打开日志文件
    int log_fd = disk_manager_->open_file(LOG_FILE_NAME);
    disk_manager_->SetLogFd(log_fd);

    //打开启动文件
    int start_fd = disk_manager_->open_file(START_FILE_NAME);
    disk_manager_->SetStartFd(start_fd);

    // 打开每个表的数据文件和索引文件
    for (auto& tab_entry : db_.tabs_) {
        const std::string& tab_name = tab_entry.first;
        fhs_[tab_name] = rm_manager_->open_file(tab_name);

        TabMeta tb_meta = tab_entry.second;
        for(auto &idx : tb_meta.indexes){
            std::string index_name = ix_manager_->get_index_name(tab_name, idx.cols);
            ihs_[index_name] = ix_manager_->open_index(tab_name, idx.cols);
        }
    }
    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 把数据库相关的元数据刷入磁盘中
 */
void SmManager::flush_meta() {
    // 默认清空文件
    std::ofstream ofs(DB_META_NAME);
    ofs << db_;
}

/**
 * @description: 关闭数据库并把数据落盘
 */
void SmManager::close_db() {
    if (chdir("..") < 0) {
        throw UnixError();
    }
    if (db_.name_.empty()) {
        return;  // 如果当前没有打开的数据库，直接返回
    }
    // 刷新元数据到磁盘
    flush_meta();
    // 关闭所有的数据文件和索引文件
    for (auto& fh : fhs_) {
        rm_manager_->close_file(fh.second.get());
    }
    fhs_.clear();
    for (auto& ih : ihs_) {
        ix_manager_->close_index(ih.second.get());
    }
    ihs_.clear();

    db_ = DbMeta();  // 清空当前数据库元数据

     // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 显示所有的表,通过测试需要将其结果写入到output.txt,详情看题目文档
 * @param {Context*} context 
 */
void SmManager::show_tables(Context* context) {
    if (!is_dir(db_.name_)) {
        throw DatabaseNotFoundError(db_.name_);
    }
    if (chdir(db_.name_.c_str()) < 0) {  // 进入数据库目录
        throw UnixError();
    }

    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "| Tables |\n";
    
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);

    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        printer.print_record({tab.name}, context);
        outfile << "| " << tab.name << " |\n";
    }

    printer.print_separator(context);
    outfile.close();

    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 显示表的元数据
 * @param {string&} tab_name 表名称
 * @param {Context*} context 
 */
void SmManager::desc_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    // Print header
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    // Print fields
    for (auto &col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    // Print footer
    printer.print_separator(context);
}

/**
 * @description: 创建表
 * @param {string&} tab_name 表的名称
 * @param {vector<ColDef>&} col_defs 表的字段
 * @param {Context*} context 
 */
void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (!is_dir(db_.name_)) {
        throw DatabaseNotFoundError(db_.name_);
    }
    if (chdir(db_.name_.c_str()) < 0) {  // 进入数据库目录
        throw UnixError();
    }
    if (db_.is_table(tab_name)) {
        if (chdir("..") < 0) {
            throw UnixError();
        }
        throw TableExistsError(tab_name);
    }
    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto &col_def : col_defs) {
        ColMeta col = {.tab_name = tab_name,
                       .name = col_def.name,
                       .type = col_def.type,
                       .len = col_def.len,
                       .offset = curr_offset,
                       .index = false};
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }
    // Create & open record file
    int record_size = curr_offset;  // record_size就是col meta所占的大小（表的元数据也是以记录的形式进行存储的）
    //std::string tab_file_name = db_.name_ + "/" + tab_name;
    rm_manager_->create_file(tab_name, record_size);
    db_.tabs_[tab_name] = tab;
    // fhs_[tab_name] = rm_manager_->open_file(tab_name);
    fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));

    flush_meta();

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除表
 * @param {string&} tab_name 表的名称
 * @param {Context*} context
 */
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    if (!is_dir(db_.name_)) {
        throw DatabaseNotFoundError(db_.name_);
    }
    if (chdir(db_.name_.c_str()) < 0) {  // 进入数据库目录
        throw UnixError();
    }
    if (!db_.is_table(tab_name)) {
        if (chdir("..") < 0) {
            throw UnixError();
        }
        throw TableNotFoundError(tab_name);
    }
    
    // 删除表的所有索引
    auto& tab_meta = db_.get_table(tab_name);
    for(auto idx_meta : tab_meta.indexes){
        drop_index(tab_name,idx_meta.cols,context);
    }

    // 删除表的数据文件
    disk_manager_->close_file(disk_manager_->get_file_fd(tab_name));
    rm_manager_->destroy_file(tab_name);
    fhs_.erase(tab_name);
    ihs_.erase(tab_name);

    // 从数据库元数据中移除表
    db_.tabs_.erase(tab_name);
    
    flush_meta();

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    if (!is_dir(db_.name_)) {
        throw DatabaseNotFoundError(db_.name_);
    }
    if (chdir(db_.name_.c_str()) < 0) {  // 进入数据库目录
        throw UnixError();
    }
    if (!db_.is_table(tab_name)) {
        if (chdir("..") < 0) {
            throw UnixError();
        }
        throw TableNotFoundError(tab_name);
    }
    //check if index has exsit
    auto& tab_meta = db_.get_table(tab_name);
    if(tab_meta.is_index(col_names)){
        throw IndexExistsError(tab_name, col_names);
    }
    //check if col exist in table
    for(auto col_name : col_names){
        if(!tab_meta.is_col(col_name)){
            throw ColumnNotFoundError(col_name);
        }
    }
    //build index
    int col_num = col_names.size();
    int col_total_size = 0;
    std::vector<ColMeta>cols;
    for(auto col_name : col_names){
        ColMeta col_meta = *(tab_meta.get_col(col_name));
        col_total_size+=col_meta.len;
        col_meta.index = true;
        (tab_meta.get_col(col_name))->index = true;
        cols.push_back(col_meta);
    }
    ix_manager_->create_index(tab_name,cols);

    //flush meta
    IndexMeta idx_meta{tab_name,col_total_size,col_num,cols};
    std::string index_name = ix_manager_->get_index_name(tab_name, cols);
    
    tab_meta.indexes.push_back(idx_meta);
    ihs_[index_name] = ix_manager_->open_index(tab_name, cols);
    
    flush_meta();

    //insert data in tables
    auto& file_hdr = fhs_[tab_name];
    auto scan = std::make_unique<RmScan>(file_hdr.get());
    while(!scan->is_end()){
        auto rid = scan->rid();
        auto rec = file_hdr->get_record(rid,nullptr);
        char* key = new char[idx_meta.col_tot_len];
        int offset = 0;
        for(int i = 0; i < idx_meta.col_num; ++i) {
            memcpy(key + offset, rec->data + idx_meta.cols[i].offset, idx_meta.cols[i].len);
            offset += idx_meta.cols[i].len;
        }
        ihs_[index_name]->insert_entry(key, rid, context->txn_);
        scan->next();
    }

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    if (!is_dir(db_.name_)) {
        throw DatabaseNotFoundError(db_.name_);
    }
    if (chdir(db_.name_.c_str()) < 0) {  // 进入数据库目录
        throw UnixError();
    }
    if (!db_.is_table(tab_name)) {
        if (chdir("..") < 0) {
            throw UnixError();
        }
        throw TableNotFoundError(tab_name);
    }

    //验证索引是否存在
    auto& tab_meta = db_.get_table(tab_name);
    if(!tab_meta.is_index(col_names)){
        throw IndexNotFoundError(tab_name,col_names);
    }

    //删除索引
    std::string ix_name = ix_manager_->get_index_name(tab_name, col_names);
    //disk_manager_->close_file(disk_manager_->get_file_fd(ix_name));
    auto ix_hdr = ihs_[ix_name].get();
    ix_manager_->close_index(ix_hdr);
    ix_manager_->destroy_index(tab_name,col_names);

    //删除索引元数据
    int col_num = col_names.size();
    int col_total_size = 0;
    std::vector<ColMeta>cols;
    for(auto col_name : col_names){
        ColMeta col_meta = *(tab_meta.get_col(col_name));
        col_total_size+=col_meta.len; 
        col_meta.index = false;
        (tab_meta.get_col(col_name))->index = false; //TODO06-13: fix 
        cols.push_back(col_meta);
       
    }
    IndexMeta idx_meta{tab_name,col_total_size,col_num,cols};

    auto index_it = std::find_if(tab_meta.indexes.begin(), tab_meta.indexes.end(), [&](const IndexMeta& index) {
        bool left = index.tab_name == tab_name&&index.col_num == col_num && index.col_tot_len == col_total_size;
        if(!left)return false;

        int i = 0;
        for(; i < index.col_num; ++i) {
            if(index.cols[i].name.compare(cols[i].name) != 0)return false;
        }
        return true;
    });
     
    tab_meta.indexes.erase(index_it);
    ihs_.erase(ix_name);

    flush_meta();

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
    return;
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} 索引包含的字段元数据
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    std::vector<std::string>str_cols;
    for(auto col : cols){
        str_cols.push_back(col.name);
    }
    drop_index(tab_name,str_cols,context);
}

void SmManager::show_indexs(const std::string& tab_name,Context* context) {
    if (!is_dir(db_.name_)) {
        throw DatabaseNotFoundError(db_.name_);
    }
    if (chdir(db_.name_.c_str()) < 0) {  // 进入数据库目录
        throw UnixError();
    }

    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    RecordPrinter printer(3);
    printer.print_separator(context);

    TabMeta &tab = db_.get_table(tab_name);
    for (auto & entry : tab.indexes){
        std::string name_list = "(";
        for(auto &col_meta : entry.cols){
            auto name = col_meta.name;
            name_list += name;
            name_list += ",";
        }
        name_list.pop_back();
        name_list+=")";

        printer.print_record({tab.name,"unique",name_list}, context);
        outfile << "| " << tab.name << " | unique | "<< name_list <<" |\n";
    }

    printer.print_separator(context);
    outfile.close();

    if (chdir("..") < 0) {
        throw UnixError();
    }
}
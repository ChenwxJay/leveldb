// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/table_cache.h"

#include "db/filename.h"
#include "leveldb/env.h" //环境变量
#include "leveldb/table.h" //表格对象
#include "util/coding.h"

namespace leveldb {

struct TableAndFile { //表格文件
  //随机访问文件指针
  RandomAccessFile* file;
  Table* table;//表格指针
};
//删除键值对
static void DeleteEntry(const Slice& key, void* value) {
  TableAndFile* tf = reinterpret_cast<TableAndFile*>(value);//将value进行强制类型转换
  delete tf->table;//析构内部的table
  delete tf->file;//析构file
  delete tf;
}

static void UnrefEntry(void* arg1, void* arg2) {
  Cache* cache = reinterpret_cast<Cache*>(arg1);//将参数1强转为Cache*类型
  //第二个参数转为Handle*指针
  Cache::Handle* h = reinterpret_cast<Cache::Handle*>(arg2);
  cache->Release(h);//释放指针
}

TableCache::TableCache(const std::string& dbname,//数据库名字，使用string对象
                       const Options& options,//选项，即配置
                       int entries)
    : env_(options.env),//环境变量
      dbname_(dbname),
      options_(options),//选项对象
      cache_(NewLRUCache(entries)) {
}

TableCache::~TableCache() {//析构函数
  delete cache_;//析构内部的缓存对象
}

Status TableCache::FindTable(uint64_t file_number, uint64_t file_size,
                             Cache::Handle** handle) {
  Status s;//状态对象
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);//编码，第二个参数是
  Slice key(buf, sizeof(buf));//由buf构建Slice
  
  *handle = cache_->Lookup(key);//根据Key去查找
  
  //判断获取的handle指针指向的对象是否为空
  if (*handle == nullptr) {
    std::string fname = TableFileName(dbname_, file_number);//构建string
    RandomAccessFile* file = nullptr;//随机访问文件指针
    Table* table = nullptr;//Table指针
	
	//创建新的可随机访问的文件
    s = env_->NewRandomAccessFile(fname, &file);
    if (!s.ok()) {
	  //获取原来的文件名称
      std::string old_fname = SSTTableFileName(dbname_, file_number);
      if (env_->NewRandomAccessFile(old_fname, &file).ok()) {
        s = Status::OK();//修改状态值
      }
    }
    if (s.ok()) {//如果状态正常
	  //打开
      s = Table::Open(options_, file, file_size, &table);//打开文件
    }

    if (!s.ok()) {//获取操作的状态
	  //判断是否为空
      assert(table == nullptr);
	  
      delete file;//析构文件对象
      // We do not cache error results so that if the error is transient,
      // or somebody repairs the file, we recover automatically.
    } else {
	  
      TableAndFile* tf = new TableAndFile;
      tf->file = file;//填充文件指针
      tf->table = table;//填充表指针
	  //插入tf,返回cache的处理器
      *handle = cache_->Insert(key, tf, 1, &DeleteEntry);
    }
  }
  return s;
}

Iterator* TableCache::NewIterator(const ReadOptions& options,
                                  uint64_t file_number,//文件数
                                  uint64_t file_size,//文件大小
                                  Table** tableptr) {
  if (tableptr != nullptr) {//校验指针
    *tableptr = nullptr;//将指针指向的单元置空
  }

  Cache::Handle* handle = nullptr;//处理器指针
  
  //查找文件
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) {//判断是否正常
    return NewErrorIterator(s);
  }
  //获取表对象指针
  Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
  Iterator* result = table->NewIterator(options);//获取表的迭代器，返回迭代器指针
  
  //在迭代器上注册清除函数，第一个参数是函数指针，第二个和第三个参数是清除函数的参数
  result->RegisterCleanup(&UnrefEntry, cache_, handle);
  if (tableptr != nullptr) {
    *tableptr = table;//填充指向获得的表的指针
  }
  return result;
}

Status TableCache::Get(const ReadOptions& options,
                       uint64_t file_number,
                       uint64_t file_size,
                       const Slice& k,//Slice对象，用来查找的键
                       void* arg,//参数，无类型指针
                       void (*saver)(void*, const Slice&, const Slice&)) {
  //处理器指针，置空					    
  Cache::Handle* handle = nullptr;
  //查找表格指针
  Status s = FindTable(file_number, file_size, &handle);
  //
  if (s.ok()) {
    Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
    s = t->InternalGet(options, k, arg, saver);
    cache_->Release(handle);
  }
  return s;
}

void TableCache::Evict(uint64_t file_number) {
  //创建缓冲区
  char buf[sizeof(file_number)];
  //根据参数编码
  EncodeFixed64(buf, file_number);
  //cache擦除函数
  cache_->Erase(Slice(buf, sizeof(buf)));
}

}  // namespace leveldb

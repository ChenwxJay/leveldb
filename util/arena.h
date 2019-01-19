// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_ARENA_H_
#define STORAGE_LEVELDB_UTIL_ARENA_H_

#include <vector> //vector头文件
#include <assert.h> 
#include <stddef.h> //定义
#include <stdint.h> //标准头文件
#include "port/port.h"

namespace leveldb {

class Arena {
 public:
  Arena();
  ~Arena();

  // Return a pointer to a newly allocated memory block of "bytes" bytes.
  char* Allocate(size_t bytes); //分配内存，以字节为单位

  // Allocate memory with the normal alignment guarantees provided by malloc
  char* AllocateAligned(size_t bytes);//分配对齐的内存，底层采用malloc实现

  // Returns an estimate of the total memory usage of data allocated
  // by the arena.
  size_t MemoryUsage() const {
	//返回当前内存使用情况，需要使用原子加载
    return reinterpret_cast<uintptr_t>(memory_usage_.NoBarrier_Load());
  }

 private:
  char* AllocateFallback(size_t bytes);//分配回调函数
  char* AllocateNewBlock(size_t block_bytes);//新的块分配回调函数

  // Allocation state
  char* alloc_ptr_;//分配指针
  size_t alloc_bytes_remaining_;//指定剩余多少字节可分配空间

  // Array of new[] allocated memory blocks
  std::vector<char*> blocks_;//底层是一个vector

  // Total memory usage of the arena.
  port::AtomicPointer memory_usage_;//使用原子类型，记录内存使用情况

  // No copying allowed
  Arena(const Arena&);//拷贝构造函数
  void operator=(const Arena&);//赋值运算符，私有
};

inline char* Arena::Allocate(size_t bytes) {
  // The semantics of what to return are a bit messy if we allow
  // 0-byte allocations, so we disallow them here (we don't need
  // them for our internal use).
  assert(bytes > 0);
  
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_; //保存当前分配的内存地址
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;//可分配字节数减少
    return result;//返回分配的起始指针
  }
  //如果所需内存太大，则调用另外的函数进行分配
  return AllocateFallback(bytes);//需要调用回调函数
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_ARENA_H_

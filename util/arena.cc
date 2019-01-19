// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/arena.h" //头文件声明
#include <assert.h> //加断言

namespace leveldb {

static const int kBlockSize = 4096;//定义块大小，使用静态const修饰

Arena::Arena() : memory_usage_(0) {
  //构造函数只需要初始化内部成员
  alloc_ptr_ = nullptr;  // First allocation will allocate a block
  alloc_bytes_remaining_ = 0;
}

Arena::~Arena() {
  //析构函数，释放所有内存，利用delete
  for (size_t i = 0; i < blocks_.size(); i++) {
	//遍历存储block的数组
    delete[] blocks_[i];
  }
}

//回调函数之一
char* Arena::AllocateFallback(size_t bytes) { 
  //需要分配的块大小超过固定的四分之一，就在新的内存块中分配
  if (bytes > kBlockSize / 4) {
    // Object is more than a quarter of our block size.  Allocate it separately
    // to avoid wasting too much space in leftover bytes.
    char* result = AllocateNewBlock(bytes);//直接
    return result;//返回分配的指针
  }

  //剩下的空间丢弃，重新分配一个固定内存块，在新的内存块中分配
  // We waste the remaining space in the current block.
  alloc_ptr_ = AllocateNewBlock(kBlockSize);//创建一个新的块，由alloc_ptr_接管
  alloc_bytes_remaining_ = kBlockSize;//设置剩余空间大小

  char* result = alloc_ptr_;//赋值
  alloc_ptr_ += bytes;//指针偏移bytes个字节
  alloc_bytes_remaining_ -= bytes;
  return result;
}

char* Arena::AllocateAligned(size_t bytes) {//分配对齐空间
  //根据void*的大小来决定使用对齐字节数
  const int align = (sizeof(void*) > 8) ? sizeof(void*) : 8;
  //必须保证是2的幂次
  assert((align & (align-1)) == 0);   // Pointer size should be a power of 2
  //计算当前地址的低位 
  size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align-1);
  
  size_t slop = (current_mod == 0 ? 0 : align - current_mod);//计算差值
  size_t needed = bytes + slop;//需要多分配的字节数
  char* result;
  if (needed <= alloc_bytes_remaining_) {//需要分配的字节数小于当前块剩余空间
    result = alloc_ptr_ + slop;//地址偏移
    alloc_ptr_ += needed;//地址偏移所需要的字节
    alloc_bytes_remaining_ -= needed;//剩余空间减少
  } else {
    // AllocateFallback always returned aligned memory
    result = AllocateFallback(bytes);//调用回调函数，默认返回对齐的内存
  }
  assert((reinterpret_cast<uintptr_t>(result) & (align-1)) == 0);//已经对齐
  return result;//返回对齐的内存块首地址
}

char* Arena::AllocateNewBlock(size_t block_bytes) {//参数是需要分配的大小
  //使用new动态分配内存，大小为参数指定
  char* result = new char[block_bytes];
  //加入vector
  blocks_.push_back(result);
  memory_usage_.NoBarrier_Store( //更改原子类型变量
       //修改原有的原子类型值，再存储
      reinterpret_cast<void*>(MemoryUsage() + block_bytes + sizeof(char*)));
  return result;//返回分配内存的首地址
}

}  // namespace leveldb

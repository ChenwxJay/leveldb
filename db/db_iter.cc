// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_iter.h"

#include "db/filename.h"
#include "db/db_impl.h"
#include "db/dbformat.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"
#include "port/port.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/random.h"

namespace leveldb {

#if 0
static void DumpInternalIter(Iterator* iter) {
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey k;
    if (!ParseInternalKey(iter->key(), &k)) {
      fprintf(stderr, "Corrupt '%s'\n", EscapeString(iter->key()).c_str());
    } else {
      fprintf(stderr, "@ '%s'\n", k.DebugString().c_str());
    }
  }
}
#endif

namespace {

// Memtables and sstables that make the DB representation contain
// (userkey,seq,type) => uservalue entries.  DBIter
// combines multiple entries for the same userkey found in the DB
// representation into a single entry while accounting for sequence
// numbers, deletion markers, overwrites, etc.
class DBIter: public Iterator {
 public:
  // Which direction is the iterator currently moving?
  // (1) When moving forward, the internal iterator is positioned at
  //     the exact entry that yields this->key(), this->value()
  // (2) When moving backwards, the internal iterator is positioned
  //     just before all entries whose user key == this->key().
  enum Direction {
    kForward,
    kReverse
  };

  DBIter(DBImpl* db, const Comparator* cmp, Iterator* iter, SequenceNumber s,
         uint32_t seed)
      : db_(db), //db具体实现类对象
        user_comparator_(cmp),//用户设置的比较器
        iter_(iter),
        sequence_(s),
        direction_(kForward),//默认是前进
        valid_(false),
        rnd_(seed), //随机数
        bytes_until_read_sampling_(RandomCompactionPeriod()) {
  }
  virtual ~DBIter() {//析构函数，只需析构迭代器对象
    delete iter_;
  }
  virtual bool Valid() const { return valid_; }//返回是否有效
  virtual Slice key() const {//获取Key，返回对应的Slice
    assert(valid_);
	//根据方向提取Key，如果是kForward，则直接返回iter指向的键值对的key
    return (direction_ == kForward) ? ExtractUserKey(iter_->key()) : saved_key_;
  }
  virtual Slice value() const {
    assert(valid_);//校验是否有效
    return (direction_ == kForward) ? iter_->value() : saved_value_;
  }
  virtual Status status() const {//返回当前状态
    if (status_.ok()) {//内部状态ok
      return iter_->status();
    } else {
      return status_;
    }
  }

  virtual void Next();
  virtual void Prev();
  virtual void Seek(const Slice& target);//虚函数，定位，参数是Slice对象
  virtual void SeekToFirst();//定位到头部
  virtual void SeekToLast();//定位到尾部

 private:
  void FindNextUserEntry(bool skipping, std::string* skip);
  void FindPrevUserEntry();//查找上一个user_key
  bool ParseKey(ParsedInternalKey* key);//解析key

  inline void SaveKey(const Slice& k, std::string* dst) {
    dst->assign(k.data(), k.size());//将Slice对象的内容赋值给string指针
  }

  inline void ClearSavedValue() {//清除保存的Key
    if (saved_value_.capacity() > 1048576) {
      std::string empty;//临时对象
	  //大内存利用swap交换清除
      swap(empty, saved_value_);//交换
    } else {
      saved_value_.clear();//清除整个字符串
    }
  }

  // Picks the number of bytes that can be read until a compaction is scheduled.
  size_t RandomCompactionPeriod() {
	//返回可以被读到的字节数，当压缩被调度的时候
    return rnd_.Uniform(2*config::kReadBytesPeriod);
  }

  DBImpl* db_;//指针，指向df插入器的对象
  const Comparator* const user_comparator_;//用户键比较器对象，两层const
  Iterator* const iter_;//迭代器
  SequenceNumber const sequence_;//序列号

  Status status_;//状态
  std::string saved_key_;     // == current key when direction_==kReverse
  std::string saved_value_;   // == current raw value when direction_==kReverse
  Direction direction_;//方向
  bool valid_;

  Random rnd_;//随机数对象
  size_t bytes_until_read_sampling_;

  // No copying allowed
  DBIter(const DBIter&);//拷贝构造函数，禁止外部拷贝构造
  void operator=(const DBIter&);//赋值运算符，重载，被禁止
};

inline bool DBIter::ParseKey(ParsedInternalKey* ikey) {
  Slice k = iter_->key();//通过内部迭代器获取键对象

  size_t bytes_read = k.size() + iter_->value().size();
  while (bytes_until_read_sampling_ < bytes_read) {
    bytes_until_read_sampling_ += RandomCompactionPeriod();
    db_->RecordReadSample(k);
  }
  assert(bytes_until_read_sampling_ >= bytes_read);
  bytes_until_read_sampling_ -= bytes_read;

  if (!ParseInternalKey(k, ikey)) {
    status_ = Status::Corruption("corrupted internal key in DBIter");
    return false;
  } else {
    return true;
  }
}

void DBIter::Next() {
  assert(valid_);

  if (direction_ == kReverse) {  // Switch directions?
    direction_ = kForward;
    // iter_ is pointing just before the entries for this->key(),
    // so advance into the range of entries for this->key() and then
    // use the normal skipping code below.
    if (!iter_->Valid()) {
      iter_->SeekToFirst();
    } else {
      iter_->Next();
    }
    if (!iter_->Valid()) {
      valid_ = false;
      saved_key_.clear();
      return;
    }
    // saved_key_ already contains the key to skip past.
  } else {
    // Store in saved_key_ the current key so we skip it below.
    SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
  }

  FindNextUserEntry(true, &saved_key_);
}

void DBIter::FindNextUserEntry(bool skipping, std::string* skip) {
  // Loop until we hit an acceptable entry to yield
  assert(iter_->Valid());
  assert(direction_ == kForward);
  do {
    ParsedInternalKey ikey;
    if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
      switch (ikey.type) {
        case kTypeDeletion:
          // Arrange to skip all upcoming entries for this key since
          // they are hidden by this deletion.
          SaveKey(ikey.user_key, skip);
          skipping = true;
          break;
        case kTypeValue:
          if (skipping &&
              user_comparator_->Compare(ikey.user_key, *skip) <= 0) {
            // Entry hidden
          } else {
            valid_ = true;
            saved_key_.clear();
            return;
          }
          break;
      }
    }
    iter_->Next();
  } while (iter_->Valid());
  saved_key_.clear();
  valid_ = false;
}

void DBIter::Prev() {
  assert(valid_);

  if (direction_ == kForward) {  // Switch directions?
    // iter_ is pointing at the current entry.  Scan backwards until
    // the key changes so we can use the normal reverse scanning code.
    assert(iter_->Valid());  // Otherwise valid_ would have been false
    SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
    while (true) {
      iter_->Prev();
      if (!iter_->Valid()) {
        valid_ = false;
        saved_key_.clear();
        ClearSavedValue();
        return;
      }
      if (user_comparator_->Compare(ExtractUserKey(iter_->key()),
                                    saved_key_) < 0) {
        break;
      }
    }
    direction_ = kReverse;
  }

  FindPrevUserEntry();
}

void DBIter::FindPrevUserEntry() {
  assert(direction_ == kReverse);

  ValueType value_type = kTypeDeletion;
  if (iter_->Valid()) {
    do {
      ParsedInternalKey ikey;
      if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
        if ((value_type != kTypeDeletion) &&
            user_comparator_->Compare(ikey.user_key, saved_key_) < 0) {
          // We encountered a non-deleted value in entries for previous keys,
          break;
        }
        value_type = ikey.type;
        if (value_type == kTypeDeletion) {
          saved_key_.clear();
          ClearSavedValue();
        } else {
          Slice raw_value = iter_->value();
          if (saved_value_.capacity() > raw_value.size() + 1048576) {
            std::string empty;
            swap(empty, saved_value_);
          }
          SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
          saved_value_.assign(raw_value.data(), raw_value.size());
        }
      }
      iter_->Prev();
    } while (iter_->Valid());
  }

  if (value_type == kTypeDeletion) {
    // End
    valid_ = false;
    saved_key_.clear();
    ClearSavedValue();
    direction_ = kForward;
  } else {
    valid_ = true;
  }
}

void DBIter::Seek(const Slice& target) {//定位
  direction_ = kForward;//方向，标记为前进
  ClearSavedValue();//清除保存值
  //调用对象的clear方法
  saved_key_.clear();
  AppendInternalKey(
      &saved_key_, ParsedInternalKey(target, sequence_, kValueTypeForSeek));
  iter_->Seek(saved_key_);//通过迭代器定位需要查找的Key
  if (iter_->Valid()) {//迭代器有效
    FindNextUserEntry(false, &saved_key_ /* temporary storage */);
  } else {
    valid_ = false;
  }
}

void DBIter::SeekToFirst() {
  direction_ = kForward;
  ClearSavedValue();
  iter_->SeekToFirst();
  if (iter_->Valid()) {
    FindNextUserEntry(false, &saved_key_ /* temporary storage */);
  } else {
    valid_ = false;
  }
}

void DBIter::SeekToLast() {
  direction_ = kReverse;
  ClearSavedValue();
  iter_->SeekToLast();
  FindPrevUserEntry();
}

}  // anonymous namespace

Iterator* NewDBIterator(
    DBImpl* db,//指向具体的db实现类对象
    const Comparator* user_key_comparator,//比较器对象，用于键比较
    Iterator* internal_iter,
    SequenceNumber sequence,
    uint32_t seed) {
  //内部使用new创建数据库迭代器，返回指针
  return new DBIter(db, user_key_comparator, internal_iter, sequence, seed);
}

}  // namespace leveldb

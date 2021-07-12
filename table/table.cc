/**
 * Copyright 2019 The Gamma Authors.
 *
 * This source code is licensed under the Apache License, Version 2.0 license
 * found in the LICENSE file in the root directory of this source tree.
 */

#include "table.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <fstream>
#include <string>

#include "utils.h"

using std::move;
using std::string;
using std::vector;

namespace tig_gamma {
namespace table {

Table::Table(const string &root_path, bool b_compress) {
  item_length_ = 0;
  field_num_ = 0;
  string_field_num_ = 0;
  key_idx_ = -1;
  root_path_ = root_path + "/table";
  seg_num_ = 0;
  b_compress_ = b_compress;

  table_created_ = false;
  last_docid_ = -1;
  table_params_ = nullptr;
  storage_mgr_ = nullptr;
  LOG(INFO) << "Table created success!";
}

Table::~Table() {
  CHECK_DELETE(table_params_);
  if (storage_mgr_) {
    delete storage_mgr_;
    storage_mgr_ = nullptr;
  }
  LOG(INFO) << "Table deleted.";
}

int Table::Load(int &num) {
  int doc_num = storage_mgr_->Size();
  storage_mgr_->Truncate(num);
  LOG(INFO) << "Load doc_num [" << doc_num << "] truncate to [" << num << "]";
  doc_num = num;

  const std::string str_id = "_id";
  const auto &iter = attr_idx_map_.find(str_id);
  if (iter == attr_idx_map_.end()) {
    LOG(ERROR) << "Cannot find field [" << str_id << "]";
    return -1;
  }

  int idx = iter->second;
  if (id_type_ == 0) {
    for (int i = 0; i < doc_num; ++i) {
      std::string key;
      GetFieldRawValue(i, idx, key);
      int64_t k = utils::StringToInt64(key);
      item_to_docid_.insert(k, i);
    }
  } else {
    for (int i = 0; i < doc_num; ++i) {
      long key = -1;
      std::string key_str;
      GetFieldRawValue(i, idx, key_str);
      memcpy(&key, key_str.c_str(), sizeof(key));
      item_to_docid_.insert(key, i);
    }
  }

  LOG(INFO) << "Table load successed! doc num [" << doc_num << "]";
  last_docid_ = doc_num - 1;
  return 0;
}

int Table::Sync() {
  int ret = storage_mgr_->Sync();
  LOG(INFO) << "Table [" << name_ << "] sync, doc num[" << storage_mgr_->Size()
            << "]";
  return ret;
}

int Table::CreateTable(TableInfo &table, TableParams &table_params) {
  if (table_created_) {
    return -10;
  }
  name_ = table.Name();
  std::vector<struct FieldInfo> &fields = table.Fields();

  b_compress_ = table.IsCompress();
  LOG(INFO) << "Table compress [" << b_compress_ << "]";

  size_t fields_num = fields.size();
  for (size_t i = 0; i < fields_num; ++i) {
    const string name = fields[i].name;
    DataType ftype = fields[i].data_type;
    bool is_index = fields[i].is_index;
    LOG(INFO) << "Add field name [" << name << "], type [" << (int)ftype
              << "], index [" << is_index << "]";
    int ret = AddField(name, ftype, is_index);
    if (ret != 0) {
      return ret;
    }
  }

  if (key_idx_ == -1) {
    LOG(ERROR) << "No field _id! ";
    return -1;
  }

  if (!utils::isFolderExist(root_path_.c_str())) {
    mkdir(root_path_.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
  }

  table_params_ = new TableParams("table");
  table_created_ = true;
  LOG(INFO) << "Create table " << name_
            << " success! item length=" << item_length_
            << ", field num=" << (int)field_num_;

  StorageManagerOptions options;
  options.segment_size = 500000;
  options.fixed_value_bytes = item_length_;
  options.seg_block_capacity = 400000;
  storage_mgr_ =
      new StorageManager(root_path_, BlockType::TableBlockType, options);
  int cache_size = 512;  // unit : M
  int str_cache_size = 512;
  int ret = storage_mgr_->Init(cache_size, name_ + "_table", str_cache_size,
                               name_ + "_string");
  if (ret) {
    LOG(ERROR) << "init gamma db error, ret=" << ret;
    return ret;
  }

  LOG(INFO) << "init storageManager success! vector byte size="
            << options.fixed_value_bytes << ", path=" << root_path_;
  return 0;
}

int Table::FTypeSize(DataType fType) {
  int length = 0;
  if (fType == DataType::INT) {
    length = sizeof(int32_t);
  } else if (fType == DataType::LONG) {
    length = sizeof(int64_t);
  } else if (fType == DataType::FLOAT) {
    length = sizeof(float);
  } else if (fType == DataType::DOUBLE) {
    length = sizeof(double);
  } else if (fType == DataType::STRING) {
    // block_id, in_block_pos, str_len
    length = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(str_len_t);
  }
  return length;
}

int Table::AddField(const string &name, DataType ftype, bool is_index) {
  if (attr_idx_map_.find(name) != attr_idx_map_.end()) {
    LOG(ERROR) << "Duplicate field " << name;
    return -1;
  }
  if (name == "_id") {
    key_idx_ = field_num_;
    id_type_ = ftype == DataType::STRING ? 0 : 1;
  }
  if (ftype == DataType::STRING) {
    str_field_id_.insert(std::make_pair(field_num_, string_field_num_));
    ++string_field_num_;
  }
  idx_attr_offset_.push_back(item_length_);
  item_length_ += FTypeSize(ftype);
  attrs_.push_back(ftype);
  idx_attr_map_.insert(std::pair<int, string>(field_num_, name));
  attr_idx_map_.insert(std::pair<string, int>(name, field_num_));
  attr_type_map_.insert(std::pair<string, DataType>(name, ftype));
  attr_is_index_map_.insert(std::pair<string, bool>(name, is_index));
  ++field_num_;
  return 0;
}

int Table::GetDocIDByKey(std::string &key, int &docid) {
  if (id_type_ == 0) {
    int64_t k = utils::StringToInt64(key);
    if (item_to_docid_.find(k, docid)) {
      return 0;
    }
  } else {
    long key_long = -1;
    memcpy(&key_long, key.data(), sizeof(key_long));

    if (item_to_docid_.find(key_long, docid)) {
      return 0;
    }
  }
  return -1;
}

int Table::Add(const std::string &key, const std::vector<struct Field> &fields,
               int docid) {
  if (fields.size() != attr_idx_map_.size()) {
    LOG(ERROR) << "Field num [" << fields.size() << "] not equal to ["
               << attr_idx_map_.size() << "]";
    return -2;
  }
  if (key.size() == 0) {
    LOG(ERROR) << "Add item error : _id is null!";
    return -3;
  }

  if (id_type_ == 0) {
    int64_t k = utils::StringToInt64(key);
    item_to_docid_.insert(k, docid);
  } else {
    long key_long = -1;
    memcpy(&key_long, key.data(), sizeof(key_long));

    item_to_docid_.insert(key_long, docid);
  }

  uint8_t doc_value[item_length_];

  for (size_t i = 0; i < fields.size(); ++i) {
    const auto &field_value = fields[i];
    const std::string &name = field_value.name;
    size_t offset = idx_attr_offset_[i];

    DataType attr = attrs_[i];

    if (attr != DataType::STRING) {
      int type_size = FTypeSize(attr);
      memcpy(doc_value + offset, field_value.value.c_str(), type_size);
    } else {
      size_t ofst = sizeof(str_offset_t);
      str_len_t len = field_value.value.size();
      int str_field_id = str_field_id_[attr_idx_map_[name]];
      uint32_t block_id, in_block_pos;
      str_offset_t str_offset = storage_mgr_->AddString(
          field_value.value.c_str(), len, block_id, in_block_pos);

      memcpy(doc_value + offset, &block_id, sizeof(block_id));
      memcpy(doc_value + offset + sizeof(block_id), &in_block_pos,
             sizeof(in_block_pos));
      memcpy(doc_value + offset + sizeof(block_id) + sizeof(in_block_pos), &len,
             sizeof(len));
    }
  }

  storage_mgr_->Add((const uint8_t *)doc_value, item_length_);

  if (docid % 10000 == 0) {
    if (id_type_ == 0) {
      LOG(INFO) << "Add item _id [" << key << "], num [" << docid << "]";
    } else {
      long key_long = -1;
      memcpy(&key_long, key.data(), sizeof(key_long));
      LOG(INFO) << "Add item _id [" << key_long << "], num [" << docid << "]";
    }
  }
  last_docid_ = docid;
  return 0;
}

int Table::BatchAdd(int start_id, int batch_size, int docid,
                    std::vector<Doc> &doc_vec, BatchResult &result) {
#ifdef PERFORMANCE_TESTING
  double start = utils::getmillisecs();
#endif

#pragma omp parallel for
  for (size_t i = 0; i < batch_size; ++i) {
    int id = docid + i;
    Doc &doc = doc_vec[start_id + i];

    std::string &key = doc.Key();
    if (key.size() == 0) {
      std::string msg = "Add item error : _id is null!";
      result.SetResult(i, -1, msg);
      LOG(ERROR) << msg;
      continue;
    }

    if (id_type_ == 0) {
      int64_t k = utils::StringToInt64(key);
      item_to_docid_.insert(k, id);
    } else {
      long key_long = -1;
      memcpy(&key_long, key.data(), sizeof(key_long));

      item_to_docid_.insert(key_long, id);
    }
  }

  for (size_t i = 0; i < batch_size; ++i) {
    int id = docid + i;
    Doc &doc = doc_vec[start_id + i];
    std::vector<Field> &fields = doc.TableFields();
    uint8_t doc_value[item_length_];

    for (size_t j = 0; j < fields.size(); ++j) {
      const auto &field_value = fields[j];
      const string &name = field_value.name;
      size_t offset = idx_attr_offset_[j];

      DataType attr = attrs_[j];

      if (attr != DataType::STRING) {
        int type_size = FTypeSize(attr);
        memcpy(doc_value + offset, field_value.value.c_str(), type_size);
      } else {
        size_t ofst = sizeof(str_offset_t);
        str_len_t len = field_value.value.size();
        uint32_t block_id, in_block_pos;
        str_offset_t str_offset = storage_mgr_->AddString(
            field_value.value.c_str(), len, block_id, in_block_pos);

        memcpy(doc_value + offset, &block_id, sizeof(block_id));
        memcpy(doc_value + offset + sizeof(block_id), &in_block_pos,
               sizeof(in_block_pos));
        memcpy(doc_value + offset + sizeof(block_id) + sizeof(in_block_pos),
               &len, sizeof(len));
      }
    }

    storage_mgr_->Add((const uint8_t *)doc_value, item_length_);
    if (id % 10000 == 0) {
      std::string &key = doc_vec[i].Key();
      if (id_type_ == 0) {
        LOG(INFO) << "Add item _id [" << key << "], num [" << id << "]";
      } else {
        long key_long = -1;
        memcpy(&key_long, key.data(), sizeof(key_long));
        LOG(INFO) << "Add item _id [" << key_long << "], num [" << id << "]";
      }
    }
  }

  // Compress();
#ifdef PERFORMANCE_TESTING
  double end = utils::getmillisecs();
  if (docid % 10000 == 0) {
    LOG(INFO) << "table cost [" << end - start << "]ms";
  }
#endif
  last_docid_ = docid + batch_size;
  return 0;
}

int Table::Update(const std::vector<Field> &fields, int docid) {
  if (fields.size() == 0) return 0;

  const uint8_t *ori_doc_value;
  storage_mgr_->Get(docid, ori_doc_value);

  uint8_t doc_value[item_length_];

  memcpy(doc_value, ori_doc_value, item_length_);

  for (size_t i = 0; i < fields.size(); ++i) {
    const struct Field &field_value = fields[i];
    const string &name = field_value.name;
    const auto &it = attr_idx_map_.find(name);
    if (it == attr_idx_map_.end()) {
      LOG(ERROR) << "Cannot find field name [" << name << "]";
      continue;
    }

    int field_id = it->second;
    int offset = idx_attr_offset_[field_id];

    if (field_value.datatype == DataType::STRING) {
      int offset = idx_attr_offset_[field_id];

      str_len_t len = field_value.value.size();
      uint32_t block_id, in_block_pos;
      str_offset_t res = storage_mgr_->UpdateString(
          docid, field_value.value.c_str(), len, block_id, in_block_pos);
      memcpy(doc_value + offset, &block_id, sizeof(block_id));
      memcpy(doc_value + offset + sizeof(block_id), &in_block_pos,
             sizeof(in_block_pos));
      memcpy(doc_value + offset + sizeof(block_id) + sizeof(in_block_pos), &len,
             sizeof(len));
    } else {
      memcpy(doc_value + offset, field_value.value.data(),
             field_value.value.size());
    }
  }

  storage_mgr_->Update(docid, doc_value, item_length_);
  delete[] ori_doc_value;
  return 0;
}

int Table::Delete(std::string &key) {
  if (id_type_ == 0) {
    int64_t k = utils::StringToInt64(key);
    item_to_docid_.erase(k);
  } else {
    long key_long = -1;
    memcpy(&key_long, key.data(), sizeof(key_long));

    item_to_docid_.erase(key_long);
  }
  return 0;
}

long Table::GetMemoryBytes() {
  long total_mem_bytes = 0;
  // for (int i = 0; i < seg_num_; ++i) {
  //   total_mem_bytes += main_file_[i]->GetMemoryBytes();
  // }
  return total_mem_bytes;
}

int Table::GetDocInfo(std::string &id, Doc &doc,
                      std::vector<std::string> &fields) {
  int doc_id = 0;
  int ret = GetDocIDByKey(id, doc_id);
  if (ret < 0) {
    return ret;
  }
  return GetDocInfo(doc_id, doc, fields);
}

int Table::GetDocInfo(const int docid, Doc &doc,
                      std::vector<std::string> &fields) {
  if (docid > last_docid_) {
    LOG(ERROR) << "doc [" << docid << "] in front of [" << last_docid_ << "]";
    return -1;
  }
  const uint8_t *doc_value;
  storage_mgr_->Get(docid, doc_value);
  std::vector<struct Field> &table_fields = doc.TableFields();

  if (fields.size() == 0) {
    int i = 0;
    table_fields.resize(attr_type_map_.size());

    for (const auto &it : attr_idx_map_) {
      DataType type = attr_type_map_[it.first];
      std::string source;
      table_fields[i].name = it.first;
      table_fields[i].source = source;
      table_fields[i].datatype = type;
      GetFieldRawValue(docid, it.second, table_fields[i].value, doc_value);
      ++i;
    }
  } else {
    table_fields.resize(fields.size());
    int i = 0;
    for (std::string &f : fields) {
      const auto &iter = attr_idx_map_.find(f);
      if (iter == attr_idx_map_.end()) {
        LOG(ERROR) << "Cannot find field [" << f << "]";
      }
      int field_idx = iter->second;
      DataType type = attr_type_map_[f];
      std::string source;
      table_fields[i].name = f;
      table_fields[i].source = source;
      table_fields[i].datatype = type;
      GetFieldRawValue(docid, field_idx, table_fields[i].value, doc_value);
      ++i;
    }
  }
  delete[] doc_value;
  return 0;
}

int Table::GetFieldRawValue(int docid, const std::string &field_name,
                            std::string &value, const uint8_t *doc_v) {
  const auto iter = attr_idx_map_.find(field_name);
  if (iter == attr_idx_map_.end()) {
    LOG(ERROR) << "Cannot find field [" << field_name << "]";
    return -1;
  }
  GetFieldRawValue(docid, iter->second, value, doc_v);
}

int Table::GetFieldRawValue(int docid, int field_id, std::string &value,
                            const uint8_t *doc_v) {
  if ((docid < 0) or (field_id < 0 || field_id >= field_num_)) return -1;

  const uint8_t *doc_value = doc_v;
  bool free = false;
  if (doc_value == nullptr) {
    free = true;
    storage_mgr_->Get(docid, doc_value);
  }

  DataType data_type = attrs_[field_id];
  size_t offset = idx_attr_offset_[field_id];

  if (data_type == DataType::STRING) {
    uint32_t block_id = 0;
    memcpy(&block_id, doc_value + offset, sizeof(block_id));

    uint32_t in_block_pos = 0;
    memcpy(&in_block_pos, doc_value + offset + sizeof(block_id),
           sizeof(in_block_pos));

    str_len_t len;
    memcpy(&len, doc_value + offset + sizeof(block_id) + sizeof(in_block_pos),
           sizeof(len));
    std::string str;
    storage_mgr_->GetString(docid, str, block_id, in_block_pos, len);
    value = std::move(str);
  } else {
    int value_len = FTypeSize(data_type);
    value = std::string((const char *)(doc_value + offset), value_len);
  }

  if (free) {
    delete[] doc_value;
  }

  return 0;
}

int Table::GetFieldType(const std::string &field_name, DataType &type) {
  const auto &it = attr_type_map_.find(field_name);
  if (it == attr_type_map_.end()) {
    LOG(ERROR) << "Cannot find field [" << field_name << "]";
    return -1;
  }
  type = it->second;
  return 0;
}

int Table::GetAttrType(std::map<std::string, DataType> &attr_type_map) {
  for (const auto attr_type : attr_type_map_) {
    attr_type_map.insert(attr_type);
  }
  return 0;
}

int Table::GetAttrIsIndex(std::map<std::string, bool> &attr_is_index_map) {
  for (const auto attr_is_index : attr_is_index_map_) {
    attr_is_index_map.insert(attr_is_index);
  }
  return 0;
}

int Table::GetAttrIdx(const std::string &field) const {
  const auto &iter = attr_idx_map_.find(field.c_str());
  return (iter != attr_idx_map_.end()) ? iter->second : -1;
}

bool Table::AlterCacheSize(uint32_t cache_size, uint32_t str_cache_size) {
  return storage_mgr_->AlterCacheSize(cache_size, str_cache_size);
}

void Table::GetCacheSize(uint32_t &cache_size, uint32_t &str_cache_size) {
  storage_mgr_->GetCacheSize(cache_size, str_cache_size);
}

}  // namespace table
}  // namespace tig_gamma

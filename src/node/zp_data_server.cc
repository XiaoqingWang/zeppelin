#include "zp_data_server.h"

#include <fstream>
#include <random>
#include <glog/logging.h>

#include "zp_data_worker_thread.h"
#include "zp_data_dispatch_thread.h"
#include <google/protobuf/text_format.h>

#include "include/rsync.h"

ZPDataServer::ZPDataServer()
  : table_count_(0),
  should_exit_(false),
  meta_epoch_(-1),
  should_pull_meta_(false) {
    pthread_rwlock_init(&meta_state_rw_, NULL);
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    pthread_rwlock_init(&table_rw_, NULL);

    // Command table
    cmds_.reserve(300);
    InitClientCmdTable();

    // Create thread
    zp_metacmd_bgworker_= new ZPMetacmdBGWorker();
    zp_trysync_thread_ = new ZPTrySyncThread();

    // Binlog related
    for (int j = 0; j < kBinlogReceiveBgWorkerCount; j++) {
      zp_binlog_receive_bgworkers_.push_back(
          new ZPBinlogReceiveBgWorker(kBinlogReceiveBgWorkerFull));
    }
    zp_binlog_receiver_thread_ = new ZPBinlogReceiverThread(g_zp_conf->local_port() + kPortShiftSync, kBinlogReceiverCronInterval);
    //binlog_send_pool_ = new ZPBinlogSendTaskPool();
    for (int i = 0; i < kNumBinlogSendThread; ++i) {
      ZPBinlogSendThread *thread = new ZPBinlogSendThread(&binlog_send_pool_);
      binlog_send_workers_.push_back(thread);
    }

    // TODO anan  configable
    // debug
    worker_num_ = 4;//from config
    for (int i = 0; i < worker_num_; i++) {
      zp_worker_thread_[i] = new ZPDataWorkerThread(kWorkerCronInterval);
    }
    zp_dispatch_thread_ = new ZPDataDispatchThread(g_zp_conf->local_port(), worker_num_, zp_worker_thread_, kDispatchCronInterval);

    zp_ping_thread_ = new ZPPingThread();

    // TODO rm
    //LOG(INFO) << "local_host " << options_.local_ip << ":" << options.local_port;
    DLOG(INFO) << "ZPDataServer constructed";
  }

ZPDataServer::~ZPDataServer() {
  DLOG(INFO) << "~ZPDataServer destoryed";
  // Order:
  // 1, Meta thread should before trysync thread
  // 2, Worker thread should before bgsave_thread
  // 3, binlog reciever should before recieve bgworker
  // 4, binlog send thread should before binlog send pool
  delete zp_ping_thread_;
  delete zp_dispatch_thread_;
  for (int i = 0; i < worker_num_; i++) {
    delete zp_worker_thread_[i];
  }


  std::vector<ZPBinlogSendThread*>::iterator it = binlog_send_workers_.begin();
  for (; it != binlog_send_workers_.end(); ++it) {
    delete *it;
  }

  {
    slash::MutexLock l(&mutex_peers_);
    std::unordered_map<std::string, pink::PinkCli*>::iterator iter = peers_.begin();
    while (iter != peers_.end()) {
      iter->second->Close();
      delete iter->second;
      iter++;
    }
  }

  //delete binlog_send_pool_;
  delete zp_binlog_receiver_thread_;
  std::vector<ZPBinlogReceiveBgWorker*>::iterator binlogbg_iter = zp_binlog_receive_bgworkers_.begin();
  while(binlogbg_iter != zp_binlog_receive_bgworkers_.end()){
    delete (*binlogbg_iter);
    ++binlogbg_iter;
  }

  delete zp_trysync_thread_;
  delete zp_metacmd_bgworker_;

  {
    slash::RWLock l(&table_rw_, true);
    // TODO
    for (auto iter = tables_.begin(); iter != tables_.end(); iter++) {
      delete iter->second;
    }
  }
  LOG(INFO) << " All Tables exit!!!";

  bgsave_thread_.set_running(true);
  bgpurge_thread_.set_running(true);

  DestoryCmdTable(cmds_);
  // TODO 
  pthread_rwlock_destroy(&meta_state_rw_);
  pthread_rwlock_destroy(&table_rw_);

  LOG(INFO) << "ZPDataServerThread " << pthread_self() << " exit!!!";
}

Status ZPDataServer::Start() {
  zp_dispatch_thread_->StartThread();
  zp_binlog_receiver_thread_->StartThread();
  zp_ping_thread_->StartThread();
  std::vector<ZPBinlogSendThread*>::iterator bsit = binlog_send_workers_.begin();
  for (; bsit != binlog_send_workers_.end(); ++bsit) {
    LOG(INFO) << "Start one binlog send worker thread";
    (*bsit)->StartThread();
  }

  // TEST 
  LOG(INFO) << "ZPDataServer started on port:" <<  g_zp_conf->local_port();
  auto iter = g_zp_conf->meta_addr().begin();
  while (iter != g_zp_conf->meta_addr().end()) {
    LOG(INFO) << "seed is: " << *iter;
    iter++;
  }

  while (!should_exit_) {
    DoTimingTask();
    int sleep_count = kNodeCronWaitCount;
    while (!should_exit_ && sleep_count-- > 0){
      usleep(kNodeCronInterval * 1000);
    }
  }
  return Status::OK();
}

void ZPDataServer::TryUpdateEpoch(int64_t epoch) {
  slash::MutexLock l(&mutex_epoch_);
  if (epoch != meta_epoch_) {
    LOG(INFO) <<  "Meta epoch changed: " << meta_epoch_ << " to " << epoch;
    should_pull_meta_ = true;
    AddMetacmdTask();
  }
}

void ZPDataServer::FinishPullMeta(int64_t epoch) {
  slash::MutexLock l(&mutex_epoch_);
  DLOG(INFO) <<  "UpdateEpoch (" << meta_epoch_ << "->" << epoch << ") ok...";
  meta_epoch_ = epoch;
  should_pull_meta_ = false;
}

void ZPDataServer::PickMeta() {
  slash::RWLock l(&meta_state_rw_, true);
  if (g_zp_conf->meta_addr().empty()) {
    return;
  }

  std::random_device rd;
  std::mt19937 mt(rd());
  std::uniform_int_distribution<int> di(0, g_zp_conf->meta_addr().size()-1);
  int index = di(mt);

  auto addr = g_zp_conf->meta_addr()[index];
  auto pos = addr.find("/");
  if (pos != std::string::npos) {
    meta_ip_ = addr.substr(0, pos);
    auto str_port = addr.substr(pos+1);
    slash::string2l(str_port.data(), str_port.size(), &meta_port_); 
  }
  LOG(INFO) << "PickMeta ip: " << meta_ip_ << " port: " << meta_port_;
}

void ZPDataServer::DumpTablePartitions() {
  slash::RWLock l(&table_rw_, false);

  DLOG(INFO) << "TablePartition==========================";
  for (auto iter = tables_.begin(); iter != tables_.end(); iter++) {
    iter->second->Dump();
  }
  DLOG(INFO) << "TablePartition--------------------------";
}

Status ZPDataServer::SendToPeer(const Node &node, const client::SyncRequest &msg) {
  pink::Status res;
  std::string ip_port = slash::IpPortString(node.ip, node.port);

  slash::MutexLock pl(&mutex_peers_);
  std::unordered_map<std::string, pink::PinkCli*>::iterator iter = peers_.find(ip_port);
  if (iter == peers_.end()) {
    pink::PinkCli *cli = pink::NewPbCli();
    res = cli->Connect(node.ip, node.port);
    if (!res.ok()) {
      delete cli;
      return Status::Corruption(res.ToString());
    }
    iter = (peers_.insert(std::pair<std::string, pink::PinkCli*>(ip_port, cli))).first;
  }
  
  res = iter->second->Send(const_cast<client::SyncRequest*>(&msg));
  if (!res.ok()) {
    // Remove when second Failed, retry outside
    iter->second->Close();
    delete iter->second;
    peers_.erase(iter);
    return Status::Corruption(res.ToString());
  }
  return Status::OK();
}

Table* ZPDataServer::GetOrAddTable(const std::string &table_name) {
  slash::RWLock l(&table_rw_, true);
  auto it = tables_.find(table_name);
  if (it != tables_.end()) {
    return it->second;
  }

  Table *table = NewTable(table_name, g_zp_conf->log_path(), g_zp_conf->data_path());
  tables_[table_name] = table;
  return table;
}

void ZPDataServer::DeleteTable(const std::string &table_name) {
  // TODO wangkang delete table
  // Before which all partition point outside should become invalid
  return;
  slash::RWLock l(&table_rw_, true);
  auto it = tables_.find(table_name);
  if (it != tables_.end()) {
    delete it->second;
  }
  tables_.erase(table_name);
}

// Note: table pointer 
Table* ZPDataServer::GetTable(const std::string &table_name) {
  slash::RWLock l(&table_rw_, false);
  auto it = tables_.find(table_name);
  if (it != tables_.end()) {
    return it->second;
  }
  return NULL;
}

// We will dump all tables when table_name is empty.
void ZPDataServer::DumpTableBinlogOffsets(const std::string &table_name,
                                          std::unordered_map<std::string, std::vector<PartitionBinlogOffset>> &all_offset) {
  slash::RWLock l(&table_rw_, false);
  if (table_name.empty()) {
    for (auto& item : tables_) {
      std::vector<PartitionBinlogOffset> poffset;
      (item.second)->DumpPartitionBinlogOffsets(poffset);
      all_offset.insert(std::pair<std::string,
                        std::vector<PartitionBinlogOffset>>(item.first, poffset));
    }
  } else {
    auto it = tables_.find(table_name);
    if (it != tables_.end()) {
      std::vector<PartitionBinlogOffset> poffset;
      it->second->DumpPartitionBinlogOffsets(poffset);
      all_offset.insert(std::pair<std::string,
                        std::vector<PartitionBinlogOffset>>(it->first, poffset));
    }
  }
}

Partition* ZPDataServer::GetTablePartition(const std::string &table_name, const std::string &key) {
  Table* table = GetTable(table_name);
  return table == NULL ? NULL : table->GetPartition(key);
}

Partition* ZPDataServer::GetTablePartitionById(const std::string &table_name, const int partition_id) {
  Table* table = GetTable(table_name);
  return table == NULL ? NULL : table->GetPartitionById(partition_id);
}

//inline uint32_t ZPDataServer::KeyToPartition(const std::string &key) {
//  assert(partition_count_ != 0);
//  return std::hash<std::string>()(key) % partition_count_;
//}

void ZPDataServer::BGSaveTaskSchedule(void (*function)(void*), void* arg) {
  slash::MutexLock l(&bgsave_thread_protector_);
  bgsave_thread_.StartThread();
  bgsave_thread_.Schedule(function, arg);
}

void ZPDataServer::BGPurgeTaskSchedule(void (*function)(void*), void* arg) {
  slash::MutexLock l(&bgpurge_thread_protector_);
  bgpurge_thread_.StartThread();
  bgpurge_thread_.Schedule(function, arg);
}

// Add Task, remove first if already exist
// Return Status::InvalidArgument means the filenum and offset is Invalid
Status ZPDataServer::AddBinlogSendTask(const std::string &table, int partition_id, const Node& node,
    int32_t filenum, int64_t offset) {
  return binlog_send_pool_.AddNewTask(table, partition_id, node, filenum, offset, true);
}

Status ZPDataServer::RemoveBinlogSendTask(const std::string &table, int partition_id, const Node& node) {
  std::string task_name = ZPBinlogSendTaskName(table, partition_id, node);
  return binlog_send_pool_.RemoveTask(task_name);
}

// Return the task filenum indicated by id and node
// -1 when the task is not exist
// -2 when the task is exist but is processing now
int32_t ZPDataServer::GetBinlogSendFilenum(const std::string &table, int partition_id, const Node& node) {
  std::string task_name = ZPBinlogSendTaskName(table, partition_id, node);
  return binlog_send_pool_.TaskFilenum(task_name);
}

void ZPDataServer::DumpBinlogSendTask() {
  LOG(INFO) << "BinlogSendTask==========================";
  binlog_send_pool_.Dump();
  LOG(INFO) << "BinlogSendTask--------------------------";
}

void ZPDataServer::AddSyncTask(Partition* partition) {
  zp_trysync_thread_->TrySyncTaskSchedule(partition);
}

void ZPDataServer::AddMetacmdTask() {
  zp_metacmd_bgworker_->AddTask();
}

// Here, we dispatch task base on its partition id
// So that the task within same partition will be located on same thread
// So there could be no lock in DoBinlogReceiveTask to keep binlogs order
void ZPDataServer::DispatchBinlogBGWorker(ZPBinlogReceiveTask *task) {
    size_t index = task->option.partition_id % zp_binlog_receive_bgworkers_.size();
    zp_binlog_receive_bgworkers_[index]->AddTask(task);
}

// Statistic related
bool ZPDataServer::GetAllTableName(std::set<std::string>& table_names) {
  slash::RWLock l(&table_rw_, false);
  for (auto iter = tables_.begin(); iter != tables_.end(); iter++) {
    table_names.insert(iter->first);
  }
  return true;
}

bool ZPDataServer::GetTableStat(const std::string& table_name, std::vector<Statistic>& stats) {
  std::set<std::string> stat_tables;
  if (table_name.empty()) {
    GetAllTableName(stat_tables);
  } else {
    stat_tables.insert(table_name);
  }

  for (auto it = stat_tables.begin(); it != stat_tables.end(); it++) {
    Statistic sum;
    sum.table_name = *it;
    for (int i = 0; i < worker_num_; i++) {
      Statistic tmp;
      zp_worker_thread_[i]->GetStat(*it, tmp);
      sum.Add(tmp);
      // TODO anan debug
      //DLOG(INFO) << "TableStat --worker " << i << ":";
      //tmp.Dump();
    }
    // TODO anan debug
    //DLOG(INFO) << "TableStat sum of " << sum.table_name << " is :";
    //sum.Dump();
    stats.push_back(sum);
  }
  return true;
}

bool ZPDataServer::GetTableCapacity(const std::string& table_name, std::vector<Statistic>& capacity_stats) {
  slash::RWLock l(&table_rw_, false);
  if (table_name.empty()) {
    for (auto& item : tables_) {
      Statistic tmp;
      tmp.table_name = item.first;
      (item.second)->GetCapacity(&tmp);
      capacity_stats.push_back(tmp);
    }
  } else {
    auto it = tables_.find(table_name);
    if (it != tables_.end()) {
      Statistic tmp;
      tmp.table_name = it->first;
      it->second->GetCapacity(&tmp);
      capacity_stats.push_back(tmp);
    }
  }
  return true;
}

void ZPDataServer::InitClientCmdTable() {
  // SetCmd
  Cmd* setptr = new SetCmd(kCmdFlagsKv | kCmdFlagsWrite);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(client::Type::SET), setptr));
  // GetCmd
  Cmd* getptr = new GetCmd(kCmdFlagsKv | kCmdFlagsRead);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(client::Type::GET), getptr));
  // DelCmd
  Cmd* delptr = new DelCmd(kCmdFlagsKv | kCmdFlagsWrite);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(client::Type::DEL), delptr));
  // One InfoCmd handle many type queries;
  Cmd* infostatsptr = new InfoCmd(kCmdFlagsAdmin | kCmdFlagsRead);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(client::Type::INFOSTATS), infostatsptr));
  Cmd* infocapacityptr = new InfoCmd(kCmdFlagsAdmin | kCmdFlagsRead);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(client::Type::INFOCAPACITY), infocapacityptr));
  Cmd* infopartitionptr = new InfoCmd(kCmdFlagsAdmin | kCmdFlagsRead);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(client::Type::INFOPARTITION), infopartitionptr));
  // SyncCmd
  Cmd* syncptr = new SyncCmd(kCmdFlagsRead | kCmdFlagsAdmin | kCmdFlagsSuspend);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(client::Type::SYNC), syncptr));
}

void ZPDataServer::DoTimingTask() {
  slash::RWLock l(&table_rw_, false);
  for (auto& pair : tables_) {
    pair.second->DoTimingTask();
  }
}

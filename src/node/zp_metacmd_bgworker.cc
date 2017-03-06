#include "zp_metacmd_bgworker.h"
#include <string.h>
#include <glog/logging.h>
#include <google/protobuf/text_format.h>

#include "zp_data_server.h"
#include "zp_command.h"

extern ZPDataServer* zp_data_server;

ZPMetacmdBGWorker::ZPMetacmdBGWorker() {
    cli_ = pink::NewPbCli();
    cli_->set_connect_timeout(1500);
    bg_thread_ = new pink::BGThread();
  }

ZPMetacmdBGWorker::~ZPMetacmdBGWorker() {
  bg_thread_->set_running(false);
  delete bg_thread_;
  delete cli_;
  LOG(INFO) << "ZPMetacmd thread " << bg_thread_->thread_id() << " exit!!!";
}

void ZPMetacmdBGWorker::AddTask() {
  bg_thread_->StartThread();
  bg_thread_->Schedule(&MetaUpdateTask, static_cast<void*>(this));
}

void ZPMetacmdBGWorker::MetaUpdateTask(void* task) {
  ZPMetacmdBGWorker* worker =  static_cast<ZPMetacmdBGWorker*>(task);
  int64_t receive_epoch = 0;
  if (!zp_data_server->ShouldPullMeta()) {
    // Avoid multiple invalid Pull
    return;
  }

  if (worker->FetchMetaInfo(receive_epoch)) {
    // When we fetch OK, we will FinishPullMeta
    zp_data_server->FinishPullMeta(receive_epoch);
  } else {
    // Sleep and try again
    sleep(kMetacmdInterval);
    zp_data_server->AddMetacmdTask();
  }
}

pink::Status ZPMetacmdBGWorker::Send() {
  ZPMeta::MetaCmd request;

  DLOG(INFO) << "MetacmdThread send pull to MetaServer(" << zp_data_server->meta_ip() << ":"
    << zp_data_server->meta_port() + kMetaPortShiftCmd
    << ") with local("<< zp_data_server->local_ip() << ":" << zp_data_server->local_port() << ")";

  request.set_type(ZPMeta::Type::PULL);
  ZPMeta::MetaCmd_Pull* pull = request.mutable_pull();
  ZPMeta::Node* node = pull->mutable_node();
  node->set_ip(zp_data_server->local_ip());
  node->set_port(zp_data_server->local_port());

  // TODO rm
  std::string text_format;
  google::protobuf::TextFormat::PrintToString(request, &text_format);
  DLOG(INFO) << "MetacmdThread send pull: [" << text_format << "]";

  return cli_->Send(&request);
}

pink::Status ZPMetacmdBGWorker::Recv(int64_t &receive_epoch) {
  pink::Status result;
  ZPMeta::MetaCmdResponse response;
  std::string meta_ip = zp_data_server->meta_ip();
  int meta_port = zp_data_server->meta_port() + kMetaPortShiftCmd;
  result = cli_->Recv(&response); 
  if (result.ok()) {
    DLOG(INFO) << "succ MetacmdThread recv from MetaServer(" << meta_ip << ":" << meta_port;
    std::string text_format;
    google::protobuf::TextFormat::PrintToString(response, &text_format);
    DLOG(INFO) << "Receive from meta(" << meta_ip << ":" << meta_port << "), size: " << response.pull().info().size() << " Response:[" << text_format << "]";

    switch (response.type()) {
      case ZPMeta::Type::PULL:
        return ParsePullResponse(response, receive_epoch);
        break;
      default:
        break;
    }
  }
  return result;
}

pink::Status ZPMetacmdBGWorker::ParsePullResponse(const ZPMeta::MetaCmdResponse &response, int64_t &receive_epoch) {
  if (response.code() != ZPMeta::StatusCode::OK) {
    return pink::Status::IOError(response.msg());
  }

  receive_epoch = response.pull().version();
  ZPMeta::MetaCmdResponse_Pull pull = response.pull();

  DLOG(INFO) << "receive Pull message, will handle " << pull.info_size() << " Tables.";
  std::set<std::string> miss_tables; // Tables I response for before but will not any more
  zp_data_server->GetAllTableName(miss_tables);

  for (int i = 0; i < pull.info_size(); i++) {
    const ZPMeta::Table& table_info = pull.info(i);
    DLOG(INFO) << " - handle Table " << table_info.name();

    // Record tables no longer response for
    miss_tables.erase(table_info.name());

    // Add or Update table info
    Table* table = zp_data_server->GetOrAddTable(table_info.name());
    assert(table != NULL);

    table->SetPartitionCount(table_info.partitions_size());
    for (int j = 0; j < table_info.partitions_size(); j++) {
      const ZPMeta::Partitions& partition = table_info.partitions(j);
      DLOG(INFO) << " - - handle Partition " << partition.id() << 
          ": master is " << partition.master().ip() << ":" << partition.master().port();

      Node master_node(partition.master().ip(), partition.master().port());
      if (master_node.empty()) {
        // No master patitions, simply ignore
        continue;
      }
      std::set<Node> slave_nodes;
      for (int j = 0; j < partition.slaves_size(); j++) {
        slave_nodes.insert(Node(partition.slaves(j).ip(), partition.slaves(j).port()));
      }

      bool result = table->UpdateOrAddPartition(partition.id(), partition.state(), master_node, slave_nodes);
      if (!result) {
        LOG(WARNING) << "Failed to AddPartition " << partition.id() << ", State: " << static_cast<int>(partition.state())
          << ", partition master is " << partition.master().ip() << ":" << partition.master().port() ;
      }
    }
  }

  // Delete expired tables
  for (auto miss : miss_tables) {
    // TODO wangkang Maybe we could support Delete Table later
    Table* table = zp_data_server->GetOrAddTable(miss);
    assert(table != NULL);
    table->LeaveAllPartition();
  }
  
  // Print partitioin info
  zp_data_server->DumpTablePartitions();
  return pink::Status::OK();

}

bool ZPMetacmdBGWorker::FetchMetaInfo(int64_t &receive_epoch) {
  pink::Status s;
  std::string meta_ip = zp_data_server->meta_ip();
  int meta_port = zp_data_server->meta_port() + kMetaPortShiftCmd;
  // No more PickMeta, which should be done by ping thread
  assert(!zp_data_server->meta_ip().empty() && zp_data_server->meta_port() != 0);
  DLOG(INFO) << "MetacmdThread will connect ("<< meta_ip << ":" << meta_port << ")";
  s = cli_->Connect(meta_ip, meta_port);
  if (s.ok()) {
    DLOG(INFO) << "Metacmd connect (" << meta_ip << ":" << meta_port << ") ok!";

    // TODO timeout
    //cli_->set_send_timeout(1000);
    //cli_->set_recv_timeout(1000);

    s = Send();
    DLOG(INFO) << "Metacmd connect (" << meta_ip << ":" << meta_port << ") ok!";
    
    if (!s.ok()) {
      LOG(WARNING) << "Metacmd send to (" << meta_ip << ":" << meta_port << ") failed! caz:" << s.ToString();
      cli_->Close();
      return false;
    }
    DLOG(INFO) << "Metacmd send to (" << meta_ip << ":" << meta_port << ") ok";

    s = Recv(receive_epoch);
    if (!s.ok()) {
      LOG(WARNING) << "Metacmd recv from (" << meta_ip << ":" << meta_port << ") failed! caz:" << s.ToString();
      LOG(WARNING) << "Metacmd recv from (" << meta_ip << ":" << meta_port << ") failed! errno:" << errno << " strerr:" << strerror(errno);
      cli_->Close();
      return false;
    }
    DLOG(INFO) << "Metacmd recv from (" << meta_ip << ":" << meta_port << ") ok";
    cli_->Close();
    return true;
  } else {
    LOG(WARNING) << "Metacmd connect (" << meta_ip << ":" << meta_port << ") failed! caz:" << s.ToString();
    return false;
  }
}

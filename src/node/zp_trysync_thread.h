#ifndef ZP_TRYSYNC_THREAD_H
#define ZP_TRYSYNC_THREAD_H
#include "include/pink_cli.h"
#include "include/slash_mutex.h"
#include "include/bg_thread.h"
#include "zp_const.h"
#include "zp_data_partition.h"

class ZPTrySyncThread {
 public:
  ZPTrySyncThread();
  virtual ~ZPTrySyncThread();
  void TrySyncTaskSchedule(Partition* partition);
  void TrySyncTask(Partition* partition);

 private:
  bool should_exit_;

  // BGThread related
  struct TrySyncTaskArg {
    ZPTrySyncThread* thread;
    Partition* partition;
    TrySyncTaskArg(ZPTrySyncThread* t, Partition* ptr)
        : thread(t), partition(ptr){}
  };
  slash::Mutex bg_thread_protector_;
  pink::BGThread* bg_thread_;
  static void DoTrySyncTask(void* arg);
  bool SendTrySync(Partition *partition);
  bool Send(const Partition* partition, pink::PinkCli* cli);
  
  struct RecvResult {
    client::StatusCode code;
    std::string message;
    uint32_t filenum;
    uint64_t offset;
  };
  bool Recv(Partition* partition, pink::PinkCli* cli, RecvResult* res);

  // Rsync related
  int rsync_flag_;
  void PrepareRsync(Partition *partition);
  void RsyncRef();
  void RsyncUnref();
  
  // Connection related
  std::map<std::string, pink::PinkCli*> client_pool_;
  pink::PinkCli* GetConnection(const Node& node);
  void DropConnection(const Node& node);
};

#endif //ZP_TRYSYNC_THREAD_H

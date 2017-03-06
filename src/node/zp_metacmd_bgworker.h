#ifndef ZP_METACMD_BGWORKER_H
#define ZP_METACMD_BGWORKER_H

#include "zp_meta.pb.h"
#include "include/bg_thread.h"
#include "include/pink_cli.h"
#include "include/slash_status.h"

class ZPMetacmdBGWorker {
 public:

  ZPMetacmdBGWorker();
  virtual ~ZPMetacmdBGWorker();
  void AddTask();

 private:
  pink::PinkCli* cli_;
  pink::BGThread* bg_thread_;
  static void MetaUpdateTask(void* task);

  Status ParsePullResponse(const ZPMeta::MetaCmdResponse &response, int64_t &epoch);
  Status Send();
  Status Recv(int64_t &receive_epoch);
  bool FetchMetaInfo(int64_t &receive_epoch);

};
#endif //ZP_METACMD_BGWORKER_H

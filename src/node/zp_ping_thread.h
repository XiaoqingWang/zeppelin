#ifndef ZP_PING_THREAD_H
#define ZP_PING_THREAD_H

#include "include/pink_cli.h"
#include "include/pink_thread.h"
#include "include/slash_status.h"

class ZPPingThread : public pink::Thread {
 public:

  ZPPingThread() {
        cli_ = pink::NewPbCli();
        cli_->set_connect_timeout(1500);
      }
  virtual ~ZPPingThread();

 private:
  pink::PinkCli *cli_;
  Status Send();
  Status RecvProc();
  virtual void* ThreadMain();
};

#endif

#ifndef ZP_DATA_CLIENT_CONN_H
#define ZP_DATA_CLIENT_CONN_H

#include <string>

#include "client.pb.h"

#include "include/pb_conn.h"
#include "include/pink_thread.h"


class ZPDataWorkerThread;

class ZPDataClientConn : public pink::PbConn {
 public:
  ZPDataClientConn(int fd, std::string ip_port, pink::Thread *thread);
  virtual ~ZPDataClientConn();

  virtual int DealMessage();
  ZPDataWorkerThread* self_thread() {
    return self_thread_;
  }

 private:

  client::CmdRequest request_;
  client::CmdResponse response_;
  ZPDataWorkerThread* self_thread_;
  
  int DealMessageInternal();
};

#endif

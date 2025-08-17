//
// Created by swx on 24-1-4.
//

#ifndef RAFTSERVERRPC_H
#define RAFTSERVERRPC_H

#include <iostream>
#include <memory>
#include <grpcpp/grpcpp.h>
#include "kvServerRPC.grpc.pb.h"

/// @brief 维护当前节点对其他某一个结点的所有rpc通信，包括接收其他节点的rpc和发送
// 对于一个节点来说，对于任意其他的节点都要维护一个rpc连接，

class raftServerRpcUtil {
 private:
  std::unique_ptr<raftKVRpcProctoc::kvServerRpc::Stub> stub;

 public:
  // 主动调用其他节点的两个方法
  bool Get(raftKVRpcProctoc::GetArgs* GetArgs, raftKVRpcProctoc::GetReply* reply);
  bool PutAppend(raftKVRpcProctoc::PutAppendArgs* args, raftKVRpcProctoc::PutAppendReply* reply);

  raftServerRpcUtil(std::string ip, short port);
  ~raftServerRpcUtil();
};

#endif  // RAFTSERVERRPC_H

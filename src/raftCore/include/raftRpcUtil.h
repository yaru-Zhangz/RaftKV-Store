//
// Created by swx on 23-12-28.
//

#ifndef RAFTRPC_H
#define RAFTRPC_H

#include <memory>
#include <grpcpp/grpcpp.h>
#include "raftRPC.grpc.pb.h"

/// @brief 维护当前节点对其他某一个结点的所有rpc发送通信的功能
// 对于一个raft节点来说，对于任意其他的节点都要维护一个rpc连接，即MprpcChannel

class RaftRpcUtil {
 private:
  std::unique_ptr<raftRpcProctoc::raftRpc::Stub> stub_;

 public:
  // 主动调用其他节点的三个方法
  bool AppendEntries(raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *response);
  bool InstallSnapshot(raftRpcProctoc::InstallSnapshotRequest *args, raftRpcProctoc::InstallSnapshotResponse *response);
  bool RequestVote(raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *response);

  RaftRpcUtil(std::string ip, short port);
  ~RaftRpcUtil();
};

#endif  // RAFTRPC_H

#include "raftRpcUtil.h"

#include <grpcpp/grpcpp.h>
#include "raftRPC.grpc.pb.h"

bool RaftRpcUtil::AppendEntries(raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *response) {
  grpc::ClientContext context;
  grpc::Status status = stub_->AppendEntries(&context, *args, response);
  return status.ok();
}

bool RaftRpcUtil::InstallSnapshot(raftRpcProctoc::InstallSnapshotRequest *args,
                                  raftRpcProctoc::InstallSnapshotResponse *response) {
  grpc::ClientContext context;
  grpc::Status status = stub_->InstallSnapshot(&context, *args, response);
  return status.ok();
}

bool RaftRpcUtil::RequestVote(raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *response) {
  grpc::ClientContext context;
  grpc::Status status = stub_->RequestVote(&context, *args, response);
  return status.ok();
}

//先开启服务器，再尝试连接其他的节点，中间给一个间隔时间，等待其他的rpc服务器节点启动

RaftRpcUtil::RaftRpcUtil(std::string ip, short port) {
  std::string target = ip + ":" + std::to_string(port);
  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  stub_ = raftRpcProctoc::raftRpc::NewStub(channel);
}

RaftRpcUtil::~RaftRpcUtil() = default;

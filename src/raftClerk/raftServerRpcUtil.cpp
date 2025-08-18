#include "raftServerRpcUtil.h"

// kvserver不同于raft节点之间，kvserver的rpc是用于clerk向kvserver调用，不会被调用，因此只用写caller功能，不用写callee功能
//先开启服务器，再尝试连接其他的节点，中间给一个间隔时间，等待其他的rpc服务器节点启动
raftServerRpcUtil::raftServerRpcUtil(std::string ip, short port) {
  //*********************************************  */
  // 接收rpc设置
  //*********************************************  */
  //发送rpc设置
    std::string target = ip + ":" + std::to_string(port);
    auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    stub = raftKVRpcProctoc::kvServerRpc::NewStub(channel);
}

raftServerRpcUtil::~raftServerRpcUtil() = default;

bool raftServerRpcUtil::Get(raftKVRpcProctoc::GetArgs *GetArgs, raftKVRpcProctoc::GetReply *reply) {
  grpc::ClientContext context;
  grpc::Status status = stub->Get(&context, *GetArgs, reply);
  return status.ok();
}

bool raftServerRpcUtil::PutAppend(raftKVRpcProctoc::PutAppendArgs *args, raftKVRpcProctoc::PutAppendReply *reply) {
  grpc::ClientContext context;
  grpc::Status status = stub->PutAppend(&context, *args, reply);
  if (!status.ok()) {
    std::cout << status.error_message() << std::endl;
  }
  return status.ok();
}

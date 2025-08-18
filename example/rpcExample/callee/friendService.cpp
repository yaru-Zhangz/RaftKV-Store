#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include<fstream>
#include <grpcpp/grpcpp.h>
#include "rpcExample/friend.pb.h"
#include "rpcExample/friend.grpc.pb.h"


// 实现gRPC服务端，负责对外提供好友列表查询服务

class FriendServiceImpl final : public fixbug::FiendServiceRpc::Service {
 public:

  grpc::Status GetFriendsList(grpc::ServerContext* context, const fixbug::GetFriendsListRequest* request,
                              fixbug::GetFriendsListResponse* response) override {
    uint32_t userid = request->userid();
    std::cout << "gRPC do GetFriendsList service! userid:" << userid << std::endl;
    std::vector<std::string> friendsList = {"gao yang", "liu hong", "wang shuo"};
    response->mutable_result()->set_errcode(0);
    response->mutable_result()->set_errmsg("");
    for (const std::string& name : friendsList) {
      response->add_friends(name);
    }
    return grpc::Status::OK;
  }
};

// 创建服务实现对象，配置gRPC服务器监听地址，注册服务并启动服务器，进入阻塞等待远程调用
int main(int argc, char** argv) {
  std::string ip = "127.0.0.1";
  short port = 7788;
  std::string server_address = ip + ":" + std::to_string(port);

  // 写入 test.conf（如果你还需要）
  std::ofstream outfile("test.conf", std::ios::app);
  if (outfile.is_open()) {
    outfile << "node1ip=" << ip << std::endl;
    outfile << "node1port=" << port << std::endl;
    outfile.close();
  } else {
    std::cerr << "无法打开 test.conf" << std::endl;
    return 1;
  }
  FriendServiceImpl service;

  grpc::ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  std::cout << "gRPC Server listening on " << server_address << std::endl;
  server->Wait();
  return 0;
}

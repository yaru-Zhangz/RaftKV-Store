#include <iostream>
#include <memory>
#include <grpcpp/grpcpp.h>
#include "rpcExample/friend.pb.h"
#include "rpcExample/friend.grpc.pb.h"


int main(int argc, char **argv) {
  std::string target_str = "127.0.0.1:7788";
  auto channel = grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials());
  std::unique_ptr<fixbug::FiendServiceRpc::Stub> stub = fixbug::FiendServiceRpc::NewStub(channel);

  fixbug::GetFriendsListRequest request;
  request.set_userid(1000);
  fixbug::GetFriendsListResponse response;
  grpc::ClientContext context;

  int count = 10;
  while (count--) {
    std::cout << " 倒数" << count << "次发起gRPC请求" << std::endl;
    grpc::Status status = stub->GetFriendsList(&context, request, &response);
    if (!status.ok()) {
      std::cout << "gRPC failed: " << status.error_message() << std::endl;
    } else {
      if (0 == response.result().errcode()) {
        std::cout << "gRPC GetFriendsList response success!" << std::endl;
        int size = response.friends_size();
        for (int i = 0; i < size; i++) {
          std::cout << "index:" << (i + 1) << " name:" << response.friends(i) << std::endl;
        }
      } else {
        std::cout << "gRPC GetFriendsList response error : " << response.result().errmsg() << std::endl;
      }
    }
    sleep(5);
    context.~ClientContext();
    new (&context) grpc::ClientContext();
  }
  return 0;
}

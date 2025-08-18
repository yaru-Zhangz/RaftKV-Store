
set(SRC_LIST callFriendService.cpp ../friend.pb.cc ../friend.grpc.pb.cc)
add_executable(consumer ${SRC_LIST})
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
target_link_libraries(consumer gRPC::grpc++ protobuf::libprotobuf pthread)
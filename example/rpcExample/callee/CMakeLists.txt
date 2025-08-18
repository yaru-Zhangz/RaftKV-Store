

set(SRC_LIST friendService.cpp ../friend.pb.cc ../friend.grpc.pb.cc)

add_executable(provider ${SRC_LIST})
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
target_link_libraries(provider gRPC::grpc++ protobuf::libprotobuf pthread)

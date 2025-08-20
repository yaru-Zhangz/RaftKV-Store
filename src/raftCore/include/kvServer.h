#ifndef SKIP_LIST_ON_RAFT_KVSERVER_H
#define SKIP_LIST_ON_RAFT_KVSERVER_H

#include <mutex>
#include <iostream>
#include <unordered_map>
#include <boost/any.hpp>
#include <boost/foreach.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/serialization/export.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/vector.hpp>

#include "kvServerRPC.grpc.pb.h"
#include "raft.h"
#include "skipList.h"

class KvServer : public raftKVRpcProctoc::kvServerRpc::Service {
private:

    std::shared_ptr<Raft> m_raftNode;
    std::shared_ptr<LockQueue<ApplyMsg> > applyChan;  // Raft与KVServer之间的消息队列，Raft日志应用后通过它通知KVServer

    std::string m_serializedKVData;                   // 用于序列化KV数据的临时字符串 
    SkipList<std::string, std::string> m_skipList;    // 实际的KV数据库，底层用跳表实现
    std::unordered_map<int, LockQueue<Op>*> waitApplyCh;    // 为每一个正在等待Raft日志应用结果的客户端请求，建立一个“等待队列”
    std::unordered_map<std::string, int> m_lastRequestId;  // 记录每个客户端最后一次请求的ID，用于幂等性和去重

    int m_lastSnapShotRaftLogIndex;                    // 记录最近一次快照对应的日志索引
    int m_maxRaftState;                               // Raft日志超过该大小时触发快照
    std::mutex m_mtx;
    int m_me;
public:
    KvServer() = delete;
    KvServer(int me, int maxraftstate, std::string nodeInforFileName, short port);

    void StartKVServer();   // 预留，暂未实现
    void DprintfKVDB();    // 在调试模式下打印KV数据库

    void ExecuteAppendOpOnKVDB(Op op);
    void ExecuteGetOpOnKVDB(Op op, std::string *value, bool *exist);
    void ExecutePutOpOnKVDB(Op op);

    void GetCommandFromRaft(ApplyMsg message);  // 从这里调用上面三个函数

    bool ifRequestDuplicate(std::string ClientId, int RequestId);   // 判断请求是否重复，保证幂等性

    // gRPC远程调用接口
    void PutAppend(const raftKVRpcProctoc::PutAppendArgs *args, raftKVRpcProctoc::PutAppendReply *reply);
    void Get(const raftKVRpcProctoc::GetArgs *args, raftKVRpcProctoc::GetReply *reply);


    void ReadRaftApplyCommandLoop();                          // 不断从applyChan取出Raft应用的日志或快照，应用到KVServer
    bool SendMessageToWaitChan(const Op &op, int raftIndex);  // 向等待队列推送操作，唤醒等待的RPC
    
    // 快照相关操作
    void ReadSnapShotToInstall(std::string snapshot);        
    void IfNeedToSendSnapShotCommand(int raftIndex, int proportion);
    void GetSnapShotFromRaft(ApplyMsg message);
    std::string MakeSnapShot();

public:  
    grpc::Status Get(grpc::ServerContext* context, const raftKVRpcProctoc::GetArgs* request, raftKVRpcProctoc::GetReply* response) override;
    grpc::Status PutAppend(grpc::ServerContext* context, const raftKVRpcProctoc::PutAppendArgs* request, raftKVRpcProctoc::PutAppendReply* response) override;

private:
    friend class boost::serialization::access;

    template <class Archive>
    void serialize(Archive &ar, const unsigned int version)  //这里面写需要序列话和反序列化的字段
    {
        ar &m_serializedKVData;
        ar &m_lastRequestId;
    }

    // 将当前对象序列化为字符串
    std::string getSnapshotData() {
        m_serializedKVData = m_skipList.dump_file();
        std::stringstream ss;
        boost::archive::text_oarchive oa(ss);
        oa << *this;
        m_serializedKVData.clear();
        return ss.str();
    }

    // 从字符串反序列化恢复对象状态
    void parseFromString(const std::string &str) {
        std::stringstream ss(str);
        boost::archive::text_iarchive ia(ss);
        ia >> *this;
        m_skipList.load_file(m_serializedKVData);
        m_serializedKVData.clear();
    }


};

#endif 

#include "kvServer.h"
#include "util.h"
#include <grpcpp/grpcpp.h>
#include "kvServerRPC.grpc.pb.h"

void KvServer::DprintfKVDB() {
    if (!Debug) {
        return;
    }
    std::lock_guard<std::mutex> lg(m_mtx);
    DEFER {
        m_skipList.display_list();
    };
}

void KvServer::ExecuteAppendOpOnKVDB(Op op) {

    m_mtx.lock();
    m_skipList.insert_set_element(op.Key, op.Value);
    m_lastRequestId[op.ClientId] = op.RequestId;    // 记录该客户端最新请求ID
    m_mtx.unlock();
    DprintfKVDB();
}

void KvServer::ExecuteGetOpOnKVDB(Op op, std::string *value, bool *exist) {
    m_mtx.lock();
    *value = "";
    *exist = false;
    if (m_skipList.search_element(op.Key, *value)) {
        *exist = true;
    }
    m_lastRequestId[op.ClientId] = op.RequestId;
    m_mtx.unlock();
    DprintfKVDB();
}

void KvServer::ExecutePutOpOnKVDB(Op op) {
    m_mtx.lock();
    m_skipList.insert_set_element(op.Key, op.Value);
    m_lastRequestId[op.ClientId] = op.RequestId;
    m_mtx.unlock();
    DprintfKVDB();
}

void KvServer::Get(const raftKVRpcProctoc::GetArgs *args, raftKVRpcProctoc::GetReply *reply) {
    // 将Get请求封装为Op交给Raft
    Op op;
    op.Operation = "Get";
    op.Key = args->key();
    op.Value = "";
    op.ClientId = args->clientid();
    op.RequestId = args->requestid();

    int raftIndex = -1;
    int _ = -1;
    bool isLeader = false;
    m_raftNode->Start(op, &raftIndex, &_, &isLeader);   // 将本次操作作为日志提交到Raft

    // 如果不是leader直接返回错误，客户端会重试到其他节点
    if (!isLeader) {
        reply->set_err(ErrWrongLeader);
        return;
    }

    // 为本次请求创建等待队列，阻塞等待 Raft 日志应用结果
    m_mtx.lock();
    if (waitApplyCh.find(raftIndex) == waitApplyCh.end()) {
        waitApplyCh.insert(std::make_pair(raftIndex, new LockQueue<Op>()));
    }
    auto chForRaftIndex = waitApplyCh[raftIndex];
    m_mtx.unlock();  

    Op raftCommitOp;

    // 阻塞等待Raft日志被应用
    if (!chForRaftIndex->timeOutPop(CONSENSUS_TIMEOUT, &raftCommitOp)) {    // 超时
        int _ = -1;
        bool isLeader = false;
        m_raftNode->GetState(&_, &isLeader);

        // 如果本次请求是重复请求，且自己还是leader就从本地数据库直接返回结果
        if (ifRequestDuplicate(op.ClientId, op.RequestId) && isLeader) {
            std::string value;
            bool exist = false;
            ExecuteGetOpOnKVDB(op, &value, &exist);
            if (exist) {
                reply->set_err(OK);
                reply->set_value(value);
            } else {
                reply->set_err(ErrNoKey);
                reply->set_value("");
            }
        } else {
            reply->set_err(ErrWrongLeader);  // 让客户端重试
        }
    } else {
        if (raftCommitOp.ClientId == op.ClientId && raftCommitOp.RequestId == op.RequestId) {
            std::string value;
            bool exist = false;
            ExecuteGetOpOnKVDB(op, &value, &exist);
            if (exist) {
                reply->set_err(OK);
                reply->set_value(value);
            } else {
                reply->set_err(ErrNoKey);
                reply->set_value("");
            }
        } else {
            reply->set_err(ErrWrongLeader);
        }
    }
    m_mtx.lock(); 
    auto tmp = waitApplyCh[raftIndex];
    waitApplyCh.erase(raftIndex);
    delete tmp;
    m_mtx.unlock();
}

void KvServer::GetCommandFromRaft(ApplyMsg message) {
    Op op;
    op.parseFromString(message.Command);

    DPrintf(
        "[KvServer::GetCommandFromRaft-kvserver{%d}] , Got Command --> Index:{%d} , ClientId {%s}, RequestId {%d}, "
        "Opreation {%s}, Key :{%s}, Value :{%s}",
        m_me, message.CommandIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
    
    // 如果日志索引小于等于当前快照点，说明已经被快照覆盖
    if (message.CommandIndex <= m_lastSnapShotRaftLogIndex) {
        return;
    }

    // 判断请求是否已经被执行过
    if (!ifRequestDuplicate(op.ClientId, op.RequestId)) {
        if (op.Operation == "Put") {
            ExecutePutOpOnKVDB(op);
        }
        if (op.Operation == "Append") {
            ExecuteAppendOpOnKVDB(op);
        }
    }

    if (m_maxRaftState != -1) {
        IfNeedToSendSnapShotCommand(message.CommandIndex, 9);
    }

    SendMessageToWaitChan(op, message.CommandIndex);
}


bool KvServer::ifRequestDuplicate(std::string ClientId, int RequestId) {
    std::lock_guard<std::mutex> lg(m_mtx);

    // 如果服务端第一次收到某个客户端的请求，表示不是重复的请求
    if (m_lastRequestId.find(ClientId) == m_lastRequestId.end()) {
        return false;
    }
    
    // 如果当前请求的 RequestId 小于等于服务端记录的该客户端的最大 RequestId，说明这个请求已经被处理过
    return RequestId <= m_lastRequestId[ClientId];
}

// get和put//append執行的具體細節是不一樣的
// PutAppend在收到raft消息之後執行，具體函數裏面只判斷冪等性（是否重複）
// get函數收到raft消息之後在，因爲get無論是否重複都可以再執行
void KvServer::PutAppend(const raftKVRpcProctoc::PutAppendArgs *args, raftKVRpcProctoc::PutAppendReply *reply) {
    Op op;
    op.Operation = args->op();
    op.Key = args->key();
    op.Value = args->value();
    op.ClientId = args->clientid();
    op.RequestId = args->requestid();
    int raftIndex = -1;
    int _ = -1;
    bool isleader = false;

    m_raftNode->Start(op, &raftIndex, &_, &isleader);

    if (!isleader) {
        DPrintf(
            "[func -KvServer::PutAppend -kvserver{%d}]From Client %s (Request %d) To Server %d, key %s, raftIndex %d , but "
            "not leader",
            m_me, &args->clientid(), args->requestid(), m_me, &op.Key, raftIndex);

        reply->set_err(ErrWrongLeader);
        return;
    }
    DPrintf(
        "[func -KvServer::PutAppend -kvserver{%d}]From Client %s (Request %d) To Server %d, key %s, raftIndex %d , is "
        "leader ",
        m_me, &args->clientid(), args->requestid(), m_me, &op.Key, raftIndex);
    m_mtx.lock();
    if (waitApplyCh.find(raftIndex) == waitApplyCh.end()) {
        waitApplyCh.insert(std::make_pair(raftIndex, new LockQueue<Op>()));
    }
    auto chForRaftIndex = waitApplyCh[raftIndex];

    m_mtx.unlock();  //直接解锁，等待任务执行完成，不能一直拿锁等待

    // timeout
    Op raftCommitOp;

    if (!chForRaftIndex->timeOutPop(CONSENSUS_TIMEOUT, &raftCommitOp)) {
        DPrintf(
            "[func -KvServer::PutAppend -kvserver{%d}]TIMEOUT PUTAPPEND !!!! Server %d , get Command <-- Index:%d , "
            "ClientId %s, RequestId %s, Opreation %s Key :%s, Value :%s",
            m_me, m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);

        if (ifRequestDuplicate(op.ClientId, op.RequestId)) {
        reply->set_err(OK);  // 超时了,但因为是重复的请求，返回ok，实际上就算没有超时，在真正执行的时候也要判断是否重复
        } else {
        reply->set_err(ErrWrongLeader);  ///这里返回这个的目的让clerk重新尝试
        }
    } else {
        DPrintf(
            "[func -KvServer::PutAppend -kvserver{%d}]WaitChanGetRaftApplyMessage<--Server %d , get Command <-- Index:%d , "
            "ClientId %s, RequestId %d, Opreation %s, Key :%s, Value :%s",
            m_me, m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
        if (raftCommitOp.ClientId == op.ClientId && raftCommitOp.RequestId == op.RequestId) {
        //可能发生leader的变更导致日志被覆盖，因此必须检查
        reply->set_err(OK);
        } else {
        reply->set_err(ErrWrongLeader);
        }
    }

    m_mtx.lock();

    auto tmp = waitApplyCh[raftIndex];
    waitApplyCh.erase(raftIndex);
    delete tmp;
    m_mtx.unlock();
}

void KvServer::ReadRaftApplyCommandLoop() {
    while (true) {
        // Raft通知KVServer有新日志需要应用
        auto message = applyChan->Pop(); 
        DPrintf("---------------tmp-------------[func-KvServer::ReadRaftApplyCommandLoop()-kvserver{%d}] 收到了下raft的消息", m_me);
        
        if (message.CommandValid) {         // 普通日志
            GetCommandFromRaft(message);
        }
        if (message.SnapshotValid) {        // 快照消息
            GetSnapShotFromRaft(message);
        }
    }
}

// raft会与persist层交互，kvserver层也会，因为kvserver层开始的时候需要恢复kvdb的状态
//  关于快照raft层与persist的交互：保存kvserver传来的snapshot；生成leaderInstallSnapshot RPC的时候也需要读取snapshot；
//  因此snapshot的具体格式是由kvserver层来定的，raft只负责传递这个东西
//  snapShot里面包含kvserver需要维护的persist_lastRequestId 以及kvDB真正保存的数据persist_kvdb

void KvServer::ReadSnapShotToInstall(std::string snapshot) {
  if (snapshot.empty()) {
    return;
  }
  parseFromString(snapshot);
}

// 当Raft日志被应用到状态机，将对应的操作推送到等待该日志索引的等待队列中，从而唤醒正在等待该日志应用结果的主线程
bool KvServer::SendMessageToWaitChan(const Op &op, int raftIndex) {
  std::lock_guard<std::mutex> lg(m_mtx);
  DPrintf(
      "[RaftApplyMessageSendToWaitChan--> raftserver{%d}] , Send Command --> Index:{%d} , ClientId {%d}, RequestId "
      "{%d}, Opreation {%v}, Key :{%v}, Value :{%v}",
      m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);

  if (waitApplyCh.find(raftIndex) == waitApplyCh.end()) {
    return false;
  }
  waitApplyCh[raftIndex]->Push(op);
  DPrintf(
      "[RaftApplyMessageSendToWaitChan--> raftserver{%d}] , Send Command --> Index:{%d} , ClientId {%d}, RequestId "
      "{%d}, Opreation {%v}, Key :{%v}, Value :{%v}",
      m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
  return true;
}

void KvServer::IfNeedToSendSnapShotCommand(int raftIndex, int proportion) {
  if (m_raftNode->GetRaftStateSize() > m_maxRaftState / 10.0) {
    // Send SnapShot Command
    auto snapshot = MakeSnapShot();
    m_raftNode->Snapshot(raftIndex, snapshot);
  }
}

void KvServer::GetSnapShotFromRaft(ApplyMsg message) {
  std::lock_guard<std::mutex> lg(m_mtx);

  if (m_raftNode->CondInstallSnapshot(message.SnapshotTerm, message.SnapshotIndex, message.Snapshot)) {
    ReadSnapShotToInstall(message.Snapshot);
    m_lastSnapShotRaftLogIndex = message.SnapshotIndex;
  }
}

std::string KvServer::MakeSnapShot() {
  std::lock_guard<std::mutex> lg(m_mtx);
  std::string snapshotData = getSnapshotData();
  return snapshotData;
}

grpc::Status KvServer::Get(grpc::ServerContext* context, const raftKVRpcProctoc::GetArgs* request, raftKVRpcProctoc::GetReply* response) {
    this->Get(request, response);
    return grpc::Status::OK;
}

grpc::Status KvServer::PutAppend(grpc::ServerContext* context, const raftKVRpcProctoc::PutAppendArgs* request, raftKVRpcProctoc::PutAppendReply* response) {
    this->PutAppend(request, response);
    return grpc::Status::OK;
}

KvServer::KvServer(int me, int maxraftstate, std::string nodeInforFileName, short port) 
    : m_skipList(6)
    , m_lastSnapShotRaftLogIndex(0) {
  std::shared_ptr<Persister> persister = std::make_shared<Persister>(me);

    m_me = me;
    m_maxRaftState = maxraftstate;
    applyChan = std::make_shared<LockQueue<ApplyMsg> >();
    m_raftNode = std::make_shared<Raft>();

    // 启动一个新线程，负责监听本节点的gRPC服务端口，写入本节点的IP和端口号到配置文件
    // 构造gRPC服务器，注册KVServer和Raft服务，启动监听
    std::thread t([this, port]() -> void {
        std::string my_ip = writeConfig(m_me, port);
        std::string server_address = my_ip + ":" + std::to_string(port);
        grpc::ServerBuilder builder;
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
        builder.RegisterService(this);
        builder.RegisterService(m_raftNode.get());
        std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
        server->Wait();
    });
    t.detach();

    std::cout << "raftServer node:" << m_me << " start to sleep to wait all other raftnode start!!!!" << std::endl;
    sleep(6);
    std::cout << "raftServer node:" << m_me << " wake up!!!! start to connect other raftnode" << std::endl;

    // 读取所有raft节点ip、port，并进行连接
    std::vector<std::pair<std::string, short>> ipPortVt;
    {
        std::ifstream conf(nodeInforFileName);
        std::string line;
        std::vector<std::string> ips;
        std::vector<short> ports;
        while (std::getline(conf, line)) {
            if (line.empty()) continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            if (key.find("ip") != std::string::npos) {
                ips.push_back(value);
            } else if (key.find("port") != std::string::npos) {
                ports.push_back(static_cast<short>(std::stoi(value)));
            }
        }
        size_t n = std::min(ips.size(), ports.size());
        for (size_t i = 0; i < n; ++i) {
            ipPortVt.emplace_back(ips[i], ports[i]);
        }
    }

    // 构造Raft节点间通信对象
    std::vector<std::shared_ptr<RaftRpcUtil>> servers;
    for (int i = 0; i < ipPortVt.size(); ++i) {
        if (i == m_me) {
        servers.push_back(nullptr);
        } else {
        std::cout << "ip = " << ipPortVt[i].first << "  port = " << ipPortVt[i].second << std::endl;
        servers.push_back(std::make_shared<RaftRpcUtil>(ipPortVt[i].first, ipPortVt[i].second));
        }
    }
    m_raftNode->init(servers, m_me, persister, applyChan);

    // 如果持久化层有快照，恢复快快照到KVServer
    auto snapshot = persister->ReadSnapshot();
    if (!snapshot.empty()) {
        ReadSnapShotToInstall(snapshot);
    }

    // 启动一个线程，循环从 applyChan 取出 Raft 应用的日志或快照，应用到KVServer
    std::thread t2(&KvServer::ReadRaftApplyCommandLoop, this);  
    t2.join(); 
}


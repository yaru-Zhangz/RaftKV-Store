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
  // if op.IfDuplicate {   //get请求是可重复执行的，因此可以不用判复
  //	return
  // }
  m_mtx.lock();

  m_skipList.insert_set_element(op.Key, op.Value);

  m_lastRequestId[op.ClientId] = op.RequestId;
  m_mtx.unlock();

  //    DPrintf("[KVServerExeAPPEND-----]ClientId :%d ,RequestID :%d ,Key : %v, value : %v", op.ClientId, op.RequestId,
  //    op.Key, op.Value)
  DprintfKVDB();
}

void KvServer::ExecuteGetOpOnKVDB(Op op, std::string *value, bool *exist) {
  m_mtx.lock();
  *value = "";
  *exist = false;
  if (m_skipList.search_element(op.Key, *value)) {
    *exist = true;
    // *value = m_skipList.se //value已经完成赋值了
  }
  m_lastRequestId[op.ClientId] = op.RequestId;
  m_mtx.unlock();

  if (*exist) {
    //                DPrintf("[KVServerExeGET----]ClientId :%d ,RequestID :%d ,Key : %v, value :%v", op.ClientId,
    //                op.RequestId, op.Key, value)
  } else {
    //        DPrintf("[KVServerExeGET----]ClientId :%d ,RequestID :%d ,Key : %v, But No KEY!!!!", op.ClientId,
    //        op.RequestId, op.Key)
  }
  DprintfKVDB();
}

void KvServer::ExecutePutOpOnKVDB(Op op) {
  m_mtx.lock();
  m_skipList.insert_set_element(op.Key, op.Value);
  m_lastRequestId[op.ClientId] = op.RequestId;
  m_mtx.unlock();

  //    DPrintf("[KVServerExePUT----]ClientId :%d ,RequestID :%d ,Key : %v, value : %v", op.ClientId, op.RequestId,
  //    op.Key, op.Value)
  DprintfKVDB();
}

// 处理来自clerk的Get RPC
void KvServer::Get(const raftKVRpcProctoc::GetArgs *args, raftKVRpcProctoc::GetReply *reply) {
  Op op;
  op.Operation = "Get";
  op.Key = args->key();
  op.Value = "";
  op.ClientId = args->clientid();
  op.RequestId = args->requestid();

  int raftIndex = -1;
  int _ = -1;
  bool isLeader = false;
  m_raftNode->Start(op, &raftIndex, &_,
                    &isLeader);  // raftIndex：raft预计的logIndex
                                 // ，虽然是预计，但是正确情况下是准确的，op的具体内容对raft来说 是隔离的

  if (!isLeader) {
    reply->set_err(ErrWrongLeader);
    return;
  }

  // create waitForCh
  m_mtx.lock();

  if (waitApplyCh.find(raftIndex) == waitApplyCh.end()) {
    waitApplyCh.insert(std::make_pair(raftIndex, new LockQueue<Op>()));
  }
  auto chForRaftIndex = waitApplyCh[raftIndex];

  m_mtx.unlock();  //直接解锁，等待任务执行完成，不能一直拿锁等待

  // timeout
  Op raftCommitOp;

  if (!chForRaftIndex->timeOutPop(CONSENSUS_TIMEOUT, &raftCommitOp)) {
    //        DPrintf("[GET TIMEOUT!!!]From Client %d (Request %d) To Server %d, key %v, raftIndex %d", args.ClientId,
    //        args.RequestId, kv.me, op.Key, raftIndex)
    // todo 2023年06月01日
    int _ = -1;
    bool isLeader = false;
    m_raftNode->GetState(&_, &isLeader);

    if (ifRequestDuplicate(op.ClientId, op.RequestId) && isLeader) {
      //如果超时，代表raft集群不保证已经commitIndex该日志，但是如果是已经提交过的get请求，是可以再执行的。
      // 不会违反线性一致性
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
      reply->set_err(ErrWrongLeader);  //返回这个，其实就是让clerk换一个节点重试
    }
  } else {
    // raft已经提交了该command（op），可以正式开始执行了
    //         DPrintf("[WaitChanGetRaftApplyMessage<--]Server %d , get Command <-- Index:%d , ClientId %d, RequestId
    //         %d, Opreation %v, Key :%v, Value :%v", kv.me, raftIndex, op.ClientId, op.RequestId, op.Operation, op.Key,
    //         op.Value)
    // todo 这里还要再次检验的原因：感觉不用检验，因为leader只要正确的提交了，那么这些肯定是符合的
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
      //            DPrintf("[GET ] 不满足：raftCommitOp.ClientId{%v} == op.ClientId{%v} && raftCommitOp.RequestId{%v}
      //            == op.RequestId{%v}", raftCommitOp.ClientId, op.ClientId, raftCommitOp.RequestId, op.RequestId)
    }
  }
  m_mtx.lock();  // todo 這個可以先弄一個defer，因爲刪除優先級並不高，先把rpc發回去更加重要
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
  if (message.CommandIndex <= m_lastSnapShotRaftLogIndex) {
    return;
  }

  // State Machine (KVServer solute the duplicate problem)
  // duplicate command will not be exed
  if (!ifRequestDuplicate(op.ClientId, op.RequestId)) {
    // execute command
    if (op.Operation == "Put") {
      ExecutePutOpOnKVDB(op);
    }
    if (op.Operation == "Append") {
      ExecuteAppendOpOnKVDB(op);
    }
    //  kv.lastRequestId[op.ClientId] = op.RequestId  在Executexxx函数里面更新的
  }
  //到这里kvDB已经制作了快照
  if (m_maxRaftState != -1) {
    IfNeedToSendSnapShotCommand(message.CommandIndex, 9);
    //如果raft的log太大（大于指定的比例）就把制作快照
  }

  // Send message to the chan of op.ClientId
  SendMessageToWaitChan(op, message.CommandIndex);
}

bool KvServer::ifRequestDuplicate(std::string ClientId, int RequestId) {
  std::lock_guard<std::mutex> lg(m_mtx);
  if (m_lastRequestId.find(ClientId) == m_lastRequestId.end()) {
    return false;
    // todo :不存在这个client就创建
  }
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
    //如果只操作applyChan不用拿锁，因为applyChan自己带锁
    auto message = applyChan->Pop();  //阻塞弹出
    DPrintf(
        "---------------tmp-------------[func-KvServer::ReadRaftApplyCommandLoop()-kvserver{%d}] 收到了下raft的消息",
        m_me);
    // listen to every command applied by its raft ,delivery to relative RPC Handler

    if (message.CommandValid) {
      GetCommandFromRaft(message);
    }
    if (message.SnapshotValid) {
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
    // bootstrap without any state?
    return;
  }
  parseFromString(snapshot);

  //    r := bytes.NewBuffer(snapshot)
  //    d := labgob.NewDecoder(r)
  //
  //    var persist_kvdb map[string]string  //理应快照
  //    var persist_lastRequestId map[int64]int //快照这个为了维护线性一致性
  //
  //    if d.Decode(&persist_kvdb) != nil || d.Decode(&persist_lastRequestId) != nil {
  //                DPrintf("KVSERVER %d read persister got a problem!!!!!!!!!!",kv.me)
  //        } else {
  //        kv.kvDB = persist_kvdb
  //        kv.lastRequestId = persist_lastRequestId
  //    }
}

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

KvServer::KvServer(int me, int maxraftstate, std::string nodeInforFileName, short port) : m_skipList(6) {
  std::shared_ptr<Persister> persister = std::make_shared<Persister>(me);

  m_me = me;
  m_maxRaftState = maxraftstate;
  applyChan = std::make_shared<LockQueue<ApplyMsg> >();
  m_raftNode = std::make_shared<Raft>();

  ////////////////clerk层面 kvserver开启rpc接受功能
  //    同时raft与raft节点之间也要开启rpc功能，因此有两个注册
  // 读取所有raft节点ip、port，并进行连接

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

  // 读取所有raft节点ip、port，并进行连接（兼容 node0ip=... node0port=... 格式）
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
  // kv的server直接与raft通信，但kv不直接与raft通信，所以需要把ApplyMsg的chan传递下去用于通信，两者的persist也是共用的

  //////////////////////////////////

  // You may need initialization code here.
  // m_kvDB; //kvdb初始化
  m_skipList;
  waitApplyCh;
  m_lastRequestId;
  m_lastSnapShotRaftLogIndex = 0;  // todo:感覺這個函數沒什麼用，不如直接調用raft節點中的snapshot值？？？
  auto snapshot = persister->ReadSnapshot();
  if (!snapshot.empty()) {
    ReadSnapShotToInstall(snapshot);
  }
  std::thread t2(&KvServer::ReadRaftApplyCommandLoop, this);  //马上向其他节点宣告自己就是leader
  t2.join();  //由於ReadRaftApplyCommandLoop一直不會結束，达到一直卡在这的目的
}


#ifndef RAFT_H
#define RAFT_H

#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/any.hpp>

#include "ApplyMsg.h"
#include "Persister.h"
#include "config.h"
#include "monsoon.h"
#include "raftRpcUtil.h"
#include "util.h"
//  表示节点的网络状态
constexpr int Disconnected = 0;  // 网络异常
constexpr int AppNormal = 1;     //网络正常 

// 网络正常
constexpr int Killed = 0;
constexpr int Voted = 1;   // 本任期内已投票
constexpr int Expire = 2;  // 投票（消息、竞选者）过期
constexpr int Normal = 3;

class Raft : public raftRpcProctoc::raftRpc::Service {
 private:
  std::mutex m_mtx;
  std::vector<std::shared_ptr<RaftRpcUtil>> m_peers; // 保存所有对等节点的RPC工具对象，用于与其他节点通信
  std::shared_ptr<Persister> m_persister;            // 持久化对象，负责保存和回复节点状态，如日志、快照等
 
  int m_me;           // 当前节点编号
  int m_currentTerm;  // 当前任期号
  int m_votedFor;     // 当前任期已投票给的候选人编号
  std::vector<raftRpcProctoc::LogEntry> m_logs;  // 日志条目数组 <日志索引，任期号，指令>
 
  int m_commitIndex;  // 已提交日志的最大索引
  int m_lastApplied;  // 已应用到状态机的最大日志索引

  std::vector<int> m_nextIndex;   // 每个follower下一个要同步的日志索引
  std::vector<int> m_matchIndex;  // 每个follwer已同步的最大日志索引

  // Raft节点的三种状态
  enum Status { 
    Follower, 
    Candidate, 
    Leader 
  };

  // 当前节点身份
  Status m_status;

  std::shared_ptr<LockQueue<ApplyMsg>> applyChan;  // 日志应用消息队列，client从这里取日志


  std::chrono::_V2::system_clock::time_point m_lastResetElectionTime; // 记录上次重置选举超时的时间点
  std::chrono::_V2::system_clock::time_point m_lastResetHearBeatTime; // 记录上次重置心跳超时的时间点


  int m_lastSnapshotIncludeIndex; // 快照中最后一个日志的索引
  int m_lastSnapshotIncludeTerm;  // 快照中最后一个日志的任期

  std::unique_ptr<monsoon::IOManager> m_ioManager = nullptr;  // 协程调度器

public:
  // 处理三种RPC请求的内部实现
  void AppendEntries1(const raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *reply);
  void InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest *args, raftRpcProctoc::InstallSnapshotResponse *reply);
  void RequestVote(const raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *reply);

  void applierTicker();                     // 定期将日志应用到状态机
  bool CondInstallSnapshot(int lastIncludedTerm, int lastIncludedIndex, std::string snapshot);  // 检查快照是否需要安装
  void doElection();                        // 发起选举流程
  void doHeartBeat();                       // 作为Leader定期发送心跳
  void electionTimeOutTicker();             // 检查是否需要发起选举
  std::vector<ApplyMsg> getApplyLogs();     // 获取需要应用的日志
  int getNewCommandIndex();                 // 获取新命令的日志索引
  void getPrevLogInfo(int server, int *preIndex, int *preTerm); // 获取指定follower的前一个日志索引和任期
  void GetState(int *term, bool *isLeader); // 查看当前节点是否是领导节点
  void leaderHearBeatTicker();              // 定期检查是否需要发送心跳
  void leaderSendSnapShot(int server);      // Leader向Follower发送快照
  void leaderUpdateCommitIndex();           // Leader更新已提交的日志索引
  bool matchLog(int logIndex, int logTerm); // 判断日志是否匹配
  void persist();                           // 持久化当前状态
  bool UpToDate(int index, int term);       // 判断日志是否比当前节点新
  int getLastLogIndex();                    // 获取最后一条日志的索引
  int getLastLogTerm();                     // 获取最后一条日志的任期号
  void getLastLogIndexAndTerm(int *lastLogIndex, int *lastLogTerm); // 获取最后一条日志的索引和任期
  int getLogTermFromLogIndex(int logIndex); // 获取指定日志索引的任期
  int GetRaftStateSize();                   // 获取当前raft状态
  int getSlicesIndexFromLogIndex(int logIndex); // 将逻辑日志索引转换为物理数组下标

  // 发送RequestVote RPC
  bool sendRequestVote(int server, std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,
                       std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply, std::shared_ptr<int> votedNum);
  // 发送AppendEntries RPC
  bool sendAppendEntries(int server, std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,
                         std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply, std::shared_ptr<int> appendNums);
  
  void pushMsgToKvServer(ApplyMsg msg);   // 向kvserver推送ApplyMsg
  void readPersist(std::string data);     // 从持久化数据恢复状态
  std::string persistData();              // 获取当前状态的持久化数据

  // Raft对外暴露的命令入口
  void Start(Op command, int *newLogIndex, int *newLogTerm, bool *isLeader);

  // Snapshot the service says it has created a snapshot that has
  // all info up to and including index. this means the
  // service no longer needs the log through (and including)
  // that index. Raft should now trim its log as much as possible.
  // index代表是快照apply应用的index,而snapshot代表的是上层service传来的快照字节流，包括了Index之前的数据
  // 这个函数的目的是把安装到快照里的日志抛弃，并安装快照数据，同时更新快照下标，属于peers自身主动更新，与leader发送快照不冲突
  // 即服务层主动发起请求raft保存snapshot里面的数据，index是用来表示snapshot快照执行到了哪条命令
  // 安装快照，丢弃快照前的日志
  void Snapshot(int index, std::string snapshot);


 public:
  // gRPC服务端接口重载
  grpc::Status AppendEntries(grpc::ServerContext* context,
                             const raftRpcProctoc::AppendEntriesArgs* request,
                             raftRpcProctoc::AppendEntriesReply* response) override;
  grpc::Status InstallSnapshot(grpc::ServerContext* context,
                               const raftRpcProctoc::InstallSnapshotRequest* request,
                               raftRpcProctoc::InstallSnapshotResponse* response) override;
  grpc::Status RequestVote(grpc::ServerContext* context,
                           const raftRpcProctoc::RequestVoteArgs* request,
                           raftRpcProctoc::RequestVoteReply* response) override;

 public:
  void init(std::vector<std::shared_ptr<RaftRpcUtil>> peers, int me, std::shared_ptr<Persister> persister,
            std::shared_ptr<LockQueue<ApplyMsg>> applyCh);

 private:
  // Raft节点持久化数据结构，支持boost序列化
  class BoostPersistRaftNode {
   public:
      friend class boost::serialization::access;
      template <class Archive>
      void serialize(Archive &ar, const unsigned int version) {
        ar &m_currentTerm;
        ar &m_votedFor;
        ar &m_lastSnapshotIncludeIndex;
        ar &m_lastSnapshotIncludeTerm;
        ar &m_logs;
      }
      int m_currentTerm;
      int m_votedFor;
      int m_lastSnapshotIncludeIndex;
      int m_lastSnapshotIncludeTerm;
      std::vector<std::string> m_logs;
      std::unordered_map<std::string, int> umap;
  };
};

#endif  // RAFT_H
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <memory>

#include "config.h"
#include "util.h"
#include "raft.h"

void Raft::AppendEntries1(const raftRpcProctoc::AppendEntriesArgs* args, raftRpcProctoc::AppendEntriesReply* reply) {
    std::lock_guard<std::mutex> locker(m_mtx);
    reply->set_appstate(AppNormal);  // RPC通信正常

    // 如果请求的term比本地小，说明leader已经过期，返回当前term和特殊的updatenextindex
    if (args->term() < m_currentTerm) {
        reply->set_success(false);
        reply->set_term(m_currentTerm);
        reply->set_updatenextindex(-100);
        DPrintf("[func-AppendEntries-rf{%d}] 拒绝了 因为Leader{%d}的term{%v}< rf{%d}.term{%d}\n", m_me, args->leaderid(),
                args->term(), m_me, m_currentTerm);
        return;
    }
    DEFER { persist(); };

    // 如果请求的term比本地大，更新本地term、身份为Follower，重置投票对象
    if (args->term() > m_currentTerm) {
        m_status = Follower;
        m_currentTerm = args->term();
        m_votedFor = -1; 
    }

    // 收到同term的AE，切换状态为Follower，重置选举超时计时器
    myAssert(args->term() == m_currentTerm, format("assert {args.Term == rf.currentTerm} fail"));
    m_status = Follower; 
    m_lastResetElectionTime = now();

    // leader的prevlogindex比本地日志海信，说明本地日志太短，拒绝请求并告知leader下次从哪里同步
    if (args->prevlogindex() > getLastLogIndex()) {
        reply->set_success(false);
        reply->set_term(m_currentTerm);
        reply->set_updatenextindex(getLastLogIndex() + 1);
        return;
    } // 如果prevlogindex比快照还旧，拒绝并告知leader需要从快照之后同步
    else if (args->prevlogindex() < m_lastSnapshotIncludeIndex) {

        reply->set_success(false);
        reply->set_term(m_currentTerm);
        reply->set_updatenextindex(m_lastSnapshotIncludeIndex + 1);  
    }
    
    // 如果prevlogindex和prevlogterm匹配，说明日志在该点之前是一致的
    if (matchLog(args->prevlogindex(), args->prevlogterm())) {
        // 遍历leader发来的日志条目
        for (int i = 0; i < args->entries_size(); i++) {
            auto log = args->entries(i);
            // 本地还没有该index，直接追加
            if (log.logindex() > getLastLogIndex()) {
                m_logs.push_back(log);
            } else {
                // term一致但command不一致
                if (m_logs[getSlicesIndexFromLogIndex(log.logindex())].logterm() == log.logterm() &&
                    m_logs[getSlicesIndexFromLogIndex(log.logindex())].command() != log.command()) {       
                    myAssert(false, format("[func-AppendEntries-rf{%d}] 两节点logIndex{%d}和term{%d}相同，但是其command{%d:%d}   "
                                            " {%d:%d}却不同！！\n",
                                            m_me, log.logindex(), log.logterm(), m_me,
                                            m_logs[getSlicesIndexFromLogIndex(log.logindex())].command(), args->leaderid(),
                                            log.command()));
                }
                // term不一致，用leader的日志覆盖本地
                if (m_logs[getSlicesIndexFromLogIndex(log.logindex())].logterm() != log.logterm()) {
                    m_logs[getSlicesIndexFromLogIndex(log.logindex())] = log;
                }
            }
        }

        myAssert(
            getLastLogIndex() >= args->prevlogindex() + args->entries_size(),
            format("[func-AppendEntries1-rf{%d}]rf.getLastLogIndex(){%d} != args.PrevLogIndex{%d}+len(args.Entries){%d}",
                m_me, getLastLogIndex(), args->prevlogindex(), args->entries_size()));
        
        // 如果leader的commitIndex比本地大，更新本地commitIndex为两者较小值
        if (args->leadercommit() > m_commitIndex) {
            m_commitIndex = std::min(args->leadercommit(), getLastLogIndex());
        }

        // leader一次发送完所有的日志
        myAssert(getLastLogIndex() >= m_commitIndex,
                format("[func-AppendEntries1-rf{%d}]  rf.getLastLogIndex{%d} < rf.commitIndex{%d}", m_me,
                        getLastLogIndex(), m_commitIndex));
        reply->set_success(true);
        reply->set_term(m_currentTerm);
        return;
    } else {
        // 如果prevlogindex和prevlogterm不匹配，说明日志有冲突
        reply->set_updatenextindex(args->prevlogindex());
        // 逐渐往前匹配
        for (int index = args->prevlogindex(); index >= m_lastSnapshotIncludeIndex; --index) {
            if (getLogTermFromLogIndex(index) != getLogTermFromLogIndex(args->prevlogindex())) {
                reply->set_updatenextindex(index + 1);
                break;
            }
        }
        reply->set_success(false);
        reply->set_term(m_currentTerm);
        return;
    }
}

// 负责定期将已提交但未应用的日志推送到状态机
void Raft::applierTicker() {
    while (true) {
        m_mtx.lock();
        if (m_status == Leader) {
            DPrintf("[Raft::applierTicker() - raft{%d}]  m_lastApplied{%d}   m_commitIndex{%d}", m_me, m_lastApplied,
                    m_commitIndex);
        }
        // 获取所有尚未应用但已提交的日志条目
        auto applyMsgs = getApplyLogs();
        m_mtx.unlock();   
        if (!applyMsgs.empty()) {
            DPrintf("[func- Raft::applierTicker()-raft{%d}] 向kvserver报告的applyMsgs长度为：{%d}", m_me, applyMsgs.size());
        }
        for (auto& message : applyMsgs) {
            applyChan->Push(message);
        }
        sleepNMilliseconds(ApplyInterval);
    }
}

bool Raft::CondInstallSnapshot(int lastIncludedTerm, int lastIncludedIndex, std::string snapshot) {
    return true;
    std::lock_guard<std::mutex> lg(m_mtx);

    // 如果快照已经过时（不如当前commitIndex新），直接丢弃
    if (lastIncludedIndex <= m_commitIndex) {
        return false;
    }

    int lastLogIndex = getLastLogIndex();
    // 如果快照比本地日志还新，清空所有日志
    if (lastIncludedIndex > lastLogIndex) {
        m_logs.clear();
    } else {
        // 否则，丢弃快照之前的日志，只保留快照之后的日志
        m_logs = std::vector<raftRpcProctoc::LogEntry>(
            m_logs.begin() + getSlicesIndexFromLogIndex(lastIncludedIndex) + 1, m_logs.end());
    }

    // 更新快照元数据
    m_lastSnapshotIncludeIndex = lastIncludedIndex;
    m_lastSnapshotIncludeTerm = lastIncludedTerm;
    m_lastApplied = lastIncludedIndex;
    m_commitIndex = lastIncludedIndex;

    // 持久化raft状态和快照
    m_persister->Save(persistData(), snapshot);

    return true;
}

void Raft::doElection() {
  std::lock_guard<std::mutex> g(m_mtx);
  if (m_status != Leader) {
    DPrintf("[       ticker-func-rf(%d)              ]  选举定时器到期且不是leader，开始选举 \n", m_me);

    m_status = Candidate;   // 将自己的状态转为Candidate
    m_currentTerm += 1;     // 增加当前任期号
    m_votedFor = m_me;      // 给自己投票
    persist();              // 持久化
    std::shared_ptr<int> votedNum = std::make_shared<int>(1);   // 初始化投票数为1

    m_lastResetElectionTime = now();    //	重置选举定时器

    DPrintf("m_peers.size = {%d}", m_peers.size());
    for (int i = 0; i < m_peers.size(); i++) {
        if (i == m_me) {
            continue;
        }
        // 构造投票请求，包含：当前任期号、候选人ID、最后的日志索引和任期
        int lastLogIndex = -1, lastLogTerm = -1;
        getLastLogIndexAndTerm(&lastLogIndex, &lastLogTerm);  // 获取最后一个log的term和下标

        std::shared_ptr<raftRpcProctoc::RequestVoteArgs> requestVoteArgs =
            std::make_shared<raftRpcProctoc::RequestVoteArgs>();
        requestVoteArgs->set_term(m_currentTerm);
        requestVoteArgs->set_candidateid(m_me);
        requestVoteArgs->set_lastlogindex(lastLogIndex);
        requestVoteArgs->set_lastlogterm(lastLogTerm);
        auto requestVoteReply = std::make_shared<raftRpcProctoc::RequestVoteReply>();

        std::thread t(&Raft::sendRequestVote, this, i, requestVoteArgs, requestVoteReply, votedNum);  
        t.detach();
    }
  }
}

// 发送心跳
void Raft::doHeartBeat() {
    std::lock_guard<std::mutex> g(m_mtx);
    // 只有当前节点是Leader才会发送心跳
    if (m_status == Leader) {
        DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了且拿到mutex，开始发送AE\n", m_me);
        auto appendNums = std::make_shared<int>(1);  // 正确返回的节点的数量

        // 只对Follower发送心跳（AppendEntries RPC）
        for (int i = 0; i < m_peers.size(); i++) {
            if (i == m_me) {
                continue;
            }
            DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了 index:{%d}\n", m_me, i);
            myAssert(m_nextIndex[i] >= 1, format("rf.nextIndex[%d] = {%d}", i, m_nextIndex[i]));
            // 如果follower太落后，leader需要先发快照
            if (m_nextIndex[i] <= m_lastSnapshotIncludeIndex) {
                std::thread t(&Raft::leaderSendSnapShot, this, i);  // 创建新线程并执行b函数，并传递参数
                t.detach();
                continue;
            }

            // 构造 AppendEntries RPC
            int preLogIndex = -1;
            int PrevLogTerm = -1;
            getPrevLogInfo(i, &preLogIndex, &PrevLogTerm);
            std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> appendEntriesArgs = std::make_shared<raftRpcProctoc::AppendEntriesArgs>();
            appendEntriesArgs->set_term(m_currentTerm);
            appendEntriesArgs->set_leaderid(m_me);
            appendEntriesArgs->set_prevlogindex(preLogIndex);
            appendEntriesArgs->set_prevlogterm(PrevLogTerm);
            appendEntriesArgs->clear_entries();
            appendEntriesArgs->set_leadercommit(m_commitIndex);

            // 根据follower的进度，发送preLogIndex之后的日志
            if (preLogIndex != m_lastSnapshotIncludeIndex) {
                for (int j = getSlicesIndexFromLogIndex(preLogIndex) + 1; j < m_logs.size(); ++j) {
                    raftRpcProctoc::LogEntry* sendEntryPtr = appendEntriesArgs->add_entries();
                    *sendEntryPtr = m_logs[j];
                }
            } else {    // 如果preLogIndex是快照点，发送全部日志
                for (const auto& item : m_logs) {
                    raftRpcProctoc::LogEntry* sendEntryPtr = appendEntriesArgs->add_entries();
                    *sendEntryPtr = item;
                }
            }
            int lastLogIndex = getLastLogIndex();
            // leader对每个节点发送的日志长短不一，但是都保证从prevIndex发送直到最后
            myAssert(appendEntriesArgs->prevlogindex() + appendEntriesArgs->entries_size() == lastLogIndex,
                    format("appendEntriesArgs.PrevLogIndex{%d}+len(appendEntriesArgs.Entries){%d} != lastLogIndex{%d}",
                            appendEntriesArgs->prevlogindex(), appendEntriesArgs->entries_size(), lastLogIndex));
            // 构造返回值
            const std::shared_ptr<raftRpcProctoc::AppendEntriesReply> appendEntriesReply =
                std::make_shared<raftRpcProctoc::AppendEntriesReply>();
            appendEntriesReply->set_appstate(Disconnected);

            std::thread t(&Raft::sendAppendEntries, this, i, appendEntriesArgs, appendEntriesReply, appendNums);  // 创建新线程并执行b函数，并传递参数
            t.detach();
        }
        m_lastResetHearBeatTime = now();  // leader发送心跳，就不是随机时间了
    }
}

// 超时选举定时器 -> 每个raft节点一个
void Raft::electionTimeOutTicker() {

    while (true) {
        while (m_status == Leader) {
            usleep(HeartBeatTimeout);   // 当节点已经是Leader时，不需要进行选举超时检测
        }
        std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleepTime{};
        std::chrono::system_clock::time_point wakeTime{};
        {
            m_mtx.lock();
            wakeTime = now();
            // getRandomizedElectionTimeout() - (now() - m_lastResetElectionTime)
            suitableSleepTime = getRandomizedElectionTimeout() + m_lastResetElectionTime - wakeTime;
            m_mtx.unlock();
        }

        if (std::chrono::duration<double, std::milli>(suitableSleepTime).count() > 1) {
            auto start = std::chrono::steady_clock::now();
            usleep(std::chrono::duration_cast<std::chrono::microseconds>(suitableSleepTime).count());
            auto end = std::chrono::steady_clock::now();
            std::chrono::duration<double, std::milli> duration = end - start;

            std::cout << "\033[1;35m electionTimeOutTicker();函数设置睡眠时间为: "
                        << std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleepTime).count() << " 毫秒\033[0m"
                        << std::endl;
            std::cout << "\033[1;35m electionTimeOutTicker();函数实际睡眠时间为: " << duration.count() << " 毫秒\033[0m"
                        << std::endl;
        }
        // m_lastResetElectionTime比唤醒时间新，说明睡眠期间有重置选举计时器的操作
        if (std::chrono::duration<double, std::milli>(m_lastResetElectionTime - wakeTime).count() > 0) continue;
        doElection();
    }
}

// 返回所有已提交待应用的日志消息
std::vector<ApplyMsg> Raft::getApplyLogs() {
    std::vector<ApplyMsg> applyMsgs;
    myAssert(m_commitIndex <= getLastLogIndex(), format("[func-getApplyLogs-rf{%d}] commitIndex{%d} >getLastLogIndex{%d}",
                                                        m_me, m_commitIndex, getLastLogIndex()));

    while (m_lastApplied < m_commitIndex) {
        m_lastApplied++;
        myAssert(m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].logindex() == m_lastApplied,
                format("rf.logs[rf.getSlicesIndexFromLogIndex(rf.lastApplied)].LogIndex{%d} != rf.lastApplied{%d} ",
                        m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].logindex(), m_lastApplied));
        ApplyMsg applyMsg;
        applyMsg.CommandValid = true;
        applyMsg.SnapshotValid = false;
        applyMsg.Command = m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].command();
        applyMsg.CommandIndex = m_lastApplied;
        applyMsgs.emplace_back(applyMsg);
    }
    return applyMsgs;
}

// 获取新命令应该分配的Index
int Raft::getNewCommandIndex() {
    auto lastLogIndex = getLastLogIndex();
    return lastLogIndex + 1;
}

// 计算 leader 发送给 follower 的 AppendEntries RPC 的 prevLogIndex 和 prevLogTerm。
void Raft::getPrevLogInfo(int server, int* preIndex, int* preTerm) {
    // 如果 follower 期望的 nextIndex 正好是快照之后的第一条，prevLogIndex/Term 就是快照的 index/term
    if (m_nextIndex[server] == m_lastSnapshotIncludeIndex + 1) {
        *preIndex = m_lastSnapshotIncludeIndex;
        *preTerm = m_lastSnapshotIncludeTerm;
        return;
    }
    // 否则 prevLogIndex 是 nextIndex-1，prevLogTerm 是该日志的 term。
    auto nextIndex = m_nextIndex[server];
    *preIndex = nextIndex - 1;
    *preTerm = m_logs[getSlicesIndexFromLogIndex(*preIndex)].logterm();
}

void Raft::GetState(int* term, bool* isLeader) {
    m_mtx.lock();
    DEFER {
        // todo 暂时不清楚会不会导致死锁
        m_mtx.unlock();
    };

    *term = m_currentTerm;
    *isLeader = (m_status == Leader);
}

void Raft::InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest* args,
                           raftRpcProctoc::InstallSnapshotResponse* reply) {
    m_mtx.lock();
    DEFER { m_mtx.unlock(); };
    if (args->term() < m_currentTerm) {
        reply->set_term(m_currentTerm);
        //        DPrintf("[func-InstallSnapshot-rf{%v}] leader{%v}.term{%v}<rf{%v}.term{%v} ", rf.me, args.LeaderId,
        //        args.Term, rf.me, rf.currentTerm)

        return;
    }
    if (args->term() > m_currentTerm) {
        // 后面两种情况都要接收日志
        m_currentTerm = args->term();
        m_votedFor = -1;
        m_status = Follower;
        persist();
    }
    m_status = Follower;
    m_lastResetElectionTime = now();
    // outdated snapshot
    if (args->lastsnapshotincludeindex() <= m_lastSnapshotIncludeIndex) {
        //        DPrintf("[func-InstallSnapshot-rf{%v}] leader{%v}.LastSnapShotIncludeIndex{%v} <=
        //        rf{%v}.lastSnapshotIncludeIndex{%v} ", rf.me, args.LeaderId, args.LastSnapShotIncludeIndex, rf.me,
        //        rf.lastSnapshotIncludeIndex)
        return;
    }
    // 截断日志，修改commitIndex和lastApplied
    // 截断日志包括：日志长了，截断一部分，日志短了，全部清空，其实两个是一种情况
    // 但是由于现在getSlicesIndexFromLogIndex的实现，不能传入不存在logIndex，否则会panic
    auto lastLogIndex = getLastLogIndex();

    if (lastLogIndex > args->lastsnapshotincludeindex()) {
        m_logs.erase(m_logs.begin(), m_logs.begin() + getSlicesIndexFromLogIndex(args->lastsnapshotincludeindex()) + 1);
    } else {
        m_logs.clear();
    }
    m_commitIndex = std::max(m_commitIndex, args->lastsnapshotincludeindex());
    m_lastApplied = std::max(m_lastApplied, args->lastsnapshotincludeindex());
    m_lastSnapshotIncludeIndex = args->lastsnapshotincludeindex();
    m_lastSnapshotIncludeTerm = args->lastsnapshotincludeterm();

    reply->set_term(m_currentTerm);
    ApplyMsg msg;
    msg.SnapshotValid = true;
    msg.Snapshot = args->data();
    msg.SnapshotTerm = args->lastsnapshotincludeterm();
    msg.SnapshotIndex = args->lastsnapshotincludeindex();

    std::thread t(&Raft::pushMsgToKvServer, this, msg);  // 创建新线程并执行b函数，并传递参数
    t.detach();
    // 看下这里能不能再优化
    //     DPrintf("[func-InstallSnapshot-rf{%v}] receive snapshot from {%v} ,LastSnapShotIncludeIndex ={%v} ", rf.me,
    //     args.LeaderId, args.LastSnapShotIncludeIndex)
    // 持久化
    m_persister->Save(persistData(), args->data());
}

void Raft::pushMsgToKvServer(ApplyMsg msg) { applyChan->Push(msg); }

void Raft::leaderHearBeatTicker() {
    while (true) {
        // 不是leader的话就没有必要进行后续操作，况且还要拿锁，很影响性能，目前是睡眠，后面再优化优化
        while (m_status != Leader) {
            usleep(1000 * HeartBeatTimeout);
        }

        static std::atomic<int32_t> atomicCount = 0;
        std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleepTime{};
        std::chrono::system_clock::time_point wakeTime{};
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            wakeTime = now();
            suitableSleepTime = std::chrono::milliseconds(HeartBeatTimeout) + m_lastResetHearBeatTime - wakeTime;
        }

        if (std::chrono::duration<double, std::milli>(suitableSleepTime).count() > 1) {
            std::cout << atomicCount << "\033[1;35m leaderHearBeatTicker();函数设置睡眠时间为: "
                        << std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleepTime).count() << " 毫秒\033[0m"
                        << std::endl;
            // 获取当前时间点
            auto start = std::chrono::steady_clock::now();

            usleep(std::chrono::duration_cast<std::chrono::microseconds>(suitableSleepTime).count());
            // std::this_thread::sleep_for(suitableSleepTime);

            // 获取函数运行结束后的时间点
            auto end = std::chrono::steady_clock::now();

            // 计算时间差并输出结果（单位为毫秒）
            std::chrono::duration<double, std::milli> duration = end - start;

            // 使用ANSI控制序列将输出颜色修改为紫色
            std::cout << atomicCount << "\033[1;35m leaderHearBeatTicker();函数实际睡眠时间为: " << duration.count()
                        << " 毫秒\033[0m" << std::endl;
            ++atomicCount;
        }

        if (std::chrono::duration<double, std::milli>(m_lastResetHearBeatTime - wakeTime).count() > 0) {
        // 睡眠的这段时间有重置定时器，没有超时，再次睡眠
            continue;
        }
        // DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了\n", m_me);
        doHeartBeat();
    }
}

void Raft::leaderSendSnapShot(int server) {
  m_mtx.lock();
  raftRpcProctoc::InstallSnapshotRequest args;
  args.set_leaderid(m_me);
  args.set_term(m_currentTerm);
  args.set_lastsnapshotincludeindex(m_lastSnapshotIncludeIndex);
  args.set_lastsnapshotincludeterm(m_lastSnapshotIncludeTerm);
  args.set_data(m_persister->ReadSnapshot());

  raftRpcProctoc::InstallSnapshotResponse reply;
  m_mtx.unlock();
  bool ok = m_peers[server]->InstallSnapshot(&args, &reply);
  m_mtx.lock();
  DEFER { m_mtx.unlock(); };
  if (!ok) {
    return;
  }
  if (m_status != Leader || m_currentTerm != args.term()) {
    return;  // 中间释放过锁，可能状态已经改变了
  }
  //	无论什么时候都要判断term
  if (reply.term() > m_currentTerm) {
    // 三变
    m_currentTerm = reply.term();
    m_votedFor = -1;
    m_status = Follower;
    persist();
    m_lastResetElectionTime = now();
    return;
  }
  m_matchIndex[server] = args.lastsnapshotincludeindex();
  m_nextIndex[server] = m_matchIndex[server] + 1;
}

void Raft::leaderUpdateCommitIndex() {
  m_commitIndex = m_lastSnapshotIncludeIndex;
  // for index := rf.commitIndex+1;index < len(rf.log);index++ {
  // for index := rf.getLastIndex();index>=rf.commitIndex+1;index--{
  for (int index = getLastLogIndex(); index >= m_lastSnapshotIncludeIndex + 1; index--) {
    int sum = 0;
    for (int i = 0; i < m_peers.size(); i++) {
      if (i == m_me) {
        sum += 1;
        continue;
      }
      if (m_matchIndex[i] >= index) {
        sum += 1;
      }
    }

    //        !!!只有当前term有新提交的，才会更新commitIndex！！！！
    // log.Printf("lastSSP:%d, index: %d, commitIndex: %d, lastIndex: %d",rf.lastSSPointIndex, index, rf.commitIndex,
    // rf.getLastIndex())
    if (sum >= m_peers.size() / 2 + 1 && getLogTermFromLogIndex(index) == m_currentTerm) {
      m_commitIndex = index;
      break;
    }
  }
  //    DPrintf("[func-leaderUpdateCommitIndex()-rf{%v}] Leader %d(term%d) commitIndex
  //    %d",rf.me,rf.me,rf.currentTerm,rf.commitIndex)
}

// 进来前要保证logIndex是存在的，即≥rf.lastSnapshotIncludeIndex	，而且小于等于rf.getLastLogIndex()
bool Raft::matchLog(int logIndex, int logTerm) {
    myAssert(logIndex >= m_lastSnapshotIncludeIndex && logIndex <= getLastLogIndex(),
            format("不满足：logIndex{%d}>=rf.lastSnapshotIncludeIndex{%d}&&logIndex{%d}<=rf.getLastLogIndex{%d}",
                    logIndex, m_lastSnapshotIncludeIndex, logIndex, getLastLogIndex()));
    return logTerm == getLogTermFromLogIndex(logIndex);
}

void Raft::persist() {
    auto data = persistData();
    m_persister->SaveRaftState(data);
}

void Raft::RequestVote(const raftRpcProctoc::RequestVoteArgs* args, raftRpcProctoc::RequestVoteReply* reply) {
    std::lock_guard<std::mutex> lg(m_mtx);

    DEFER {
        persist();
    };
    // 对args的term的三种情况分别进行处理，大于小于等于自己的term都是不同的处理
    //  reason: 出现网络分区，该竞选者已经OutOfDate(过时）
    if (args->term() < m_currentTerm) {
        reply->set_term(m_currentTerm);
        reply->set_votestate(Expire);
        reply->set_votegranted(false);
        return;
    }
    // fig2:右下角，如果任何时候rpc请求或者响应的term大于自己的term，更新term，并变成follower
    if (args->term() > m_currentTerm) {
        //        DPrintf("[	    func-RequestVote-rf(%v)		] : 变成follower且更新term
        //        因为candidate{%v}的term{%v}> rf{%v}.term{%v}\n ", rf.me, args.CandidateId, args.Term, rf.me,
        //        rf.currentTerm)
        m_status = Follower;
        m_currentTerm = args->term();
        m_votedFor = -1;

        //	重置定时器：收到leader的ae，开始选举，透出票
        // 这时候更新了term之后，votedFor也要置为-1
    }
    myAssert(args->term() == m_currentTerm,
            format("[func--rf{%d}] 前面校验过args.Term==rf.currentTerm，这里却不等", m_me));
    //	现在节点任期都是相同的(任期小的也已经更新到新的args的term了)，还需要检查log的term和index是不是匹配的了

    int lastLogTerm = getLastLogTerm();
    // 只有没投票，且candidate的日志的新的程度 ≥ 接受者的日志新的程度 才会授票
    if (!UpToDate(args->lastlogindex(), args->lastlogterm())) {
        // args.LastLogTerm < lastLogTerm || (args.LastLogTerm == lastLogTerm && args.LastLogIndex < lastLogIndex) {
        // 日志太旧了
        if (args->lastlogterm() < lastLogTerm) {
        //                    DPrintf("[	    func-RequestVote-rf(%v)		] : refuse voted rf[%v] ,because
        //                    candidate_lastlog_term{%v} < lastlog_term{%v}\n", rf.me, args.CandidateId, args.LastLogTerm,
        //                    lastLogTerm)
        } else {
        //            DPrintf("[	    func-RequestVote-rf(%v)		] : refuse voted rf[%v] ,because
        //            candidate_log_index{%v} < log_index{%v}\n", rf.me, args.CandidateId, args.LastLogIndex,
        //            rf.getLastLogIndex())
        }
        reply->set_term(m_currentTerm);
        reply->set_votestate(Voted);
        reply->set_votegranted(false);

        return;
    }
    // todo ： 啥时候会出现rf.votedFor == args.CandidateId ，就算candidate选举超时再选举，其term也是不一样的呀
    //     当因为网络质量不好导致的请求丢失重发就有可能！！！！
    if (m_votedFor != -1 && m_votedFor != args->candidateid()) {
        reply->set_term(m_currentTerm);
        reply->set_votestate(Voted);
        reply->set_votegranted(false);
        return;
    } else {
        m_votedFor = args->candidateid();
        m_lastResetElectionTime = now();  // 认为必须要在投出票的时候才重置定时器，
        reply->set_term(m_currentTerm);
        reply->set_votestate(Normal);
        reply->set_votegranted(true);

        return;
    }
}

bool Raft::UpToDate(int index, int term) {
    int lastIndex = -1;
    int lastTerm = -1;
    getLastLogIndexAndTerm(&lastIndex, &lastTerm);
    return term > lastTerm || (term == lastTerm && index >= lastIndex);
}

void Raft::getLastLogIndexAndTerm(int* lastLogIndex, int* lastLogTerm) {
    if (m_logs.empty()) {
        *lastLogIndex = m_lastSnapshotIncludeIndex;
        *lastLogTerm = m_lastSnapshotIncludeTerm;
        return;
    } else {
        *lastLogIndex = m_logs[m_logs.size() - 1].logindex();
        *lastLogTerm = m_logs[m_logs.size() - 1].logterm();
        return;
    }
}

int Raft::getLastLogIndex() {
    int lastLogIndex = -1;
    int _ = -1;
    getLastLogIndexAndTerm(&lastLogIndex, &_);
    return lastLogIndex;
}

int Raft::getLastLogTerm() {
    int _ = -1;
    int lastLogTerm = -1;
    getLastLogIndexAndTerm(&_, &lastLogTerm);
    return lastLogTerm;
}

int Raft::getLogTermFromLogIndex(int logIndex) {
    myAssert(logIndex >= m_lastSnapshotIncludeIndex,
            format("[func-getSlicesIndexFromLogIndex-rf{%d}]  index{%d} < rf.lastSnapshotIncludeIndex{%d}", m_me,
                    logIndex, m_lastSnapshotIncludeIndex));

    int lastLogIndex = getLastLogIndex();

    myAssert(logIndex <= lastLogIndex, format("[func-getSlicesIndexFromLogIndex-rf{%d}]  logIndex{%d} > lastLogIndex{%d}",
                                                m_me, logIndex, lastLogIndex));

    if (logIndex == m_lastSnapshotIncludeIndex) {
        return m_lastSnapshotIncludeTerm;
    } else {
        return m_logs[getSlicesIndexFromLogIndex(logIndex)].logterm();
    }
}

int Raft::GetRaftStateSize() { return m_persister->RaftStateSize(); }

// 找到index对应的真实下标位置！！！
// 限制，输入的logIndex必须保存在当前的logs里面（不包含snapshot）
int Raft::getSlicesIndexFromLogIndex(int logIndex) {
  myAssert(logIndex > m_lastSnapshotIncludeIndex,
           format("[func-getSlicesIndexFromLogIndex-rf{%d}]  index{%d} <= rf.lastSnapshotIncludeIndex{%d}", m_me,
                  logIndex, m_lastSnapshotIncludeIndex));
  int lastLogIndex = getLastLogIndex();
  myAssert(logIndex <= lastLogIndex, format("[func-getSlicesIndexFromLogIndex-rf{%d}]  logIndex{%d} > lastLogIndex{%d}",
                                            m_me, logIndex, lastLogIndex));
  int SliceIndex = logIndex - m_lastSnapshotIncludeIndex - 1;
  return SliceIndex;
}

bool Raft::sendRequestVote(int server, std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,
                           std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply, std::shared_ptr<int> votedNum) {
    auto start = now();
    DPrintf("[func-sendRequestVote rf{%d}] 向server{%d} 发送 RequestVote 开始", m_me, m_currentTerm, getLastLogIndex());
    bool ok = m_peers[server]->RequestVote(args.get(), reply.get());  // 表示通信是否正常
    DPrintf("[func-sendRequestVote rf{%d}] 向server{%d} 发送 RequestVote 完毕，耗时:{%d} ms", m_me, m_currentTerm,
            getLastLogIndex(), now() - start);

    if (!ok) {
        return ok;  
    }

    std::lock_guard<std::mutex> lg(m_mtx);
    // 如果收到的term比自己大，说明自己已经落后，立即转为Follower，更新term和投票对象
    if (reply->term() > m_currentTerm) {
        m_status = Follower;
        m_currentTerm = reply->term();
        m_votedFor = -1;
        persist();
        return true;
    } else if (reply->term() < m_currentTerm) { // 收到的term比自己小，对方过期
        return true;
    }
    myAssert(reply->term() == m_currentTerm, format("assert {reply.Term==rf.currentTerm} fail"));

    // 如果没有获得对象投票，直接返回
    if (!reply->votegranted()) {
        return true;
    }

    // 网络通信成功，对方的term == 自己的term，且获得投票
    *votedNum = *votedNum + 1;
    // 获取半数以上投票，成为Leader
    if (*votedNum >= m_peers.size() / 2 + 1) {
        *votedNum = 0;
        if (m_status == Leader) {
            myAssert(false, format("[func-sendRequestVote-rf{%d}]  term:{%d} 同一个term当两次领导，error", m_me, m_currentTerm));
        }

        m_status = Leader;

        DPrintf("[func-sendRequestVote rf{%d}] elect success  ,current term:{%d} ,lastLogIndex:{%d}\n", m_me, m_currentTerm,
                getLastLogIndex());

        int lastLogIndex = getLastLogIndex();   // 当前日志的最后一条
        for (int i = 0; i < m_nextIndex.size(); i++) {
            m_nextIndex[i] = lastLogIndex + 1;  // leader认为下一个要发送给该follower的日志条目索引
            m_matchIndex[i] = 0;                // leader已经知道该follower同步到的最大日志index
        }
        std::thread t(&Raft::doHeartBeat, this);  // 马上向其他节点宣告自己就是leader
        t.detach();

        persist();
    }
    return true;
}

bool Raft::sendAppendEntries(int server, std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,
                             std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply,
                             std::shared_ptr<int> appendNums) {
    DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc开始， args->entries_size():{%d}", m_me,
            server, args->entries_size());
    bool ok = m_peers[server]->AppendEntries(args.get(), reply.get());  // 表示网络通信是否成功

    if (!ok) {
        DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc失敗", m_me, server);
        return ok;
    }
    DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc成功", m_me, server);
    if (reply->appstate() == Disconnected) {
        return ok;
    }
    std::lock_guard<std::mutex> lg1(m_mtx);

    //  如果对方返回的term比自己大，说明自己已经落后，立刻转为Follower，更新term和投票对象
    if (reply->term() > m_currentTerm) {
        m_status = Follower;
        m_currentTerm = reply->term();
        m_votedFor = -1;
        return ok;
    } else if (reply->term() < m_currentTerm) { // 如果对方返回的term比自己小，说明对方过期
        DPrintf("[func -sendAppendEntries  rf{%d}]  节点：{%d}的term{%d}<rf{%d}的term{%d}\n", m_me, server, reply->term(),
                m_me, m_currentTerm);
        return ok;
    }

    if (m_status != Leader) {
        return ok;
    }

    // term相等
    myAssert(reply->term() == m_currentTerm,
            format("reply.Term{%d} != rf.currentTerm{%d}   ", reply->term(), m_currentTerm));
    if (!reply->success()) {    //日志冲突，需要回退nextIndex
        if (reply->updatenextindex() != -100) { 
        DPrintf("[func -sendAppendEntries  rf{%d}]  返回的日志term相等，但是不匹配，回退nextIndex[%d]：{%d}\n", m_me,
                server, reply->updatenextindex());
        m_nextIndex[server] = reply->updatenextindex();  // 将nextIndex设置为对方建议的回退点，matchIndex不更新
        }

    } else {    // 日志同步成功
        *appendNums = *appendNums + 1;
        DPrintf("---------------------------tmp------------------------- 节点{%d}返回true,当前*appendNums{%d}", server,
                *appendNums);
        m_matchIndex[server] = std::max(m_matchIndex[server], args->prevlogindex() + args->entries_size());
        m_nextIndex[server] = m_matchIndex[server] + 1;
        int lastLogIndex = getLastLogIndex();

        myAssert(m_nextIndex[server] <= lastLogIndex + 1,
                format("error msg:rf.nextIndex[%d] > lastLogIndex+1, len(rf.logs) = %d   lastLogIndex{%d} = %d", server,
                        m_logs.size(), server, lastLogIndex));
        if (*appendNums >= 1 + m_peers.size() / 2) {
        *appendNums = 0;
        // leader只有在当前term有日志提交的时候才更新commitIndex，因为raft无法保证之前term的Index是否提交
        // 只有当前term有日志提交，之前term的log才可以被提交
        if (args->entries_size() > 0) {
            DPrintf("args->entries(args->entries_size()-1).logterm(){%d}   m_currentTerm{%d}",
                    args->entries(args->entries_size() - 1).logterm(), m_currentTerm);
        }
        if (args->entries_size() > 0 && args->entries(args->entries_size() - 1).logterm() == m_currentTerm) {
            DPrintf(
                "---------------------------tmp------------------------- 当前term有log成功提交，更新leader的m_commitIndex "
                "from{%d} to{%d}",
                m_commitIndex, args->prevlogindex() + args->entries_size());

            m_commitIndex = std::max(m_commitIndex, args->prevlogindex() + args->entries_size());
        }
        myAssert(m_commitIndex <= lastLogIndex,
                format("[func-sendAppendEntries,rf{%d}] lastLogIndex:%d  rf.commitIndex:%d\n", m_me, lastLogIndex,
                        m_commitIndex));
        }
    }
    return ok;
}

// gRPC风格接口实现
grpc::Status Raft::AppendEntries(grpc::ServerContext* context, const raftRpcProctoc::AppendEntriesArgs* request,
                                 raftRpcProctoc::AppendEntriesReply* response) {
  AppendEntries1(request, response);
  return grpc::Status::OK;
}

grpc::Status Raft::InstallSnapshot(grpc::ServerContext* context, const raftRpcProctoc::InstallSnapshotRequest* request,
                                   raftRpcProctoc::InstallSnapshotResponse* response) {
  InstallSnapshot(request, response);
  return grpc::Status::OK;
}

grpc::Status Raft::RequestVote(grpc::ServerContext* context, const raftRpcProctoc::RequestVoteArgs* request,
                               raftRpcProctoc::RequestVoteReply* response) {
  RequestVote(request, response);
  return grpc::Status::OK;
}

void Raft::Start(Op command, int* newLogIndex, int* newLogTerm, bool* isLeader) {
    std::lock_guard<std::mutex> lg1(m_mtx);
    if (m_status != Leader) {
        DPrintf("[func-Start-rf{%d}]  is not leader");
        *newLogIndex = -1;
        *newLogTerm = -1;
        *isLeader = false;
        return;
    }

    raftRpcProctoc::LogEntry newLogEntry;
    newLogEntry.set_command(command.asString());
    newLogEntry.set_logterm(m_currentTerm);
    newLogEntry.set_logindex(getNewCommandIndex());
    m_logs.emplace_back(newLogEntry);

    int lastLogIndex = getLastLogIndex();

    // leader应该不停的向各个Follower发送AE来维护心跳和保持日志同步，目前的做法是新的命令来了不会直接执行，而是等待leader的心跳触发
    DPrintf("[func-Start-rf{%d}]  lastLogIndex:%d,command:%s\n", m_me, lastLogIndex, &command);
    // rf.timer.Reset(10) //接收到命令后马上给follower发送,改成这样不知为何会出现问题，待修正 todo
    persist();
    *newLogIndex = newLogEntry.logindex();
    *newLogTerm = newLogEntry.logterm();
    *isLeader = true;
}

/*
  服务或测试程序希望创建一个 Raft 服务器。所有 Raft 服务器（包括本节点）的端口都在 peers[] 数组中。
  本节点的端口是 peers[me]。所有服务器的 peers[] 数组顺序一致。persister 用于保存该服务器的持久化状态，
  并且在初始化时持有最近一次保存的状态（如果有的话）。applyCh 是一个通道，Raft 会通过它向上层发送 ApplyMsg 消息。
  Make() 必须快速返回，因此应为所有耗时任务启动 goroutine（或线程）。
*/
void Raft::init(std::vector<std::shared_ptr<RaftRpcUtil>> peers, int me, std::shared_ptr<Persister> persister,
                std::shared_ptr<LockQueue<ApplyMsg>> applyCh) {
    m_peers = peers;
    m_persister = persister;
    m_me = me;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        this->applyChan = applyCh;
        m_currentTerm = 0;
        m_status = Follower;
        m_commitIndex = 0;
        m_lastApplied = 0;
        m_logs.clear();

        for (int i = 0; i < m_peers.size(); i++) {
            m_matchIndex.push_back(0);
            m_nextIndex.push_back(0);
        }

        m_votedFor = -1;
        m_lastSnapshotIncludeIndex = 0;
        m_lastSnapshotIncludeTerm = 0;
        m_lastResetElectionTime = now();
        m_lastResetHearBeatTime = now();

        readPersist(m_persister->ReadRaftState());
        if (m_lastSnapshotIncludeIndex > 0) {
            m_lastApplied = m_lastSnapshotIncludeIndex;
            // rf.commitIndex = rf.lastSnapshotIncludeIndex   todo ：崩溃恢复为何不能读取commitIndex
        }

        DPrintf("[Init&ReInit] Sever %d, term %d, lastSnapshotIncludeIndex {%d} , lastSnapshotIncludeTerm {%d}", m_me,
                m_currentTerm, m_lastSnapshotIncludeIndex, m_lastSnapshotIncludeTerm);
    }

    std::thread t(&Raft::leaderHearBeatTicker, this);
    t.detach();
    
    std::thread t2(&Raft::electionTimeOutTicker, this);
    t2.detach();
    
    std::thread t3(&Raft::applierTicker, this);
    t3.detach();
}

std::string Raft::persistData() {
    BoostPersistRaftNode boostPersistRaftNode;
    boostPersistRaftNode.m_currentTerm = m_currentTerm;
    boostPersistRaftNode.m_votedFor = m_votedFor;
    boostPersistRaftNode.m_lastSnapshotIncludeIndex = m_lastSnapshotIncludeIndex;
    boostPersistRaftNode.m_lastSnapshotIncludeTerm = m_lastSnapshotIncludeTerm;
    for (auto& item : m_logs) {
        boostPersistRaftNode.m_logs.push_back(item.SerializeAsString());
    }

    std::stringstream ss;
    boost::archive::text_oarchive oa(ss);
    oa << boostPersistRaftNode;
    return ss.str();
}

void Raft::readPersist(std::string data) {
    if (data.empty()) {
        return;
    }
    std::stringstream iss(data);
    boost::archive::text_iarchive ia(iss);
    // read class state from archive
    BoostPersistRaftNode boostPersistRaftNode;
    ia >> boostPersistRaftNode;

    m_currentTerm = boostPersistRaftNode.m_currentTerm;
    m_votedFor = boostPersistRaftNode.m_votedFor;
    m_lastSnapshotIncludeIndex = boostPersistRaftNode.m_lastSnapshotIncludeIndex;
    m_lastSnapshotIncludeTerm = boostPersistRaftNode.m_lastSnapshotIncludeTerm;
    m_logs.clear();
    for (auto& item : boostPersistRaftNode.m_logs) {
        raftRpcProctoc::LogEntry logEntry;
        logEntry.ParseFromString(item);
        m_logs.emplace_back(logEntry);
    }
}

void Raft::Snapshot(int index, std::string snapshot) {
    std::lock_guard<std::mutex> lg(m_mtx);

    if (m_lastSnapshotIncludeIndex >= index || index > m_commitIndex) {
        DPrintf(
            "[func-Snapshot-rf{%d}] rejects replacing log with snapshotIndex %d as current snapshotIndex %d is larger or "
            "smaller ",
            m_me, index, m_lastSnapshotIncludeIndex);
        return;
    }
    auto lastLogIndex = getLastLogIndex();  // 为了检查snapshot前后日志是否一样，防止多截取或者少截取日志

    // 制造完此快照后剩余的所有日志
    int newLastSnapshotIncludeIndex = index;
    int newLastSnapshotIncludeTerm = m_logs[getSlicesIndexFromLogIndex(index)].logterm();
    std::vector<raftRpcProctoc::LogEntry> trunckedLogs;
    // todo :这种写法有点笨，待改进，而且有内存泄漏的风险
    for (int i = index + 1; i <= getLastLogIndex(); i++) {
        // 注意有=，因为要拿到最后一个日志
        trunckedLogs.push_back(m_logs[getSlicesIndexFromLogIndex(i)]);
    }
    m_lastSnapshotIncludeIndex = newLastSnapshotIncludeIndex;
    m_lastSnapshotIncludeTerm = newLastSnapshotIncludeTerm;
    m_logs = trunckedLogs;
    m_commitIndex = std::max(m_commitIndex, index);
    m_lastApplied = std::max(m_lastApplied, index);

    // rf.lastApplied = index //lastApplied 和 commit应不应该改变呢？？？ 为什么  不应该改变吧
    m_persister->Save(persistData(), snapshot);

    DPrintf("[SnapShot]Server %d snapshot snapshot index {%d}, term {%d}, loglen {%d}", m_me, index,
            m_lastSnapshotIncludeTerm, m_logs.size());
    myAssert(m_logs.size() + m_lastSnapshotIncludeIndex == lastLogIndex,
            format("len(rf.logs){%d} + rf.lastSnapshotIncludeIndex{%d} != lastLogjInde{%d}", m_logs.size(),
                    m_lastSnapshotIncludeIndex, lastLogIndex));
}
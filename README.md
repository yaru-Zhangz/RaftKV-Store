# RaftKV-Store



## 项目介绍

主要分为以下四个模块：

#### Raft算法的实现
Raft算法是强一致性算法，能够保证分布式节点之间的数据一致性。
复制状态机：相同的初始状态 + 相同的输入 = 相同的结束状态。
在任何时刻，每一个raft节点都处于leader, follower或candidate这三个状态之一。
主要分为三个部分：领导者选举(leader election)，日志复制(log replication)和安全性(safety)。

- 领导者选举：Raft内部有一种心跳机制，如果存在leader，那么它就会周期性地向所有follower发送心跳来维持自己的地位。如果follower一段时间没有收到心跳，那么他就会认为系统中没有可用的leader，然后发起选举。follower先增加自己的任期号，再转换到candidate状态，并给自己投一票。然后并行地向集群中的其他raft节点发送投票请求。如果这个candiate获得了超过半数以上的投票，它就会成为leader并开始发送心跳；如果其他节点赢得了选举，它会在收到新leader的心跳消息后，从candidate状态回到follower状态；如果没有任何获胜者，每个candidate都在一个自己的随机选举超时时间后增加任期号开始新一轮的投票。
- 日志复制：leader被选举出来后，开始为客户端请求提供服务。leader接收到客户端的指令后，会把指令作为一个新的条目追加到日志中去。一条日志中需要三个信息：<指令，任期号，日志索引>。生成日志后, leader并行发送条目请求给follower，让它们复制该条目。当该条目被超过半数的follower复制后，leader就可以在本地执行该指令并把结果返回客户端。在这个过程中，leader和follower随时都有崩溃或缓慢的可能性，具体有三种可能：
- - 如果follower因为某些原因没有给leader响应，那么leader会不断重发追加条目请求。
- - 如果有follower崩溃后恢复，会通过raft的一致性检查来保证follower能按顺序回复崩溃后的缺失的日志。(一致性检查：leader在每一个发往follower的追酵母请求中会放入前一个日志条目的索引和任期号，如果follower在它的日志中找不到前一个日志，那么它就会拒绝此日志，leader收到follower的拒绝后，会发送前一个日志条目，从而逐渐向前定位到follower第一个缺失的日志。)
- - 如果leader崩溃，那么崩溃的leader可能已经复制了日志到部分follower但还没有提交，而被选出的新leader有可能不具备这些日志，这样就有部分follower中的日志和新leader的日志不同，这种情况下，leader通过强制followerr复制它的日志来解决不一致的问题，这意味着follower中跟leader冲突的日志条目会被新leader的日志条目覆盖。
- 安全性：
- - 选举限制：保证被选出的leader一定包含了之前各任期的所有被提交的日志条目，如果投票者自己的日志比candidate的还新，它会拒绝掉该投票请求。
- - 新leader是否提交之前任期内的日志条目：leader 只能通过提交当前任期日志来间接提交旧日志

#### 存储引擎层 —— 跳表


#### 服务协调层 —— KVserver


#### 通信层 —— gRPC



## 参考资料

- 论文：<In Search of an Understandable Consensus Algorithm>
- b站视频：https://www.bilibili.com/video/BV1pr4y1b7H5/?spm_id_from=333.337.search-card.all.click&vd_source=5eae1b2580d836bc51a9f4cb2fb7ad10
- 
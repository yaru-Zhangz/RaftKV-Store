#ifndef SKIPLIST_H
#define SKIPLIST_H
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <atomic>
#define STORE_FILE "store/dumpFile"

static std::string delimiter = ":";

// Node 类
template <typename K, typename V>
class Node {
public:
    Node() = default;
    Node(K k, V v, int);
    ~Node();

    K get_key() const;
    V get_value() const;
    void set_value(V);

    std::atomic<Node<K, V>*> *forward;   // 每一层的下一个节点指针数组，支持CAS
    std::atomic<bool> marked{false}; // 逻辑删除标记
    int node_level;

    
private:
    K key;
    V value;
};

template <typename K, typename V>
Node<K, V>::Node(const K k, const V v, int level)
    : key(k),
      value(v),
      node_level(level),
      forward(new std::atomic<Node<K, V>*>[level + 1])
{
    for (int i = 0; i <= level; ++i) {
        forward[i].store(nullptr, std::memory_order_relaxed);
    }
}

template <typename K, typename V>
Node<K, V>::~Node() {
    delete[] forward;
};

template <typename K, typename V>
K Node<K, V>::get_key() const {
    return key;
};

template <typename K, typename V>
V Node<K, V>::get_value() const {
    return value;
};

template <typename K, typename V>
void Node<K, V>::set_value(V value) {
    this->value = value;
};

// 跳表类
template <typename K, typename V>
class SkipList {
public:
    SkipList(int);
    ~SkipList();
    int get_random_level();
    Node<K, V> *create_node(K, V, int);
    int insert_element(K, V); // lock-free
    void delete_element(K);   // lock-free
    bool search_element(K, V &value);
    void display_skiplist();
    void insert_set_element(K &, V &);
    std::string dump_file();
    void load_file(const std::string &dumpStr);
    void clear(Node<K, V> *);
    int size();

private:
    void get_key_value_from_string(const std::string &str, std::string *key, std::string *value);
    bool is_valid_string(const std::string &str);

    // 延迟回收链表
    struct RetiredNode {
        Node<K, V>* node;
        RetiredNode* next;
        RetiredNode(Node<K, V>* n, RetiredNode* nx) : node(n), next(nx) {}
    };
    std::atomic<RetiredNode*> _retired_head{nullptr};

private:
    int _max_level;
    std::atomic<int> _skip_list_level; // 当前跳表的最高层级，原子化
    Node<K, V> *_header;
    std::ofstream _file_writer;
    std::ifstream _file_reader;
    std::atomic<int> _element_count; // 原子化
    void retire_node(Node<K, V>* node);
    void scan_and_reclaim();
};

template <typename K, typename V>
SkipList<K, V>::SkipList(int max_level) {
    this->_max_level = max_level;
    this->_skip_list_level = 0;
    this->_element_count = 0;

    K k;
    V v;
    this->_header = new Node<K, V>(k, v, _max_level);
};

template <typename K, typename V>
SkipList<K, V>::~SkipList() {
    if (_file_writer.is_open()) {
        _file_writer.close();
    }
    if (_file_reader.is_open()) {
        _file_reader.close();
    }

    //递归删除跳表链条
    if (_header->forward[0] != nullptr) {
        clear(_header->forward[0]);
    }
    delete (_header);
}

// 创建新节点
template <typename K, typename V>
Node<K, V> *SkipList<K, V>::create_node(const K k, const V v, int level) {
    Node<K, V> *n = new Node<K, V>(k, v, level);
    return n;
}

template <typename K, typename V>
int SkipList<K, V>::get_random_level() {
    static thread_local std::mt19937 gen(std::random_device{}());
    static thread_local std::uniform_int_distribution<> dist(0, 1);
    int k = 1;
    while (dist(gen)) {
        k++;
    }
    k = (k < _max_level) ? k : _max_level;
    return k;
}

// Insert given key and value in skip list
// return 1 means element exists
// return 0 means insert successfully
/*
                           +------------+
                           |  insert 50 |
                           +------------+
level 4     +-->1+                                                      100
                 |
                 |                      insert +----+
level 3         1+-------->10+---------------> | 50 |          70       100
                                               |    |
                                               |    |
level 2         1          10         30       | 50 |          70       100
                                               |    |
                                               |    |
level 1         1    4     10         30       | 50 |          70       100
                                               |    |
                                               |    |
level 0         1    4   9 10         30   40  | 50 |  60      70       100
                                               +----+

*/

// 插入元素
/*
跳表的插入操作首先从最高层开始查找，在每一层找到最后一个小于目标键的节点并记录下来，然后检查键是否已存在，
如果不存在则生成一个随机层数作为新节点的高度，若该层数超过当前跳表层数则扩展跳表层级，
接着创建新节点并在所有相关层级中将其插入到之前记录的节点之后，最后更新元素计数。
平均时间复杂度为O(log n)
*/
template <typename K, typename V>
int SkipList<K, V>::insert_element(const K key, const V value) {
    Node<K, V> *update[_max_level + 1];
    Node<K, V> *current;
    while (true) {
        current = _header;
        for (int i = _skip_list_level; i >= 0; i--) {
            Node<K, V> *next = current->forward[i].load(std::memory_order_acquire);
            while (next && next->get_key() < key) {
                current = next;
                next = current->forward[i].load(std::memory_order_acquire);
            }
            update[i] = current;
        }
        Node<K, V> *succ = current->forward[0].load(std::memory_order_acquire);
        if (succ && succ->get_key() == key) {
            std::cout << "key: " << key << ", exists" << std::endl;
            return 1;
        }
        int random_level = get_random_level();
        int old_level = _skip_list_level;
        while (random_level > old_level) {
            if (_skip_list_level.compare_exchange_weak(old_level, random_level)) {
                for (int i = old_level + 1; i <= random_level; ++i) {
                    update[i] = _header;
                }
                break;
            }
            old_level = _skip_list_level;
        }
        Node<K, V> *new_node = create_node(key, value, random_level);
        for (int i = 0; i <= random_level; ++i) {
            Node<K, V> *next;
            do {
                next = update[i]->forward[i].load(std::memory_order_acquire);
                new_node->forward[i].store(next, std::memory_order_relaxed);
            } while (!update[i]->forward[i].compare_exchange_weak(next, new_node));
        }
        _element_count.fetch_add(1);
        std::cout << "Successfully inserted key:" << key << ", value:" << value << std::endl;
        return 0;
    }
}

// 删除元素
/*
首先从最高层开始，逐层向下查找，在每一层记录下目标 key 前驱节点的位置。
查找结束后，若第 0 层的下一个节点存在且 key 匹配，
则在所有相关层级中将前驱节点的 forward 指针指向被删除节点的下一个节点，从而将目标节点从跳表中断开。
最后，如果最高层已经没有节点，则适当降低跳表层数，并释放被删除节点的内存，更新元素计数。
*/
template <typename K, typename V>
void SkipList<K, V>::delete_element(K key) {
    Node<K, V> *update[_max_level + 1];
    Node<K, V> *current;
    while (true) {
        current = _header;
        for (int i = _skip_list_level; i >= 0; i--) {
            Node<K, V> *next = current->forward[i].load(std::memory_order_acquire);
            while (next && next->get_key() < key) {
                current = next;
                next = current->forward[i].load(std::memory_order_acquire);
            }
            update[i] = current;
        }
        Node<K, V> *target = current->forward[0].load(std::memory_order_acquire);
        if (!target || target->get_key() != key) {
            return;
        }
        // 逻辑删除标记
        bool expected = false;
        if (!target->marked.compare_exchange_strong(expected, true)) {
            return; // 已被标记删除
        }
        // 物理删除：CAS将前驱的forward指向target的下一个节点
        for (int i = 0; i <= target->node_level; ++i) {
            Node<K, V> *succ;
            do {
                succ = target->forward[i].load(std::memory_order_acquire);
            } while (!update[i]->forward[i].compare_exchange_weak(target, succ));
        }
        while (_skip_list_level > 0 && _header->forward[_skip_list_level].load() == nullptr) {
            _skip_list_level--;
        }
        std::cout << "Successfully deleted key " << key << std::endl;
        retire_node(target); // 延迟回收
        _element_count.fetch_sub(1);
        scan_and_reclaim(); // 尝试回收
        return;
    }
}
// 延迟回收节点，将其加入待回收链表
template <typename K, typename V>
void SkipList<K, V>::retire_node(Node<K, V>* node) {
    RetiredNode* old_head = _retired_head.load(std::memory_order_relaxed);
    RetiredNode* new_node = new RetiredNode(node, old_head);
    while (!_retired_head.compare_exchange_weak(old_head, new_node));
}

// 简单回收：遍历链表并delete所有节点（仅供单线程测试或无并发访问时调用）
template <typename K, typename V>
void SkipList<K, V>::scan_and_reclaim() {
    RetiredNode* head = _retired_head.exchange(nullptr);
    while (head) {
        delete head->node;
        RetiredNode* tmp = head;
        head = head->next;
        delete tmp;
    }
}

// Search for element in skip list
/*
                           +------------+
                           |  select 60 |
                           +------------+
level 4     +-->1+                                                      100
                 |
                 |
level 3         1+-------->10+------------------>50+           70       100
                                                   |
                                                   |
level 2         1          10         30         50|           70       100
                                                   |
                                                   |
level 1         1    4     10         30         50|           70       100
                                                   |
                                                   |
level 0         1    4   9 10         30   40    50+-->60      70       100
*/

// 查找元素
/*
跳表查找元素时，首先从最高层开始，逐层向下，在每一层沿着 forward 指针向右遍历，
直到遇到第一个 key 不小于目标 key 的节点为止，然后进入下一层继续查找。
最终在第 0 层定位到目标 key 所在的位置，如果该节点存在且 key 匹配，则查找成功并返回对应的 value，
否则查找失败。整个过程利用多层索引加速，平均时间复杂度为 O(log n)。
*/
template <typename K, typename V>
bool SkipList<K, V>::search_element(K key, V &value) {
    std::cout << "search_element-----------------" << std::endl;
retry:
    Node<K, V> *current = _header;
    for (int i = _skip_list_level; i >= 0; i--) {
        Node<K, V>* next = current->forward[i].load(std::memory_order_acquire);
        while (next && next->get_key() < key) {
            if (next->marked.load(std::memory_order_acquire)) {
                // 如果遇到已逻辑删除节点，重启查找，保证不会访问到已物理删除的节点
                goto retry;
            }
            current = next;
            next = current->forward[i].load(std::memory_order_acquire);
        }
    }
    current = current->forward[0].load(std::memory_order_acquire);
    if (current && current->get_key() == key && !current->marked.load(std::memory_order_acquire)) {
        value = current->get_value();
        std::cout << "Found key: " << key << ", value: " << current->get_value() << std::endl;
        return true;
    }
    std::cout << "Not Found Key:" << key << std::endl;
    return false;
}

// 打印跳表
template <typename K, typename V>
void SkipList<K, V>::display_skiplist() {
    std::cout << "\n*****Skip List*****" << "\n";
    for (int i = 0; i <= _skip_list_level; i++) {
            Node<K, V> *node = this->_header->forward[i];
            std::cout << "Level " << i << ": ";
            while (node != NULL) {
                std::cout << node->get_key() << ":" << node->get_value() << ";";
                node = node->forward[i];
            }
        std::cout << std::endl;
    }
}

// 插入元素，如果元素存在先删除再插入
template <typename K, typename V>
void SkipList<K, V>::insert_set_element(K &key, V &value) {
    V oldValue;
    if (search_element(key, oldValue)) {
        delete_element(key);
    }
    insert_element(key, value);
}

// 序列化
template <typename K, typename V>
std::string SkipList<K, V>::dump_file() {
    Node<K, V> *node = this->_header->forward[0];
    SkipListDump<K, V> dumper;
    while (node != nullptr) {
        dumper.insert(*node);
        node = node->forward[0];
    }
    std::stringstream ss;
    boost::archive::text_oarchive oa(ss);
    oa << dumper;
    return ss.str();
}

// 反序列化
template <typename K, typename V>
void SkipList<K, V>::load_file(const std::string &dumpStr) {

    if (dumpStr.empty()) {
        return;
    }
    SkipListDump<K, V> dumper;
    std::stringstream iss(dumpStr);
    boost::archive::text_iarchive ia(iss);
    ia >> dumper;
    for (int i = 0; i < dumper.keyDumpVt_.size(); ++i) {
        insert_element(dumper.keyDumpVt_[i], dumper.keyDumpVt_[i]);
    }
}

template <typename K, typename V>
void SkipList<K, V>::clear(Node<K, V> *cur) {
    if (cur->forward[0] != nullptr) {
        clear(cur->forward[0]);
    }
    delete (cur);
}

// 获取跳表中的元素个数
template <typename K, typename V>
int SkipList<K, V>::size() {
    return _element_count;
}

template <typename K, typename V>
void SkipList<K, V>::get_key_value_from_string(const std::string &str, std::string *key, std::string *value) {
    if (!is_valid_string(str)) {
        return;
    }
    *key = str.substr(0, str.find(delimiter));
    *value = str.substr(str.find(delimiter) + 1, str.length());
}

template <typename K, typename V>
bool SkipList<K, V>::is_valid_string(const std::string &str) {
    if (str.empty()) {
        return false;
    }
    if (str.find(delimiter) == std::string::npos) {
        return false;
    }
    return true;
}

template <typename K, typename V>
class SkipListDump { public:
    friend class boost::serialization::access;

    template <class Archive>
    void serialize(Archive &ar, const unsigned int version) {
        ar &keyDumpVt_;
        ar &valDumpVt_;
    }
    std::vector<K> keyDumpVt_;
    std::vector<V> valDumpVt_;

public:
    void insert(const Node<K, V> &node);
};

template <typename K, typename V>
void SkipListDump<K, V>::insert(const Node<K, V> &node) {
    keyDumpVt_.emplace_back(node.get_key());
    valDumpVt_.emplace_back(node.get_value());
}

#endif  // SKIPLIST_H

# kvstore — 内存 KV 存储服务（C++17）

一个单机键值存储服务：文本协议走 TCP，底层是可插拔的存储引擎（数组 / 哈希 / 红黑树 / 跳表），
网络层是 epoll Reactor，支持多 reactor 分片、`std::pmr` 节点池和 WAL 崩溃恢复。

> 这个目录是 `legacy/` 里那份 C 语言实现的 **C++17 重写版**。重写不是换语法，而是逐条修复了
> 原实现的正确性缺陷，并补上了持久化、并发和测试。原实现的缺陷清单和对应修复见
> [§7](#7-原实现的缺陷与对应修复)。

---

## 1. 特性

| 能力 | 实现 |
|---|---|
| 存储引擎 | `array` / `hash`（拉链法+扩容）/ `unordered`（`std::unordered_map` 基线）/ `rbtree`（CLRS 红黑树）/ `skiplist`（跳表），启动时 `--engine` 选择 |
| 协议 | 行式文本协议，CRLF 分帧，支持 pipeline；命令大小写不敏感 |
| 网络 | epoll（Linux）+ level-triggered Reactor；非阻塞 socket，读写全量 draining，写缓冲处理部分写 |
| 并发 | 按 key 哈希分片，每片独立 `std::shared_mutex`；Linux 下可用 `SO_REUSEPORT` 起多 reactor |
| 内存 | 红黑树节点走 `std::pmr::unsynchronized_pool_resource`，不再每节点一次 malloc |
| 持久化 | 追加写 WAL + CRC32 校验 + 启动重放，截断/损坏尾部可识别 |
| 测试 | 54 个用例 / 55617 次断言，零第三方依赖 |

代码规模：实现约 3000 行，测试约 1300 行。

---

## 2. 架构

```
                       ┌──────────────────────────────────────────┐
   TCP 连接 ──────────►│ net::EventLoop  (每线程一个, 一个 poller) │
                       │  · accept / recv draining / send 缓冲     │
                       └───────────────┬──────────────────────────┘
                                       │ RequestParser（增量分帧）
                                       ▼
                       ┌──────────────────────────────────────────┐
                       │ net::CommandHandler  命令表 → 存储操作    │
                       └───────────────┬──────────────────────────┘
                                       ▼
                       ┌──────────────────────────────────────────┐
                       │ ShardedStore   N 个分片，各带 shared_mutex│
                       │   shard = hash(key) % N                   │
                       └───────┬───────────────────────┬──────────┘
                               ▼                       ▼
                     ┌──────────────────┐    ┌──────────────────┐
                     │ Engine (抽象)    │    │ Wal (可选)        │
                     │ array/hash/      │    │ 先写日志再改内存  │
                     │ rbtree/skiplist  │    └──────────────────┘
                     └──────────────────┘
```

分层的边界是**接口**而不是宏开关：`Engine` 是纯虚类，`Poller` 也是。所以换引擎、换 IO 多路复用后端
都只影响一层。

---

## 3. 目录

```
include/kv/
  engine.hpp           Engine 抽象接口 + 工厂
  engines.hpp          五种引擎的声明（含红黑树/跳表节点结构）
  status.hpp           统一的 Status 枚举
  buffer.hpp           双游标字节队列（读写共用）
  protocol.hpp         增量请求解析器
  store.hpp            分片存储 + 锁策略
  wal.hpp              预写日志
  net/socket.hpp       平台 socket 封装（Win/Linux）
  net/poller.hpp       就绪通知抽象（epoll / poll）
  net/event_loop.hpp   单 Reactor
  net/server.hpp       多 Reactor 编排 + 信号处理
src/                   对应实现
tests/                 测试框架 + 5 组测试
bench/bench.cpp        压测客户端
scripts/build.py       无 make/cmake 时的构建脚本
```

---

## 4. 构建

### Linux（部署目标）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/kv_tests          # 跑测试
./build/kvstore --help
```

或者不用 CMake：

```bash
make -j && make test
```

### 没有 make / cmake 的环境

```bash
python3 scripts/build.py --cxx g++
# 交叉编译（例如从 Windows 验证 Linux 目标）：
python3 scripts/build.py --cxx "zig c++" --target x86_64-linux-gnu --out build-linux
```

---

## 5. 运行

```bash
./build/kvstore --engine rbtree --port 9096            # 默认 hash，16 分片
./build/kvstore --engine hash --threads 4 --shards 64  # Linux：4 个 reactor 共享端口
./build/kvstore --engine hash --db data/kv.wal         # 开启持久化
./build/kvstore --engine hash --db data/kv.wal --fsync # 每条写入都 fsync（慢但抗断电）
```

手工试一下：

```bash
printf 'SET hello world\r\nGET hello\r\nCOUNT\r\n' | nc 127.0.0.1 9096
```

### 协议

一行一条命令，`\r\n` 结尾，空格/制表符分隔，命令大小写不敏感，支持 pipeline
（一次写入多条命令，按序返回多条响应）。

| 命令 | 参数 | 响应 |
|---|---|---|
| `SET key value` | 3 | `SUCCESS` |
| `GET key` | 2 | `SUCCESS:<value>` / `NO EXIST` |
| `DEL key` | 2 | `SUCCESS` / `NO EXIST` |
| `MOD key value` | 3 | `SUCCESS` / `NO EXIST`（不创建新 key） |
| `COUNT` | 1 | 键数量 |
| `KEYS [prefix]` | 1–2 | `SUCCESS:<k1> <k2> ...`（无匹配时是空列表；超过 1000 个键返回错误） |
| `PING` | 1 | `PONG` |
| `INFO` | 1 | `keys:..,engine:..,shards:..,commands:..,connections:..,poller:..` |
| `FLUSH` | 1 | `SUCCESS`（清空） |
| `QUIT` | 1 | `BYE` 后关闭连接 |

错误一律是 `ERROR:<原因>`，并且**不会断开连接**（除协议错误和 `QUIT`）。

---

## 6. 关键设计决策

**1. 分帧解析，而不是直接 `strtok` 缓冲区。**
TCP 是字节流，一条命令可能被拆成两个包，也可能三条命令挤在一个包里。原实现把 `recv` 到的
`char[128]` 直接切词，既没有重组、也没有剥掉行尾的 `\r\n`（所以存进去的 value 永远带着 CRLF）。
现在 `RequestParser` 自己持有缓冲区，只在遇到 `\n` 时产出一条请求，支持 pipelining，
并对超长行（64KB）和超多参数（64 个）直接报错。

**2. 未知命令返回错误，而不是 `assert(0)`。**
原实现命令表里找不到就 `assert(0)`，客户端发一句 `FOO\r\n` 整个服务进程 abort。现在未知命令
是一条普通的 `ERROR:unknown command 'FOO'`，连接继续可用。

**3. epoll 用水平触发，但读写都 draining 到 EAGAIN。**
ET 需要每个回调都正确 drain，否则会永久丢失事件；LT + drain 在行为上更安全，并且不牺牲吞吐。
读写事件在同一个循环里**独立处理**（原实现用 `else if`，可读可写同时就绪时只处理读，响应发不出去）。

**4. 写缓冲 + 部分写处理。**
`send()` 可能只写出一部分，原实现直接假定全发出去了。现在没发完的字节留在 `Buffer` 里，
注册 `EPOLLOUT` 等下次可写继续发；发完立刻撤销对写事件的关注，避免 busy loop。

**5. 连接表用 map 而不是 `connlist[1024]` 按 fd 下标。**
原实现用 fd 直接索引固定数组，fd ≥ 1024 就是越界写。现在 `unordered_map<socket_t, Connection>`，
并发连接上限由 `--connections` 显式限制。

**6. 按 key 分片 + 每片独立 `shared_mutex`。**
读多写少是 KV 的常态。全局一把锁会让 reactor 数量失去意义；分片后不同 key 的读完全不互相阻塞，
同一分片的读也能并发。引擎本身不加锁，锁策略只存在于 `ShardedStore` 一处，便于验证。

**7. 红黑树用内嵌哨兵 + `pmr` 节点池。**
原实现的 `nil` 是 `malloc` 出来的，只初始化了 `key` 和 `color`，`left/right/parent` 是野指针——
删除修复时一旦比较到兄弟节点就会随机崩溃。现在 `nil_` 是成员对象，三根指针都指向自己（Linux 内核
rbtree 的同款写法），既消除了崩溃，也去掉了哨兵的 malloc/free。节点从
`std::pmr::unsynchronized_pool_resource` 分配，因为每个分片独占一个引擎实例，天然单线程访问。

**8. WAL：先写日志再改内存，记录带 CRC。**
每条记录是 `len|crc32|payload`，payload 里是 `op|klen|key|vlen|value`。每次 append 都
`fflush`（进程被 `kill -9` 也不丢已确认的写），`--fsync` 再额外落盘（抗断电）。重放时遇到
截断的尾部或 CRC 不匹配就停下并提示，而不是把垃圾数据replay 进去。

**9. `KEYS` 有硬上限，而不是无界扫描。**
`KEYS` 要遍历全部分片、且在一条响应里拼出整个键列表，是典型的 O(N) 阻塞操作（Redis 的 `KEYS`
之所以是"危险命令"就是这个原因）。这里把结果限制在 1000 个键，超出直接返回
`ERROR:too many keys (N), narrow the prefix`。**报错比悄悄截断好**：调用方至少知道结果不完整，
不会把半份列表当成全量。

---

## 7. 原实现的缺陷与对应修复

下面每一条都在 `tests/` 里有对应的回归用例。

| # | 原实现位置 | 缺陷 | 后果 | 本项目的处理 |
|---|---|---|---|---|
| 1 | `kvstore.c` 命令分派 | 未知命令 `assert(0)` | 一行输入打死整个进程 | 返回 `ERROR:unknown command`，`net_unknown_command_keeps_server_alive` |
| 2 | `kvstore.c` `kvstore_split_token` | 用 `strtok` 直接切接收缓冲区，不剥 `\r\n`，不重组分片 | 粘包/半包必错；value 尾部带 CRLF | 增量 `RequestParser`，`protocol_*` 8 个用例 |
| 3 | `kvstore_hash.c` delete 链表 | 循环里比较的是 `head->key` 而不是当前节点 | 非桶头节点永远删不掉 | 正确遍历，`engine_hash_deletes_non_head_nodes` |
| 4 | `kvstore_hash.c` hash 函数 | 字节求和 | 所有字母异位词同桶 | FNV-1a，`engine_hash_separates_anagrams` |
| 5 | `kvstore_hash.c` 建表 | `sizeof(hashnode_t)` 当成 `sizeof(hashnode_t*)` | 多分配 3 倍内存 | 用 `vector`，并做负载因子扩容 |
| 6 | `kvstore_rbtree.c` 哨兵 | `nil` 的 `left/right/parent` 未初始化 | 删除修复时解引用野指针，随机崩溃 | 内嵌自指哨兵 |
| 7 | `kvstore_rbtree.c` `rbtree_kv_set` | 重复 key 时 `rbtree_insert` 早退，节点泄漏且 `count++` 照加 | 计数虚高 + 内存泄漏 | 先查再插，计数只在实际插入时加 |
| 8 | `kvstore_rbtree.c` `destroy` | `if (!node)` 恒假 | 销毁时全量泄漏 | 显式后序遍历释放 |
| 9 | `kvstore_array.c` delete | 只做 `array_idx--`，不搬移尾部元素 | 每删一条丢一条表尾数据 | 尾部元素填洞后 `pop_back` |
| 10 | `kvstore_array.c` set | 不检查重复 key | 同 key 多副本，GET 永远读旧值 | 先线性查再改 |
| 11 | `kvstore_array.c` destroy | `if (!arr->array_table) free(...)` 条件写反 | 永不释放 | RAII |
| 12 | `epoll_entry.c` 事件循环 | 读/写用 `else if` | 同时就绪时响应发不出去 | 独立处理两种事件 |
| 13 | `epoll_entry.c` send | 不处理部分写 | 大响应被截断 | `Buffer` + `EPOLLOUT` |
| 14 | `epoll_entry.c` 连接表 | `connlist[1024]` 按 fd 下标、无边界检查 | fd ≥ 1024 越界写 | `unordered_map` |
| 15 | `my_malloc_gu.c` `mp_alloc` | 分配最后一块后 `free_ptr==NULL` 直接返回 NULL | 最后一块永远分配不到 | 用 `std::pmr` 池资源 |
| 16 | `my_malloc_gu.c` 接口 | `kvstore_malloc(size)` 忽略 `size` | 超过块大小的分配踩内存 | `pmr` 按需分配 |
| 17 | `test3.c` 压测客户端 | 响应逐字节 `recv(...,1,...)` | 客户端成为瓶颈，QPS 数字失真 | 批量读 + pipeline + 错误计数 |

---

## 8. 测试

```bash
./build/kv_tests
# 54 test(s), 55617 check(s), 0 failure(s)
```

覆盖方式不是"跑一遍不崩"，而是：

- **契约测试**：同一组断言跑遍 5 种引擎，保证 PUT/GET/DEL/MOD/COUNT 语义一致。
- **差分测试**：红黑树和跳表各用 2 万 / 1 万次随机操作与 `std::map` 对拍，每 500 步校验一次不变量。
- **结构不变量**：红黑树每次校验 BST 有序性、根黑、红节点无红子、各路径黑高相等、计数一致，
  并检查树高 ≤ 2·log₂(n+1)。跳表校验各层有序且是第 0 层的子序列。
- **并发**：8 线程并发写 8000 个 key 再并发读回，校验无丢失、无错值。
- **崩溃**：WAL 文件尾部截断 5 字节、中间翻转一个 bit，均需被识别且只丢掉受损记录。
- **端到端**：起真实 Reactor，用真实 socket 测流水线、半包、2 万字节 value、`QUIT`、以及
  "客户端发半个请求就跑"的场景。

---

## 9. 性能

全部数据在 **Linux（WSL2，16 核）+ epoll 后端**上测得，服务端日志确认 `poller=epoll`。
客户端与服务端同机走 loopback，GET:SET = 1:1，value 32 字节，每个请求都校验响应、统计错误数。

> 延迟一栏是"每批耗时 / 批内请求数"，即流水线下的**均摊**服务时间，不是单次 RTT。

> **关于平台差异**：WSL2 的 loopback 要穿过虚拟化层，比原生 Linux 慢很多。同一套代码在
> GitHub Actions 的 Ubuntu runner（原生 loopback）上，2 Reactor / 64 分片 / 4 连接 / pipeline=32
> 跑出 **3,297,361 ops/s**（p50 1.0 µs，50 万请求 0 错误，见 CI 的 `linux epoll benchmark` job）。
> 所以下面所有 WSL2 数字应视为**下界**，而不是这台实现的极限。

### 9.1 数据结构 × 分片 × Reactor（4 连接、pipeline=32、键空间 1 万）

| 引擎 | 1 Reactor · 1 分片 | 2 Reactor · 1 分片 | 2 Reactor · 64 分片 |
|---|---:|---:|---:|
| hash（拉链 + 扩容） | **548,000** | 464,071 | 529,674 |
| rbtree（红黑树） | 442,029 | 220,222 | 484,060 |
| array（线性扫描） | 49,645 | 33,442 | **443,318** |

同一张表能读出三件事：

1. **数据结构本身**：单 Reactor + 单分片下，数组比哈希慢 11 倍，这就是 O(n) 与 O(1) 的差距。
2. **加 Reactor 而不分片是负优化**：两个 Reactor 抢同一把分片锁，临界区越长掉得越狠
   （红黑树 −50%、数组 −33%）。并行度不是线程数，而是"能同时进入临界区的操作数"。
3. **分片把锁拆开后，数组反而追平哈希**：从 33,442 涨到 443,318（13 倍），因为它那趟 O(n)
   扫描只覆盖自己那 1/64 的数据，锁持有时间也被切成 64 份。

所以"某个引擎快/慢"这句话本身没有意义——必须带上分片数、Reactor 数和键空间。

### 9.2 全引擎对比（64 分片、2 Reactor、4 连接、pipeline=32、键空间 1 万）

| 引擎 | 吞吐 (ops/s) | p50 | p99 |
|---|---:|---:|---:|
| unordered（标准库基线） | 555,603 | 6.2 µs | 16.1 µs |
| hash | 529,674 | 6.5 µs | 16.9 µs |
| rbtree | 484,060 | 6.5 µs | 18.0 µs |
| skiplist | 462,576 | 7.1 µs | 18.9 µs |
| array | 443,318 | 7.9 µs | 21.1 µs |

自研哈希与 `std::unordered_map` 差距在 5% 以内——手写链条 + FNV-1a 在短 key 场景没有明显劣势。

### 9.3 多 Reactor 扩展性（hash、64 分片、**16 连接**、pipeline=32、40 万请求）

| Reactor 数 | 吞吐 (ops/s) | 相对单 Reactor | p50 |
|---:|---:|---:|---:|
| 1 | 561,824 | 1.00× | 24.4 µs |
| 2 | 855,113 | 1.52× | 13.3 µs |
| 4 | 1,022,091 | 1.82× | 10.4 µs |
| 8 | 1,143,671 | 2.04× | 9.2 µs |

扩展性到 2 个 Reactor 接近线性，之后受限于内存带宽、共享分片锁和客户端本身。

⚠️ 同一份代码，**只把客户端连接数从 4 改成 16**，结论就从"多 Reactor 没用"（544k→530k→523k）
翻成"2 个 Reactor 提升 52%"。压测先确认瓶颈在哪一端，比调实现更重要。

### 9.4 单请求往返（64 连接、pipeline=1）——与原项目同口径

原项目 README 报的是 64 连接、一次一个往返下的 33k / 37k QPS，这一组用完全相同的口径复测：

| 引擎 | 本项目 | 原项目声称 | |
|---|---:|---:|---|
| hash | **43,009** | 33,000 | +30% |
| rbtree | **47,068** | 37,000 | +27% |

同时 p50 延迟 1.14–1.19 ms（64 条连接共享一个 Reactor，排队时间占主导）。

### 9.5 关于那两个原始数字

即便数值接近，原项目的测法也是**不可复现**的：它的客户端对每个响应做逐字节 `recv(..., 1, ...)`，
一个请求十几次系统调用；而且它不校验响应内容，丢包、错包也能"跑出高分"。本项目把客户端改成
批量读、支持 pipeline、逐条校验并统计错误数，上表才成立。

### 9.6 非 Linux 平台（poll 后端，仅供参考）

同一套代码在 Windows 上用 WSAPoll 后端、单 Reactor 跑出 hash 501k / rbtree 412k ops/s
（pipeline=32），pipeline=1 时约 20k。差距主要来自 WSAPoll 与 epoll 的事件分发成本，
不影响上面的 Linux 结论。

### 9.7 复现

```bash
# 1) 数据结构对比（关分片）
./build/kvstore --port 9096 --engine array --threads 1 --shards 1 &
./build/kv_bench --port 9096 --threads 4 --requests 20000 --pipeline 32 --keys 10000

# 2) 全引擎对比
./build/kvstore --port 9096 --engine rbtree --threads 2 --shards 64 &
./build/kv_bench --port 9096 --threads 4 --requests 500000 --pipeline 32 --keys 10000

# 3) Reactor 扩展性（客户端连接数一定要给够）
./build/kvstore --port 9096 --engine hash --threads 8 --shards 64 &
./build/kv_bench --port 9096 --threads 16 --requests 400000 --pipeline 32 --keys 10000

# 4) 单请求往返
./build/kvstore --port 9096 --engine hash --threads 2 --shards 64 &
./build/kv_bench --port 9096 --threads 64 --requests 100000 --pipeline 1 --keys 10000
```

---

## 10. 已知限制

- 只支持单机；没有集群、复制、一致性哈希。
- 协议是自定义文本协议，不是 RESP；没有二进制安全的多行 value。
- 无 TTL / 淘汰策略；`FLUSH` 不会截断 WAL（重启后会重放回旧数据，这是已知取舍）。
- `KEYS` 是 O(N) 全扫且需要一次性拼出响应，超过 1000 个键会报错；生产场景应该用游标式
  `SCAN` 而不是 `KEYS`。
- WAL 无 compaction，长期运行会一直增长。
- 多 Reactor 依赖 `SO_REUSEPORT`，非 Linux 平台退化为单 Reactor。
- 跳表没有做内存池，节点仍是逐个 `new`。

---

## 11. 写进简历怎么说

先明确一件事：**原项目是网上流传的课程级实现，不能声称"从零设计"**。正确的定位是
"对一份 C 实现做了 C++ 重构与工程化改造"，然后把上面那张缺陷表讲成你的设计动机。

可以直接用的句式（数字按你实测的填）：

> **KV 存储服务（C++17，个人项目）**
> 将一份约 2500 行的 C 语言 KV 存储重构为 C++17 工程：以抽象 `Engine` 接口替换宏开关实现五种
> 可插拔存储引擎（数组/拉链哈希/红黑树/跳表/标准库基线），用 `std::pmr` 节点池替代手写定长内存池；
> 网络层基于 epoll Reactor 重写，修正原实现的分帧缺失、读写事件互斥、部分写丢失等问题；
> 引入按 key 分片 + `shared_mutex` 支持多 Reactor 并发，并补充 CRC32 校验的 WAL 崩溃恢复；
> 编写 54 个用例 / 5.6 万次断言的测试（含红黑树与 `std::map` 的随机对拍和红黑性质校验）；
> 16 核 Linux 上 8 Reactor 混合读写达 114 万 ops/s（pipeline=32），与原项目同口径的重测
> （64 连接、单请求往返）4.7 万 ops/s、较其自报的 3.7 万提升 27%。

### 面试大概率会问，提前准备

1. **红黑树删除的四种情况**，以及为什么"兄弟节点为红"要先旋转。
2. **为什么哨兵要自指**——`nil->parent` 会被 `transplant` 改写，这是 CLRS 实现里最容易漏的细节。
3. **epoll 的 LT 和 ET 区别**，为什么这里选 LT，ET 下漏读会怎样。
4. **粘包怎么解决**，你的 `RequestParser` 在缓冲多大时会报错，报错后连接怎么办。
5. **为什么用 `shared_mutex` 而不是 `mutex`**，分片数怎么选，分片过多有什么代价。
6. **WAL 的 fsync 策略**：`fflush` 和 `fsync` 分别防什么故障；为什么先写日志再改内存。
7. **`std::pmr` 的 `unsynchronized_` 前缀意味着什么**，为什么这里可以不用同步版本。
8. **压测方法学**：pipeline 为什么把 QPS 拉高 25 倍，那这个数字还有意义吗，怎么测真实延迟。

# kvstore

一个内存键值存储服务。这个仓库里有两份实现：

| 位置 | 内容 | 说明 |
|---|---|---|
| [`cpp/`](cpp/) | **C++17 重写版**（主要工作） | epoll Reactor、五种可插拔引擎、分片并发、`std::pmr` 节点池、WAL 崩溃恢复、54 个测试用例 |
| [`legacy/`](legacy/) | 原始的 C 语言实现 | 作为对照基线保留，构建依赖一个未随仓库提供的协程库，**无法直接编译** |

重写版的完整说明、架构图、原实现缺陷清单、性能数据和面试要点都在
**[`cpp/README.md`](cpp/README.md)**。

## 快速开始

```bash
cd cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/kv_tests                 # 54 个用例
./build/kvstore --help
```

```bash
./build/kvstore --engine rbtree --port 9096 &
printf 'SET hello world\r\nGET hello\r\nCOUNT\r\n' | nc 127.0.0.1 9096
```

## 一句话结论

原始实现是"能跑通的玩具"：没有分帧、未知命令会 `assert(0)` 打死进程、哈希表删不掉非桶头节点、
红黑树的哨兵指针未初始化会随机崩溃、数组引擎删一条丢一条、压测客户端逐字节 `recv` 导致
QPS 数字失真。

C++ 版逐条修掉了这些问题，并用测试把每一条都钉住；然后补上了原版完全没有的持久化与并发。

## 目录

```
cpp/        C++17 实现（cmake / make / python scripts/build.py 三种构建方式）
legacy/     原始 C 实现，仅作对照，不参与构建
.github/    CI：Ubuntu 上编译 + 跑全部测试 + 跑一次 epoll 压测
```

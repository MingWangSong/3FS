# Fire-Flyer 文件系统 (3FS)

[![Build](https://github.com/deepseek-ai/3fs/actions/workflows/build.yml/badge.svg)](https://github.com/deepseek-ai/3fs/actions/workflows/build.yml)
[![License](https://img.shields.io/badge/LICENSE-MIT-blue.svg)](LICENSE)

Fire-Flyer 文件系统（3FS）是一种高性能分布式文件系统，专为解决 AI 训练和推理工作负载的挑战而设计。它利用现代 SSD 和 RDMA 网络提供共享存储层，从而简化分布式应用程序的开发。

## 主要特性与优势

### 性能与可用性
- **分离式架构**：结合了数千个 SSD 的吞吐量和数百个存储节点的网络带宽，使应用程序能够以位置无关的方式访问存储资源。
- **强一致性**：实现了带有分配查询的链式复制（CRAQ），确保强一致性，使应用程序代码简单且易于推理。
- **文件接口**：开发了由事务性键值存储（如 FoundationDB）支持的无状态元数据服务。文件接口广为人知且使用广泛，无需学习新的存储 API。

### 多样化工作负载支持
- **数据准备**：将数据分析管道的输出组织成层次化的目录结构，并高效管理大量中间输出。
- **数据加载器**：通过支持计算节点之间对训练样本的随机访问，消除了预取或打乱数据集的需求。
- **检查点保存**：支持大规模训练的高吞吐量并行检查点保存。
- **推理 KV 缓存**：为基于 DRAM 的缓存提供了一种经济高效的替代方案，提供高吞吐量和显著更大的容量。

## 系统架构与实现原理

3FS 系统由四个主要组件组成：集群管理器、元数据服务、存储服务和客户端。所有组件都通过 RDMA 网络（InfiniBand 或 RoCE）连接，形成一个高性能分布式系统。

### 集群管理器
- **功能职责**：
  - 处理成员变更并将集群配置分发给其他服务和客户端
  - 通过心跳机制监控系统中的服务状态
  - 维护链表和存储目标的状态
- **高可用设计**：
  - 多个集群管理器部署，通过选举确定一个主管理器
  - 当主管理器失效时，系统自动提升另一个管理器接管工作
  - 集群配置通常存储在可靠的分布式协调服务中（如 ZooKeeper 或 etcd）

### 元数据服务
- **功能职责**：
  - 处理文件元数据操作（如打开或创建文件/目录）
  - 实现文件系统语义
- **无状态设计**：
  - 文件元数据存储在事务性键值存储中（如 FoundationDB）
  - 无状态设计提高了系统的可维护性和可靠性
  - 客户端可以连接到任何元数据服务
- **元数据存储结构**：
  - **索引节点（Inodes）**：存储文件、目录和符号链接的属性信息，每个由全局唯一的64位标识符标识
  - **目录条目**：由"DENT"前缀、父索引节点ID和条目名称组成
- **事务处理**：
  - 只读事务用于元数据查询：fstat、lookup、listdir 等
  - 读写事务用于元数据更新：create、link、unlink、rename 等
  - 利用 FoundationDB 的事务能力确保元数据操作的一致性

### 存储服务
- **功能职责**：
  - 管理本地 SSD 并提供块存储接口
  - 实现数据复制和一致性协议
- **数据组织**：
  - 文件被分割成大小相等的块（chunks）
  - 每个块通过多个复制链（replication chains）进行条带化
  - 块ID由文件的inode ID和块索引连接生成
- **CRAQ协议实现**：
  - 实现带有分配查询的链式复制（CRAQ）以确保强一致性
  - 写请求发送到链头目标并沿链传播
  - 读请求可以发送到链中的任何存储目标
  - 采用写入全部-读取任意（write-all-read-any）方法，充分利用SSD和RDMA网络的吞吐量
- **块存储引擎**：
  - 由固定数量的数据文件和RocksDB实例组成
  - 维护块元数据的内存缓存以提高查询性能
  - 实现块分配器用于快速分配新块
  - 支持open/close、get、update、commit等核心操作

### 客户端
- **FUSE客户端**：
  - 大多数应用程序使用，采用障碍低
  - 将I/O操作重定向到用户空间进程
  - 适合一般用途应用
- **原生客户端**：
  - 为性能关键型应用程序设计
  - 提供异步零拷贝I/O操作
  - 关键数据结构：
    - **Iov**：用于零拷贝读/写操作的大内存区域
    - **Ior**：用于用户进程和原生客户端之间通信的小型共享环形缓冲区

### 故障检测与恢复机制
- **故障检测**：
  - 集群管理器通过心跳机制检测故障停止
  - 存储目标具有公共状态和本地状态
  - 公共状态：serving（服务中）、syncing（同步中）、waiting（等待中）、lastsrv（最后服务）、offline（离线）
  - 本地状态：up-to-date（最新）、online（在线）、offline（离线）
- **数据恢复流程**：
  1. 服务定期从集群管理器拉取最新链表
  2. 恢复期间的写请求始终是完整块替换写入
  3. 前任发送块元数据转储请求到返回的服务
  4. 比较本地和远程块元数据，确定需要传输的块
  5. 传输所有必要的块后发送同步完成消息

### 数据放置策略
- **链表设计**：
  - 通过数学优化模型设计链表，确保故障恢复期间的负载均衡
  - 当某个节点失败时，其读请求会被重定向到其他节点
  - 优化的链表设计确保重定向流量均匀分布在剩余节点上
- **平衡恢复流量**：
  - 数据放置问题被建模为平衡不完全块设计（BIBD）
  - 使用整数规划求解器获得最优解决方案
  - 确保在恢复期间最大化读取吞吐量

## 性能表现

### 1. 峰值吞吐量

下图展示了在大型 3FS 集群上进行读取压力测试的吞吐量。该集群由 180 个存储节点组成，每个节点配备 2×200Gbps InfiniBand NIC 和十六个 14TiB NVMe SSD。约 500+ 客户端节点用于读取压力测试，每个客户端节点配置有 1x200Gbps InfiniBand NIC。最终聚合读取吞吐量在训练作业的背景流量下达到约 6.6 TiB/s。

![大型集群压力测试下的大块读取吞吐量](docs/images/peak_throughput.jpg)

### 2. GraySort 基准测试

我们使用 GraySort 基准测试评估了 [smallpond](https://github.com/deepseek-ai/smallpond)，该基准测试衡量大规模数据集上的排序性能。我们的实现采用两阶段方法：(1) 通过使用键的前缀位进行洗牌来分区数据，(2) 分区内排序。两个阶段都从 3FS 读取/写入数据。

测试集群由 25 个存储节点（每节点 2 个 NUMA 域，每 NUMA 1 个存储服务，每节点 2×400Gbps NIC）和 50 个计算节点（2 个 NUMA 域，192 个物理核心，2.2 TiB RAM，每节点 1×200 Gbps NIC）组成。在 8,192 个分区上对 110.5 TiB 的数据进行排序在 30 分钟 14 秒内完成，实现了平均吞吐量 *3.66 TiB/分钟*。

![GraySort 服务器性能](docs/images/gray_sort_server.png)
![GraySort 客户端性能](docs/images/gray_sort_client.png)

### 3. KV 缓存

KV 缓存是一种用于优化 LLM 推理过程的技术。它通过缓存解码器层中先前令牌的键和值向量来避免冗余计算。
上图展示了所有 KV 缓存客户端（每节点 1×400Gbps NIC）的读取吞吐量，突出显示了峰值和平均值，峰值吞吐量高达 40 GiB/s。下图展示了同一时期垃圾回收（GC）中移除操作的 IOPS。

![KV 缓存读取吞吐量](docs/images/kvcache_read_throughput.png)
![KV 缓存 GC IOPS](docs/images/kvcache_gc_iops.png)

## 部署环境与操作指南

### 硬件要求

以下是部署一个六节点集群的推荐硬件配置：

| 节点类型 | 操作系统 | 内存 | 存储 | RDMA网络 |
|---------|---------|------|------|---------|
| 元数据节点 | Ubuntu 22.04 | 128GB | - | RoCE |
| 存储节点 | Ubuntu 22.04 | 512GB | 14TB × 16 | RoCE |

> **RDMA网络配置**
> 1. 为RDMA网卡分配IP地址。每个节点支持多个RDMA网卡（InfiniBand或RoCE）。
> 2. 使用`ib_write_bw`检查节点间的RDMA连接。

### 第三方依赖

在生产环境中，建议在专用节点上安装以下服务：

| 服务 | 推荐节点 |
|------|---------|
| [ClickHouse](https://clickhouse.com/docs/install) | 元数据节点 |
| [FoundationDB](https://apple.github.io/foundationdb/administration.html) | 元数据节点 |

> **FoundationDB注意事项**
> 1. 确保FoundationDB客户端版本与服务器版本匹配，或复制相应版本的`libfdb_c.so`以保持兼容性。
> 2. 在安装了FoundationDB的节点上，可以在`/etc/foundationdb/fdb.cluster`和`/usr/lib/libfdb_c.so`找到`fdb.cluster`文件和`libfdb_c.so`。

### 部署步骤

#### 1. 构建3FS

按照[构建说明](#构建-3fs)构建3FS。二进制文件可以在`build/bin`中找到。

#### 2. 创建ClickHouse表用于指标收集

在元数据节点上导入SQL文件到ClickHouse：
```bash
clickhouse-client -n < ~/3fs/deploy/sql/3fs-monitor.sql
```

#### 3. 部署监控服务

在元数据节点上安装`monitor_collector`服务：
```bash
mkdir -p /opt/3fs/{bin,etc}
mkdir -p /var/log/3fs
cp ~/3fs/build/bin/monitor_collector_main /opt/3fs/bin
cp ~/3fs/configs/monitor_collector_main.toml /opt/3fs/etc
```

更新配置文件并启动服务：
```bash
systemctl start monitor_collector_main
```

#### 4. 部署管理客户端

在所有节点上安装`admin_cli`：
```bash
mkdir -p /opt/3fs/{bin,etc}
rsync -avz meta:~/3fs/build/bin/admin_cli /opt/3fs/bin
rsync -avz meta:~/3fs/configs/admin_cli.toml /opt/3fs/etc
rsync -avz meta:/etc/foundationdb/fdb.cluster /opt/3fs/etc
```

#### 5. 部署集群管理服务

在元数据节点上安装`mgmtd`服务：
```bash
cp ~/3fs/build/bin/mgmtd_main /opt/3fs/bin
cp ~/3fs/configs/{mgmtd_main.toml,mgmtd_main_launcher.toml,mgmtd_main_app.toml} /opt/3fs/etc
```

初始化集群并启动服务：
```bash
/opt/3fs/bin/admin_cli -cfg /opt/3fs/etc/admin_cli.toml "init-cluster --mgmtd /opt/3fs/etc/mgmtd_main.toml 1 1048576 16"
systemctl start mgmtd_main
```

#### 6. 部署元数据服务

在元数据节点上安装`meta`服务：
```bash
cp ~/3fs/build/bin/meta_main /opt/3fs/bin
cp ~/3fs/configs/{meta_main_launcher.toml,meta_main.toml,meta_main_app.toml} /opt/3fs/etc
```

上传配置文件并启动服务：
```bash
/opt/3fs/bin/admin_cli -cfg /opt/3fs/etc/admin_cli.toml --config.mgmtd_client.mgmtd_server_addresses '["RDMA://192.168.1.1:8000"]' "set-config --type META --file /opt/3fs/etc/meta_main.toml"
systemctl start meta_main
```

#### 7. 部署存储服务

在每个存储节点上：

1. 格式化并挂载SSD：
```bash
mkdir -p /storage/data{1..16}
mkdir -p /var/log/3fs
for i in {1..16};do mkfs.xfs -L data${i} /dev/nvme${i}n1;mount -o noatime,nodiratime -L data${i} /storage/data${i};done
mkdir -p /storage/data{1..16}/3fs
```

2. 增加异步IO请求的最大数量：
```bash
sysctl -w fs.aio-max-nr=67108864
```

3. 安装存储服务：
```bash
rsync -avz meta:~/3fs/build/bin/storage_main /opt/3fs/bin
rsync -avz meta:~/3fs/configs/{storage_main_launcher.toml,storage_main.toml,storage_main_app.toml} /opt/3fs/etc
```

4. 上传配置文件并启动服务：
```bash
/opt/3fs/bin/admin_cli -cfg /opt/3fs/etc/admin_cli.toml --config.mgmtd_client.mgmtd_server_addresses '["RDMA://192.168.1.1:8000"]' "set-config --type STORAGE --file /opt/3fs/etc/storage_main.toml"
systemctl start storage_main
```

#### 8. 创建管理员用户、存储目标和链表

1. 创建管理员用户：
```bash
/opt/3fs/bin/admin_cli -cfg /opt/3fs/etc/admin_cli.toml --config.mgmtd_client.mgmtd_server_addresses '["RDMA://192.168.1.1:8000"]' "user-add --root --admin 0 root"
```

2. 生成链表：
```bash
pip install -r ~/3fs/deploy/data_placement/requirements.txt
python ~/3fs/deploy/data_placement/src/model/data_placement.py \
   -ql -relax -type CR --num_nodes 5 --replication_factor 3 --min_targets_per_disk 6
python ~/3fs/deploy/data_placement/src/setup/gen_chain_table.py \
   --chain_table_type CR --node_id_begin 10001 --node_id_end 10005 \
   --num_disks_per_node 16 --num_targets_per_disk 6 \
   --target_id_prefix 1 --chain_id_prefix 9 \
   --incidence_matrix_path output/DataPlacementModel-v_5-b_10-r_6-k_3-λ_2-lb_1-ub_1/incidence_matrix.pickle
```

3. 创建存储目标并上传链表：
```bash
/opt/3fs/bin/admin_cli --cfg /opt/3fs/etc/admin_cli.toml --config.mgmtd_client.mgmtd_server_addresses '["RDMA://192.168.1.1:8000"]' --config.user_info.token $(<"/opt/3fs/etc/token.txt") < output/create_target_cmd.txt

/opt/3fs/bin/admin_cli --cfg /opt/3fs/etc/admin_cli.toml --config.mgmtd_client.mgmtd_server_addresses '["RDMA://192.168.1.1:8000"]' --config.user_info.token $(<"/opt/3fs/etc/token.txt") "upload-chains output/generated_chains.csv"

/opt/3fs/bin/admin_cli --cfg /opt/3fs/etc/admin_cli.toml --config.mgmtd_client.mgmtd_server_addresses '["RDMA://192.168.1.1:8000"]' --config.user_info.token $(<"/opt/3fs/etc/token.txt") "upload-chain-table --desc stage 1 output/generated_chain_table.csv"
```

#### 9. 部署FUSE客户端

在客户端节点上：

1. 安装FUSE客户端：
```bash
cp ~/3fs/build/bin/hf3fs_fuse_main /opt/3fs/bin
cp ~/3fs/configs/{hf3fs_fuse_main_launcher.toml,hf3fs_fuse_main.toml,hf3fs_fuse_main_app.toml} /opt/3fs/etc
```

2. 创建挂载点：
```bash
mkdir -p /3fs/stage
```

3. 上传配置文件并启动服务：
```bash
/opt/3fs/bin/admin_cli -cfg /opt/3fs/etc/admin_cli.toml --config.mgmtd_client.mgmtd_server_addresses '["RDMA://192.168.1.1:8000"]' "set-config --type FUSE --file /opt/3fs/etc/hf3fs_fuse_main.toml"
systemctl start hf3fs_fuse_main
```

4. 检查挂载状态：
```bash
mount | grep '/3fs/stage'
```

### 常见问题解答

1. **如何排查`admin_cli init-cluster`错误？**
   如果在运行`init-cluster`后mgmtd无法启动，最可能的原因是`mgmtd_main.toml`中的错误。对此文件的任何更改都需要清除所有FoundationDB数据并重新运行`init-cluster`。

2. **如何构建单节点集群？**
   数据复制至少需要两个存储服务。在测试环境中，可以通过在单台机器上部署多个存储服务来绕过这一限制。

3. **如何更新配置文件？**
   所有配置文件都由mgmtd管理。如果更新了任何`*_main.toml`文件，如`storage_main.toml`，应使用`admin_cli set-config`上传修改后的文件。

4. **如何排查常见部署问题？**
   - 使用`journalctl`检查`stdout/stderr`中的日志消息，尤其是在服务启动期间。
   - 检查存储在服务和客户端节点上的`/var/log/3fs/`中的日志文件。
   - 确保在启动任何服务之前存在目录`/var/log/3fs/`。

## 获取源代码

从 GitHub 克隆 3FS 仓库：

```bash
git clone https://github.com/deepseek-ai/3fs
```

当 `deepseek-ai/3fs` 被克隆到本地文件系统后，运行以下命令检出子模块：

```bash
cd 3fs
git submodule update --init --recursive
./patches/apply.sh
```

## 安装依赖

安装依赖：

```bash
# 对于 Ubuntu 20.04
apt install cmake libuv1-dev liblz4-dev liblzma-dev libdouble-conversion-dev libdwarf-dev libunwind-dev \
  libaio-dev libgflags-dev libgoogle-glog-dev libgtest-dev libgmock-dev clang-format-14 clang-14 clang-tidy-14 lld-14 \
  libgoogle-perftools-dev google-perftools libssl-dev libclang-rt-14-dev gcc-10 g++-10 libboost1.71-all-dev

# 对于 Ubuntu 22.04
apt install cmake libuv1-dev liblz4-dev liblzma-dev libdouble-conversion-dev libdwarf-dev libunwind-dev \
  libaio-dev libgflags-dev libgoogle-glog-dev libgtest-dev libgmock-dev clang-format-14 clang-14 clang-tidy-14 lld-14 \
  libgoogle-perftools-dev google-perftools libssl-dev gcc-12 g++-12 libboost-all-dev
```

安装其他构建先决条件：

- [`libfuse`](https://github.com/libfuse/libfuse/releases/tag/fuse-3.16.1) 3.16.1 或更新版本
- [FoundationDB](https://apple.github.io/foundationdb/getting-started-linux.html) 7.1 或更新版本
- [Rust](https://www.rust-lang.org/tools/install) 工具链：最低 1.75.0，推荐 1.85.0 或更新版本（最新稳定版本）

## 构建 3FS

在 `build` 文件夹中构建 3FS：

```bash
cmake -S . -B build -DCMAKE_CXX_COMPILER=clang++-14 -DCMAKE_C_COMPILER=clang-14 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j 32
```
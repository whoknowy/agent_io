# vindex — 高性能 I/O 优化向量检索系统

## 项目简介

vindex 是一个基于图索引（KNN Graph / Vamana）的**外存向量检索存储引擎**，针对 SSD 上的大规模高维向量相似性搜索场景，实现了三个核心 I/O 优化：

- **用户态 I/O 缓存池**：替换操作系统页面缓存，加权淘汰策略，内存占用可控
- **拓扑感知 I/O 预取**：利用图拓扑提前异步拉取"下一跳"节点，隐藏磁盘延迟
- **LSM-Tree 写入优化**：将随机写转为顺序追加写，后台分层合并控制写放大

适用于大语言模型 Agent 的"长期记忆"检索、以图搜图等场景。

## 快速开始

### 编译

```bash
mkdir build && cd build
cmake .. -G Ninja
cmake --build .
```

编译器要求：C++17（Clang 10+ / GCC 9+ / MSVC 2019+）。

> **Linux 用户**：如需 io_uring 异步 I/O 后端，请安装 `liburing-dev`，然后添加 `-DVINDEX_USE_IO_URING=ON` 编译选项。

### 数据集

项目使用标准 SIFT 格式数据集，目录结构如下：

```
siftsmall/                        # SIFT 小规模测试集（1 万条）
├── siftsmall_base.fvecs          # 基础向量
├── siftsmall_query.fvecs         # 查询向量
├── siftsmall_groundtruth.ivecs   # 真值标注
└── siftsmall_learn.fvecs         # 训练向量

SIFT-1M/                          # SIFT 百万级完整数据集
├── sift_base.fvecs
├── sift_query.fvecs
├── sift_groundtruth.ivecs
└── sift_learn.fvecs
```

---

## 命令详解

### 1. 训练 PQ 码本：`train-pq`

对训练向量进行乘积量化（Product Quantization），生成码本文件，用于后续构建和查询时的有损压缩加速。

```bash
vindex train-pq \
    --input siftsmall/siftsmall_learn.fvecs \
    --out   siftsmall/codebook.pcb \
    --M 64 \
    --K 256 \
    --iters 25 \
    --limit 100000
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--input` | (必填) | 训练向量路径 (.fvecs) |
| `--out` | (必填) | 码本输出路径 (.pcb) |
| `--M` | 64 | 子空间数量（必须整除向量维度） |
| `--K` | 256 | 每个子空间的聚类中心数 |
| `--iters` | 25 | K-Means 最大迭代次数 |
| `--limit` | 100000 | 最多使用的训练向量数 |

---

### 2. 构建索引：`build`

加载基础向量，构建 KNN 图索引，写入磁盘段文件和清单（manifest）。

```bash
# 无 PQ 压缩
vindex build \
    --base    siftsmall/siftsmall_base.fvecs \
    --out_dir data_no_pq \
    --degree  32

# 带 PQ 压缩
vindex build \
    --base     siftsmall/siftsmall_base.fvecs \
    --out_dir  data_pq \
    --degree   32 \
    --codebook siftsmall/codebook.pcb

# 使用 Vamana 近似构建（适用于 >1 万向量）
vindex build \
    --base    SIFT-1M/sift_base.fvecs \
    --out_dir data_sift1m \
    --degree  32 \
    --builder vamana
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--base` | (必填) | 基础向量路径 (.fvecs) |
| `--out_dir` | data | 输出目录 |
| `--degree` | 32 | 图节点最大出度 |
| `--codebook` | (空) | PQ 码本路径，指定后启用 PQ 压缩 |
| `--builder` | auto | 图构建算法：`brute`（精确 O(N²)）、`vamana`（近似）、`auto`（≤1 万用 brute，>1 万用 vamana） |
| `--limit` | 0 | 最多加载的向量数（0 = 全部） |

**输出文件：**
- `data_pq/segment_0.vsg` — 磁盘段（含向量数据、图邻居、PQ 码）
- `data_pq/manifest.txt` — 段清单

---

### 3. 查询：`query`

对已构建的索引执行 Top-K 近似最近邻搜索。

```bash
# 基础查询
vindex query \
    --manifest  data_pq/manifest.txt \
    --query     siftsmall/siftsmall_query.fvecs \
    --topk 10 \
    --beam 32

# 开启缓存 + 预取 + 多线程
vindex query \
    --manifest  data_pq/manifest.txt \
    --query     siftsmall/siftsmall_query.fvecs \
    --topk 10 \
    --beam 32 \
    --cache_mb 20 \
    --prefetch \
    --threads 4
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--manifest` | (必填) | 段清单路径 |
| `--query` | (必填) | 查询向量路径 (.fvecs) |
| `--topk` | 10 | 返回结果数 K |
| `--beam` | 32 | Beam Search 的束宽（越大精度越高，越慢） |
| `--max_visits` | 10000 | 每次搜索最多访问的节点数 |
| `--limit` | 0 | 最多查询的向量数（0 = 全部） |
| `--cache_mb` | 0 | 用户态缓存大小（MB），0 = 不启用 |
| `--no_cache` | — | 显式禁用缓存 |
| `--prefetch` | — | 启用拓扑感知异步预取 |
| `--threads` | 1 | 并行查询线程数 |

**输出示例：**
```
Top-10 results for query 0:
  id=2176 dist=76608
  id=3752 dist=77004
  ...
Avg visited per query: 7914.1 (PQ dist: 1237334, exact reads: 79141, prefetch_hits: 47371) [threads=4]
Cache: hits=19583 misses=12192 hit_rate=0.616 peak_mb=5.00
```

---

### 4. 精度评估：`eval`

对比真值标注计算 Recall@K。

```bash
vindex eval \
    --manifest    data_pq/manifest.txt \
    --query       siftsmall/siftsmall_query.fvecs \
    --groundtruth siftsmall/siftsmall_groundtruth.ivecs \
    --topk 10 \
    --beam 32 \
    --cache_mb 20 \
    --prefetch
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--manifest` | (必填) | 段清单路径 |
| `--query` | (必填) | 查询向量路径 (.fvecs) |
| `--groundtruth` | (必填) | 真值标注路径 (.ivecs) |
| 其余参数同 `query` | | |

---

### 5. 增量插入：`insert`

向现有索引批量追加新向量。内部采用 LSM-Tree 风格：小批次写入 Level-0 段，后台自动合并。

```bash
vindex insert \
    --manifest  data_pq/manifest.txt \
    --input     new_vectors.fvecs \
    --out_dir   data_pq \
    --flush 1000 \
    --degree 32
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--manifest` | (必填) | 现有段清单路径 |
| `--input` | (必填) | 新增向量路径 (.fvecs) |
| `--out_dir` | data | 新段输出目录 |
| `--flush` | 0 | 每批次向量数（0 = 一次性全部写入一个段） |
| `--degree` | 32 | 图节点最大出度 |

**工作流程：**
1. 向量缓冲到内存 MemTable
2. MemTable 满（达到 `--flush`）后，构建 KNN 图并写入 Level-0 段
3. 更新 Manifest
4. 后台 Compaction 线程检测各层段数，触发合并

---

## 核心优化配置指南

### 缓存池 (`--cache_mb`)

建议设置为数据集原始向量大小的 10-20%。例如：

- siftsmall（1 万 × 128 维 × 4 字节 ≈ 5 MB）：`--cache_mb 1` 到 `--cache_mb 5`
- SIFT1M（100 万 × 128 × 4 ≈ 512 MB）：`--cache_mb 50` 到 `--cache_mb 100`

缓存对 PQ 模式的加速尤为明显——频繁访问的节点向量和 PQ 码被缓存在内存中，大幅减少磁盘 I/O。

### 预取 (`--prefetch`)

启用后，在 Beam Search 遍历图时，后台线程会异步预取最可能被访问的下一批节点。与缓存协同工作时效果最佳。

- PQ 模式：预取命中率通常可达 40-60%（邻居距离用 ADC 表快速计算，仅访问节点时才读盘）
- 无 PQ 模式：预取可显著减少同步等待

### 图构建器 (`--builder`)

| 选项 | 适用场景 | 复杂度 |
|------|---------|--------|
| `brute` | ≤ 1 万向量，追求精确图 | O(N²·d) |
| `vamana` | > 1 万向量，可接受近似 | O(N·log N·d) |
| `auto` | 推荐，自动选择 | — |

Vamana 参数（通过代码配置）：
- `alpha`：剪枝系数，默认 1.2（越大图越稀疏）
- `beam_width`：构建时的束宽，默认 64

---

## 磁盘文件格式

### 段文件 (.vsg)

```
┌─────────────────────────────────┐
│  Header (4096 bytes)            │
│  magic: "VSG1"                  │
│  version, dim, count, degree,   │
│  entry, record_size, data_offset│
│  [pq_M, pq_K]  (V2 only)       │
├─────────────────────────────────┤
│  Data Records                   │
│  每个节点: [dim×float | degree×uint32]│
├─────────────────────────────────┤
│  PQ Section (V2 only)           │
│  pq_header: [M, K, d_sub]      │
│  centroids: [M×K×d_sub×float]  │
│  codes: [count×M×uint8]         │
└─────────────────────────────────┘
```

### 清单文件 (manifest.txt)

```
段文件路径|起始ID偏移|层级
data/segment_0.vsg|0|0
data/segment_L1_0.vsg|0|1
```

- Level 0：MemTable 直接刷盘的段（≤4 个时触发合并）
- Level 1+：合并后的段（每层 ≤2 个）

---

## 性能预期

在 siftsmall（1 万向量，128 维）上的实测结果：

| 配置 | Recall@10 | 缓存命中率 | 预取命中率 |
|------|-----------|-----------|-----------|
| PQ + 缓存 5MB | 1.0 | 77% | — |
| PQ + 缓存 + 预取 | 1.0 | 75% | 49% |
| PQ + 缓存 + 预取 + 4 线程 | 1.0 | 62% | 60% |

> 注：无 PQ 模式下搜索每个邻居都需读盘，I/O 开销极大，强烈建议配合 PQ 使用。

---

## 项目结构

```
src/
├── main.cpp                  # CLI 入口
├── core/
│   ├── types.h               # VectorId, SearchParams, SearchResult
│   ├── distance.h            # L2Squared 距离计算
│   └── topk.h                # 堆优化的 Top-K 收集器
├── io/
│   ├── io_backend.h          # 抽象 I/O 后端接口
│   ├── sync_io.h/cpp         # 同步 I/O 后端
│   ├── cached_io.h/cpp       # 缓存装饰器
│   ├── prefetch_scheduler.h/cpp  # 异步预取调度器
│   └── uring_io.h/cpp        # io_uring 后端（Linux only）
├── cache/
│   ├── cache_types.h         # 缓存数据结构
│   └── cache_pool.h/cpp      # 加权淘汰缓存池
├── dataset/
│   ├── fvecs.h/cpp           # SIFT 向量加载
│   └── ivecs.h/cpp           # SIFT 真值加载
├── pq/
│   ├── pq.h                  # PQ 配置/码本类型
│   ├── pq_trainer.h/cpp      # K-Means 码本训练
│   └── pq_codec.h/cpp        # 编码/解码/ADC 距离
├── storage/
│   ├── segment.h/cpp         # 磁盘段读写
│   ├── manifest.h/cpp        # 段清单管理
│   └── compaction.h/cpp      # LSM-Tree 分层合并
├── index/
│   ├── graph_builder.h/cpp   # KNN 图构建（精确+Vamana）
│   ├── vamana_builder.h/cpp  # Vamana/DiskANN 算法
│   └── graph_search.h/cpp    # Beam Search + 预取
├── mem/
│   └── memtable.h/cpp        # 内存写缓冲 + 搜索
└── util/
    ├── arg_parser.h/cpp      # CLI 参数解析
    └── thread_pool.h/cpp     # 线程池
```

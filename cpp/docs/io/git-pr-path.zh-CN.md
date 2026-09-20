# Pixels C++ Git/PR 路径

## 当前正式分支链

所有 C++ 功能分支均从 `origin/master` 的 `b99eef2b` 开始，按下列顺序
线性叠加。后一个分支包含前一个分支的全部提交。

```text
origin/master (b99eef2b)
  └─ feature/cpp-flatbuffers (9e0c2b4f)
      └─ feature/cpp-runtime-profiler (528721c4)
          └─ feature/cpp-buffer-pool (a22ce1cf)
              └─ feature/cpp-io-uring-backends (efd04bf4)
                  └─ feature/cpp-footer-cache (7432ee99)
                      └─ feature/cpp-parallel-scan (dd6e7dea)
                          └─ feature/cpp-scan-performance (a2a51b05)
                              └─ feature/cpp-spdk (5ec3cce1)
                                  └─ feature/cpp-parquet
```

`feature/cpp-parquet` 的最新提交可能随文档和兼容性修复前进；判断依赖关系
应使用分支名，不应把文档中的 tip commit 当成永久固定值。

## 各分支职责

| 顺序 | 分支 | 职责 |
|---:|---|---|
| 1 | `feature/cpp-flatbuffers` | C++ 元数据序列化迁移到 FlatBuffers |
| 2 | `feature/cpp-runtime-profiler` | 可运行时控制的性能统计基础设施 |
| 3 | `feature/cpp-buffer-pool` | 正式 BufferPool；包含原 `b4ab3ff2` 性能实验 |
| 4 | `feature/cpp-io-uring-backends` | fixed、non-fixed、dynamic io_uring 后端 |
| 5 | `feature/cpp-footer-cache` | Footer cache 生命周期和测试 |
| 6 | `feature/cpp-parallel-scan` | DuckDB 并行扫描主链 |
| 7 | `feature/cpp-scan-performance` | 并发测试、内存视图与 batch validity 优化 |
| 8 | `feature/cpp-spdk` | 可选 SPDK NVMe 后端、DMA ownership 和说明 |
| 9 | `feature/cpp-parquet` | 基于 SPDK 之后主链的 Parquet io_uring 扫描 |

## SPDK 与 Parquet 的关系

SPDK 和 Parquet 不再是兄弟分支。合并或创建 PR 时必须先处理 SPDK，随后
以 `feature/cpp-spdk` 为 base 处理 Parquet。Parquet 使用
`cpp/third-party/arrow` 中固定的 Apache Arrow 18.0.0，不查找系统
Arrow/Parquet。

## 已淘汰路径

`whzruc/rebuild/*` 和 `whzruc/wip/cpp-buffer-pool-performance-investigation`
已经由上述正式分支替代并从远端删除。历史文档中若仍出现这些名字，只
表示当时的整理过程，不应再作为 PR base 或测试入口。

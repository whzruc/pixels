#!/bin/bash


echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid
# 创建输出目录
mkdir -p output

# 遍历查询 q01 到 q22
for i in {1..22}; do
    QUERY="q$(printf "%02d" $i)"
    
    # 记录性能数据
    perf record -F 99 -g -- ./build/debug/benchmark/benchmark_runner "benchmark/tpch/parquet/tpch_1/${QUERY}.benchmark"
    
    # 生成 perf 脚本
    perf script > out.perf
    
    # 生成火焰图
    ./stackcollapse-perf.pl out.perf > out.folded
    ./flamegraph.pl out.folded > "output/${QUERY}_flamegraph.svg"
    
    # 清理临时文件
    rm out.perf out.folded
done

echo "All flame graphs have been generated in the output directory."


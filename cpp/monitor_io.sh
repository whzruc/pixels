#!/bin/bash

# 创建输出目录
mkdir -p output

# 初始化执行时间数组
declare -a execution_times

# 遍历查询 q01 到 q22
for i in {1..2}; do
    QUERY="q$(printf "%02d" $i)"
    PROGRAM="./build/release/benchmark/benchmark_runner \"benchmark/tpch/parquet/tpch_1000/${QUERY}.benchmark\" --profile"
    DEVICE="nvme2n1"
    OUTPUT_FILE="output/${QUERY}_output.csv"

    # 添加CSV标题
    echo "Device, rMB/s, wMB/s, %util" > "$OUTPUT_FILE"

    # 清除 page cache
    sudo bash -c "sync; echo 3 > /proc/sys/vm/drop_caches"

    # 启动 iostat 监控
    iostat -dx "$DEVICE" 1 >> "$OUTPUT_FILE" &

    # 获取 iostat 的进程ID
    IOSTAT_PID=$!

    # 执行目标程序并捕获输出
    OUTPUT=$(eval "$PROGRAM")
    

    # 从输出中提取执行时间
    if [[ $OUTPUT =~ Result:\ ([0-9.]+) ]]; then
        execution_time="${BASH_REMATCH[1]}"
        execution_times+=("$execution_time")
    else
        echo "Could not find execution time for ${QUERY}."
    fi

    # 等待程序完成
    wait

    # 结束 iostat 监控
    kill $IOSTAT_PID

    echo "iostat monitoring for ${QUERY} stopped and results saved to $OUTPUT_FILE."
done

# 将执行时间数组写入文件
echo "Execution Times:" > output/execution_times.txt
for time in "${execution_times[@]}"; do
    echo "$time" >> output/execution_times.txt
done

echo "Execution times saved to output/execution_times.txt."


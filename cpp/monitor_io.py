import os
import subprocess
import re

# 创建输出目录
output_dir = "output-parquet-1000"
# output_dir = "output-parquet-1"
os.makedirs(output_dir, exist_ok=True)

# 初始化执行时间数组
execution_times = []

# 遍历查询 q01 到 q22
for i in range(1, 23):
    query = f"q{str(i).zfill(2)}"
    program = f"./build/release/benchmark/benchmark_runner benchmark/tpch/parquet/tpch_1000/{query}.benchmark"
    device = "nvme2n1"
    output_file = os.path.join(output_dir, f"{query}_output.csv")
    tmp_file = "iostat_tmp.txt"

    # 添加CSV标题
    with open(output_file, 'w') as f:
        f.write("Device, rs/s, rMB/s, ws/s, MB/s, %util\n")

    # 清除 page cache
    subprocess.run(["sudo", "bash", "-c", "sync; echo 3 > /proc/sys/vm/drop_caches"])

    # 启动 iostat 监控，并将其输出重定向到临时文件
    iostat_process = subprocess.Popen(["iostat", "-dx", device, "1"], stdout=open(tmp_file, 'w'), text=True)

    try:
        # 执行目标程序并捕获输出
        output = subprocess.check_output(program, shell=True, text=True,stderr=subprocess.STDOUT)

        # print(type(output),output)
        match = re.search(r'Result:\s+([0-9]+\.[0-9]+)', output)
        if match:
            execution_time = match.group(1)
            execution_times.append(execution_time)
        else:
            print(f"Could not find execution time for {query}. Full output:\n{output}")

    finally:
        # 等待程序执行完毕后，终止 iostat 监控
        iostat_process.terminate()
        iostat_process.wait()

        # 从临时文件读取 iostat 输出并写入最终的 CSV 文件
        with open(tmp_file, 'r') as tmp:
            for line in tmp:
                # 解析 iostat 输出
                lines=line.split()
                # print(line.split())
                if(len(lines)>0 and lines[0]==device):
                    rs=float(lines[1])
                    rMB=float(lines[2])/1024
                    ws=float(lines[7])
                    wMB=float(lines[8])/1024
                    util=float(lines[-1])
                    # 写入 CSV
                    with open(output_file, 'a') as f:
                        f.write(f"{device}, {rs:.2f}, {rMB:.2f}, {ws:.2f}, {wMB:.2f}, {util:.2f}\n")

        # 删除临时文件
        if os.path.exists(tmp_file):
            os.remove(tmp_file)

    print(f"iostat monitoring for {query} stopped and results saved to {output_file}.")

# 将执行时间数组写入文件
with open(os.path.join(output_dir, "execution_times.txt"), 'w') as f:
    f.write("Execution Times:\n")
    print(execution_times)
    for time in execution_times:
        f.write(f"{time}\n")

print("Execution times saved to output/execution_times.txt.")



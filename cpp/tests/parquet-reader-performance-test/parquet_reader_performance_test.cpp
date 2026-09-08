/*
 * Copyright 2023 PixelsDB.
 *
 * This file is part of Pixels.
 *
 * Pixels is free software: you can redistribute it and/or modify
 * it under the terms of the Affero GNU General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * Pixels is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * Affero GNU General Public License for more details.
 *
 * You should have received a copy of the Affero GNU General Public
 * License along with Pixels.  If not, see
 * <https://www.gnu.org/licenses/>.
 */

/*
 * @author whz
 * @create 2026-03-11
 */

#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <glob.h>
#include <fstream>
#include <sstream>
#include <map>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>

#include <parquet/api/reader.h>
#include <parquet/arrow/reader.h>
#include <arrow/io/file.h>
#include <arrow/table.h>

using namespace std;

// Helper function to extract file number from filename
int extractFileNumber(const string& filepath) {
    size_t lastSlash = filepath.find_last_of('/');
    string filename = (lastSlash != string::npos) ? filepath.substr(lastSlash + 1) : filepath;
    
    size_t dotPos = filename.rfind('.');
    if (dotPos != string::npos) {
        filename = filename.substr(0, dotPos);
    }
    
    size_t underscorePos = filename.rfind('_');
    if (underscorePos != string::npos && underscorePos + 1 < filename.length()) {
        try {
            return stoi(filename.substr(underscorePos + 1));
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

// Comparator for sorting files by their number
bool compareFilesByNumber(const string& path1, const string& path2) {
    return extractFileNumber(path1) < extractFileNumber(path2);
}

// Global state for multi-threaded reading
struct ParquetReadGlobalState {
    mutex lock;  // Lock for work distribution and Arrow API protection
    vector<string> files;
    int file_index;
    long rowCount;
    long totalRowGroups;
    long totalBytesRead;
};

// Local state for each thread
struct ParquetReadLocalState {
    int file_index;
    string file_name;
    long rowCount;
    long rowGroups;
    long bytesRead;
};

// Configuration structure
struct TestConfig {
    vector<string> dataFiles;
    int threads;
    bool enableDirectIO;
    int rgStart;
    int rgCount;
    long queryId;
    bool verifyData;
};

ParquetReadGlobalState global;

// Parse configuration file
TestConfig parseConfig(const string& configPath) {
    TestConfig config;
    ifstream file(configPath);
    if (!file.is_open()) {
        cerr << "Error: Cannot open config file: " << configPath << endl;
        exit(1);
    }

    map<string, string> properties;
    string line;
    while (getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        
        size_t pos = line.find('=');
        if (pos != string::npos) {
            string key = line.substr(0, pos);
            string value = line.substr(pos + 1);
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            properties[key] = value;
        }
    }
    file.close();

    config.threads = stoi(properties["test.threads"]);
    config.enableDirectIO = (properties["localfs.enable.direct.io"] == "true");
    config.rgStart = stoi(properties["test.rg.start"]);
    config.rgCount = stoi(properties["test.rg.count"]);
    config.queryId = stol(properties["test.query.id"]);
    config.verifyData = (properties["test.verify.data"] == "true");

    // Parse data files
    string filesPattern = properties["test.data.files"];
    
    if (filesPattern.find(',') != string::npos) {
        stringstream ss(filesPattern);
        string pattern;
        while (getline(ss, pattern, ',')) {
            pattern.erase(0, pattern.find_first_not_of(" \t"));
            pattern.erase(pattern.find_last_not_of(" \t") + 1);
            
            glob_t glob_result;
            glob(pattern.c_str(), GLOB_TILDE, NULL, &glob_result);
            for (unsigned int i = 0; i < glob_result.gl_pathc; ++i) {
                config.dataFiles.push_back(string(glob_result.gl_pathv[i]));
            }
            globfree(&glob_result);
        }
    } else {
        glob_t glob_result;
        glob(filesPattern.c_str(), GLOB_TILDE, NULL, &glob_result);
        for (unsigned int i = 0; i < glob_result.gl_pathc; ++i) {
            config.dataFiles.push_back(string(glob_result.gl_pathv[i]));
        }
        globfree(&glob_result);
    }
    
    sort(config.dataFiles.begin(), config.dataFiles.end(), compareFilesByNumber);

    return config;
}

// Update local state with next file to process
bool updateLocalState(ParquetReadLocalState& local) {
    unique_lock<mutex> parallel_lock(global.lock);
    global.rowCount += local.rowCount;
    global.totalRowGroups += local.rowGroups;
    global.totalBytesRead += local.bytesRead;
    local.rowCount = 0;
    local.rowGroups = 0;
    local.bytesRead = 0;
    
    if (global.file_index >= global.files.size()) {
        return false;
    } else {
        local.file_index = global.file_index;
        local.file_name = global.files.at(local.file_index);
        global.file_index++;
        return true;
    }
}

// Read row groups using low-level pread for pure I/O performance
void readRowGroupsWithPread(const string& filepath, int rgStart, int rgCount, 
                             bool enableDirectIO, ParquetReadLocalState& local) {
    // Store metadata information we need
    struct RowGroupInfo {
        int64_t num_rows;
        vector<pair<int64_t, int64_t>> column_chunks; // offset, size
    };
    vector<RowGroupInfo> rgInfos;
    
    try {
        // Open file to get metadata - use a separate scope to ensure cleanup
        {
            // Lock to protect Arrow API calls (Arrow Parquet is not thread-safe)
            unique_lock<mutex> arrow_lock(global.lock);
            
            std::shared_ptr<arrow::io::ReadableFile> input;
            PARQUET_ASSIGN_OR_THROW(input, arrow::io::ReadableFile::Open(filepath));
            
            // Create parquet reader
            std::unique_ptr<parquet::ParquetFileReader> parquet_reader = 
                parquet::ParquetFileReader::Open(input);
            
            auto metadata = parquet_reader->metadata();
            int totalRowGroups = metadata->num_row_groups();
            
            // Determine row groups to read
            int actualRgCount = (rgCount == -1) ? totalRowGroups : rgCount;
            actualRgCount = min(actualRgCount, totalRowGroups - rgStart);
            
            if (actualRgCount <= 0) {
                return;
            }
            
            // Extract all metadata we need
            for (int rg = rgStart; rg < rgStart + actualRgCount; rg++) {
                auto row_group_metadata = metadata->RowGroup(rg);
                RowGroupInfo info;
                info.num_rows = row_group_metadata->num_rows();
                
                for (int col = 0; col < row_group_metadata->num_columns(); col++) {
                    auto column_chunk = row_group_metadata->ColumnChunk(col);
                    int64_t offset = column_chunk->data_page_offset();
                    int64_t size = column_chunk->total_compressed_size();
                    info.column_chunks.push_back({offset, size});
                }
                rgInfos.push_back(info);
            }
            
            // Close and cleanup - scope exit will destroy parquet_reader and input
            input->Close();
        }
        // arrow_lock released here, parquet_reader and input are now destroyed
        // Now safe to do I/O operations in parallel
        
        // Open file with pread
        int flags = O_RDONLY;
        if (enableDirectIO) {
            flags |= O_DIRECT;
        }
        
        int fd = open(filepath.c_str(), flags);
        if (fd < 0) {
            cerr << "Error opening file " << filepath << ": " << strerror(errno) << endl;
            return;
        }
        
        // Read each row group using stored metadata
        for (size_t rg_idx = 0; rg_idx < rgInfos.size(); rg_idx++) {
            const auto& rgInfo = rgInfos[rg_idx];
            
            // Read all columns in this row group
            for (size_t col = 0; col < rgInfo.column_chunks.size(); col++) {
                int64_t offset = rgInfo.column_chunks[col].first;
                int64_t size = rgInfo.column_chunks[col].second;
                
                // For Direct I/O, align offset and size
                int64_t aligned_offset = offset;
                int64_t aligned_size = size;
                
                if (enableDirectIO) {
                    const int alignment = 4096;
                    aligned_offset = (offset / alignment) * alignment;
                    int64_t end = offset + size;
                    int64_t aligned_end = ((end + alignment - 1) / alignment) * alignment;
                    aligned_size = aligned_end - aligned_offset;
                }
                
                // Allocate buffer
                void* buffer = nullptr;
                if (enableDirectIO) {
                    if (posix_memalign(&buffer, 4096, aligned_size) != 0) {
                        cerr << "Error allocating aligned buffer" << endl;
                        close(fd);
                        return;
                    }
                } else {
                    buffer = malloc(aligned_size);
                }
                
                if (buffer == nullptr) {
                    cerr << "Error allocating buffer" << endl;
                    close(fd);
                    return;
                }
                
                // Read data using pread
                ssize_t bytes_read = pread(fd, buffer, aligned_size, aligned_offset);
                
                if (bytes_read < 0) {
                    cerr << "Error reading from file: " << strerror(errno) << endl;
                    free(buffer);
                    close(fd);
                    return;
                }
                
                local.bytesRead += bytes_read;
                free(buffer);
            }
            
            local.rowGroups++;
            local.rowCount += rgInfo.num_rows;
        }
        
        close(fd);
        
    } catch (const exception& e) {
        cerr << "Error reading file " << filepath << ": " << e.what() << endl;
    }
}

// Scan implementation for each thread
void scanImplementation(const TestConfig& config) {
    ParquetReadLocalState local;
    local.rowCount = 0;
    local.rowGroups = 0;
    local.bytesRead = 0;
    
    while (true) {
        if (!updateLocalState(local)) {
            break;
        }
        
        readRowGroupsWithPread(local.file_name, config.rgStart, config.rgCount, 
                               config.enableDirectIO, local);
    }
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <config_file>" << endl;
        return 1;
    }

    string configPath = argv[1];
    cout << "Loading configuration from: " << configPath << endl;

    // Parse configuration
    TestConfig config = parseConfig(configPath);
    
    cout << "Configuration loaded:" << endl;
    cout << "  Files to read: " << config.dataFiles.size() << endl;
    cout << "  Threads: " << config.threads << endl;
    cout << "  Direct I/O: " << (config.enableDirectIO ? "enabled" : "disabled") << endl;
    cout << "  Row group range: [" << config.rgStart << ", ";
    if (config.rgCount == -1) {
        cout << "all)" << endl;
    } else {
        cout << config.rgStart + config.rgCount << ")" << endl;
    }
    cout << "  Data verification: " << (config.verifyData ? "enabled" : "disabled") << endl;
    cout << endl;

    // Initialize global state
    global.files = config.dataFiles;
    global.file_index = 0;
    global.rowCount = 0;
    global.totalRowGroups = 0;
    global.totalBytesRead = 0;

    // Determine number of threads
    int numThreads = config.threads;
    if (numThreads == -1) {
        numThreads = thread::hardware_concurrency();
    }
    
    cout << "Starting Parquet reader performance test with " << numThreads << " threads..." << endl;
    
    // Start timing
    auto startTime = chrono::steady_clock::now();
    
    // Create and start threads
    vector<thread> threads;
    for (int i = 0; i < numThreads; i++) {
        threads.emplace_back(scanImplementation, config);
    }
    
    // Wait for all threads to complete
    for (auto& t : threads) {
        t.join();
    }
    
    // End timing
    auto endTime = chrono::steady_clock::now();
    auto duration = chrono::duration_cast<chrono::milliseconds>(endTime - startTime).count();
    
    // Print results
    cout << endl;
    cout << "========================================" << endl;
    cout << "Parquet Reader Performance Test Results" << endl;
    cout << "========================================" << endl;
    cout << "Total files read: " << config.dataFiles.size() << endl;
    cout << "Total row groups read: " << global.totalRowGroups << endl;
    cout << "Total rows read: " << global.rowCount << endl;
    cout << "Total bytes read: " << (global.totalBytesRead / 1024.0 / 1024.0) << " MB" << endl;
    cout << "Elapsed time: " << duration << " ms" << endl;
    cout << "Throughput: " << (global.totalBytesRead / 1024.0 / 1024.0 / (duration / 1000.0)) << " MB/s" << endl;
    cout << "Row throughput: " << (global.rowCount * 1000.0 / duration) << " rows/sec" << endl;
    cout << "========================================" << endl;
    
    return 0;
}
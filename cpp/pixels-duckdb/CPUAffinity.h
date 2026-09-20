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

#pragma once

#ifndef PIXELS_CPU_AFFINITY_H
#define PIXELS_CPU_AFFINITY_H

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <iostream>
#include <vector>

namespace pixels {

class CPUAffinity {
public:
    /**
     * Bind the current thread to a specific CPU core
     * @param coreId The CPU core ID to bind to
     * @return true if successful, false otherwise
     */
    static bool BindToCore(int coreId) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(coreId, &cpuset);
        
        pthread_t currentThread = pthread_self();
        int result = pthread_setaffinity_np(currentThread, sizeof(cpu_set_t), &cpuset);
        
        if (result != 0) {
            std::cerr << "Failed to bind thread to CPU core " << coreId 
                      << ", error code: " << result << std::endl;
            return false;
        }
        
        return true;
    }
    
    /**
     * Bind the current thread to multiple CPU cores
     * @param coreIds Vector of CPU core IDs to bind to
     * @return true if successful, false otherwise
     */
    static bool BindToCores(const std::vector<int>& coreIds) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        
        for (int coreId : coreIds) {
            CPU_SET(coreId, &cpuset);
        }
        
        pthread_t currentThread = pthread_self();
        int result = pthread_setaffinity_np(currentThread, sizeof(cpu_set_t), &cpuset);
        
        if (result != 0) {
            std::cerr << "Failed to bind thread to CPU cores, error code: " 
                      << result << std::endl;
            return false;
        }
        
        return true;
    }
    
    /**
     * Get the number of available CPU cores
     * @return Number of CPU cores
     */
    static int GetNumCores() {
        return sysconf(_SC_NPROCESSORS_ONLN);
    }
    
    /**
     * Get the current CPU affinity of the thread
     * @return Vector of CPU core IDs the thread is bound to
     */
    static std::vector<int> GetCurrentAffinity() {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        
        pthread_t currentThread = pthread_self();
        int result = pthread_getaffinity_np(currentThread, sizeof(cpu_set_t), &cpuset);
        
        std::vector<int> coreIds;
        if (result == 0) {
            int numCores = GetNumCores();
            for (int i = 0; i < numCores; i++) {
                if (CPU_ISSET(i, &cpuset)) {
                    coreIds.push_back(i);
                }
            }
        }
        
        return coreIds;
    }
    
    /**
     * Clear CPU affinity (allow thread to run on any core)
     * @return true if successful, false otherwise
     */
    static bool ClearAffinity() {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        
        int numCores = GetNumCores();
        for (int i = 0; i < numCores; i++) {
            CPU_SET(i, &cpuset);
        }
        
        pthread_t currentThread = pthread_self();
        int result = pthread_setaffinity_np(currentThread, sizeof(cpu_set_t), &cpuset);
        
        return result == 0;
    }
};

} // namespace pixels

#endif // PIXELS_CPU_AFFINITY_H
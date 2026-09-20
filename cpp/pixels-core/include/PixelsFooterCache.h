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
 * @author liyu
 * @create 2023-03-14
 */
#ifndef PIXELS_PIXELSFOOTERCACHE_H
#define PIXELS_PIXELSFOOTERCACHE_H

#include <iostream>
#include <string>
#include <memory>
#include <shared_mutex>
#include <utility>
#include "pixels_generated.h"
#include <unordered_map>
#include "physical/natives/ByteBuffer.h"

// Each FileTail entry owns its backing ByteBuffer so the FlatBuffer pointer
// remains valid as long as the cache entry lives, regardless of reader lifetime.
struct FileTailEntry {
    std::shared_ptr<ByteBuffer> buffer;
    const pixels::fb::FileTail* fileTail;
};

// Same ownership model for row group footers.
struct RGFooterEntry {
    std::shared_ptr<ByteBuffer> buffer;
    const pixels::fb::RowGroupFooter* rgFooter;
};

typedef std::unordered_map<std::string, FileTailEntry> FileTailTable;
typedef std::unordered_map<std::string, RGFooterEntry> RGFooterTable;

class PixelsFooterCache
{
public:
    PixelsFooterCache();

    // Takes ownership of buffer to extend its lifetime beyond the reader.
    void putFileTail(const std::string &id, std::shared_ptr<ByteBuffer> buffer,
                     const pixels::fb::FileTail* fileTail);

    // Insert only if the key is absent (put-if-absent). Returns the cached
    // pointer: the newly inserted one, or the pre-existing one if another
    // thread raced and inserted first. Eliminates TOCTOU between contains+put.
    const pixels::fb::FileTail* putFileTailIfAbsent(const std::string &id,
                                                     std::shared_ptr<ByteBuffer> buffer,
                                                     const pixels::fb::FileTail* fileTail);

    bool containsFileTail(const std::string &id);

    const pixels::fb::FileTail* getFileTail(const std::string &id);


    // Takes ownership of buffer to keep the FlatBuffer pointer valid in the cache.
    void putRGFooter(const std::string &id, std::shared_ptr<ByteBuffer> buffer,
                     const pixels::fb::RowGroupFooter* footer);

    bool containsRGFooter(const std::string &id);

    const pixels::fb::RowGroupFooter* getRGFooter(const std::string &id);


private:
    mutable std::shared_mutex mutex_;
    FileTailTable fileTailCacheMap;
    RGFooterTable rowGroupFooterCacheMap;

};
#endif //PIXELS_PIXELSFOOTERCACHE_H

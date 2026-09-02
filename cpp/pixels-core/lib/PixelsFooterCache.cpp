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
#include "PixelsFooterCache.h"
#include "exception/InvalidArgumentException.h"
#include <mutex>

PixelsFooterCache::PixelsFooterCache()
{
}

const pixels::fb::FileTail *PixelsFooterCache::putFileTailIfAbsent(
        const std::string &id, std::shared_ptr<ByteBuffer> buffer,
        const pixels::fb::FileTail *fileTail)
{
    std::unique_lock lock(mutex_);
    auto result = fileTailCacheMap.emplace(id, FileTailEntry{std::move(buffer), fileTail});
    return result.first->second.fileTail;
}

const pixels::fb::FileTail* PixelsFooterCache::getFileTail(const std::string &id)
{
    std::shared_lock lock(mutex_);
    auto it = fileTailCacheMap.find(id);
    if (it != fileTailCacheMap.end())
    {
        return it->second.fileTail;
    }
    else
    {
        throw InvalidArgumentException("No such a FileTail id.");
    }
}

const pixels::fb::RowGroupFooter *PixelsFooterCache::putRGFooterIfAbsent(
        const std::string &id, std::shared_ptr<ByteBuffer> buffer,
        const pixels::fb::RowGroupFooter *footer)
{
    std::unique_lock lock(mutex_);
    auto result = rowGroupFooterCacheMap.emplace(id, RGFooterEntry{std::move(buffer), footer});
    return result.first->second.footer;
}

bool PixelsFooterCache::containsFileTail(const std::string &id)
{
    std::shared_lock lock(mutex_);
    return fileTailCacheMap.find(id) != fileTailCacheMap.end();
}

const pixels::fb::RowGroupFooter* PixelsFooterCache::getRGFooter(const std::string &id)
{
    std::shared_lock lock(mutex_);
    auto it = rowGroupFooterCacheMap.find(id);
    if (it != rowGroupFooterCacheMap.end())
    {
        return it->second.footer;
    }
    else
    {
        throw InvalidArgumentException("No such a RGFooter id.");
    }
}

bool PixelsFooterCache::containsRGFooter(const std::string &id)
{
    std::shared_lock lock(mutex_);
    return rowGroupFooterCacheMap.find(id) != rowGroupFooterCacheMap.end();
}

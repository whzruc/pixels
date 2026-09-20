/*
 * Copyright 2024 PixelsDB.
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
 * @create 2026-02-09
 */
#include "utils/ColumnSizeCSVWriter.h"
#include <algorithm>

ColumnSizeCSVWriter::ColumnSizeCSVWriter(const std::string& csvPath)
    : csvPath(csvPath), written(false)
{
}

ColumnSizeCSVWriter::~ColumnSizeCSVWriter()
{
    // Auto-write if not already written
    if (!written && !colSize.empty())
    {
        write();
    }
}

void ColumnSizeCSVWriter::set(const std::string& columnName, uint64_t maxSize)
{
    colSize[columnName] = maxSize;
}

void ColumnSizeCSVWriter::setAll(const std::unordered_map<std::string, uint64_t>& columnSizes)
{
    for (const auto& pair : columnSizes)
    {
        colSize[pair.first] = pair.second;
    }
}

bool ColumnSizeCSVWriter::write()
{
    std::ofstream file;
    file.open(csvPath);
    
    if (!file.is_open())
    {
        std::cerr << "Failed to open file for writing: " << csvPath << std::endl;
        return false;
    }
    
    // Sort columns by name for consistent output
    std::vector<std::pair<std::string, uint64_t>> sortedColumns(colSize.begin(), colSize.end());
    std::sort(sortedColumns.begin(), sortedColumns.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    
    // Write each column size entry
    // Format: <columnName> <maxSize>
    for (const auto& pair : sortedColumns)
    {
        file << pair.first << " " << pair.second << std::endl;
    }
    
    file.close();
    written = true;
    
    return true;
}

bool ColumnSizeCSVWriter::has(const std::string& columnName) const
{
    return colSize.find(columnName) != colSize.end();
}

void ColumnSizeCSVWriter::clear()
{
    colSize.clear();
    written = false;
}

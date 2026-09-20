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
#ifndef PIXELS_COLUMNSIZECSVWRITER_H
#define PIXELS_COLUMNSIZECSVWRITER_H

#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "exception/InvalidArgumentException.h"

/**
 * ColumnSizeCSVWriter
 * Writes column size information to a CSV file
 * Format: <columnName> <maxSize>
 */
class ColumnSizeCSVWriter
{
public:
    /**
     * Constructor
     * @param csvPath Path to the CSV file to write
     */
    ColumnSizeCSVWriter(const std::string& csvPath);
    
    /**
     * Destructor - ensures file is properly closed
     */
    ~ColumnSizeCSVWriter();
    
    /**
     * Add or update a column size entry
     * @param columnName Name of the column
     * @param maxSize Maximum size for the column
     */
    void set(const std::string& columnName, uint64_t maxSize);
    
    /**
     * Add multiple column sizes at once
     * @param columnSizes Map of column names to their sizes
     */
    void setAll(const std::unordered_map<std::string, uint64_t>& columnSizes);
    
    /**
     * Write all column sizes to the CSV file
     * @return true if successful, false otherwise
     */
    bool write();
    
    /**
     * Get the number of columns stored
     */
    size_t size() const { return colSize.size(); }
    
    /**
     * Check if a column exists
     */
    bool has(const std::string& columnName) const;
    
    /**
     * Clear all stored column sizes
     */
    void clear();

private:
    std::string csvPath;
    std::unordered_map<std::string, uint64_t> colSize;
    bool written;
};

#endif //PIXELS_COLUMNSIZECSVWRITER_H

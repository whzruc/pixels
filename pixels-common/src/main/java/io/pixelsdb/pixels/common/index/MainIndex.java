/*
 * Copyright 2025 PixelsDB.
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
package io.pixelsdb.pixels.common.index;

import io.pixelsdb.pixels.common.exception.RowIdException;
import io.pixelsdb.pixels.index.IndexProto;

import java.io.Closeable;
import java.io.IOException;

/**
 * The main index of a table is the mapping from row id to data file.
 * Each version of each row has a unique row id.
 * The row id of each table is an unsigned int_64 increases from zero.
 *
 * @author hank
 * @create 2025-02-10
 */
public interface MainIndex extends Closeable
{
    /**
     * Get the tableId of this mainIndex
     * @return the tableId
     */
    long getTableId();

    /**
     * Allocate rowId batch for single point index
     * @param tableId the table id of single point index
     * @param numRowIds the rowId nums need to allocate
     * @return the RowIdBatch
     */
    IndexProto.RowIdBatch allocateRowIdBatch(long tableId, int numRowIds) throws RowIdException;

    /**
     * Get the physical location of a row given the row id
     * @param rowId the row id
     * @return the row location
     */
    IndexProto.RowLocation getLocation(long rowId);

    /**
     * Put a single row id into the main index, in order to enable simultaneous insert into the main index while
     * inserting a single point index.
     * @param rowId the row id
     * @param rowLocation the location of the row id
     * @return true on success
     */
    boolean putRowId(long rowId, IndexProto.RowLocation rowLocation);

    /**
     * Delete a single row id from the main index. {@link #getLocation(long)} of a row id within a deleted range
     * should return null.
     * @param rowId the row id range to be deleted
     * @return true on success
     */
    boolean deleteRowId(long rowId);

    /**
     * Put a range of row ids into the main index. In pixels, we may allocate the row ids in batches (ranges)
     * and put the row id range into the main index at once.
     * @param rowIdRange the row id range
     * @param rgLocation the location
     * @return true on success
     */
    boolean putRowIds(RowIdRange rowIdRange, RgLocation rgLocation);

    /**
     * Delete a range of row ids from the main index. {@link #getLocation(long)} of a row id within a deleted range
     * should return null.
     * @param rowIdRange the row id range to be deleted
     * @return true on success
     */
    boolean deleteRowIds(RowIdRange rowIdRange);

    /**
     * Persist the main index into persistent storage.
     * @return true on success
     */
    boolean persist();

    /**
     * Persist the main index and close it.
     * @throws IOException
     */
    @Override
    void close() throws IOException;

    /**
     * The location of the row group, composed of the file id and the row group id inside the file.
     */
    class RgLocation
    {
        /**
         * The file id of the file in pixels metadata, starts from 1.
         */
        private final long fileId;
        /**
         * The row group id inside the file, starts from 0.
         */
        private final int rowGroupId;

        public RgLocation(long fileId, int rowGroupId)
        {
            this.fileId = fileId;
            this.rowGroupId = rowGroupId;
        }

        public long getFileId()
        {
            return fileId;
        }

        public int getRowGroupId()
        {
            return rowGroupId;
        }
    }
}

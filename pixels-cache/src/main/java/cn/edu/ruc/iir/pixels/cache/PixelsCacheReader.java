package cn.edu.ruc.iir.pixels.cache;

import cn.edu.ruc.iir.pixels.cache.mq.MappedBusWriter;

import java.io.EOFException;
import java.io.IOException;

import static com.google.common.base.Preconditions.checkArgument;

/**
 * pixels cache reader.
 *
 * @author guodong
 */
public class PixelsCacheReader implements AutoCloseable
{
    private final MemoryMappedFile cacheFile;
    private final MemoryMappedFile indexFile;
    private final MappedBusWriter mqWriter;
    private final long childrenOffsetMask = 0x00FFFFFFFFFFFFFFL;
    private int version = 1;
    private PixelsRadix cachedRadix;

    private PixelsCacheReader(MemoryMappedFile cacheFile, MemoryMappedFile indexFile, MappedBusWriter mqWriter)
    {
        this.cacheFile = cacheFile;
        this.indexFile = indexFile;
        this.mqWriter = mqWriter;
    }

    public static class Builder
    {
        private String builderCacheLocation = "";
        private long builderCacheSize;
        private String builderIndexLocation = "";
        private long builderIndexSize;
        private String builderMQLocation = "";
        private long builderMQFileSize;
        private int builderMQRecordSize;
        private boolean builderMQAppend;

        private Builder()
        {}

        public PixelsCacheReader.Builder setCacheLocation(String cacheLocation)
        {
            checkArgument(!cacheLocation.isEmpty(), "location should not be empty");
            this.builderCacheLocation = cacheLocation;

            return this;
        }

        public PixelsCacheReader.Builder setCacheSize(long cacheSize)
        {
            checkArgument(cacheSize > 0, "size should be positive");
            this.builderCacheSize = cacheSize;

            return this;
        }

        public PixelsCacheReader.Builder setIndexLocation(String location)
        {
            checkArgument(!location.isEmpty(), "index location should not be empty");
            this.builderIndexLocation = location;

            return this;
        }

        public PixelsCacheReader.Builder setIndexSize(long size)
        {
            checkArgument(size > 0, "index size should be positive");
            this.builderIndexSize = size;

            return this;
        }

        public PixelsCacheReader.Builder setMQLocation(String mqLocation)
        {
            checkArgument(!mqLocation.isEmpty(), "location should not be empty");
            this.builderMQLocation = mqLocation;

            return this;
        }

        public PixelsCacheReader.Builder setMQFileSize(long mqFileSize)
        {
            checkArgument(mqFileSize > 0, "message queue file size should be positive");
            this.builderMQFileSize = mqFileSize;

            return this;
        }

        public PixelsCacheReader.Builder setMQRecordSize(int mqRecordSize)
        {
            checkArgument(mqRecordSize > 0, "message queue record size should be positive");
            this.builderMQRecordSize = mqRecordSize;

            return this;
        }

        public PixelsCacheReader.Builder setMQAppend(boolean mqAppend)
        {
            this.builderMQAppend = mqAppend;

            return this;
        }

        public PixelsCacheReader build() throws Exception
        {
            MappedBusWriter mqWriter = new MappedBusWriter(builderMQLocation, builderMQFileSize,
                    builderMQRecordSize, builderMQAppend);
            mqWriter.open();
            MemoryMappedFile cacheFile = new MemoryMappedFile(builderCacheLocation, builderCacheSize);
            MemoryMappedFile indexFile = new MemoryMappedFile(builderIndexLocation, builderIndexSize);

            return new PixelsCacheReader(cacheFile, indexFile, mqWriter);
        }
    }

    public static PixelsCacheReader.Builder newBuilder()
    {
        return new PixelsCacheReader.Builder();
    }

    /**
     * Read specified columnlet from cache.
     * If cache is not hit, empty byte array is returned, and an access message is sent to the mq.
     * If cache is hit, columnlet content is returned as byte array.
     * @param blockId block id
     * @param rowGroupId row group id
     * @param columnId column id
     * @return columnlet content
     * */
    public byte[] get(long blockId, short rowGroupId, short columnId) throws EOFException
    {
        byte[] content = new byte[0];
        // check rw flag
        short rwFlag = indexFile.getShortVolatile(0);
        if (rwFlag != 0) {
            return content;
        }

        // check if reader count reaches its max value (short max value)
        int readerCount = indexFile.getShortVolatile(2);
        if (readerCount >= Short.MAX_VALUE) {
            return content;
        }
        // update reader count
        readerCount = readerCount + 1;
        indexFile.putShortVolatile(2, (short) readerCount);

        // search index file for columnlet id
        PixelsCacheKey cacheKey = new PixelsCacheKey(blockId, rowGroupId, columnId);
        byte[] cacheKeyBytes = cacheKey.getBytes();

        // search cache key
        PixelsCacheIdx cacheIdx = search(cacheKeyBytes);
        // if found, read content from cache
        if (cacheIdx != null) {
            long offset = cacheIdx.getOffset();
            int length = cacheIdx.getLength();
            content = new byte[length];
            cacheFile.getBytes(offset, content, 0, length);
        }
        // if not found, send cache miss message
        else {
            mqWriter.write(cacheKeyBytes, 0, 16);
        }

        // decrease reader count
        readerCount = indexFile.getShortVolatile(2);
        if (readerCount >= 1) {
            readerCount--;
        }
        indexFile.putShortVolatile(2, (short) readerCount);

        return content;
    }

    private void updateIndex()
    {
    }

    /**
     * Search key from radix tree.
     * If found, update counter in cache idx.
     * Else, return null
     * */
    private PixelsCacheIdx search(byte[] key)
    {
        long nodeOffset = 0;
        final int keyLen = key.length;
        int bytesMatched = 0;
        int childrenNum = 0;
        int edgeSize = 0;
        byte[] nodeHeader = new byte[2];
        while (bytesMatched < keyLen) {
            boolean matched = false;
            nodeHeader = new byte[2];
            indexFile.getBytes(nodeOffset, nodeHeader, 0, 2);
            // get children num, if 0, return empty
            childrenNum = nodeHeader[1] + 128;
            if (childrenNum == 0) {
                return null;
            }
            for (int i = 0; i < childrenNum; i++) {
                byte childLead = indexFile.getByte(nodeOffset + 2 + i * 8);
                if (childLead == key[bytesMatched]) {
                    nodeOffset = indexFile.getLong(nodeOffset + 2 + i * 8);
                    nodeOffset = nodeOffset & childrenOffsetMask;
                    bytesMatched++;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                return null;
            }
        }
        // found matching key, check if it has value
        if ((nodeHeader[0] >> 7 & 0x01) == 1) {
            // if it has value, get idx and increment counter
            long valueOffset = nodeOffset + 2 + childrenNum + edgeSize;
            long offset = indexFile.getLong(valueOffset);
            long timestamp = indexFile.getLong(valueOffset + 8);
            int length = indexFile.getInt(valueOffset + 16);
            int counter = indexFile.getInt(valueOffset + 20);
            indexFile.putInt(valueOffset + 20, counter+1);
            return new PixelsCacheIdx(offset, timestamp, length, counter);
        }
        return null;
    }

    public void close() throws IOException
    {
        mqWriter.close();
    }
}

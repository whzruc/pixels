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
package io.pixelsdb.pixels.storage.stream.io;

import io.netty.handler.codec.http.HttpHeaderNames;
import io.netty.handler.codec.http.HttpHeaderValues;
import io.pixelsdb.pixels.common.utils.Constants;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;
import org.asynchttpclient.*;

import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.concurrent.*;

public class StreamOutputStream extends OutputStream
{
    private static final Logger logger = LogManager.getLogger(StreamInputStream.class);

    /**
     * indicates whether the stream is still open / valid
     */
    private boolean open;

    /**
     * The schema of http stream.
     * Default value is http.
     */
    private final String schema = "http";

    /**
     * The host of http stream.
     */
    private String host;

    /**
     * The port of http stream.
     */
    private int port;

    /**
     * The uri of http stream.
     */
    private String uri;

    /**
     * The maximum retry count.
     */
    private static final int MAX_RETRIES = Constants.MAX_STREAM_RETRY_COUNT;

    /**
     * The delay between two tries.
     */
    private static final long RETRY_DELAY_MS = Constants.STREAM_DELAY_MS;

    /**
     * The temporary buffer used for storing the chunks.
     */
    private final byte[] buffer;

    /**
     * The position in the buffer.
     */
    private int bufferPosition;

    /**
     * The capacity of buffer.
     */
    private int bufferCapacity;

    /**
     * The background thread to send requests.
     */
    private final ExecutorService executorService;

    /**
     * The http client.
     */
    private final AsyncHttpClient httpClient;

    /**
     * The queue to put pending requests.
     */
    private final BlockingQueue<byte[]> contentQueue;

    public StreamOutputStream(String host, int port, int bufferCapacity) {
        this.open = true;
        this.host = host;
        this.port = port;
        this.uri = this.schema + "://" + host + ":" + port;
        this.bufferCapacity = bufferCapacity;
        this.buffer = new byte[bufferCapacity];
        this.bufferPosition = 0;
        this.httpClient = Dsl.asyncHttpClient();
        this.executorService = Executors.newSingleThreadExecutor();
        this.contentQueue = new LinkedBlockingQueue<>();

        // Start background thread to send requests.
        this.executorService.submit(() -> {
            while (true)
            {
                try
                {
                    byte[] content = contentQueue.take();
                    if (content.length == 0)
                    {
                        closeStreamReader();
                        break;
                    }
                    sendContentWithRetry(content);
                } catch (InterruptedException e)
                {
                    logger.error("Background thread interrupted", e);
                    break;
                }
            }
            try
            {
                this.httpClient.close();
            } catch (IOException e)
            {
                throw new RuntimeException(e);
            }
        });
    }

    /**
     * Write an array to the S3 output stream
     *
     * @param b
     * @throws IOException
     */
    @Override
    public void write(byte[] b) throws IOException
    {
        write(b, 0, b.length);
    }

    @Override
    public void write(final byte[] buf, final int off, final int len) throws IOException
    {
        this.assertOpen();
        int offsetInBuf = off, remainToRead = len;
        int remainInBuffer;
        while (remainToRead > (remainInBuffer = this.buffer.length - bufferPosition))
        {
            System.arraycopy(buf, offsetInBuf, this.buffer, this.bufferPosition, remainInBuffer);
            this.bufferPosition += remainInBuffer;
            flushBufferAndRewind();
            offsetInBuf += remainInBuffer;
            remainToRead -= remainInBuffer;
        }
        System.arraycopy(buf, offsetInBuf, this.buffer, this.bufferPosition, remainToRead);
        this.bufferPosition += remainToRead;
    }

    @Override
    public void write(int b) throws IOException
    {
        this.assertOpen();
        if (this.bufferPosition >= this.buffer.length)
        {
            flushBufferAndRewind();
        }
        this.buffer[this.bufferPosition++] = (byte) b;
    }

    @Override
    public synchronized void flush()
    {
        assertOpen();
        if (this.bufferPosition > 0)
        {
            this.contentQueue.add(Arrays.copyOfRange(this.buffer, 0, this.bufferPosition));
            this.bufferPosition = 0;
        }
    }

    protected void flushBufferAndRewind() throws IOException
    {
        logger.debug("Sending {} bytes to stream", this.bufferPosition);
        byte[] content = Arrays.copyOfRange(this.buffer, 0, this.bufferPosition);
        this.bufferPosition = 0;
        this.contentQueue.add(content);
    }

    @Override
    public void close() throws IOException
    {
        if (this.open)
        {
            this.open = false;
            if (this.bufferPosition > 0)
            {
                flush();
            }
            this.contentQueue.add(new byte[0]);
            this.executorService.shutdown();
            try
            {
                if (!this.executorService.awaitTermination(60, TimeUnit.SECONDS))
                {
                    this.executorService.shutdownNow();
                }
            } catch (InterruptedException e)
            {
                throw new IOException("Interrupted while waiting for termination", e);
            }
        }
    }

    private void sendContentWithRetry(byte[] content)
    {
        int retry = 0;
        while (retry <= MAX_RETRIES)
        {
            try
            {
                Request req = httpClient.preparePost(this.uri)
                        .setBody(ByteBuffer.wrap(content))
                        .addHeader(HttpHeaderNames.CONTENT_TYPE, "application/x-protobuf")
                        .addHeader(HttpHeaderNames.CONTENT_LENGTH, String.valueOf(content.length))
                        .addHeader(HttpHeaderNames.CONNECTION, "keep-alive")
                        .build();
                httpClient.executeRequest(req).get();
                break;
            } catch (Exception e)
            {
                retry++;
                if (retry > MAX_RETRIES ||
                        !(e.getCause() instanceof java.net.ConnectException || e.getCause() instanceof java.io.IOException))
                {
                    logger.error("Failed to send content after {} retries, exception: {}", retry, e.getMessage());
                    break;
                }
                try
                {
                    Thread.sleep(RETRY_DELAY_MS);
                } catch (InterruptedException e1)
                {
                    logger.error("Retry interrupted", e1);
                    break;
                }
            }
        }
    }

    /**
     * Tell stream reader that this stream closes.
     */
    private void closeStreamReader()
    {
        Request req = httpClient.preparePost(this.uri)
                .addHeader(HttpHeaderNames.CONTENT_TYPE, "application/x-protobuf")
                .addHeader(HttpHeaderNames.CONTENT_LENGTH, "0")
                .addHeader(HttpHeaderNames.CONNECTION, HttpHeaderValues.CLOSE)
                .build();
        int retry = 0;
        while (retry <= MAX_RETRIES)
        {
            try
            {
                httpClient.executeRequest(req).get();
                break;
            } catch (Exception e)
            {
                retry++;
                if (retry > MAX_RETRIES || !(e.getCause() instanceof java.net.ConnectException))
                {
                    logger.error("Failed to close stream reader after {} retries, exception: {}", retry, e.getMessage());
                    break;
                }
                try
                {
                    Thread.sleep(RETRY_DELAY_MS);
                } catch (InterruptedException e1)
                {
                    logger.error("Retry interrupted", e1);
                    break;
                }
            }
        }
    }

    private void assertOpen()
    {
        if (!this.open)
        {
            throw new IllegalStateException("Closed");
        }
    }
}

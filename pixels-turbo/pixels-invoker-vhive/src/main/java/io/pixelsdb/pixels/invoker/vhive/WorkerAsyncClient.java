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
package io.pixelsdb.pixels.invoker.vhive;

import com.alibaba.fastjson.JSON;
import com.alibaba.fastjson.serializer.SerializerFeature;
import com.google.common.util.concurrent.ListenableFuture;
import io.grpc.ConnectivityState;
import io.grpc.ManagedChannel;
import io.grpc.ManagedChannelBuilder;
import io.pixelsdb.pixels.common.turbo.WorkerType;
import io.pixelsdb.pixels.planner.plan.physical.input.*;
import io.pixelsdb.pixels.turbo.TurboProto;
import io.pixelsdb.pixels.turbo.vHiveWorkerServiceGrpc;

import java.util.concurrent.TimeUnit;

import static com.google.common.base.Preconditions.checkArgument;

public class WorkerAsyncClient
{
    private final ManagedChannel channel;
    private final vHiveWorkerServiceGrpc.vHiveWorkerServiceFutureStub stub;

    public WorkerAsyncClient(String host, int port)
    {
        checkArgument(host != null, "illegal rpc host");
        checkArgument(port > 0 && port <= 65535, "illegal rpc port");

        this.channel = ManagedChannelBuilder.forAddress(host, port)
                .usePlaintext().build();
        this.stub = vHiveWorkerServiceGrpc.newFutureStub(channel);
    }

    public void shutdown() throws InterruptedException
    {
        this.channel.shutdown().awaitTermination(5, TimeUnit.SECONDS);
    }

    public ConnectivityState getState() throws InterruptedException
    {
        return this.channel.getState(true);
    }

    public ListenableFuture<TurboProto.vHiveGetMemoryResponse> getMemory()
    {
        TurboProto.vHiveGetMemoryRequest request = TurboProto.vHiveGetMemoryRequest.newBuilder()
                .build();
        return this.stub.getMemory(request);
    }

    public ListenableFuture<TurboProto.vHiveWorkerResponse> aggregation(AggregationInput input)
    {
        TurboProto.vHiveWorkerRequest request = TurboProto.vHiveWorkerRequest.newBuilder()
                .setWorkerType(String.valueOf(WorkerType.AGGREGATION))
                .setJson(JSON.toJSONString(input, SerializerFeature.DisableCircularReferenceDetect))
                .build();
        return this.stub.process(request);
    }

    public ListenableFuture<TurboProto.vHiveWorkerResponse> broadcastChainJoin(BroadcastChainJoinInput input)
    {
        TurboProto.vHiveWorkerRequest request = TurboProto.vHiveWorkerRequest.newBuilder()
                .setWorkerType(String.valueOf(WorkerType.BROADCAST_CHAIN_JOIN))
                .setJson(JSON.toJSONString(input, SerializerFeature.DisableCircularReferenceDetect))
                .build();
        return this.stub.process(request);
    }

    public ListenableFuture<TurboProto.vHiveWorkerResponse> broadcastChainJoinStream(BroadcastChainJoinInput input)
    {
        TurboProto.vHiveWorkerRequest request = TurboProto.vHiveWorkerRequest.newBuilder()
                .setWorkerType(String.valueOf(WorkerType.BROADCAST_CHAIN_JOIN_STREAMING))
                .setJson(JSON.toJSONString(input, SerializerFeature.DisableCircularReferenceDetect))
                .build();
        return this.stub.process(request);
    }

    public ListenableFuture<TurboProto.vHiveWorkerResponse> broadcastJoin(BroadcastJoinInput input)
    {
        TurboProto.vHiveWorkerRequest request = TurboProto.vHiveWorkerRequest.newBuilder()
                .setWorkerType(String.valueOf(WorkerType.BROADCAST_JOIN))
                .setJson(JSON.toJSONString(input, SerializerFeature.DisableCircularReferenceDetect))
                .build();
        return this.stub.process(request);
    }

    public ListenableFuture<TurboProto.vHiveWorkerResponse> broadcastJoinStream(BroadcastJoinInput input)
    {
        TurboProto.vHiveWorkerRequest request = TurboProto.vHiveWorkerRequest.newBuilder()
                .setWorkerType(String.valueOf(WorkerType.BROADCAST_JOIN_STREAMING))
                .setJson(JSON.toJSONString(input, SerializerFeature.DisableCircularReferenceDetect))
                .build();
        return this.stub.process(request);
    }

    public ListenableFuture<TurboProto.vHiveWorkerResponse> partitionChainJoin(PartitionedChainJoinInput input)
    {
        TurboProto.vHiveWorkerRequest request = TurboProto.vHiveWorkerRequest.newBuilder()
                .setWorkerType(String.valueOf(WorkerType.PARTITIONED_CHAIN_JOIN))
                .setJson(JSON.toJSONString(input, SerializerFeature.DisableCircularReferenceDetect))
                .build();
        return this.stub.process(request);
    }

    public ListenableFuture<TurboProto.vHiveWorkerResponse> partitionChainJoinStreaming(PartitionedChainJoinInput input)
    {
        TurboProto.vHiveWorkerRequest request = TurboProto.vHiveWorkerRequest.newBuilder()
                .setWorkerType(String.valueOf(WorkerType.PARTITIONED_CHAIN_JOIN_STREAMING))
                .setJson(JSON.toJSONString(input, SerializerFeature.DisableCircularReferenceDetect))
                .build();
        return this.stub.process(request);
    }

    public ListenableFuture<TurboProto.vHiveWorkerResponse> partitionJoin(PartitionedJoinInput input)
    {
        TurboProto.vHiveWorkerRequest request = TurboProto.vHiveWorkerRequest.newBuilder()
                .setWorkerType(String.valueOf(WorkerType.PARTITIONED_JOIN))
                .setJson(JSON.toJSONString(input, SerializerFeature.DisableCircularReferenceDetect))
                .build();
        return this.stub.process(request);
    }

    public ListenableFuture<TurboProto.vHiveWorkerResponse> partitionJoinStreaming(PartitionedJoinInput input)
    {
        TurboProto.vHiveWorkerRequest request = TurboProto.vHiveWorkerRequest.newBuilder()
                .setWorkerType(String.valueOf(WorkerType.PARTITIONED_JOIN_STREAMING))
                .setJson(JSON.toJSONString(input, SerializerFeature.DisableCircularReferenceDetect))
                .build();
        return this.stub.process(request);
    }

    public ListenableFuture<TurboProto.vHiveWorkerResponse> partition(PartitionInput input)
    {
        TurboProto.vHiveWorkerRequest request = TurboProto.vHiveWorkerRequest.newBuilder()
                .setWorkerType(String.valueOf(WorkerType.PARTITION))
                .setJson(JSON.toJSONString(input, SerializerFeature.DisableCircularReferenceDetect))
                .build();
        return this.stub.process(request);
    }

    public ListenableFuture<TurboProto.vHiveWorkerResponse> partitionStreaming(PartitionInput input)
    {
        TurboProto.vHiveWorkerRequest request = TurboProto.vHiveWorkerRequest.newBuilder()
                .setWorkerType(String.valueOf(WorkerType.PARTITION_STREAMING))
                .setJson(JSON.toJSONString(input, SerializerFeature.DisableCircularReferenceDetect))
                .build();
        return this.stub.process(request);
    }

    public ListenableFuture<TurboProto.vHiveWorkerResponse> scan(ScanInput input)
    {
        TurboProto.vHiveWorkerRequest request = TurboProto.vHiveWorkerRequest.newBuilder()
                .setWorkerType(String.valueOf(WorkerType.SCAN))
                .setJson(JSON.toJSONString(input, SerializerFeature.DisableCircularReferenceDetect))
                .build();
        return this.stub.process(request);
    }

    public ListenableFuture<TurboProto.vHiveWorkerResponse> scanStream(ScanInput input)
    {
        TurboProto.vHiveWorkerRequest request = TurboProto.vHiveWorkerRequest.newBuilder()
                .setWorkerType(String.valueOf(WorkerType.SCAN_STREAM))
                .setJson(JSON.toJSONString(input, SerializerFeature.DisableCircularReferenceDetect))
                .build();
        return this.stub.process(request);
    }
}

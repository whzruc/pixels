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
package io.pixelsdb.pixels.worker.lambda;

import com.amazonaws.services.lambda.runtime.Context;
import com.amazonaws.services.lambda.runtime.RequestHandler;
import io.pixelsdb.pixels.planner.plan.physical.input.PartitionedJoinInput;
import io.pixelsdb.pixels.planner.plan.physical.output.JoinOutput;
import io.pixelsdb.pixels.worker.common.BasePartitionedJoinWorker;
import io.pixelsdb.pixels.worker.common.WorkerContext;
import io.pixelsdb.pixels.worker.common.WorkerMetrics;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * @author hank
 * @create 2023-04-23
 */
public class PartitionedJoinWorker implements RequestHandler<PartitionedJoinInput, JoinOutput>
{
    private static final Logger logger = LoggerFactory.getLogger(PartitionedJoinWorker.class);
    private final WorkerMetrics workerMetrics = new WorkerMetrics();

    @Override
    public JoinOutput handleRequest(PartitionedJoinInput event, Context context)
    {
        WorkerContext workerContext = new WorkerContext(logger, workerMetrics, context.getAwsRequestId());
        BasePartitionedJoinWorker baseWorker = new BasePartitionedJoinWorker(workerContext);
        return baseWorker.process(event);
    }
}
/**
 * MIT License
 *
 * Copyright (c) 2024 liudegui
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file osp/opt.hpp
 * @brief Central compile-time tuning switch table (lwIP opt.h port).
 * OSP_* macros default here; -DOSP_XXX=value overrides via #ifndef. Platform
 * config lives in platform.hpp; runtime parsing lives in config.hpp.
 */

#ifndef OSP_OPT_HPP_
#define OSP_OPT_HPP_

// ============================================================================
// 1. Queue / buffer depth
// ============================================================================

/// AsyncBus lock-free MPSC queue depth.
#ifndef OSP_BUS_QUEUE_DEPTH
#define OSP_BUS_QUEUE_DEPTH 4096U
#endif

/// AsyncBus maximum messages drained per ProcessBatch call.
#ifndef OSP_BUS_BATCH_SIZE
#define OSP_BUS_BATCH_SIZE 256U
#endif

/// WorkerPool per-worker SPSC queue depth.
#ifndef OSP_WORKER_QUEUE_DEPTH
#define OSP_WORKER_QUEUE_DEPTH 1024U
#endif

/// AsyncLog per-thread buffer queue depth.
#ifndef OSP_ASYNC_LOG_QUEUE_DEPTH
#define OSP_ASYNC_LOG_QUEUE_DEPTH 256U
#endif

/// AsyncLog maximum entries flushed per write batch.
#ifndef OSP_ASYNC_LOG_BATCH_SIZE
#define OSP_ASYNC_LOG_BATCH_SIZE 8U
#endif

/// AsyncLog maximum concurrent producer threads.
#ifndef OSP_ASYNC_LOG_MAX_THREADS
#define OSP_ASYNC_LOG_MAX_THREADS 8U
#endif

/// Serial receive ring buffer depth (bytes).
#ifndef OSP_SERIAL_RX_RING_SIZE
#define OSP_SERIAL_RX_RING_SIZE 4096U
#endif

/// Serial maximum frame length (bytes).
#ifndef OSP_SERIAL_MAX_FRAME_SIZE
#define OSP_SERIAL_MAX_FRAME_SIZE 1024U
#endif

/// TCP transport receive ring depth (frames).
#ifndef OSP_TRANSPORT_RECV_RING_DEPTH
#define OSP_TRANSPORT_RECV_RING_DEPTH 32U
#endif

/// TCP transport maximum frame length (bytes).
#ifndef OSP_TRANSPORT_MAX_FRAME_SIZE
#define OSP_TRANSPORT_MAX_FRAME_SIZE 4096U
#endif

/// Shared-memory channel slot count.
#ifndef OSP_SHM_SLOT_COUNT
#define OSP_SHM_SLOT_COUNT 256
#endif

/// Shared-memory single slot size (bytes).
#ifndef OSP_SHM_SLOT_SIZE
#define OSP_SHM_SLOT_SIZE 4096
#endif

/// Shared-memory byte ring capacity (default 1 MB).
#ifndef OSP_SHM_BYTE_RING_CAPACITY
#define OSP_SHM_BYTE_RING_CAPACITY (1024 * 1024)
#endif

/// Shared-memory channel name maximum length.
#ifndef OSP_SHM_CHANNEL_NAME_MAX
#define OSP_SHM_CHANNEL_NAME_MAX 64
#endif

/// Shared-memory maximum consumers per channel.
#ifndef OSP_SHM_SPMC_MAX_CONSUMERS
#define OSP_SHM_SPMC_MAX_CONSUMERS 8
#endif

// ============================================================================
// 2. Capacity limits
// ============================================================================

/// AsyncBus maximum message types (upper bound on variant size).
#ifndef OSP_BUS_MAX_MESSAGE_TYPES
#define OSP_BUS_MAX_MESSAGE_TYPES 8U
#endif

/// AsyncBus maximum callbacks per message type.
#ifndef OSP_BUS_MAX_CALLBACKS_PER_TYPE
#define OSP_BUS_MAX_CALLBACKS_PER_TYPE 16U
#endif

/// WorkerPool maximum worker count (compile-time capacity).
#ifndef OSP_WORKER_POOL_MAX_WORKERS
#define OSP_WORKER_POOL_MAX_WORKERS 4U
#endif

/// Service maximum concurrent worker threads (FixedVector capacity).
#ifndef OSP_SERVICE_MAX_WORKERS
#define OSP_SERVICE_MAX_WORKERS 16U
#endif

/// Executor maximum registered nodes.
#ifndef OSP_EXECUTOR_MAX_NODES
#define OSP_EXECUTOR_MAX_NODES 16U
#endif

/// NodeManager maximum managed nodes.
#ifndef OSP_NODE_MANAGER_MAX_NODES
#define OSP_NODE_MANAGER_MAX_NODES 64U
#endif

/// IO Poller maximum events per poll call.
#ifndef OSP_IO_POLLER_MAX_EVENTS
#define OSP_IO_POLLER_MAX_EVENTS 64U
#endif

/// Shell history entry count.
#ifndef OSP_SHELL_HISTORY_SIZE
#define OSP_SHELL_HISTORY_SIZE 16
#endif

/// Shell line buffer length (bytes).
#ifndef OSP_SHELL_LINE_BUF_SIZE
#define OSP_SHELL_LINE_BUF_SIZE 256
#endif

/// Shell maximum command arguments.
#ifndef OSP_SHELL_MAX_ARGS
#define OSP_SHELL_MAX_ARGS 16
#endif

/// Maximum subscriptions per node.
#ifndef OSP_MAX_NODE_SUBSCRIPTIONS
#define OSP_MAX_NODE_SUBSCRIPTIONS 16U
#endif

/// Maximum subscriptions per static node.
#ifndef OSP_MAX_STATIC_NODE_SUBSCRIPTIONS
#define OSP_MAX_STATIC_NODE_SUBSCRIPTIONS 16U
#endif

/// Data dispatcher maximum pipeline stages.
#ifndef OSP_JOB_MAX_STAGES
#define OSP_JOB_MAX_STAGES 8U
#endif

/// Data dispatcher maximum consumers.
#ifndef OSP_JOB_MAX_CONSUMERS
#define OSP_JOB_MAX_CONSUMERS 8U
#endif

/// Data dispatcher maximum edges per job.
#ifndef OSP_JOB_MAX_EDGES
#define OSP_JOB_MAX_EDGES 16U
#endif

/// HSM maximum nesting depth.
#ifndef OSP_HSM_MAX_DEPTH
#define OSP_HSM_MAX_DEPTH 32
#endif

/// App maximum instances.
#ifndef OSP_APP_MAX_INSTANCES
#define OSP_APP_MAX_INSTANCES 64U
#endif

/// App message inline buffer size (bytes).
#ifndef OSP_APP_MSG_INLINE_SIZE
#define OSP_APP_MSG_INLINE_SIZE 48U
#endif

/// App queue depth.
#ifndef OSP_APP_QUEUE_DEPTH
#define OSP_APP_QUEUE_DEPTH 256U
#endif

/// App response data maximum length (bytes).
#ifndef OSP_RESPONSE_DATA_SIZE
#define OSP_RESPONSE_DATA_SIZE 256U
#endif

/// Connection pool maximum connections.
#ifndef OSP_CONNECTION_POOL_MAX
#define OSP_CONNECTION_POOL_MAX 32U
#endif

/// Post maximum apps.
#ifndef OSP_POST_MAX_APPS
#define OSP_POST_MAX_APPS 64U
#endif

/// Behavior tree maximum nodes.
#ifndef OSP_BT_MAX_NODES
#define OSP_BT_MAX_NODES 32
#endif

/// Behavior tree maximum children per node.
#ifndef OSP_BT_MAX_CHILDREN
#define OSP_BT_MAX_CHILDREN 8
#endif

/// Config parser maximum file size (bytes).
#ifndef OSP_CONFIG_MAX_FILE_SIZE
#define OSP_CONFIG_MAX_FILE_SIZE 8192U
#endif

// ============================================================================
// 3. Behavior switches
// ============================================================================

/// Use the coarse clock for bus message timestamps (1 = faster, ~4ms res).
#ifndef OSP_BUS_COARSE_TIMESTAMP
#define OSP_BUS_COARSE_TIMESTAMP 0
#endif

/// Compile-time minimum log level (0=DEBUG,1=INFO,2=WARN,3=ERROR,4=FATAL,5=OFF).
/// NDEBUG builds default to INFO; debug builds default to DEBUG.
#ifndef OSP_LOG_MIN_LEVEL
#ifdef NDEBUG
#define OSP_LOG_MIN_LEVEL 1
#else
#define OSP_LOG_MIN_LEVEL 0
#endif
#endif

/// AsyncLog drop-stats report interval (seconds; 0 disables).
#ifndef OSP_ASYNC_LOG_DROP_REPORT_INTERVAL_S
#define OSP_ASYNC_LOG_DROP_REPORT_INTERVAL_S 10U
#endif

/// Discovery multicast port.
#ifndef OSP_DISCOVERY_PORT
#define OSP_DISCOVERY_PORT 9999
#endif

/// Discovery multicast group address.
#ifndef OSP_DISCOVERY_MULTICAST_GROUP
#define OSP_DISCOVERY_MULTICAST_GROUP "239.255.0.1"
#endif

/// Discovery announce interval (milliseconds).
#ifndef OSP_DISCOVERY_INTERVAL_MS
#define OSP_DISCOVERY_INTERVAL_MS 1000
#endif

/// Discovery node timeout (milliseconds).
#ifndef OSP_DISCOVERY_TIMEOUT_MS
#define OSP_DISCOVERY_TIMEOUT_MS 3000
#endif

/// Data dispatcher job pool magic (validation).
#ifndef OSP_JOB_POOL_MAGIC
#define OSP_JOB_POOL_MAGIC 0x4A4F4250U
#endif

/// Data dispatcher block alignment (bytes).
#ifndef OSP_JOB_BLOCK_ALIGN
#define OSP_JOB_BLOCK_ALIGN 64U
#endif

/// Data dispatcher consumer heartbeat interval (microseconds). Consumers call
/// ConsumerHeartbeat() at least this often while alive.
#ifndef OSP_JOB_CONSUMER_HEARTBEAT_US
#define OSP_JOB_CONSUMER_HEARTBEAT_US 1000000U
#endif

/// Data dispatcher dead-consumer heartbeat timeout (microseconds). A
/// ShmStore consumer whose heartbeat is older than this threshold is reaped by
/// CleanupDeadConsumers(). Must be several multiples of
/// OSP_JOB_CONSUMER_HEARTBEAT_US to tolerate scheduling jitter.
#ifndef OSP_JOB_CONSUMER_TIMEOUT_US
#define OSP_JOB_CONSUMER_TIMEOUT_US 3000000U
#endif

#endif  // OSP_OPT_HPP_

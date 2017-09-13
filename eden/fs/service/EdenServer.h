/*
 *  Copyright (c) 2016-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <folly/Executor.h>
#include <folly/File.h>
#include <folly/Range.h>
#include <folly/SocketAddress.h>
#include <folly/Synchronized.h>
#include <folly/ThreadLocal.h>
#include <folly/experimental/StringKeyedMap.h>
#include <folly/futures/SharedPromise.h>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "eden/fs/config/InterpolatedPropertyTree.h"
#include "eden/fs/fuse/EdenStats.h"
#include "eden/fs/utils/PathFuncs.h"
#include "folly/experimental/FunctionScheduler.h"

constexpr folly::StringPiece kPeriodicUnloadCounterKey{"PeriodicUnloadCounter"};

namespace apache {
namespace thrift {
class ThriftServer;
}
}

namespace folly {
class EventBase;
}

namespace facebook {
namespace eden {

class BackingStore;
class Dirstate;
class EdenMount;
class EdenServiceHandler;
class LocalStore;

/*
 * EdenServer contains logic for running the Eden main loop.
 *
 * It performs locking to ensure only a single EdenServer instance is running
 * for a particular location, then starts the thrift management server
 * and the fuse session.
 */
class EdenServer {
 public:
  using ConfigData = InterpolatedPropertyTree;
  using MountList = std::vector<std::shared_ptr<EdenMount>>;
  using DirstateMap = folly::StringKeyedMap<std::shared_ptr<Dirstate>>;

  EdenServer(
      AbsolutePathPiece edenDir,
      AbsolutePathPiece etcEdenDir,
      AbsolutePathPiece configPath,
      AbsolutePathPiece rocksPath);
  virtual ~EdenServer();

  /**
   * Run the EdenServer.
   */
  void run();

  /**
   * Prepare to run the EdenServer.
   *
   * This acquires the lock on the eden directory, remounts configured mount
   * points, and prepares the thrift server to run.
   *
   * After prepare returns the caller can call getServer()->serve() to
   * run the thrift server main loop.
   */
  void prepare();

  /**
   * Stops this server, which includes the underlying Thrift server.
   *
   * This may be called from any thread while a call to run() is outstanding,
   * and will cause run() to return.
   */
  void stop() const;

  /**
   * Mount an EdenMount.
   *
   * This function blocks until the main mount point is successfully started,
   * and throws an error if an error occurs.
   */
  FOLLY_NODISCARD folly::Future<folly::Unit> mount(
      std::shared_ptr<EdenMount> edenMount);

  /**
   * Unmount an EdenMount.
   */
  FOLLY_NODISCARD folly::Future<folly::Unit> unmount(
      folly::StringPiece mountPath);

  /**
   * Unmount all mount points maintained by this server, and wait for them to
   * be completely unmounted.
   */
  FOLLY_NODISCARD folly::Future<folly::Unit> unmountAll();

  const std::shared_ptr<EdenServiceHandler>& getHandler() const {
    return handler_;
  }
  const std::shared_ptr<apache::thrift::ThriftServer>& getServer() const {
    return server_;
  }

  AbsolutePath getSocketPath() const;

  MountList getMountPoints() const;

  /**
   * Look up an EdenMount by the path where it is mounted.
   *
   * Throws an EdenError if no mount exists with the specified path.
   */
  std::shared_ptr<EdenMount> getMount(folly::StringPiece mountPath) const;

  /**
   * Look up an EdenMount by the path where it is mounted.
   *
   * Returns nullptr if no mount exists with the specified path.
   */
  std::shared_ptr<EdenMount> getMountOrNull(folly::StringPiece mountPath) const;

  std::shared_ptr<LocalStore> getLocalStore() const {
    return localStore_;
  }

  void reloadConfig();
  std::shared_ptr<ConfigData> getConfig();

  /**
   * Look up the BackingStore object for the specified repository type+name.
   *
   * EdenServer maintains an internal cache of all known BackingStores,
   * so that multiple mount points that use the same repository can
   * share the same BackingStore object.
   *
   * If this is the first time this given (type, name) has been used, a new
   * BackingStore object will be created and returned.  Otherwise this will
   * return the existing BackingStore that was previously created.
   */
  std::shared_ptr<BackingStore> getBackingStore(
      folly::StringPiece type,
      folly::StringPiece name);

  AbsolutePathPiece getEdenDir() {
    return edenDir_;
  }

  fusell::ThreadLocalEdenStats* getStats() const {
    return &edenStats_;
  }

  /**
   * Flush all thread-local stats to the main ServiceData object.
   *
   * Thread-local counters are normally flushed to the main ServiceData once
   * a second.  flushStatsNow() can be used to flush thread-local counters on
   * demand, in addition to the normal once-a-second flush.
   *
   * This is mainly useful for unit and integration tests that want to ensure
   * they see up-to-date counter information without waiting for the normal
   * flush interval.
   */
  void flushStatsNow() const;

  /**
   * Get the main thread's EventBase.
   *
   * Callers can use this for scheduling work to be run in the main thread.
   */
  folly::EventBase* getMainEventBase() const {
    return mainEventBase_;
  }

 private:
  // Struct to store EdenMount along with SharedPromise that is set
  // during unmount to allow synchronizaiton between unmoutFinished
  // and unmount functions.
  struct EdenMountInfo {
    std::shared_ptr<EdenMount> edenMount;
    folly::SharedPromise<folly::Unit> unmountPromise;

    explicit EdenMountInfo(const std::shared_ptr<EdenMount>& mount)
        : edenMount(mount),
          unmountPromise(folly::SharedPromise<folly::Unit>()) {}
  };

  using BackingStoreKey = std::pair<std::string, std::string>;
  using BackingStoreMap =
      std::unordered_map<BackingStoreKey, std::shared_ptr<BackingStore>>;
  using MountMap = folly::StringKeyedMap<struct EdenMountInfo>;
  class ThriftServerEventHandler;

  // Forbidden copy constructor and assignment operator
  EdenServer(EdenServer const&) = delete;
  EdenServer& operator=(EdenServer const&) = delete;

  // Schedules a timer to flush stats (and reschedule itself).
  // We should have at most one of these pending at a time.
  // Must be called only from the eventBase thread.
  void scheduleFlushStats();

  // Schedule a call to unloadInodes() to happen after timeout
  // has expired.
  // Must be called only from the eventBase thread.
  void scheduleInodeUnload(std::chrono::milliseconds timeout);

  // Perform unloading of inodes based on their last access time
  // and then schedule another call to unloadInodes() to happen
  // at the next appropriate interval.  The unload attempt applies to
  // all mounts.
  void unloadInodes();

  std::shared_ptr<BackingStore> createBackingStore(
      folly::StringPiece type,
      folly::StringPiece name);
  void createThriftServer();
  void acquireEdenLock();
  void prepareThriftAddress();

  // Called when a mount has been unmounted and has stopped.
  void mountFinished(EdenMount* mountPoint);

  // Called before destructing EdenServer
  void shutdown();

  /*
   * Member variables.
   *
   * Note that the declaration order below is important for initialization
   * and cleanup order.  lockFile_ is near the top so it will be released last.
   * mountPoints_ are near the bottom, so they get destroyed before the
   * backingStores_ and localStore_.
   */

  AbsolutePath edenDir_;
  AbsolutePath etcEdenDir_;
  AbsolutePath configPath_;
  AbsolutePath rocksPath_;
  folly::File lockFile_;
  folly::Synchronized<std::shared_ptr<ConfigData>> configData_;
  std::shared_ptr<EdenServiceHandler> handler_;
  std::shared_ptr<apache::thrift::ThriftServer> server_;
  std::shared_ptr<ThriftServerEventHandler> serverEventHandler_;

  std::shared_ptr<LocalStore> localStore_;
  folly::Synchronized<BackingStoreMap> backingStores_;

  folly::Synchronized<MountMap> mountPoints_;
  mutable fusell::ThreadLocalEdenStats edenStats_;

  /**
   * The EventBase driving the main thread loop.
   *
   * This is used to drive the the thrift server and can also be used for
   * scheduling other asynchronous operations.
   *
   * This is set when the EdenServer is started and is never updated after
   * this, so we do not need synchronization when reading it.
   */
  folly::EventBase* mainEventBase_;

  /**
   * A CPU executor for running arbitrary tasks.
   * This is here because we need to keep it alive for the duration
   * of the server lifetime.
   */
  std::shared_ptr<folly::Executor> threadPool_;
};
}
} // facebook::eden

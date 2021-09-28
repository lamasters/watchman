/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include <folly/Synchronized.h>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "watchman/ContentHash.h"
#include "watchman/CookieSync.h"
#include "watchman/PendingCollection.h"
#include "watchman/PerfSample.h"
#include "watchman/QueryableView.h"
#include "watchman/Result.h"
#include "watchman/RingBuffer.h"
#include "watchman/SymlinkTargets.h"
#include "watchman/WatchmanConfig.h"
#include "watchman/query/FileResult.h"
#include "watchman/watchman_opendir.h"
#include "watchman/watchman_string.h"
#include "watchman/watchman_system.h"

struct watchman_client;
struct watchman_file;

namespace watchman {

struct GlobTree;
class Watcher;

// Helper struct to hold caches used by the InMemoryView
struct InMemoryViewCaches {
  ContentHashCache contentHashCache;
  SymlinkTargetCache symlinkTargetCache;

  InMemoryViewCaches(
      const w_string& rootPath,
      size_t maxHashes,
      size_t maxSymlinks,
      std::chrono::milliseconds errorTTL);
};

class InMemoryFileResult final : public FileResult {
 public:
  InMemoryFileResult(const watchman_file* file, InMemoryViewCaches& caches);
  std::optional<FileInformation> stat() override;
  std::optional<struct timespec> accessedTime() override;
  std::optional<struct timespec> modifiedTime() override;
  std::optional<struct timespec> changedTime() override;
  std::optional<size_t> size() override;
  w_string_piece baseName() override;
  w_string_piece dirName() override;
  std::optional<bool> exists() override;
  std::optional<w_string> readLink() override;
  std::optional<w_clock_t> ctime() override;
  std::optional<w_clock_t> otime() override;
  std::optional<FileResult::ContentHash> getContentSha1() override;
  void batchFetchProperties(
      const std::vector<std::unique_ptr<FileResult>>& files) override;

 private:
  const watchman_file* file_;
  w_string dirName_;
  InMemoryViewCaches& caches_;
  std::optional<w_string> symlinkTarget_;
  Result<FileResult::ContentHash> contentSha1_;
};

/**
 * In-memory data structure representing Watchman's understanding of the watched
 * root. Files are ordered in a linked recency index as well as hierarchically
 * from the root.
 */
class ViewDatabase {
 public:
  explicit ViewDatabase(const w_string& root_path);

  watchman_file* getLatestFile() const {
    return latestFile_;
  }

  ino_t getRootInode() const {
    return rootInode_;
  }

  void setRootInode(ino_t ino) {
    rootInode_ = ino;
  }

  watchman_dir* resolveDir(const w_string& dirname, bool create);

  const watchman_dir* resolveDir(const w_string& dirname) const;

  /**
   * Returns the direct child file named name if it already exists, else creates
   * that entry and returns it.
   */
  watchman_file* getOrCreateChildFile(
      Watcher& watcher,
      watchman_dir* dir,
      const w_string& file_name,
      w_clock_t ctime);

  /**
   * Updates the otime for the file and bubbles it to the front of recency
   * index.
   */
  void markFileChanged(Watcher& watcher, watchman_file* file, w_clock_t otime);

  /**
   * Mark a directory as being removed from the view. Marks the contained set of
   * files as deleted. If recursive is true, is recursively invoked on child
   * dirs.
   */
  void markDirDeleted(
      Watcher& watcher,
      watchman_dir* dir,
      w_clock_t otime,
      bool recursive);

 private:
  void insertAtHeadOfFileList(struct watchman_file* file);

  const w_string rootPath_;

  /* the most recently changed file */
  watchman_file* latestFile_ = nullptr;

  std::unique_ptr<watchman_dir> rootDir_;

  // Inode number for the root dir.  This is used to detect what should
  // be impossible situations, but is needed in practice to workaround
  // eg: BTRFS not delivering all events for subvolumes
  ino_t rootInode_{0};
};

/**
 * Keeps track of the state of the filesystem in-memory and drives a notify
 * thread which consumes events from the watcher.
 */
class InMemoryView final : public QueryableView {
 public:
  InMemoryView(
      const w_string& root_path,
      Configuration config,
      std::shared_ptr<Watcher> watcher);
  ~InMemoryView() override;

  InMemoryView(InMemoryView&&) = delete;
  InMemoryView& operator=(InMemoryView&&) = delete;

  ClockPosition getMostRecentRootNumberAndTickValue() const override;
  uint32_t getLastAgeOutTickValue() const override;
  std::chrono::system_clock::time_point getLastAgeOutTimeStamp() const override;
  w_string getCurrentClockString() const override;

  w_clock_t getClock(std::chrono::system_clock::time_point now) const {
    return w_clock_t{
        mostRecentTick_.load(std::memory_order_acquire),
        std::chrono::system_clock::to_time_t(now),
    };
  }

  void ageOut(PerfSample& sample, std::chrono::seconds minAge) override;
  void syncToNow(
      const std::shared_ptr<Root>& root,
      std::chrono::milliseconds timeout,
      std::vector<w_string>& cookieFileNames) override;

  bool doAnyOfTheseFilesExist(
      const std::vector<w_string>& fileNames) const override;

  void timeGenerator(Query* query, QueryContext* ctx) const override;

  void pathGenerator(Query* query, QueryContext* ctx) const override;

  void globGenerator(Query* query, QueryContext* ctx) const override;

  void allFilesGenerator(Query* query, QueryContext* ctx) const override;

  std::shared_future<void> waitUntilReadyToQuery(
      const std::shared_ptr<Root>& root) override;

  void startThreads(const std::shared_ptr<Root>& root) override;
  void signalThreads() override;
  void wakeThreads() override;
  void clientModeCrawl(const std::shared_ptr<Root>& root);

  const w_string& getName() const override;
  const std::shared_ptr<Watcher>& getWatcher() const;
  json_ref getWatcherDebugInfo() const override;
  void clearWatcherDebugInfo() override;
  json_ref getViewDebugInfo() const;
  void clearViewDebugInfo();

  // If content cache warming is configured, do the warm up now
  void warmContentCache();

  SCM* getSCM() const override;

  InMemoryViewCaches& debugAccessCaches() const {
    return caches_;
  }

 private:
  void syncToNowCookies(
      const std::shared_ptr<Root>& root,
      std::chrono::milliseconds timeout,
      std::vector<w_string>& cookieFileNames);

  // Returns the erased file's otime.
  w_clock_t ageOutFile(
      std::unordered_set<w_string>& dirs_to_erase,
      watchman_file* file);

  // When a watcher is desynced, it sets the W_PENDING_IS_DESYNCED flag, and the
  // crawler will set these recursively. If one of these flag is set,
  // processPending will return IsDesynced::Yes and it is expected that the
  // caller will abort all pending cookies after processAllPending returns.
  enum class IsDesynced { Yes, No };

  // Consume entries from `pending` and apply them to the InMemoryView. Any new
  // pending paths generated by processPath will be crawled before
  // processAllPending returns.
  IsDesynced processAllPending(
      const std::shared_ptr<Root>& root,
      ViewDatabase& view,
      PendingChanges& pending);

  void processPath(
      const std::shared_ptr<Root>& root,
      ViewDatabase& view,
      PendingChanges& coll,
      const PendingChange& pending,
      const watchman_dir_ent* pre_stat);

  /** Recursively walks files under a specified dir */
  void dirGenerator(
      Query* query,
      QueryContext* ctx,
      const watchman_dir* dir,
      uint32_t depth) const;
  void globGeneratorTree(
      QueryContext* ctx,
      const GlobTree* node,
      const struct watchman_dir* dir) const;
  void globGeneratorDoublestar(
      QueryContext* ctx,
      const struct watchman_dir* dir,
      const GlobTree* node,
      const char* dir_name,
      uint32_t dir_name_len) const;
  /**
   * Crawl the given directory.
   *
   * Allowed flags:
   *  - W_PENDING_RECURSIVE: the directory will be recursively crawled,
   *  - W_PENDING_VIA_NOTIFY when the watcher only supports directory
   *    notification (W_PENDING_NONRECURSIVE_SCAN), this will stat all
   *    the files and directories contained in the passed in directory and stop.
   */
  void crawler(
      const std::shared_ptr<Root>& root,
      ViewDatabase& view,
      PendingChanges& coll,
      const PendingChange& pending);
  void notifyThread(const std::shared_ptr<Root>& root);
  void ioThread(const std::shared_ptr<Root>& root);
  bool handleShouldRecrawl(Root& root);
  void fullCrawl(const std::shared_ptr<Root>& root, PendingChanges& pending);

  /**
   * Called on the IO thread. If `pending` is not in the ignored directory list,
   * lstat() the file and update the InMemoryView. This may insert work into
   * `coll` if a directory needs to be rescanned.
   */
  void statPath(
      Root& root,
      ViewDatabase& view,
      PendingChanges& coll,
      const PendingChange& pending,
      const watchman_dir_ent* pre_stat);

  bool propagateToParentDirIfAppropriate(
      const Root& root,
      PendingChanges& coll,
      std::chrono::system_clock::time_point now,
      const FileInformation& entryStat,
      const w_string& dirName,
      const watchman_dir* parentDir,
      bool isUnlink);

  const Configuration config_;

  folly::Synchronized<ViewDatabase> view_;
  // The most recently observed tick value of an item in the view
  // Only incremented by the iothread, but may be read by other threads.
  std::atomic<uint32_t> mostRecentTick_{1};
  const uint32_t rootNumber_{0};
  const w_string rootPath_;

  // This allows a client to wait for a recrawl to complete.
  // The primary use of this is so that "watch-project" doesn't
  // send its return PDU to the client until after the initial
  // crawl is complete.  Note that a recrawl can happen at any
  // point, so this is a bit of a weak promise that a query can
  // be immediately executed, but is good enough assuming that
  // the system isn't in a perpetual state of recrawl.
  struct CrawlState {
    std::unique_ptr<std::promise<void>> promise;
    std::shared_future<void> future;
  };
  folly::Synchronized<CrawlState> crawlState_;

  uint32_t lastAgeOutTick_{0};
  // This is system_clock instead of steady_clock because it's compared with a
  // file's otime.
  std::chrono::system_clock::time_point lastAgeOutTimestamp_{};

  /*
   * Queue of items that we need to stat/process.
   *
   * Populated by both the IO thread (fullCrawl) and the notify thread (from the
   * watcher).
   */
  PendingCollection pending_;

  std::atomic<bool> stopThreads_{false};
  std::shared_ptr<Watcher> watcher_;

  // mutable because we pass a reference to other things from inside
  // const methods
  mutable InMemoryViewCaches caches_;

  // Should we warm the cache when we settle?
  bool enableContentCacheWarming_{false};
  // How many of the most recent files to warm up when settling?
  size_t maxFilesToWarmInContentCache_{1024};
  // If true, we will wait for the items to be hashed before
  // dispatching the settle to watchman clients
  bool syncContentCacheWarming_{false};
  // Remember what we've already warmed up
  uint32_t lastWarmedTick_{0};

  // The source control system that we detected during initialization
  std::unique_ptr<SCM> scm_;

  struct PendingChangeLogEntry {
    PendingChangeLogEntry() noexcept {
      // time_point is not noexcept so this can't be defaulted.
    }
    explicit PendingChangeLogEntry(
        const PendingChange& pc,
        std::error_code errcode,
        const FileInformation& st) noexcept;

    json_ref asJsonValue() const;

    // 55 should cover many filenames.
    static constexpr size_t kPathLength = 55;

    // fields from PendingChange
    std::chrono::system_clock::time_point now;
    PendingFlags::UnderlyingType pending_flags;
    char path_tail[kPathLength];

    // results of calling getFileInformation
    int32_t errcode;
    mode_t mode;
    off_t size;
    time_t mtime;
  };

  static_assert(88 == sizeof(PendingChangeLogEntry));

  // If set, paths processed by processPending are logged here.
  std::unique_ptr<RingBuffer<PendingChangeLogEntry>> processedPaths_;
};

} // namespace watchman

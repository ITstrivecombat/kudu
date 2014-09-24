// Copyright (c) 2014, Cloudera, inc.

#ifndef KUDU_FS_BLOCK_MANAGER_H
#define KUDU_FS_BLOCK_MANAGER_H

#include <cstddef>
#include <stdint.h>
#include <vector>

#include "kudu/gutil/gscoped_ptr.h"

namespace kudu {

class BlockId;
class Slice;
class Status;

namespace fs {

class BlockManager;

// The smallest unit of Kudu data that is backed by the local filesystem.
//
// The block interface reflects Kudu on-disk storage design principles:
// - Blocks are append only.
// - Blocks are immutable once written.
// - Blocks opened for reading are thread-safe and may be used by multiple
//   concurrent readers.
// - Blocks opened for writing are not thread-safe.
class Block {
 public:
  virtual ~Block() {}

  // Returns the identifier for this block.
  virtual const BlockId& id() const = 0;
};

// A block that has been opened for writing. There may only be a single
// writing thread, and data may only be appended to the block.
//
// Close() is an expensive operation, as it must flush both dirty block data
// and metadata to disk. The block manager API provides two ways to improve
// Close() performance:
// 1. FlushDataAsync() before Close(). If there's enough work to be done
//    between the two calls, there will be less outstanding I/O to wait for
//    during Close().
// 2. CloseBlocks() on a group of blocks. This at least ensures that, when
//    waiting on outstanding I/O, the waiting is done in parallel.
class WritableBlock : public Block {
 public:
  enum State {
    // There is no dirty data in the block.
    CLEAN,

    // There is some dirty data in the block.
    DIRTY,

    // There is an outstanding flush operation asynchronously flushing
    // dirty block data to disk.
    FLUSHING,

    // The block is closed. No more operations can be performed on it.
    CLOSED
  };

  virtual ~WritableBlock() {}

  // Destroys the in-memory representation of the block and synchronizes
  // dirty block data and metadata with the disk. On success, guarantees
  // that the entire block is durable.
  virtual Status Close() = 0;

  // Get a pointer back to this block's manager.
  virtual BlockManager* block_manager() const = 0;

  // Appends the chunk of data referenced by 'data' to the block.
  //
  // Does not guarantee durability of 'data'; Close() must be called for all
  // outstanding data to reach the disk.
  virtual Status Append(const Slice& data) = 0;

  // Begins an asynchronous flush of dirty block data to disk.
  //
  // This is purely a performance optimization for Close(); if there is
  // other work to be done between the final Append() and the future
  // Close(), FlushDataAsync() will reduce the amount of time spent waiting
  // for outstanding I/O to complete in Close(). This is analogous to
  // readahead or prefetching.
  //
  // Data may not be written to the block after FlushDataAsync() is called.
  virtual Status FlushDataAsync() = 0;

  // Returns the number of bytes successfully appended via Append().
  virtual size_t BytesAppended() const = 0;

  virtual State state() const = 0;
};

// A block that has been opened for reading. Multiple in-memory blocks may
// be constructed for the same logical block, and the same in-memory block
// may be shared amongst threads for concurrent reading.
class ReadableBlock : public Block {
 public:
  virtual ~ReadableBlock() {}

  // Destroys the in-memory representation of the block.
  virtual Status Close() = 0;

  // Returns the on-disk size of a written block.
  virtual Status Size(size_t* sz) const = 0;

  // Reads exactly 'length' bytes beginning from 'offset' in the block,
  // returning an error if fewer bytes exist. A slice referencing the
  // results is written to 'result' and may be backed by memory in
  // 'scratch'. As such, 'scratch' must be at least 'length' in size and
  // must remain alive while 'result' is used.
  //
  // Does not modify 'result' on error (but may modify 'scratch').
  virtual Status Read(uint64_t offset, size_t length,
                      Slice* result, uint8_t* scratch) const = 0;
};

// Provides options and hints for block placement.
struct CreateBlockOptions {
};

// Utilities for Kudu block lifecycle management. All methods are
// thread-safe.
class BlockManager {
 public:
  virtual ~BlockManager() {}

  // Creates a new on-disk representation for this block manager.
  //
  // Returns an error if one already exists or cannot be created.
  virtual Status Create() = 0;

  // Opens an existing on-disk representation of this block manager.
  //
  // Returns an error if one does not exist or cannot be opened.
  virtual Status Open() = 0;

  // Creates a new block using the provided options and opens it for
  // writing. The block's ID will be generated.
  //
  // Does not guarantee the durability of the block; it must be closed to
  // ensure that it reaches disk.
  //
  // Does not modify 'block' on error.
  virtual Status CreateAnonymousBlock(const CreateBlockOptions& opts,
                                      gscoped_ptr<WritableBlock>* block) = 0;

  // Like the above but uses default options.
  virtual Status CreateAnonymousBlock(gscoped_ptr<WritableBlock>* block) = 0;

  // Creates a new block using the provided options and opens it for
  // writing. The block's ID must be provided by the caller.
  //
  // Does not guarantee the durability of the block; it must be closed to
  // ensure that it reaches disk.
  //
  // Does not modify 'block' on error.
  virtual Status CreateNamedBlock(const CreateBlockOptions& opts,
                                  const BlockId& block_id,
                                  gscoped_ptr<WritableBlock>* block) = 0;

  // Like the above but uses default options.
  virtual Status CreateNamedBlock(const BlockId& block_id,
                                  gscoped_ptr<WritableBlock>* block) = 0;

  // Opens an existing block for reading.
  //
  // Does not modify 'block' on error.
  virtual Status OpenBlock(const BlockId& block_id,
                           gscoped_ptr<ReadableBlock>* block) = 0;

  // Deletes an existing block, allowing its space to be reclaimed by the
  // filesystem. The change is immediately made durable.
  //
  // Blocks may be deleted while they are open for reading or writing;
  // the actual deletion will take place after the last open reader or
  // writer is closed.
  virtual Status DeleteBlock(const BlockId& block_id) = 0;

  // Closes (and fully synchronizes) the given blocks. Effectively like
  // Close() for each block but may be optimized for groups of blocks.
  //
  // On success, guarantees that outstanding data is durable.
  virtual Status CloseBlocks(const std::vector<WritableBlock*>& blocks) = 0;
};

} // namespace fs
} // namespace kudu

#endif

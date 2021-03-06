#include "storage/tuple_access_strategy.h"
#include <utility>
#include "common/container/concurrent_bitmap.h"

namespace terrier::storage {

TupleAccessStrategy::TupleAccessStrategy(BlockLayout layout)
    : layout_(std::move(layout)), column_offsets_(layout_.NumColumns()) {
  // Calculate the start position of each column
  // we use 64-bit vectorized scans on bitmaps.
  uint32_t acc_offset = layout_.HeaderSize();
  TERRIER_ASSERT(acc_offset % sizeof(uint64_t) == 0, "size of a header should already be padded to aligned to 8 bytes");
  for (uint16_t i = 0; i < layout_.NumColumns(); i++) {
    column_offsets_[i] = acc_offset;
    uint32_t column_size =
        layout_.AttrSize(col_id_t(i)) * layout_.NumSlots()  // content
        + StorageUtil::PadUpToSize(sizeof(uint64_t),
                                   common::RawBitmap::SizeInBytes(layout_.NumSlots()));  // padded-bitmap size
    acc_offset += StorageUtil::PadUpToSize(sizeof(uint64_t), column_size);
    TERRIER_ASSERT(acc_offset <= common::Constants::BLOCK_SIZE, "Offsets cannot be out of block bounds");
  }
}

void TupleAccessStrategy::InitializeRawBlock(storage::DataTable *const data_table, RawBlock *const raw,
                                             const layout_version_t layout_version) const {
  // Intentional unsafe cast
  raw->data_table_ = data_table;
  raw->layout_version_ = layout_version;
  raw->insert_head_ = 0;
  auto *result = reinterpret_cast<TupleAccessStrategy::Block *>(raw);
  for (uint16_t i = 0; i < layout_.NumColumns(); i++) result->AttrOffsets()[i] = column_offsets_[i];
  result->GetArrowBlockMetadata().Initialize(GetBlockLayout().NumColumns());

  for (uint16_t i = 0; i < layout_.NumColumns(); i++) result->AttrOffets(layout_)[i] = column_offsets_[i];

  result->SlotAllocationBitmap(layout_)->UnsafeClear(layout_.NumSlots());
  result->Column(layout_, VERSION_POINTER_COLUMN_ID)->NullBitmap()->UnsafeClear(layout_.NumSlots());
  // TODO(Tianyu): This can be a slight drag on insert performance. With the exception of some test cases where GC is
  // not enabled, we should be able to do this step in the GC and still be good.

  // Also need to clean up any potential dangling version pointers (in cases where GC is off, or when a table is deleted
  // and individual tuples in it are not)
  std::memset(ColumnStart(raw, VERSION_POINTER_COLUMN_ID), 0, sizeof(void *) * layout_.NumSlots());
}

bool TupleAccessStrategy::Allocate(RawBlock *const block, TupleSlot *const slot) const {
  common::RawConcurrentBitmap *bitmap = reinterpret_cast<Block *>(block)->SlotAllocationBitmap(layout_);
  const uint32_t start = block->insert_head_;

  // We are not allowed to insert into this block any more
  if (start == layout_.NumSlots()) return false;

  uint32_t pos = start;

  while (bitmap->FirstUnsetPos(layout_.NumSlots(), pos, &pos)) {
    if (bitmap->Flip(pos, false)) {
      *slot = TupleSlot(block, pos);
      block->insert_head_++;
      return true;
    }
  }

  return false;
}
}  // namespace terrier::storage

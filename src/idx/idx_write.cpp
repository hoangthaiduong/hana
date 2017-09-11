#include "allocator.h"
#include "array.h"
#include "macros.h"
#include "math.h"
#include "error.h"
#include "idx.h"
#include "idx_common.h"
#include "utils.h"
#include <cstdint>
#include <thread>
#include <iostream>

namespace hana {

extern FreelistAllocator<Mallocator> freelist;

/** Copy data from a rectilinear grid to an idx block, assuming the samples in
both are in row-major order. Here we don't need to specify the input grid's
from/to/stride because most of the time (a subset of) the original grid is given. */
template <typename T>
struct put_grid_to_block {
void operator()(const Grid& grid, IN_OUT IdxBlock* block)
{
  Vector3i from, to;
  if (!intersect_grid(grid.extent, block->from, block->to, block->stride, &from, &to)) {
    return;
  }

  T* src = reinterpret_cast<T*>(grid.data.ptr);
  T* dst = reinterpret_cast<T*>(block->data.ptr);
  HANA_ASSERT(src && dst);
  // TODO: optimize this loop (parallelize?)
  Vector3i output_dims = (block->to - block->from) / block->stride + 1;
  uint64_t sx = output_dims.x, sxy = output_dims.x * output_dims.y;
  Vector3i input_dims = grid.extent.to - grid.extent.from + 1;
  uint64_t dx = input_dims.x, dxy = input_dims.x * input_dims.y;
  for (int z = from.z,
    k = (from.z - block->from.z) / block->stride.z; // index into the block's buffer
    z <= to.z; // loop variable and index into the grid's buffer
    z += block->stride.z, ++k) {
    for (int y = from.y,
      j = (from.y - block->from.y) / block->stride.y;
      y <= to.y;
      y += block->stride.y, ++j) {
      for (int x = from.x,
        i = (from.x - block->from.x) / block->stride.x;
        x <= to.x;
        x += block->stride.x, ++i) {
        uint64_t ijk = i + j * sx + k * sxy;
        uint64_t xyz = x + y * dx + z * dxy;
        dst[ijk] = src[xyz];
      }
    }
  }
}
};

/* Write an IDX grid at one particular HZ level.
TODO: this function overlaps quite a bit with read_idx_grid. */
// TODO: remove the last_first_block parameter and replace with simple modulo check
Error write_idx_grid_impl(
  const IdxFile& idx_file, int field, int time, int hz_level, const Grid& grid, IN_OUT FILE** file,
  IN_OUT Array<IdxBlock>* idx_blocks, IN_OUT Array<IdxBlockHeader>* block_headers,
  IN_OUT uint64_t* last_first_block)
{
  /* check the inputs */
  if (!verify_idx_file(idx_file)) { return Error::InvalidIdxFile; }
  if (field < 0 || field > idx_file.num_fields) { return Error::FieldNotFound; }
  if (time < idx_file.time.begin || time > idx_file.time.end) { return Error::TimeStepNotFound; }
  if (hz_level < 0 || hz_level > idx_file.get_max_hz_level()) { return Error::InvalidHzLevel; }
  if (!grid.extent.is_valid()) { return Error::InvalidVolume; }
  if (!grid.extent.is_inside(idx_file.box)) { return Error::VolumeTooBig; }
  HANA_ASSERT(grid.data.ptr);

  /* figure out which blocks touch this grid */
  get_block_addresses(idx_file, grid.extent, hz_level, idx_blocks);

  size_t samples_per_block = (size_t)pow2[idx_file.bits_per_block];
  size_t block_size = idx_file.fields[field].type.bytes() * samples_per_block;
  if (freelist.max_size() != block_size) {
    freelist.set_min_max_size(block_size / 2, std::max(sizeof(void*), block_size));
  }

  Error error = Error::NoError;

  size_t num_thread_max = size_t(std::thread::hardware_concurrency());
  num_thread_max = min(num_thread_max * 2, (size_t)1024);
  std::thread threads[1024];

  /* (read and) write the blocks */
  std::cout << (*block_headers)[0].offset() << "\n";
  for (size_t i = 0; i < idx_blocks->size(); i += num_thread_max) {
    int thread_count = 0;
    for (size_t j = 0; j < num_thread_max && i + j < idx_blocks->size(); ++j) {
      IdxBlock& block = (*idx_blocks)[i + j];
      std::cout << "block " << i+j << "\n";
      uint64_t first_block = 0;
      int block_in_file = 0;
      get_first_block_in_file(
        block.hz_address, idx_file.bits_per_block, idx_file.blocks_per_file, &first_block, &block_in_file);
      std::cout << "block in file = " << block_in_file << "\n";
      char bin_path[PATH_MAX]; // path to the binary file that stores the block
      StringRef bin_path_str(STR_REF(bin_path));
      get_file_name_from_hz(idx_file, time, first_block, bin_path_str);
      if (first_block != *last_first_block) { // open new file
        if (*file != nullptr) {
          fclose(*file);
        }
        *file = fopen(bin_path_str.cptr, "rb+");
      }
      Error err = Error::NoError;
      if (*file == nullptr) {
        err = Error::FileNotFound;
      }
      else { // file exists
        if (*last_first_block != first_block) { // open new file
          err = read_idx_block(
            idx_file, field, true, block_in_file, file, block_headers, &block, freelist);
        }
        else { // read the currently opened file
          if ((*block_headers)[block_in_file].offset() > 0) {
            err = read_idx_block(
              idx_file, field, false, block_in_file, file, block_headers, &block, freelist);
          }
          else {
            err = Error::BlockNotFound;
          }
        }
      }
      *last_first_block = first_block;
      IdxBlockHeader& header = (*block_headers)[block_in_file];
      if (err == Error::FileNotFound || err == Error::HeaderNotFound || err == Error::BlockNotFound) {
        if (err == Error::FileNotFound) {
          size_t last_slash = find_last(bin_path_str, STR_REF("/"));
          StringRef bin_dir_str = sub_string(bin_path_str, 0, last_slash);
          if (!dir_exists(bin_dir_str)) {
            create_full_dir(bin_dir_str);
          }
          *file = fopen(bin_path, "ab"); fclose(*file); // create the file it it does not exist
          *file = fopen(bin_path, "rb+");
        }
        block.data = freelist.allocate(block_size);
        block.bytes = static_cast<uint32_t>(block_size);
        block.compression = Compression::None;
        block.type = idx_file.fields[field].type;
        header.set_bytes(block.bytes);
        header.set_format(Format::RowMajor);
        size_t header_size = sizeof(IdxFileHeader) + sizeof(IdxBlockHeader) * idx_file.blocks_per_file * idx_file.num_fields;
        fseek(*file, 0, SEEK_END);
        size_t file_size = ftell(*file);
        size_t offset = std::max(header_size, file_size);
        header.set_offset(static_cast<int64_t>(offset));
        std::cout << "write: offset = " << header.offset() << "\n";
        header.set_compression(block.compression); // TODO
      }
      else if (err == Error::InvalidCompression || err == Error::BlockReadFailed) {
        error = err; // critical errors
        goto END;
      }
      if (block.compression != Compression::None) { // TODO
        error = Error::CompressionUnsupported;
        goto END;
      }
      forward_functor<put_grid_to_block, int>(block.type.bytes(), grid, &block);

      /* write the block to disk */
      fseek(*file, header.offset(), SEEK_SET);
      if (fwrite(block.data.ptr, block.bytes, 1, *file) != 1) {
        error = Error::BlockWriteFailed;
        goto END;
      }

      /* if i am the last block in the file, write all the block headers for the file */
      if (block_in_file - first_block + 1 == idx_file.blocks_per_file) {
        // TODO: what if we never write the last block of the file?
        for (size_t k = 0; k < block_headers->size(); ++k) {
          (*block_headers)[k].swap_bytes();
        }
        size_t offset = sizeof(IdxFileHeader) + sizeof(IdxBlockHeader) * idx_file.blocks_per_file * field;
        fseek(*file, offset, SEEK_SET);
        if (fwrite(&(*block_headers)[0], sizeof(IdxBlockHeader), idx_file.blocks_per_file, *file) != idx_file.blocks_per_file) {
          error = Error::HeaderWriteFailed;
          goto END;
        }
      }
    }
  }
END:
  return error;
}

Error write_idx_grid(
  const IdxFile& idx_file, int field, int time, int hz_level, const Grid& grid)
{
  Mallocator mallocator;
  Array<IdxBlock> idx_blocks(&mallocator);
  Array<IdxBlockHeader> block_headers(&mallocator); // all headers for one file
  block_headers.resize(idx_file.blocks_per_file);
  FILE* file = nullptr;
  uint64_t last_first_block = (uint64_t)-1;
  Error error = write_idx_grid_impl(idx_file, field, time, hz_level, grid, &file, &idx_blocks, &block_headers, &last_first_block);
  if (file != nullptr) {
    fclose(file);
  }
  return error;
}

Error write_idx_grid(
  const IdxFile& idx_file, int field, int time, const Grid& grid)
{
  HANA_ASSERT(grid.data.ptr != nullptr);
  Mallocator mallocator;
  Array<IdxBlock> idx_blocks(&mallocator);
  Array<IdxBlockHeader> block_headers(&mallocator); // all headers for one file
  block_headers.resize(idx_file.blocks_per_file);
  int min_hz = idx_file.get_min_hz_level();
  int max_hz = idx_file.get_max_hz_level();
  FILE* file = nullptr;
  uint64_t last_first_block = (uint64_t)-1;
  Error error = write_idx_grid_impl(idx_file, field, time, min_hz-1, grid, &file, &idx_blocks, &block_headers, &last_first_block);
  std::cout << block_headers[0].offset();
  if (error.code != Error::NoError) {
    goto END;
  }
  for (int l = min_hz; l <= max_hz; ++l) {
    error = write_idx_grid_impl(idx_file, field, time, l, grid, &file, &idx_blocks, &block_headers, &last_first_block);
    if (error.code != Error::NoError) {
      goto END;
    }
  }
END:
  if (file != nullptr) {
    fclose(file);
  }
  return error;
}

}

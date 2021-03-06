/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <memory>
#include <mutex>
#include <string>

#include <unwindstack/Elf.h>
#include <unwindstack/MapInfo.h>
#include <unwindstack/Maps.h>
#include <unwindstack/Memory.h>

namespace unwindstack {

bool MapInfo::InitFileMemoryFromPreviousReadOnlyMap(MemoryFileAtOffset* memory) {
  // One last attempt, see if the previous map is read-only with the
  // same name and stretches across this map.
  for (auto iter = maps_->begin(); iter != maps_->end(); ++iter) {
    if (*iter == this) {
      if (iter == maps_->begin()) {
        return false;
      }
      --iter;
      MapInfo* prev_map = *iter;
      // Make sure this is a read-only map.
      if (prev_map->flags != PROT_READ) {
        return false;
      }
      uint64_t map_size = end - prev_map->end;
      if (!memory->Init(name, prev_map->offset, map_size)) {
        return false;
      }
      uint64_t max_size;
      if (!Elf::GetInfo(memory, &max_size) || max_size < map_size) {
        return false;
      }
      if (!memory->Init(name, prev_map->offset, max_size)) {
        return false;
      }
      elf_offset = offset - prev_map->offset;
      return true;
    }
  }
  return false;
}

Memory* MapInfo::GetFileMemory() {
  std::unique_ptr<MemoryFileAtOffset> memory(new MemoryFileAtOffset);
  if (offset == 0) {
    if (memory->Init(name, 0)) {
      return memory.release();
    }
    return nullptr;
  }

  // These are the possibilities when the offset is non-zero.
  // - There is an elf file embedded in a file, and the offset is the
  //   the start of the elf in the file.
  // - There is an elf file embedded in a file, and the offset is the
  //   the start of the executable part of the file. The actual start
  //   of the elf is in the read-only segment preceeding this map.
  // - The whole file is an elf file, and the offset needs to be saved.
  //
  // Map in just the part of the file for the map. If this is not
  // a valid elf, then reinit as if the whole file is an elf file.
  // If the offset is a valid elf, then determine the size of the map
  // and reinit to that size. This is needed because the dynamic linker
  // only maps in a portion of the original elf, and never the symbol
  // file data.
  uint64_t map_size = end - start;
  if (!memory->Init(name, offset, map_size)) {
    return nullptr;
  }

  // Check if the start of this map is an embedded elf.
  uint64_t max_size = 0;
  uint64_t file_offset = offset;
  if (Elf::GetInfo(memory.get(), &max_size)) {
    if (max_size > map_size) {
      if (memory->Init(name, file_offset, max_size)) {
        return memory.release();
      }
      // Try to reinit using the default map_size.
      if (memory->Init(name, file_offset, map_size)) {
        return memory.release();
      }
      return nullptr;
    }
    return memory.release();
  }

  // No elf at offset, try to init as if the whole file is an elf.
  if (memory->Init(name, 0) && Elf::IsValidElf(memory.get())) {
    elf_offset = offset;
    return memory.release();
  }

  // See if the map previous to this one contains a read-only map
  // that represents the real start of the elf data.
  if (InitFileMemoryFromPreviousReadOnlyMap(memory.get())) {
    return memory.release();
  }

  // Failed to find elf at start of file or at read-only map, return
  // file object from the current map.
  if (memory->Init(name, offset, map_size)) {
    return memory.release();
  }
  return nullptr;
}

Memory* MapInfo::CreateMemory(const std::shared_ptr<Memory>& process_memory) {
  if (end <= start) {
    return nullptr;
  }

  elf_offset = 0;

  // Fail on device maps.
  if (flags & MAPS_FLAGS_DEVICE_MAP) {
    return nullptr;
  }

  // First try and use the file associated with the info.
  if (!name.empty()) {
    Memory* memory = GetFileMemory();
    if (memory != nullptr) {
      return memory;
    }
  }

  // Need to verify that this elf is valid. It's possible that
  // only part of the elf file to be mapped into memory is in the executable
  // map. In this case, there will be another read-only map that includes the
  // first part of the elf file. This is done if the linker rosegment
  // option is used.
  std::unique_ptr<MemoryRange> memory(new MemoryRange(process_memory, start, end - start, 0));
  if (Elf::IsValidElf(memory.get())) {
    return memory.release();
  }

  if (name.empty() || maps_ == nullptr) {
    return nullptr;
  }

  // Find the read-only map by looking at the previous map. The linker
  // doesn't guarantee that this invariant will always be true. However,
  // if that changes, there is likely something else that will change and
  // break something.
  MapInfo* ro_map_info = nullptr;
  for (auto iter = maps_->begin(); iter != maps_->end(); ++iter) {
    if (*iter == this) {
      if (iter != maps_->begin()) {
        --iter;
        ro_map_info = *iter;
      }
      break;
    }
  }

  if (ro_map_info == nullptr || ro_map_info->name != name || ro_map_info->offset >= offset) {
    return nullptr;
  }

  // Make sure that relative pc values are corrected properly.
  elf_offset = offset - ro_map_info->offset;

  MemoryRanges* ranges = new MemoryRanges;
  ranges->Insert(new MemoryRange(process_memory, ro_map_info->start,
                                 ro_map_info->end - ro_map_info->start, 0));
  ranges->Insert(new MemoryRange(process_memory, start, end - start, elf_offset));

  return ranges;
}

Elf* MapInfo::GetElf(const std::shared_ptr<Memory>& process_memory, ArchEnum expected_arch) {
  // Make sure no other thread is trying to add the elf to this map.
  std::lock_guard<std::mutex> guard(mutex_);

  if (elf.get() != nullptr) {
    return elf.get();
  }

  bool locked = false;
  if (Elf::CachingEnabled() && !name.empty()) {
    Elf::CacheLock();
    locked = true;
    if (Elf::CacheGet(this)) {
      Elf::CacheUnlock();
      return elf.get();
    }
  }

  Memory* memory = CreateMemory(process_memory);
  if (locked) {
    if (Elf::CacheAfterCreateMemory(this)) {
      delete memory;
      Elf::CacheUnlock();
      return elf.get();
    }
  }
  elf.reset(new Elf(memory));
  // If the init fails, keep the elf around as an invalid object so we
  // don't try to reinit the object.
  elf->Init();
  if (elf->valid() && expected_arch != elf->arch()) {
    // Make the elf invalid, mismatch between arch and expected arch.
    elf->Invalidate();
  }

  if (locked) {
    Elf::CacheAdd(this);
    Elf::CacheUnlock();
  }
  return elf.get();
}

uint64_t MapInfo::GetLoadBias(const std::shared_ptr<Memory>& process_memory) {
  uint64_t cur_load_bias = load_bias.load();
  if (cur_load_bias != static_cast<uint64_t>(-1)) {
    return cur_load_bias;
  }

  {
    // Make sure no other thread is trying to add the elf to this map.
    std::lock_guard<std::mutex> guard(mutex_);
    if (elf != nullptr) {
      if (elf->valid()) {
        cur_load_bias = elf->GetLoadBias();
        load_bias = cur_load_bias;
        return cur_load_bias;
      } else {
        load_bias = 0;
        return 0;
      }
    }
  }

  // Call lightweight static function that will only read enough of the
  // elf data to get the load bias.
  std::unique_ptr<Memory> memory(CreateMemory(process_memory));
  cur_load_bias = Elf::GetLoadBias(memory.get());
  load_bias = cur_load_bias;
  return cur_load_bias;
}

}  // namespace unwindstack

// reach - keep file sizes current after writes.
//
// The runtime caches each host file's size in its VFS entry and refreshes it only
// for some NtSetInformationFile classes, so a file that NtWriteFile extends keeps
// its old size in NtQueryFullAttributesFile. File Share downloads write the file in
// 4 KB pieces and then size it by its attributes: the game saw 4096 of 8192 bytes,
// a chunk ran past the end and the transfer failed. This wraps NtWriteFile and
// refreshes the entry when the write moved the file position past the cached size.

#include "../platform/sdk_import.h"

#include <rex/filesystem/entry.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xfile.h>


#include <cstdint>

namespace {
using GuestFunc = void (*)(PPCContext&, uint8_t*);
}  // namespace

// NTSTATUS NtWriteFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, PVOID apc_context,
//     PIO_STATUS_BLOCK io_status, PVOID buffer, ULONG length, PLARGE_INTEGER byte_offset)
extern "C" REX_FUNC(__imp__NtWriteFile) {
  static GuestFunc sdk = reach::SdkImport("__imp__NtWriteFile");
  const uint32_t handle = ctx.r3.u32, length = ctx.r9.u32, offset_ptr = ctx.r10.u32;
  uint64_t end = 0;  // where the write ends, if it has an explicit offset
  if (offset_ptr) {
    for (int i = 0; i < 8; ++i) end = end << 8 | base[offset_ptr + i];
    end += length;
  }
  if (sdk) sdk(ctx, base);
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return;
  auto file = kernel->object_table()->LookupObject<rex::system::XFile>(handle);
  if (!file) return;
  if (!offset_ptr) end = file->position();
  rex::filesystem::Entry* entry = file->entry();
  if (entry && end > entry->size()) entry->update();
}

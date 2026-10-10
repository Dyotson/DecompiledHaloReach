// reach - honour delete-on-close for directories.
//
// At boot Reach opens cache1:\webcache, sets FileDispositionInformation
// (delete on close) on it, closes the handle and immediately creates the
// directory again. The runtime records the flag (Entry::SetForDeletion) but
// does not delete a directory when its last handle closes, so the re-create
// fails with STATUS_OBJECT_NAME_COLLISION and the game's web cache (object at
// 0x82ABE090) never initializes. Xenia deletes it. This wraps NtClose: when the
// handle refers to a directory marked for deletion and the entry still exists
// after the SDK closed it, delete it through the VFS (which removes the host
// directory too).

#include "../platform/sdk_import.h"

#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xfile.h>


#include <cstdint>
#include <string>

namespace {
using GuestFunc = void (*)(PPCContext&, uint8_t*);
}  // namespace

// NTSTATUS NtClose(HANDLE handle)
extern "C" REX_FUNC(__imp__NtClose) {
  static GuestFunc sdk = reach::SdkImport("__imp__NtClose");

  std::string pending_path;
  if (auto* kernel = rex::system::kernel_state()) {
    auto file = kernel->object_table()->LookupObject<rex::system::XFile>(ctx.r3.u32);
    if (file) {
      rex::filesystem::Entry* entry = file->entry();
      if (entry && entry->delete_on_close() &&
          (entry->attributes() & rex::filesystem::kFileAttributeDirectory)) {
        pending_path = entry->absolute_path();
      }
    }
  }

  if (sdk) sdk(ctx, base);

  if (!pending_path.empty()) {
    auto* vfs = rex::system::kernel_state()->file_system();
    if (rex::filesystem::Entry* still = vfs->ResolvePath(pending_path)) {
      bool deleted = still->Delete();
      REXLOG_INFO("NtClose: deleted directory marked delete-on-close: {} ({})", pending_path,
                  deleted ? "ok" : "failed");
    }
  }
}

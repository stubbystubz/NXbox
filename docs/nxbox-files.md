# Files on Xbox

**Files** replaces the Shop tab. It uses the same two-pane focus model as Settings:
choose a root on the left, then press A or Right to browse on the right.

- **Console** is the app's LocalState: `games`, `eden/keys`, `firmware`, recordings,
  saves and other app data.
- **hd**, **hd2**, etc. expose each external drive's **NXbox folder**, matching FTP.
  The drive display name appears when available. Discovery follows
  `ExternalGameFolders()` order; re-enter Files to discover connected drives again.
- **FTP** is the last rail entry. A toggles the server from its right pane. The
  panel shows its address, credentials and connected peer. See [FTP](nxbox-ftp.md).

## Controller

A opens a folder. B goes up; at the root it returns to the rail, then to the top
tabs. Left returns to the rail. Up from Console also returns to the top tabs.
LB/RB page through eight rows; on the top navigation they switch tabs.
Y cycles name, size and modified-date sorting. Folders always come first.

X opens **Copy to…**, **Move to…**, **Rename**, **Delete** and **New folder**.
Rename and New folder use the existing controller keyboard. Select a copy/move
destination on any root, enter its folder with A, and press X to use that folder.
B cancels destination selection from the rail. For name clashes, A explicitly
confirms replacement of a file; Y keeps both as `name (2).ext`, `name (3).ext`, etc.
Folders are kept separately rather than merged or replaced. Copying an item onto
itself or a folder into its own subtree is refused. Delete requires a separate
confirmation and recursively removes a folder's contents.

## I/O and recovery

Discovery, listings, FTP toggling and mutations run on an MTA worker. Rendering
polls completion without waiting. Transfers use a 1 MiB buffer, a progress bar,
and B cancellation between chunks. They use the same `FromApp` enumeration and
`Common::FS::IOFile` broker fallback as FTP, plus StorageFolder/StorageFile for
removable-drive names and publication. No external-drive operation uses plain
`std::filesystem` I/O.

A copy first writes a unique sibling `.partial` tree and publishes it after all
bytes are flushed. A cancelled or failed copy cleans up staging and preserves an
existing destination. A move deletes its source only after publication. Cancelling
during source cleanup can leave both copies or a partially removed source tree;
the completed destination remains available. Recursive delete is not transactional:
cancellation can leave some source items already deleted. A drive unplugged during
cleanup may retain a `.partial` tree; reconnect and review it before deleting it.

Roots cannot be renamed or deleted. Paths cannot escape a root, use ADS/device
names, or follow reparse points. A directory containing a reparse point is refused
rather than silently skipped during a destructive operation. Operations notify the
same change counter used by FTP to schedule a library rescan, including failures
that may have changed part of a tree.

## Console verification

Host tests cover normalization, traversal rejection, ordering, human byte counts,
transfer planning and name collisions. Windows/UWP compilation and Xbox checks
remain necessary: both languages, controller focus and keyboard, empty/error states,
hot-plugged drives and display names, recursive transfers in both directions,
large files, cancellation, disk-full/unplugged-drive recovery, overwrite/delete
confirmation, reparse rejection, FTP peer updates, autostart, suspension and rescans.

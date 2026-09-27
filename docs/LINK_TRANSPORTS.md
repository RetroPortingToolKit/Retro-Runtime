# Link transports: Linux, macOS, Windows

**Status: approved by Alex and built, 2026-09-26.** Proof so far, following §9:
- **Linux:** the refactor onto `transport.hpp` is byte-identical to the
  2026-09-25 link. That covers 4 scenarios on `pokemonstadium_core.so`, all 8
  artifacts of each, `runner.log` and the Transfer Pak save included.
- **Windows:** the whole ctest suite passes under Wine (a MinGW build),
  including the crash test.
- **Windows (MSVC) and macOS 14:** the whole ctest suite passes in CI
  (run 36218382611), crash test included. The 78 layout `static_assert`s hold
  under MSVC.
- **Still unproven:** fds landing on the right message when that message is
  split across several reads on macOS (§3). `SaveRegions` is about 1.2 KiB and
  arrives in one read, so no test splits it yet.
- **Not tested at all yet:** a real core on macOS or Windows; the Windows job
  object ending a runner stuck in `run_frame` when the hub is killed.

`CORE_LINK.md` describes the link as it was built on Linux. This page is how
the same link runs on macOS and Windows. The approach is to change only what
is Linux-specific, and leave the protocol exactly as it is.

## 1. What must not change

The link's behavior is already checked byte for byte: through n64lle's
`core_parity.sh`, all 10 scenarios match `rcore_probe` exactly. Every property
below either carries that result or keeps a save safe, so every transport
must keep all of them:

| Property | Why it is load-bearing |
|---|---|
| One message per packet, with the whole packet or nothing | `as_msg` reads whole packets (since 1.1, a longer one's tail is ignored); a torn message is a malformed session |
| Handles travel with the message that names them | `SaveRegions` carries one memory handle per region, in order |
| **The hub sees EOF when the runner dies**, however it dies | The only way a hub learns of a crash mid-frame; saves are written on it |
| The shared region and the save memory are the **hub's** memory | A runner crash cannot lose the picture, the queued audio or a save |
| The hub never blocks on the runner | Frames use the lock-free triple buffer and audio the SPSC ring; pausing is not granting |
| **Two processes** | Workspace ruling: code under test and host never share a process |

The **protocol stays 1.0 on every OS.** The messages, `SharedHeader`, the
triple buffer, the audio ring and the session order are unchanged. A transport
is fixed per OS: a host and a runner on the same machine always share one. So
the transport is part of the platform, not a protocol version. Changing an
OS's transport later is a major bump.

## 2. The three transports

| | Linux (built) | macOS | Windows |
|---|---|---|---|
| Control channel | `socketpair(AF_UNIX, SOCK_SEQPACKET)` | `socketpair(AF_UNIX, SOCK_STREAM)`, framed by `MsgHeader.size` | Named pipe in **message mode**; the hub opens both ends before spawning |
| Packet boundaries | Kernel | Framing (§3) | Kernel (`PIPE_TYPE_MESSAGE`) |
| Runner-death EOF | `recv` → 0 | `recv` → 0 | `ReadFile` → `ERROR_BROKEN_PIPE` |
| Shared region | `memfd_create` | `shm_open` under a random name, then `shm_unlink` at once | `CreateFileMapping(INVALID_HANDLE_VALUE)`, unnamed |
| Save memory, runner → hub | `memfd` per region, `SCM_RIGHTS` | `shm_open` + `shm_unlink` per region, `SCM_RIGHTS` | Unnamed section per region; the hub `DuplicateHandle`s it out of the runner (§4) |
| Handing the runner its channels | Inherited at fds 3 and 4 | Inherited at fds 3 and 4 | Inherited through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`; values passed as `--link-handles <control>,<shared>` |
| Spawn | `fork` + `execve` | `posix_spawn` with `POSIX_SPAWN_CLOEXEC_DEFAULT` | `CreateProcessW` + `STARTUPINFOEXW`, `CREATE_NO_WINDOW` |
| Wait / kill | `waitpid`, `SIGKILL` | `waitpid`, `SIGKILL` | `WaitForSingleObject(process)`, `TerminateProcess` |
| Exit code shown | code, or `-signal` | code, or `-signal` | code; a crash is an `NTSTATUS` shown in hex (`0xC0000005`) |

### Why these, and not the alternatives

- **macOS has no `SOCK_SEQPACKET` for `AF_UNIX`, and no `memfd`.**
  `SOCK_DGRAM` would keep boundaries, but a datagram socket gives no EOF when
  its peer dies, and that EOF is how the hub detects a crash. So it is a stream
  socket with framing. `shm_open` is chosen over an unlinked temp file because
  it is memory-backed: an unlinked file under `$TMPDIR` can still have its
  dirty pages written back, and the frame slots change 60 times a second.
- **Windows: a named pipe in message mode, not an anonymous pipe or `AF_UNIX`.**
  Anonymous pipes are byte streams and do not support overlapped I/O. Windows
  `AF_UNIX` is stream-only and cannot pass handles. A message-mode pipe gives
  SEQPACKET's semantics directly. The name is only used for the moment of
  creation:
  - the hub creates the server end with `FILE_FLAG_FIRST_PIPE_INSTANCE`,
    `nMaxInstances = 1`, `PIPE_REJECT_REMOTE_CLIENTS`, and a DACL that allows
    only the current user;
  - the name is `\\.\pipe\retro-core-link-<hub pid>-<128 random bits>`;
  - the hub opens the client end itself, before spawning.

  If a name already exists, `FIRST_PIPE_INSTANCE` fails and the hub picks a new
  one. After that, the single instance is already connected. So nothing else
  can squat the pipe or connect to it.
- **An unnamed, inherited mapping on Windows, not a named one.**
  `HOST_LIFECYCLE.md` §4 sketched a named file mapping. That adds a global
  namespace entry that another process could open. Inheriting through an
  explicit handle list has neither problem, so this page supersedes that line
  of the sketch.
- **`posix_spawn` on macOS.** `POSIX_SPAWN_CLOEXEC_DEFAULT` (a macOS extension)
  means the runner inherits only the fds that are listed. That enforces what
  the Linux code achieves by marking every fd `CLOEXEC`. Linux keeps
  `fork`/`execve` as it is.

## 3. Framing on macOS

The stream carries the same bytes as a SEQPACKET packet, back to back. Each
message already begins with `MsgHeader { type; size }`, and `size` is the whole
packet, so no new header is needed.

- **Sending:** one `sendmsg` loop until every byte has gone. Any fds ride as
  `SCM_RIGHTS` on the first `sendmsg` of that message.
- **Receiving:** each channel keeps a reassembly buffer.
  - `recv_packet` reads with `recvmsg` into that buffer, collecting fds from
    every call's control data.
  - It returns a packet only when `size` bytes are present.
  - A `size` below `sizeof(MsgHeader)` or above `kMaxMsgSize` is `Error`: the
    stream is unrecoverable, as a torn SEQPACKET would be.
  - With `block = false`, a partial message returns `WouldBlock` and keeps its
    bytes.
- **Fds and packets:** fds are assigned to the packet whose first byte arrived
  with them. BSD should deliver ancillary data together with the bytes it was
  sent with, not before them. **Not verified on macOS yet.** The first macOS
  test sends `SaveRegions` split across several `recvmsg` calls and checks
  which packet the fds land on.
- **macOS lacks** `MSG_NOSIGNAL`, `SOCK_CLOEXEC` and `MSG_CMSG_CLOEXEC`.
  Instead, use `SO_NOSIGPIPE` on both ends, and `fcntl(FD_CLOEXEC)` on the
  socketpair and on every fd received.
- **Buffer sizes:** raise `SO_SNDBUF` and `SO_RCVBUF` to 256 KiB. The macOS
  `AF_UNIX` default is 8 KiB, which a burst of `Log` messages (about 1 KiB
  each) would fill.

## 4. Handles on Windows

A Windows handle means nothing in another process until it is duplicated into
that process. The hub holds the runner's process handle, with full access,
from `CreateProcessW`. So **the hub always performs the duplication, in both
directions.** The runner is never given a handle to the hub process.

- **Runner → hub** (save memory, today's only case): the runner creates
  `CreateFileMapping(INVALID_HANDLE_VALUE, …, size)` for each region and sends
  their handle values. The hub calls `DuplicateHandle(runner, value, self, …,
  DUPLICATE_SAME_ACCESS)` for each one. The runner keeps its handles open for
  the whole session: it holds the mapping anyway, and an open section handle
  costs nothing. If the runner dies before the hub duplicates, the duplication
  fails, and the hub treats that as a malformed `SaveRegions`. No save is lost,
  because none has been filled yet.
- **Hub → runner** (for future messages): the hub duplicates into the runner
  first, then sends the resulting values.

**The handle values travel in a transport trailer, not in the message.** On
the wire, a Windows packet is the message bytes followed by
`{ uint32 count; uint32 pad; uint64 handle[count] }`, and only when `count > 0`.
`recv_packet` strips the trailer, duplicates the handles, and returns them in
the same `fds` vector that `SCM_RIGHTS` fills on POSIX. The receiver tells a
message from its trailer by `MsgHeader.size`. So `SaveRegionsMsg` and every
other message stay byte-identical across the three OSes, and
`core_link.cpp` / `runner_link.cpp` never see the difference.

The hub puts the runner in a **job object** with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, so a hub that is killed takes the runner
with it. A broken pipe already ends a runner that is waiting for a grant. The
job also ends one that is stuck inside `run_frame`.

## 5. Code shape

Move everything OS-specific behind one small interface, and make the session
code OS-free:

```
corelink/transport.hpp        NativeHandle, Channel, SharedMemory, RunnerProcess
corelink/transport_posix.cpp  Linux + macOS: the two differ in socket type, memfd vs
                              shm_open, and the framing layer (#if __APPLE__)
corelink/transport_win32.cpp  Windows
corelink/link_io.*            send_packet / recv_packet over a Channel (moves onto transport)
corelink/core_link.cpp        hub session: no <sys/*.h>, no fork, no mmap
runner/runner_link.cpp        runner session: no <sys/*.h>, no memfd
```

```cpp
// The interface (sketch). NativeHandle is int on POSIX, HANDLE (as uintptr_t) on Windows.
SharedMemory create_shared(std::size_t size);                  // hub region, save regions
SharedMemory map_shared(NativeHandle h, std::size_t size);     // the other side's mapping
bool send_packet(Channel&, const void* msg, std::size_t size,
                 const NativeHandle* handles = nullptr, std::size_t n = 0);
RecvResult recv_packet(Channel&, std::vector<unsigned char>& buf,
                       std::vector<NativeHandle>* handles, int timeout_ms); // -1 blocks, 0 polls
bool spawn_runner(const SpawnSpec&, Channel& hub_end, const SharedMemory& region,
                  RunnerProcess& out, std::string* error);
std::optional<int> wait_exit(RunnerProcess&, int timeout_ms);  // replaces reap()
void kill_runner(RunnerProcess&);
Channel runner_channel(const LinkArgs&);   // fd 3, or --link-handles on Windows
```

`recv_packet` taking a timeout folds `pump()`'s separate `poll()` into the
transport. On Windows that is an overlapped `ReadFile` on the hub end, waited
on with its event. The runner end is opened synchronously, because the runner
only ever blocks on the next message. `wait_exit` with a timeout replaces
`stop()`'s 10 ms sleep loop. On Windows it is `WaitForSingleObject`. On POSIX
it is `pidfd` on Linux and `kqueue` `EVFILT_PROC` on macOS. That also honors
`HOST_LIFECYCLE.md` §4's "nothing polls with sleeps", which `stop()` breaks
today.

**Linux behavior does not change.** The refactor must be proven that way: rerun
the 2026-09-25 parity set (10 `check` scenarios and 2 `replay` scenarios, on
the same core) and require byte-identical artifacts before any other OS lands.

## 6. Pinning the wire layout across compilers

On Windows the hub is built with MSVC (Retro Launcher's `windows-2022` job).
The runner could be built with MSVC or MinGW. So every shared struct gets
`static_assert`s on its `sizeof` and on each field's `offsetof`, in
`link_protocol.hpp`:
- `SharedHeader`, `FrameInfo`, every `*Msg`, `RegionDesc`;
- `rcore_pad`, as embedded in `GrantMsg` and `SavesFilledMsg`.

The expected numbers are the ones the Linux GCC build produces today. A
compiler that lays them out differently then fails to build, instead of
corrupting a session. Every shared `std::atomic` is already asserted
lock-free; add `alignof` checks next to those.

## 7. What else the runner needs on each OS

These are separate from the transport, but a runner cannot ship without them.

| | macOS | Windows |
|---|---|---|
| **Loading the core, and its identity** | There is no `/proc/self/fd`, so the hash-then-`dlopen` trick has no direct equivalent. Instead, copy the library from the one open fd into a private `0700` temporary directory, hashing while copying, and `dlopen` the copy. The identity still names the exact bytes that run. A signed dylib keeps its signature in the file. | Already written, not yet run. It holds the file open with `FILE_SHARE_READ` only, so the file cannot be written, renamed or deleted between hashing and `LoadLibraryW`. Add `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR \| LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`, so a core's own DLLs resolve beside it and the working directory is never searched. |
| **Lent GL** | **macOS OpenGL stops at 4.1, and `GL_COMPUTE` needs 4.3** (`CORE_ABI.md`, ruling 1). The runner must ask for 4.3 core. If it cannot get that, it lends **no** context. Under the ABI a `GL_COMPUTE` core then runs its software rasterizer and says so in `log()`. That is honest, but slower. **n64lle on a Mac will use the software rasterizer.** A faster path would need a different lent API (Metal, or Vulkan through MoltenVK). That is an ABI change and needs your ruling, not a transport change. | WGL through SDL3's hidden window, with a 4.3 core request, the same as Linux should make. |
| **The same GL rule on Linux** | `lend_gl_context` asks SDL for no particular version today. It should ask for 4.3 core and lend nothing below that on every OS. Otherwise a 3.x context could be lent to a core that assumes compute. | |
| **Sidecar `library`** | `.dylib` | `.dll` |
| **Paths** | UTF-8 already | `argv` arrives in the ANSI code page. Use `wmain` / `GetCommandLineW`, convert to UTF-8 for the core, and to wide strings for Win32 calls. Quote the spawn command line by the MSVCRT rules, so paths with spaces arrive whole. |
| **Signing and quarantine** | arm64 needs a signature; the linker's ad-hoc one is enough to run. If the runner is ever signed with the hardened runtime, it needs `com.apple.security.cs.disable-library-validation`, because cores are signed by others. A file the launcher downloads with libcurl gets no quarantine flag. | An unsigned exe started by the hub through `CreateProcessW` gets no SmartScreen prompt. That prompt comes from Explorer launching a file marked as downloaded. The launcher's downloader must not add that mark. |

The fake core's sidecar currently hard-codes `library = "fake_core.so"`. It
becomes `$<TARGET_FILE_NAME:rcore_fake_core>`.

## 8. Releases

Extending `RELEASES.md`:

| Platform | Built on | Archive | Imports gate | Floor recorded |
|---|---|---|---|---|
| `macos-arm64`, `macos-x86_64` | `macos-14`, as one universal binary (`CMAKE_OSX_ARCHITECTURES="arm64;x86_64"`); the x86_64 slice tested under Rosetta | `.tar.gz`, executable bit kept | `otool -L`: only `/usr/lib/libSystem.B.dylib`, `/usr/lib/libc++.1.dylib` and `/System/Library/Frameworks/*` | `requires.macos` from `LC_BUILD_VERSION` `minos` |
| `windows-x86_64` | `windows-2022`, MSVC, static CRT (`/MT`) | `.zip` | `dumpbin /dependents`: an allowlist of system DLLs; no MSVC runtime DLL, no SDL3 DLL | `requires.windows` from the PE's subsystem version |

- Both manifest keys, `macos-arm64` and `macos-x86_64`, point at the one
  universal archive. The launcher already ships both Mac architectures. Every
  host keeps choosing by its own platform key, so the manifest format does not
  change.
- Every gate from `package-release.sh` carries over per OS: the allowlist, the
  version read back from the extracted archive, and the fake core run headless
  and over the link from that extracted copy.

`ci.yml` gains `windows-2022` and `macos-14` jobs that run `ctest`, including
`link_fake_core`. That is the test proving each transport carries a session.

Windows on arm64 (`windows-11-arm`) can come later as one more matrix row.

## 9. How it will be proven

In order, each step a gate for the next:

1. **Refactor onto `transport.hpp`, Linux only.** Byte-identical parity on the
   2026-09-25 set.
2. **macOS transport.** `ctest` passes on `macos-14`, then a crash test: a
   runner `SIGKILL`ed mid-session must produce EOF at the hub and a rewritten
   save.
3. **Windows transport.** `ctest` passes on `windows-2022`, then the same crash
   test with `TerminateProcess`, plus a hub killed with the runner stuck
   inside `run_frame`: the job object must end the runner.
4. **Parity per OS**, where the core builds: the same `core_parity.sh` artifacts
   over the link, byte-identical to that OS's headless run. Headless output is
   the oracle; the link must not change a single byte.
5. **The picture, the sound and controller feel** on a real Mac and a real
   Windows machine are your verdict, as they are on Linux.

Hub-side work sits in Retro Launcher: `hub_play.cpp` is compiled only on
Linux (`CMakeLists.txt:380`). It must build on Windows and macOS once
`retro_corelink` does, and it must launch the runner from the updated location
(`RELEASES.md`, the update rule).

## 10. Found while reading, not caused by this design

- **The minor-version rule is not implemented by the code.**
  - `CORE_LINK.md` says a minor may append fields at the end of a message.
  - But `as_msg` accepts only an exact size, and every sender sends
    `sizeof(M)`.
  - So a 1.1 runner that appended a field to `FrameDone` would have every
    `FrameDone` silently dropped by a 1.0 hub, and the session would hang
    waiting for a frame.

  The fix is one of two:
  - each sender sizes its messages by the session's minor;
  - or `as_msg` accepts a longer packet and ignores the tail.

  This design adds no fields, so it does not depend on the fix. It must land
  before the first minor bump. Filed at the claim site in `CORE_LINK.md`.

  **Resolved 2026-09-26, with 1.1** (the first minor bump): `as_msg` now
  ignores a longer packet's tail and zero-fills a shorter one down to a
  message's pre-append size. A message that grows must still be sent at its old
  size to a 1.0 peer. 1.1 grows none; it adds message types
  (`CORE_LINK.md`, "Versioning").
- `CORE_LINK.md` names `src/corelink/`, `src/runner/` and `src/hub/`. Since the
  split they are `corelink/`, `runner/`, and Retro Launcher's `src/hub/`.

## 11. Rulings (Alex, 2026-09-26)

1. **The transports are approved as designed:** stream socket with framing on
   macOS; message-mode named pipe, inherited sections and hub-side
   `DuplicateHandle` on Windows.
2. **The Mac runner ships now.** On macOS a `GL_COMPUTE` core runs its software
   rasterizer, as the ABI already provides. Metal and Vulkan lending come
   later, as an ABI change.
3. **One universal Mac binary** serves both `macos-arm64` and `macos-x86_64`.
   Players never start the runner directly. Retro Launcher starts it, either
   the desktop app or Direct mode for a standalone title.

# Shared GPU frames

rcore draft 11 and CoreLink 2.4 add optional separate-process presentation.
CPU RGBA8 frames remain the portable contract. A host or core without the
extension continues using them. This is not a simulation profile and does
not change build, content, configuration or rollback identity checks.

The first implementation shares Windows Vulkan RGBA8 images with an OpenGL
frontend using opaque NT memory and binary semaphore handles. Other platforms
and APIs decline through the same interface. No process-local texture name
crosses the link. See the normative structures and comments in
`include/rcore/rcore.h`; the C table and Rust mirror retain their old prefixes.

## Setup and ownership

The frontend samples actual GL extensions, adapter UUID, LUID, node mask and
texture limits once per session. It sends `GpuCapabilities` after Hello and
before initialization. The runner copies this offer; core callbacks do not
poll the transport. Missing support, mismatched adapters, invalid geometry or
allocation sizes decline to CPU frames. OS version and GPU names are not
capability evidence.

The core owns up to three dedicated images per generation. Each image has
ready and released binary semaphores. A registration describes immutable
geometry, format, adapter identity and allocations. The Windows transport
duplicates nine handles into the receiving process; raw producer handle values
are zeroed on the wire. Every layer keeps its own references before returning
from a callback. The receiver closes received handles after importing them.
At most two generations may exist during resize and retirement.

For each image:

1. The producer completes its ready signal before publishing the frame. A
   queued signal alone cannot protect the frontend against producer death.
2. The frontend waits on ready on its GPU, copies the image into its owned
   display texture, signals released, inserts a fence and flushes commands.
3. Only a completed frontend fence permits a CPU release acknowledgment.
   The acknowledgment identifies the generation, image and submitted link
   frame number. A stale acknowledgment cannot free a reused image.
4. Only then may the producer wait on released and reuse the image. When
   no acknowledged image is available, it emits an ordinary CPU frame.

The frontend consumes and releases obsolete images even when a newer CPU
picture is already displayed. Link frame numbers order presentation; guest
ticks may repeat during rollback. Retired generations are destroyed after
outstanding consumer fences complete. The frontend's display copy survives
producer exit. Shutdown may drain GPU work; normal release polling is
nonblocking. Import/copy/synchronization failure refuses the affected ring
and retains CPU delivery. Refusal and retirement do not renegotiate the
simulation profile or startup capabilities.

## Explicit observers

Steady shared frames do not carry pixel payloads through CPU shared memory.
Screenshots and save thumbnails are real pixel observers and may read back.
`StateThumbnail` carries exactly 160 by 120 RGBA bytes and their displayed
link frame number before a save request. The runner uses it only for the
matching GPU frame; CPU presentation keeps its existing thumbnail path.
Malformed or stale observer data must not replace a valid current picture.

## Validation and limits

`gpu_host_test` covers a fixed offer, old short tables, adapter mismatch,
invalid sizes/overflow, aliased handles, refusal and portable fallback.
The existing CoreLink and startup-profile tests continue to run. Runtime's
34-test Windows suite passes with the coordinated Launcher/engine candidate.

Real consumer/producer evidence is recorded in n64lle's
`docs/evidence/DIRECT-GPU-PRESENTATION.md`: actual game pixels/state/PCM,
resize, slot exhaustion, stale releases, refusal and intentional producer
death. Hardware qualification is currently one NVIDIA Windows adapter.
Compilation on another target is not external-memory or netplay qualification.

The completion rule follows
[Vulkan synchronization signal ordering](https://docs.vulkan.org/spec/latest/chapters/synchronization.html):
signals to binary semaphores in one submission are unordered with one
another; a submission fence completes after its semaphore signals.
Handle lifetime follows
[Khronos external objects on Windows](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_external_objects_win32.txt).

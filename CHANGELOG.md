# Changelog

## 0.4.7-beta (Unreleased)

### Launcher

- Redesigned the launcher with Play at the bottom, brighter golden accents, compact expandable categories and persistent drawer state during the visit.
- Moved Overlay into its own top-level section, outside Information & support.
- Added three saved settings profiles and per-section option resets. Profiles preserve display, graphics, control options and overlay settings; driver, control layout and launcher preferences remain independent.
- Added a portrait/landscape launcher preference, while gameplay remains landscape.
- Kept the game and controls editor in landscape orientation, switching automatically when Play or Edit layout is opened.
- Added a persistent launcher language selector for English and Brazilian Portuguese, including preparation and controls screens.

### Controls

- Redesigned touch controls with translucent gray fills, thinner outlines and visible pressed states; updated the layout editor preview to match.
- Added support for generic USB and wired PS2-compatible controllers, including face buttons, shoulder buttons, triggers, Start, Select, Mode and stick clicks.

### Audio

- Reset the device clock and latency estimate when reopening AAudio, preserving queued audio positions.
- Serialized stream recreation, pause and timestamp queries; recovery respects mute/pause and also retries from the completion worker.
- Replaced per-frame ring copies with two block copies at most and moved callback diagnostic logging to the completion worker.
- Restored game audio after locking or waking the device, receiving a call, or disconnecting audio hardware by recreating disconnected AAudio streams automatically.

### Graphics and video

- Added Light/Standard background prewarm load. Work is bounded per presented frame rather than per command submission; Light starts at most one pipeline per frame with a 250 µs scheduling budget and at most eight in flight. A single shader preparation can exceed that budget.
- Changed the former aggressive prewarm setting to bounded Intensive mode, removing waits for entire prewarm batches during gameplay. First-use pipeline compilation can still block.
- Added an optional frame-time graph showing the last 120 host presentation intervals, latest interval and worst interval. This measures presentation timing, not GPU execution time.
- Fixed green or distorted lines in cutscenes on Qualcomm/Snapdragon devices by applying decoder stride and row alignment independently.
- Prevented PowerVR driver crashes when pipelines contain unused color attachments by bypassing the pipeline cache for those pipelines.
- Ended active render passes before image layout changes, avoiding invalid Vulkan commands and improving PowerVR stability.
- Reset bound pipeline and dynamic state after a render pass ends, preventing stale viewport, scissor and pipeline state from causing incorrect colors or white stripes.

### Validation

- Added automatic push/PR checks for the complete launcher Java sources, settings defaults, save/mod handling, native helpers, Android native syntax and the touch overlay shader.

### Credits

- Compatibility fixes adapted from the Persona 5 Royal Android project by Brandon04Gonzalez and its contributors.


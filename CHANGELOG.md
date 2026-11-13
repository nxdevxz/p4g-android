# Changelog

## 0.4.7-beta (Unreleased)

### Launcher

- Redesigned the launcher for portrait orientation with Play at the top, visible quick settings and four compact categories for advanced options.
- Kept the game and controls editor in landscape orientation, switching automatically when Play or Edit layout is opened.
- Added a persistent launcher language selector for English and Brazilian Portuguese, including preparation and controls screens.

### Controls

- Added support for generic USB and wired PS2-compatible controllers, including face buttons, shoulder buttons, triggers, Start, Select, Mode and stick clicks.

### Audio

- Restored game audio after locking or waking the device, receiving a call, or disconnecting audio hardware by recreating disconnected AAudio streams automatically.

### Graphics and video

- Fixed green or distorted lines in cutscenes on Qualcomm/Snapdragon devices by applying decoder stride and row alignment independently.
- Prevented PowerVR driver crashes when pipelines contain unused color attachments by bypassing the pipeline cache for those pipelines.
- Ended active render passes before image layout changes, avoiding invalid Vulkan commands and improving PowerVR stability.
- Reset bound pipeline and dynamic state after a render pass ends, preventing stale viewport, scissor and pipeline state from causing incorrect colors or white stripes.

### Credits

- Compatibility fixes adapted from the Persona 5 Royal Android project by Brandon04Gonzalez and its contributors.



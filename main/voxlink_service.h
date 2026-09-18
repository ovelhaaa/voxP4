#pragma once

// Starts the VoxLink v1 UART control server when CONFIG_VOXLINK_ENABLE_SERVER
// is set. When the option is disabled (the production default) this is a no-op
// and the qualified realtime audio path is completely unchanged.
//
// The server runs on Core 1 and only submits parameters through the existing
// bounded SPSC queue via the engine binding; it never touches DSP objects.
bool voxlink_service_start();

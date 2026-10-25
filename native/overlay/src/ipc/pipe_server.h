// \\.\pipe\nms_overlay: the Runtime's Python side connects here, speaking for every mod in the
// game; see protocol.h.
#pragma once

namespace pipe_server {

// Serves the pipe on a thread of its own for the life of the process.
void start();

}  // namespace pipe_server

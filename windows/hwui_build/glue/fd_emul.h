// fd_emul.h -- an emulated eventfd for the glue's own descriptors (the choreographer's).
#pragma once

namespace winapk::hwui {

// A new emulated eventfd (non-blocking), which a Looper can watch.
int emulated_eventfd();
// Adds one to its counter: the Looper watching it wakes.
void emulated_signal(int fd);
// Reads and clears its counter; true when it was not zero.
bool emulated_drain(int fd);

}  // namespace winapk::hwui

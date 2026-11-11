#pragma once
// Module 3: filesystem isolation. Runs inside the child, in its new mount namespace.

#include <string>

// Mounts /proc, /sys (ro), /tmp and a minimal /dev under `rootfs`, makes it the new `/`
// via pivot_root, then detaches the host root. Throws std::system_error on failure.
void setup_rootfs(const std::string& rootfs);

#pragma once
// Image import: Docker only fetches the image (pull + export); crun runs it from the unpacked
// directory like any other --rootfs. No Docker daemon is involved at run time.

#include "cli.hpp"

// docker pull IMAGE; docker create IMAGE; docker export | tar -x -C DEST; docker rm.
// DEST must not exist or be empty. Throws std::runtime_error / std::system_error on failure
// and removes the partly unpacked DEST.
void pull_image(const PullOptions& opt);

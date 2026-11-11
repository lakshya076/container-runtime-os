#include "fs.hpp"
#include "util.hpp"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static void ensure_dir(const std::string& path) {
    if (mkdir(path.c_str(), 0755) < 0 && errno != EEXIST) check(-1, "mkdir " + path);
}

static void ensure_file(const std::string& path) {
    int fd = static_cast<int>(check(open(path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644), "create " + path));
    close(fd);
}

void setup_rootfs(const std::string& rootfs) {
    // Stop our mounts from propagating back to the host's mount namespace.
    check(mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr), "make / private");
    // pivot_root needs new_root to be a mount point.
    check(mount(rootfs.c_str(), rootfs.c_str(), nullptr, MS_BIND | MS_REC, nullptr), "bind " + rootfs);

    // Mount everything under rootfs while the host tree is still visible. These mounts live only
    // in our private mount namespace, so containers sharing one rootfs do not see each other's.
    const std::string proc = rootfs + "/proc", sys = rootfs + "/sys", tmp = rootfs + "/tmp", dev = rootfs + "/dev";
    ensure_dir(proc);
    check(mount("proc", proc.c_str(), "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr), "mount /proc");
    ensure_dir(sys);
    check(mount("sysfs", sys.c_str(), "sysfs", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr), "mount /sys");
    ensure_dir(tmp);
    check(mount("tmpfs", tmp.c_str(), "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777"), "mount /tmp");

    // Minimal /dev: only harmless host devices are bind-mounted in, never the whole host /dev.
    ensure_dir(dev);
    check(mount("tmpfs", dev.c_str(), "tmpfs", MS_NOSUID | MS_NOEXEC, "mode=755"), "mount /dev");
    for (const char* name : {"null", "zero", "full", "random", "urandom", "tty"}) {
        const std::string src = "/dev/" + std::string(name);
        const std::string dst = dev + "/" + name;
        ensure_file(dst);
        check(mount(src.c_str(), dst.c_str(), nullptr, MS_BIND, nullptr), "bind " + src);
    }

    // pivot_root(".", ".") stacks the old root on top of the new one, so no shared
    // ".oldroot" directory is needed (see pivot_root(2)); then detach the host tree.
    check(chdir(rootfs.c_str()), "chdir " + rootfs);
    check(syscall(SYS_pivot_root, ".", "."), "pivot_root");
    check(umount2(".", MNT_DETACH), "umount old root");
    check(chdir("/"), "chdir /");
}

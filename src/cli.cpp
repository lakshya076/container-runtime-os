#include "cli.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <stdexcept>

const char* const kUsage =
    "usage: crun pull IMAGE [DIR]\n"
    "  fetch IMAGE with Docker and unpack its filesystem into DIR\n"
    "  (default: /var/lib/crun/images/<image>), for use with run --rootfs\n"
    "usage: crun ps\n"
    "  list running containers with live memory, limits and engine actions\n"
    "usage: crun run [options] --rootfs DIR [--] CMD [ARGS...]\n"
    "  --memory SIZE    hard memory limit (memory.max), e.g. 64M, 1G, max (default: max)\n"
    "  --swap SIZE      swap limit (memory.swap.max) (default: 0)\n"
    "  --pids N         max number of processes (pids.max) (default: max)\n"
    "  --hostname NAME  container hostname (default: container)\n"
    "  --name NAME      name shown by `crun ps` (default: container_<pid>)\n"
    "  --stats MS       print live memory usage every MS milliseconds (default: off)\n"
    "  --rootfs DIR     root filesystem for the container (required)\n"
    "elastic memory engine (all need a finite --memory):\n"
    "  --elastic        adapt memory.high to memory pressure between --memory-min and --memory\n"
    "  --memory-min SIZE  elastic floor (default: max(16M, memory/8))\n"
    "  --soft-oom POLICY  freeze at a tripwire below --memory instead of a kernel OOM kill,\n"
    "                   then: kill | grant (raise limit from --reserve) | notify (SIGUSR1 the app)\n"
    "  --reserve SIZE   extra memory soft-oom may grant (default: memory/4)\n"
    "  --record PATH    write a flight recording to PATH.csv and PATH.html\n";

const char* soft_oom_name(SoftOom p) {
    switch (p) {
        case SoftOom::kill: return "kill";
        case SoftOom::grant: return "grant";
        case SoftOom::notify: return "notify";
        default: return "off";
    }
}

bool valid_image_ref(const std::string& ref) {
    if (ref.empty() || ref.size() > 255 || !std::isalnum(static_cast<unsigned char>(ref[0]))) return false;
    return std::all_of(ref.begin(), ref.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '/' || c == ':' ||
               c == '@' || c == '-';
    });
}

std::string default_image_dir(const std::string& ref) {
    std::string name = ref;
    std::replace_if(name.begin(), name.end(), [](char c) { return c == '/' || c == ':' || c == '@'; }, '_');
    return std::string(kImagesRoot) + "/" + name;
}

PullOptions parse_pull_args(const std::vector<std::string>& args) {
    if (args.empty() || args.size() > 2) throw std::invalid_argument("pull needs IMAGE and an optional DIR");
    if (!valid_image_ref(args[0])) throw std::invalid_argument("invalid image reference: '" + args[0] + "'");
    PullOptions p{args[0], args.size() == 2 ? args[1] : default_image_dir(args[0])};
    if (p.dest.empty()) throw std::invalid_argument("DIR must not be empty");
    return p;
}

long long parse_size(const std::string& s) {
    if (s == "max") return -1;
    if (s.empty() || !std::isdigit(static_cast<unsigned char>(s[0])))
        throw std::invalid_argument("invalid size: '" + s + "'");
    size_t pos = 0;
    unsigned long long value = std::stoull(s, &pos);
    std::string suffix = s.substr(pos);
    unsigned long long mult = 1;
    if (suffix.size() == 1) {
        switch (std::toupper(static_cast<unsigned char>(suffix[0]))) {
            case 'K': mult = 1ULL << 10; break;
            case 'M': mult = 1ULL << 20; break;
            case 'G': mult = 1ULL << 30; break;
            default: throw std::invalid_argument("invalid size suffix: '" + s + "'");
        }
    } else if (!suffix.empty()) {
        throw std::invalid_argument("invalid size suffix: '" + s + "'");
    }
    if (value > static_cast<unsigned long long>(LLONG_MAX) / mult)
        throw std::invalid_argument("size too large: '" + s + "'");
    return static_cast<long long>(value * mult);
}

static int parse_int(const std::string& opt, const std::string& v) {
    size_t pos = 0;
    int n = -1;
    try { n = std::stoi(v, &pos); } catch (const std::exception&) {}
    if (n < 0 || pos != v.size()) throw std::invalid_argument(opt + " needs a non-negative number");
    return n;
}

Options parse_run_args(const std::vector<std::string>& args) {
    Options o;
    size_t i = 0;
    auto value = [&](const std::string& opt) -> const std::string& {
        if (i + 1 >= args.size()) throw std::invalid_argument(opt + " needs a value");
        return args[++i];
    };
    for (; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--") { ++i; break; }
        if (a.rfind("--", 0) != 0) break;  // first non-option starts the command
        if (a == "--memory") o.memory = parse_size(value(a));
        else if (a == "--swap") o.swap = parse_size(value(a));
        else if (a == "--pids") { const std::string& v = value(a); o.pids = v == "max" ? -1 : parse_int(a, v); }
        else if (a == "--hostname") o.hostname = value(a);
        else if (a == "--name") {
            o.name = value(a);
            bool ok = !o.name.empty() && o.name.size() <= 64 &&
                      std::all_of(o.name.begin(), o.name.end(), [](unsigned char c) {
                          return std::isalnum(c) || c == '-' || c == '_' || c == '.';
                      });
            if (!ok) throw std::invalid_argument("--name must be 1-64 characters of [A-Za-z0-9._-]");
        }
        else if (a == "--stats") o.stats_ms = parse_int(a, value(a));
        else if (a == "--rootfs") o.rootfs = value(a);
        else if (a == "--elastic") o.elastic = true;
        else if (a == "--memory-min") o.memory_min = parse_size(value(a));
        else if (a == "--reserve") o.reserve = parse_size(value(a));
        else if (a == "--record") o.record = value(a);
        else if (a == "--soft-oom") {
            const std::string& v = value(a);
            if (v == "kill") o.soft_oom = SoftOom::kill;
            else if (v == "grant") o.soft_oom = SoftOom::grant;
            else if (v == "notify") o.soft_oom = SoftOom::notify;
            else throw std::invalid_argument("--soft-oom must be kill, grant or notify");
        }
        else throw std::invalid_argument("unknown option: " + a);
    }
    o.argv.assign(args.begin() + static_cast<long>(i), args.end());
    if (o.rootfs.empty()) throw std::invalid_argument("--rootfs is required");
    if (o.argv.empty()) throw std::invalid_argument("missing command to run");
    if (o.memory == 0) throw std::invalid_argument("--memory must be greater than 0");

    if ((o.elastic || o.soft_oom != SoftOom::off) && o.memory < 0)
        throw std::invalid_argument("--elastic and --soft-oom need a finite --memory");
    if (o.memory_min >= 0 && !o.elastic) throw std::invalid_argument("--memory-min needs --elastic");
    if (o.reserve >= 0 && o.soft_oom == SoftOom::off) throw std::invalid_argument("--reserve needs --soft-oom");
    if (o.elastic) {
        if (o.memory_min < 0) o.memory_min = std::min(std::max(16LL << 20, o.memory / 8), o.memory / 2);
        if (o.memory_min <= 0 || o.memory_min >= o.memory)
            throw std::invalid_argument("--memory-min must be between 0 and --memory");
    }
    if (o.soft_oom != SoftOom::off && o.reserve < 0) o.reserve = o.memory / 4;
    return o;
}

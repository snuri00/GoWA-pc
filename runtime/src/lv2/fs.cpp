#include "object.h"
#include "runtime/vfs.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

#include <io.h>
#include <sys/stat.h>

namespace crash {
void log_here(const char* tag);
}

namespace lv2 {

namespace {

namespace fsys = std::filesystem;

struct DirEntry {
    std::string name;
    bool dir;
    uint64_t size;
    int64_t mtime;
};

struct Fd {
    FILE* f = nullptr;
    std::string guest;
    std::string host;
    int32_t flags = 0;
    bool readonly_mount = false;
    bool is_dir = false;
    std::vector<DirEntry> entries;
    size_t dir_pos = 0;
};

std::mutex g_lock;
std::map<uint32_t, std::shared_ptr<Fd>> g_fds;

constexpr int32_t O_ACCMODE = 3, O_WRONLY = 1, O_RDWR = 2, O_CREAT = 0100, O_EXCL = 0200, O_TRUNC = 01000, O_APPEND = 02000;
constexpr int64_t CELL_EROFS = static_cast<int32_t>(0x80010036);
constexpr int64_t CELL_ENOTMOUNTED = static_cast<int32_t>(0x8001003A);
constexpr int64_t CELL_ENAMETOOLONG = static_cast<int32_t>(0x80010032);
constexpr uint32_t CELL_ENOTSUP_CODE = 0x80010037;

bool is_ro(const std::string& guest) {
    return guest.rfind("/dev_bdvd", 0) == 0 || guest.rfind("/dev_ps2disc", 0) == 0;
}

uint32_t block_size(const std::string& guest) {
    if (guest.rfind("/dev_hdd0", 0) == 0) return 4096;
    if (guest.rfind("/dev_hdd1", 0) == 0) return 32768;
    if (guest.rfind("/dev_bdvd", 0) == 0) return 2048;
    if (guest.rfind("/dev_flash", 0) == 0) return 8192;
    if (guest.rfind("/dev_usb", 0) == 0) return 4096;
    return 512;
}

bool no_uid(const std::string& guest) { return guest.rfind("/dev_hdd0", 0) != 0; }

int64_t read_path(uint32_t addr, std::string& out) {
    if (!addr) return CELL_EFAULT;
    const char* p = reinterpret_cast<const char*>(vm::ptr(addr));
    size_t n = strnlen(p, 1025);
    if (n > 1024) return CELL_ENAMETOOLONG;
    out.assign(p, n);
    if (out.empty() || out[0] != '/') return CELL_ENOENT;
    return CELL_OK;
}

int64_t resolve(uint32_t addr, std::string& guest, std::string& host) {
    if (int64_t e = read_path(addr, guest)) return e;
    host = vfs::host_path(guest);
    if (host.empty()) return CELL_ENOTMOUNTED;
    return CELL_OK;
}

uint32_t alloc_fd(std::shared_ptr<Fd> fd) {
    std::lock_guard lk(g_lock);
    for (uint32_t i = 3; i < 255; i++) {
        if (!g_fds.count(i)) {
            g_fds[i] = std::move(fd);
            return i;
        }
    }
    return 0;
}

std::shared_ptr<Fd> get_fd(uint32_t fd) {
    std::lock_guard lk(g_lock);
    auto it = g_fds.find(fd);
    return it == g_fds.end() ? nullptr : it->second;
}

void write_stat(uint32_t sb, bool dir, uint64_t size, int64_t atime, int64_t mtime, int64_t ctime, const std::string& guest) {
    bool ro = is_ro(guest);
    uint32_t mode = dir ? (0040000 | 0777) : (0100000 | 0666);
    if (ro) mode &= ~0222u;
    int32_t id = no_uid(guest) ? -1 : 0;
    vm::write32(sb, mode);
    vm::write32(sb + 4, static_cast<uint32_t>(id));
    vm::write32(sb + 8, static_cast<uint32_t>(id));
    vm::write64(sb + 0xC, static_cast<uint64_t>(atime));
    vm::write64(sb + 0x14, static_cast<uint64_t>(mtime));
    vm::write64(sb + 0x1C, static_cast<uint64_t>(ctime));
    vm::write64(sb + 0x24, dir ? block_size(guest) : size);
    vm::write64(sb + 0x2C, block_size(guest));
}

int64_t host_stat(const std::string& host, struct _stat64& st) {
    if (_stat64(host.c_str(), &st) != 0) return CELL_ENOENT;
    return CELL_OK;
}

void fill_dirent(uint32_t d, const DirEntry& e) {
    vm::ptr(d)[0] = e.dir ? 1 : 2;
    size_t n = std::min<size_t>(e.name.size(), 255);
    vm::ptr(d)[1] = static_cast<uint8_t>(n);
    std::memcpy(vm::ptr(d + 2), e.name.data(), n);
    vm::ptr(d + 2)[n] = 0;
}

int64_t do_read(Fd& fd, uint32_t buf, uint64_t n, uint64_t& got) {
    got = 0;
    if (fd.is_dir || !fd.f) return CELL_EBADF;
    if ((fd.flags & O_ACCMODE) == O_WRONLY) return CELL_EBADF;
    got = std::fread(vm::ptr(buf), 1, n, fd.f);
    return CELL_OK;
}

int64_t do_write(Fd& fd, uint32_t buf, uint64_t n, uint64_t& put) {
    put = 0;
    if (fd.is_dir || !fd.f) return CELL_EBADF;
    if ((fd.flags & O_ACCMODE) == 0) return CELL_EBADF;
    if (fd.readonly_mount) return CELL_EROFS;
    if (fd.flags & O_APPEND) _fseeki64(fd.f, 0, SEEK_END);
    put = std::fwrite(vm::ptr(buf), 1, n, fd.f);
    return CELL_OK;
}

}

LV2_SYSCALL(801, sys_fs_open) {
    if (const char* k = std::getenv("GOWA_OPEN_STACK")) {
        std::string g;
        if (!read_path(u32arg(ctx, 0), g) && g.find(k) != std::string::npos) crash::log_here(g.c_str());
    }
    std::string guest, host;
    if (int64_t e = resolve(u32arg(ctx, 0), guest, host)) return e;
    int32_t flags = s32arg(ctx, 1);
    uint32_t out = u32arg(ctx, 2);
    bool ro = is_ro(guest);
    std::error_code ec;
    if (fsys::is_directory(host, ec)) return CELL_EISDIR;
    bool exists = fsys::exists(host, ec);
    int32_t acc = flags & O_ACCMODE;
    if (ro && (acc != 0 || (flags & (O_TRUNC | O_CREAT)))) {
        if (!exists) return CELL_ENOENT;
        flags &= ~(O_ACCMODE | O_TRUNC | O_CREAT | O_APPEND);
        acc = 0;
    }
    if (exists && (flags & O_CREAT) && (flags & O_EXCL)) return CELL_EEXIST;
    if (!exists && !(flags & O_CREAT)) {
        LOG_DEBUG("fs", "open '%s' -> ENOENT", guest.c_str());
        return CELL_ENOENT;
    }
    const char* mode = "rb";
    if (acc == 0) mode = "rb";
    else if (!exists || (flags & O_TRUNC)) mode = acc == O_WRONLY ? "wb" : "w+b";
    else mode = "r+b";
    FILE* f = std::fopen(host.c_str(), mode);
    if (!f) return exists ? CELL_EIO : CELL_ENOENT;
    auto fd = std::make_shared<Fd>();
    fd->f = f;
    fd->guest = guest;
    fd->host = host;
    fd->flags = flags;
    fd->readonly_mount = ro;
    uint32_t id = alloc_fd(fd);
    if (!id) {
        std::fclose(f);
        return CELL_EMFILE;
    }
    vm::write32(out, id);
    LOG_INFO("fs", "open '%s' flags 0%o -> %u", guest.c_str(), flags, id);
    return CELL_OK;
}

LV2_SYSCALL(802, sys_fs_read) {
    auto fd = get_fd(u32arg(ctx, 0));
    uint32_t buf = u32arg(ctx, 1);
    uint64_t n = u64arg(ctx, 2);
    uint32_t nread = u32arg(ctx, 3);
    if (!nread) return CELL_EFAULT;
    if (!fd) return CELL_EBADF;
    uint64_t got = 0;
    int64_t r = n ? do_read(*fd, buf, n, got) : CELL_OK;
    vm::write64(nread, got);
    return r;
}

LV2_SYSCALL(803, sys_fs_write) {
    auto fd = get_fd(u32arg(ctx, 0));
    uint32_t nwrite = u32arg(ctx, 3);
    if (!nwrite) return CELL_EFAULT;
    if (!fd) return CELL_EBADF;
    uint64_t put = 0;
    int64_t r = do_write(*fd, u32arg(ctx, 1), u64arg(ctx, 2), put);
    vm::write64(nwrite, put);
    return r;
}

LV2_SYSCALL(804, sys_fs_close) {
    std::shared_ptr<Fd> fd;
    {
        std::lock_guard lk(g_lock);
        auto it = g_fds.find(u32arg(ctx, 0));
        if (it == g_fds.end() || it->second->is_dir) return CELL_EBADF;
        fd = it->second;
        g_fds.erase(it);
    }
    if (fd->f) std::fclose(fd->f);
    return CELL_OK;
}

LV2_SYSCALL(805, sys_fs_opendir) {
    std::string guest, host;
    if (int64_t e = resolve(u32arg(ctx, 0), guest, host)) return e;
    std::error_code ec;
    if (!fsys::exists(host, ec)) return CELL_ENOENT;
    if (!fsys::is_directory(host, ec)) return CELL_ENOTDIR;
    auto fd = std::make_shared<Fd>();
    fd->is_dir = true;
    fd->guest = guest;
    fd->host = host;
    fd->entries.push_back({".", true, 0, 0});
    fd->entries.push_back({"..", true, 0, 0});
    std::vector<DirEntry> list;
    for (auto& e : fsys::directory_iterator(host, ec)) {
        DirEntry d;
        d.name = e.path().filename().string();
        d.dir = e.is_directory(ec);
        d.size = d.dir ? 0 : e.file_size(ec);
        d.mtime = 0;
        list.push_back(d);
    }
    std::sort(list.begin(), list.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    fd->entries.insert(fd->entries.end(), list.begin(), list.end());
    uint32_t id = alloc_fd(fd);
    if (!id) return CELL_EMFILE;
    vm::write32(u32arg(ctx, 1), id);
    return CELL_OK;
}

LV2_SYSCALL(806, sys_fs_readdir) {
    auto fd = get_fd(u32arg(ctx, 0));
    uint32_t dir = u32arg(ctx, 1), nread = u32arg(ctx, 2);
    if (!dir || !nread) return CELL_EFAULT;
    if (!fd || !fd->is_dir) return CELL_EBADF;
    if (fd->dir_pos >= fd->entries.size()) {
        std::memset(vm::ptr(dir), 0, 3);
        vm::write64(nread, 0);
        return CELL_OK;
    }
    fill_dirent(dir, fd->entries[fd->dir_pos++]);
    vm::write64(nread, 0x102);
    return CELL_OK;
}

LV2_SYSCALL(807, sys_fs_closedir) {
    std::lock_guard lk(g_lock);
    auto it = g_fds.find(u32arg(ctx, 0));
    if (it == g_fds.end() || !it->second->is_dir) return CELL_EBADF;
    g_fds.erase(it);
    return CELL_OK;
}

LV2_SYSCALL(808, sys_fs_stat) {
    std::string guest, host;
    if (int64_t e = resolve(u32arg(ctx, 0), guest, host)) return e;
    struct _stat64 st;
    if (host_stat(host, st)) {
        LOG_DEBUG("fs", "stat '%s' -> ENOENT", guest.c_str());
        return CELL_ENOENT;
    }
    write_stat(u32arg(ctx, 1), (st.st_mode & _S_IFDIR) != 0, static_cast<uint64_t>(st.st_size), st.st_atime, st.st_mtime, st.st_ctime, guest);
    LOG_DEBUG("fs", "stat '%s' -> size %llu mtime %lld", guest.c_str(), static_cast<unsigned long long>(st.st_size), static_cast<long long>(st.st_mtime));
    return CELL_OK;
}

LV2_SYSCALL(809, sys_fs_fstat) {
    auto fd = get_fd(u32arg(ctx, 0));
    if (!fd) return CELL_EBADF;
    struct _stat64 st;
    if (host_stat(fd->host, st)) return CELL_EIO;
    if (fd->f) {
        int64_t pos = _ftelli64(fd->f);
        _fseeki64(fd->f, 0, SEEK_END);
        st.st_size = _ftelli64(fd->f);
        _fseeki64(fd->f, pos, SEEK_SET);
    }
    write_stat(u32arg(ctx, 1), fd->is_dir, static_cast<uint64_t>(st.st_size), st.st_atime, st.st_mtime, st.st_ctime, fd->guest);
    return CELL_OK;
}

LV2_SYSCALL(811, sys_fs_mkdir) {
    std::string guest, host;
    if (int64_t e = resolve(u32arg(ctx, 0), guest, host)) return e;
    if (is_ro(guest)) return CELL_EROFS;
    std::error_code ec;
    if (fsys::exists(host, ec)) return CELL_EEXIST;
    if (!fsys::exists(fsys::path(host).parent_path(), ec)) return CELL_ENOENT;
    fsys::create_directory(host, ec);
    return ec ? CELL_EIO : CELL_OK;
}

LV2_SYSCALL(812, sys_fs_rename) {
    std::string g1, h1, g2, h2;
    if (int64_t e = resolve(u32arg(ctx, 0), g1, h1)) return e;
    if (int64_t e = resolve(u32arg(ctx, 1), g2, h2)) return e;
    if (is_ro(g1) || is_ro(g2)) return CELL_EROFS;
    std::error_code ec;
    if (!fsys::exists(h1, ec)) return CELL_ENOENT;
    if (fsys::exists(h2, ec)) return CELL_EEXIST;
    fsys::rename(h1, h2, ec);
    return ec ? CELL_EIO : CELL_OK;
}

LV2_SYSCALL(813, sys_fs_rmdir) {
    std::string guest, host;
    if (int64_t e = resolve(u32arg(ctx, 0), guest, host)) return e;
    if (is_ro(guest)) return CELL_EROFS;
    std::error_code ec;
    if (!fsys::exists(host, ec)) return CELL_ENOENT;
    if (!fsys::is_directory(host, ec)) return CELL_ENOTDIR;
    if (!fsys::is_empty(host, ec)) return CELL_ENOTEMPTY;
    fsys::remove(host, ec);
    return ec ? CELL_EIO : CELL_OK;
}

LV2_SYSCALL(814, sys_fs_unlink) {
    std::string guest, host;
    if (int64_t e = resolve(u32arg(ctx, 0), guest, host)) return e;
    if (is_ro(guest)) return CELL_EROFS;
    std::error_code ec;
    if (!fsys::exists(host, ec)) return CELL_ENOENT;
    if (fsys::is_directory(host, ec)) return CELL_EISDIR;
    fsys::remove(host, ec);
    return ec ? CELL_EIO : CELL_OK;
}

LV2_SYSCALL(816, sys_fs_access) {
    return CELL_OK;
}

LV2_SYSCALL(818, sys_fs_lseek) {
    auto fd = get_fd(u32arg(ctx, 0));
    if (!fd || fd->is_dir || !fd->f) return CELL_EBADF;
    int64_t off = static_cast<int64_t>(u64arg(ctx, 1));
    int32_t whence = s32arg(ctx, 2);
    if (whence < 0 || whence > 2) return CELL_EINVAL;
    int64_t base_pos = whence == 0 ? 0 : whence == 1 ? _ftelli64(fd->f) : (_fseeki64(fd->f, 0, SEEK_END), _ftelli64(fd->f));
    int64_t np = base_pos + off;
    if (np < 0) return CELL_EINVAL;
    _fseeki64(fd->f, np, SEEK_SET);
    if (uint32_t pos = u32arg(ctx, 3)) vm::write64(pos, static_cast<uint64_t>(np));
    return CELL_OK;
}

LV2_SYSCALL(819, sys_fs_fdatasync) {
    auto fd = get_fd(u32arg(ctx, 0));
    if (!fd) return CELL_EBADF;
    if (fd->f) std::fflush(fd->f);
    return CELL_OK;
}

LV2_SYSCALL(820, sys_fs_fsync) {
    auto fd = get_fd(u32arg(ctx, 0));
    if (!fd) return CELL_EBADF;
    if (fd->f) std::fflush(fd->f);
    return CELL_OK;
}

LV2_SYSCALL(831, sys_fs_truncate) {
    std::string guest, host;
    if (int64_t e = resolve(u32arg(ctx, 0), guest, host)) return e;
    if (is_ro(guest)) return CELL_EROFS;
    std::error_code ec;
    if (!fsys::exists(host, ec)) return CELL_ENOENT;
    fsys::resize_file(host, u64arg(ctx, 1), ec);
    return ec ? CELL_EIO : CELL_OK;
}

LV2_SYSCALL(832, sys_fs_ftruncate) {
    auto fd = get_fd(u32arg(ctx, 0));
    if (!fd || !fd->f) return CELL_EBADF;
    if (fd->readonly_mount) return CELL_EROFS;
    std::fflush(fd->f);
    return _chsize_s(_fileno(fd->f), static_cast<int64_t>(u64arg(ctx, 1))) == 0 ? CELL_OK : CELL_EIO;
}

LV2_SYSCALL(840, sys_fs_disk_free) {
    std::string guest;
    uint32_t p = u32arg(ctx, 0);
    if (!p) return CELL_EFAULT;
    guest = reinterpret_cast<const char*>(vm::ptr(p));
    uint64_t avail = is_ro(guest) ? 0 : (40ull << 30) - 512;
    if (uint32_t t = u32arg(ctx, 1)) vm::write64(t, avail);
    if (uint32_t a = u32arg(ctx, 2)) vm::write64(a, avail);
    return CELL_OK;
}

LV2_SYSCALL(817, sys_fs_fcntl) {
    uint32_t fdn = u32arg(ctx, 0);
    uint32_t op = u32arg(ctx, 1);
    uint32_t arg = u32arg(ctx, 2);
    uint32_t size = u32arg(ctx, 3);
    switch (op) {
    case 0x80000004:
        if (size > 4) return CELL_EINVAL;
        vm::write32(arg, 0);
        return CELL_OK;
    case 0x8000000A:
    case 0x8000000B: {
        if (size < 0x38) return CELL_EINVAL;
        auto fd = get_fd(vm::read32(arg + 0x10));
        if (!fd || !fd->f) return CELL_EBADF;
        int64_t old = _ftelli64(fd->f);
        _fseeki64(fd->f, static_cast<int64_t>(vm::read64(arg + 0x18)), SEEK_SET);
        uint64_t done = 0;
        int64_t r = op == 0x8000000A ? do_read(*fd, vm::read32(arg + 0x14), vm::read64(arg + 0x20), done)
                                     : do_write(*fd, vm::read32(arg + 0x14), vm::read64(arg + 0x20), done);
        _fseeki64(fd->f, old, SEEK_SET);
        if (r) return r;
        vm::write32(arg + 0x28, 0);
        vm::write64(arg + 0x30, done);
        return CELL_OK;
    }
    case 0xC0000002:
        vm::write32(arg + 0x18, 0);
        vm::write32(arg + 0x1C, 4096);
        vm::write64(arg + 0x20, ((40ull << 30) - 1) / 4096);
        return CELL_OK;
    case 0xC0000006:
        if (size == 0x20 && vm::read32(arg) == 0x20 && vm::read32(arg + 4) == 0x10 && vm::read32(arg + 8) == 0x18) {
            vm::write32(arg + 0x18, CELL_ENOTSUP_CODE);
            vm::write32(arg + 0x1C, 0);
        }
        return CELL_OK;
    case 0xC0000008:
        vm::write32(arg + 0x20, 0);
        return CELL_OK;
    case 0xE0000012: {
        if (size < 0x10) return CELL_EINVAL;
        auto fd = get_fd(fdn);
        if (!fd || !fd->is_dir) return CELL_EBADF;
        uint32_t ptr = vm::read32(arg + 8);
        uint32_t max = vm::read32(arg + 0xC);
        uint32_t n = 0;
        if (max && fd->dir_pos < fd->entries.size()) {
            const DirEntry& e = fd->entries[fd->dir_pos++];
            std::string guest = fd->guest + "/" + e.name;
            struct _stat64 st{};
            host_stat(fd->host + "/" + e.name, st);
            write_stat(ptr, e.dir, e.size, st.st_atime, st.st_mtime, st.st_ctime, guest);
            fill_dirent(ptr + 0x34, e);
            n = 1;
        }
        if (max > n) std::memset(vm::ptr(ptr + n * 0x138), 0, (max - n) * 0x138);
        vm::write32(arg + 4, n);
        vm::write32(arg, 0);
        return CELL_OK;
    }
    default:
        LOG_DEBUG("fs", "fcntl op 0x%x on fd %u ignored", op, fdn);
        return CELL_OK;
    }
}

}

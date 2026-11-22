// kernel32: handles, guest<->host paths, files, directories, find.
//
// Guest paths are Windows paths. GuestGameDir() (GameConfig::guest_dir)
// maps onto the game install, read-only, with writes going to an overlay
// (see HostPath); any other path maps below the per-user data directory
// ($XDG_DATA_HOME/<id>/drive_c/...), where saves and
// settings end up. Lookups are case-insensitive like NTFS.

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <vector>

#include "runtime/lazy_mem.h"
#include "runtime/env.h"
#include "runtime/log.h"
#include "runtime/platform.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/win32.h"

namespace recomp {

namespace {

std::mutex g_handles_mu;
std::map<uint32_t, std::shared_ptr<KObject>> g_handles;
uint32_t g_next_handle = 0x100;

std::mutex g_cwd_mu;
std::string g_cwd;  // guest current directory; empty: GuestGameDir()

struct StdIoObject : KObject {
  explicit StdIoObject(int f) : KObject(Type::StdIo), fd(f) {}
  int fd;
};

void RemoveOnClose(const std::string& guest, bool dir);

struct FileObject : KObject {
  FileObject() : KObject(Type::File) {}
  ~FileObject() override {
    if (fd >= 0) close(fd);
    if (delete_on_close) RemoveOnClose(guest, dir);
  }
  int fd = -1;
  std::string guest;  // the absolute guest path it was opened by
  // SetFileInformationByHandle's disposition: deleted when the last handle
  // to it closes (this object is shared by the handles DuplicateHandle makes).
  bool delete_on_close = false;
  bool dir = false;
};

struct FindObject : KObject {
  FindObject() : KObject(Type::Find) {}
  // Matching entries: host directory and name (as on disk).
  std::vector<std::pair<std::string, std::string>> entries;
  size_t next = 0;
};

uint32_t g_std_handles[3];

std::string UserDataRoot() { return DataDir() + "/drive_c"; }

// Splits a normalised guest path ("C:\a\b") into components after the drive.
std::vector<std::string> Components(const std::string& p) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 3; i <= p.size(); i++) {
    if (i == p.size() || p[i] == '\\') {
      if (cur == "..") {
        if (!out.empty()) out.pop_back();
      } else if (!cur.empty() && cur != ".") {
        out.push_back(cur);
      }
      cur.clear();
    } else {
      cur += p[i];
    }
  }
  return out;
}

// Absolute, normalised guest path: "C:\x\y".
std::string AbsGuest(std::string p) {
  std::replace(p.begin(), p.end(), '/', '\\');
  if (p.size() >= 2 && p[1] == ':' && (p.size() == 2 || p[2] != '\\')) {
    // Drive-relative ("C:", "C:.\x", "C:x"): against the current directory
    // when it is on that drive, else the drive's root (C: is the only one).
    std::string cwd = GuestCurrentDir();
    std::string rest = p.substr(2);
    if (std::toupper(uint8_t(p[0])) == std::toupper(uint8_t(cwd[0])))
      p = cwd + (rest.empty() ? "" : "\\" + rest);
    else
      p = p.substr(0, 2) + "\\" + rest;
  } else if (p.size() >= 2 && p[1] == ':') {
  } else if (!p.empty() && p[0] == '\\') {
    p = "C:" + p;
  } else {
    p = GuestCurrentDir() + "\\" + p;
  }
  std::string drive = p.substr(0, 3);
  drive[0] = char(std::toupper(drive[0]));
  std::string out = drive;
  auto comps = Components(p);
  for (size_t i = 0; i < comps.size(); i++) {
    if (i) out += '\\';
    out += comps[i];
  }
  return out;
}

// Case-insensitive resolution of path components `comps` under host dir
// `root`.
std::string Resolve(const std::string& root, const std::vector<std::string>& comps) {
  std::string cur = root;
  for (size_t i = 0; i < comps.size(); i++) {
    std::string want = comps[i];
    std::string next = cur + "/" + want;
    struct stat st;
    if (stat(next.c_str(), &st) != 0) {
      if (DIR* d = opendir(cur.c_str())) {
        std::string lw = win32::Lower(want);
        while (dirent* e = readdir(d)) {
          if (win32::Lower(e->d_name) == lw) {
            next = cur + "/" + e->d_name;
            break;
          }
        }
        closedir(d);
      }
    }
    cur = next;
  }
  return cur;
}

}  // namespace

std::string GuestFullPath(const std::string& p) { return AbsGuest(p); }

const std::string& GuestGameDir() { return Game().guest_dir; }

std::string GuestExePath() { return GuestGameDir() + "\\" + Game().exe_name; }

std::string GuestCurrentDir() {
  std::lock_guard<std::mutex> l(g_cwd_mu);
  return g_cwd.empty() ? GuestGameDir() : g_cwd;
}

namespace {

std::string OverlayRoot() { return DataDir() + "/install"; }

// Components below the guest install directory, or false if g (absolute)
// is not inside it.
bool InstallComponents(const std::string& g, std::vector<std::string>* comps) {
  std::string lg = win32::Lower(g);
  std::string lroot = win32::Lower(GuestGameDir());
  if (lg != lroot && lg.rfind(lroot + "\\", 0) != 0) return false;
  *comps = Components("C:\\" + g.substr(std::min(g.size(), lroot.size() + 1)));
  return true;
}

bool Exists(const std::string& host, struct stat* st = nullptr) {
  struct stat s;
  return stat(host.c_str(), st ? st : &s) == 0;
}

// Copies regular file `from` to `to` (created or truncated).
bool CopyHostFile(const std::string& from, const std::string& to) {
  int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) return false;
  int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    close(in);
    return false;
  }
  char buf[65536];
  ssize_t n;
  bool ok = true;
  while ((n = read(in, buf, sizeof buf)) > 0)
    if (write(out, buf, size_t(n)) != n) ok = false;
  close(in);
  close(out);
  return ok && n == 0;
}

}  // namespace

std::string HostPath(const std::string& guest_path, PathAccess access) {
  std::string g = AbsGuest(guest_path);
  std::vector<std::string> comps;
  if (InstallComponents(g, &comps)) {
    std::string over = Resolve(OverlayRoot(), comps);
    if (access == PathAccess::kRead) {
      return Exists(over) ? over : Resolve(g_proc->game_dir, comps);
    }
    // Mirror the parent directory into the overlay if the guest sees it.
    std::string parent = over.substr(0, over.find_last_of('/'));
    std::vector<std::string> pcomps(comps.begin(), comps.end() - (comps.empty() ? 0 : 1));
    struct stat st;
    if (!Exists(parent) && Exists(Resolve(g_proc->game_dir, pcomps), &st) &&
        S_ISDIR(st.st_mode))
      MakeDirs(parent);
    std::string inst = Resolve(g_proc->game_dir, comps);
    if (access == PathAccess::kUpdate && !Exists(over) && Exists(inst, &st) &&
        S_ISREG(st.st_mode))
      CopyHostFile(inst, over);
    return over;
  }
  // Everything else lives in the per-user tree, by drive letter.
  std::string root = UserDataRoot();
  if (g[0] != 'C') root += std::string("/drive_") + char(std::tolower(g[0]));
  return Resolve(root, Components(g));
}

std::vector<std::string> HostDirs(const std::string& guest_dir) {
  std::string g = AbsGuest(guest_dir);
  std::vector<std::string> comps;
  if (!InstallComponents(g, &comps)) return {HostPath(g)};
  return {Resolve(OverlayRoot(), comps), Resolve(g_proc->game_dir, comps)};
}

std::string NarrowPath(const std::u16string& path) {
  for (char16_t ch : path) {
    bool ok;
    win32::UnicodeToCp1252(ch, &ok);
    if (!ok) FatalF("path with a character outside Windows-1252 (U+%04X): not supported", unsigned(ch));
  }
  return win32::Narrow(path);
}

uint32_t NewHandle(std::shared_ptr<KObject> obj) {
  std::lock_guard<std::mutex> l(g_handles_mu);
  uint32_t h = g_next_handle;
  g_next_handle += 4;
  g_handles[h] = std::move(obj);
  return h;
}

std::shared_ptr<KObject> GetHandle(uint32_t h) {
  if (auto o = PseudoHandle(h)) return o;
  std::lock_guard<std::mutex> l(g_handles_mu);
  auto it = g_handles.find(h);
  return it == g_handles.end() ? nullptr : it->second;
}

int FileFd(KObject* o) {
  if (o->type == KObject::Type::File) return static_cast<FileObject*>(o)->fd;
  if (o->type == KObject::Type::StdIo) return static_cast<StdIoObject*>(o)->fd;
  return -1;
}

bool CloseHandleValue(uint32_t h) {
  std::lock_guard<std::mutex> l(g_handles_mu);
  return g_handles.erase(h) != 0;
}

namespace hle {
namespace {

using namespace win32;

constexpr uint32_t kInvalidHandle = 0xFFFFFFFF;
constexpr uint32_t kVolumeSerial = 0x1DEADD51;  // drive C:

// FILETIME from a POSIX time.
void PutFileTime(gaddr va, const timespec& ts) {
  Wr64(va, uint64_t(ts.tv_sec) * 10000000ull + uint64_t(ts.tv_nsec) / 100 +
               kFileTimeUnixEpoch);
}

uint32_t AttributesOf(const struct stat& st, const std::string& name) {
  uint32_t a = S_ISDIR(st.st_mode) ? 0x10 : 0x80;  // DIRECTORY : NORMAL
  if (!(st.st_mode & S_IWUSR)) a = (a & ~0x80u) | 0x01;  // READONLY
  if (!name.empty() && name[0] == '.' && name != "." && name != "..")
    a |= 0x02;  // HIDDEN
  return a;
}

uint32_t ErrnoToWin(int e) {
  switch (e) {
    case ENOENT: return kErrorFileNotFound;
    case ENOTDIR: return kErrorPathNotFound;
    case EACCES:
    case EPERM: return kErrorAccessDenied;
    case EEXIST: return kErrorFileExists;
    case ENOTEMPTY: return 145;  // ERROR_DIR_NOT_EMPTY
    default: return kErrorAccessDenied;
  }
}

std::string WidePath(gaddr p) { return NarrowPath(GWStr(p)); }

HLE64("kernel32.dll", GetStdHandle, 1) {
  int32_t which = int32_t(Arg(c, 0));
  int idx = which == -10 ? 0 : which == -11 ? 1 : which == -12 ? 2 : -1;
  if (idx < 0) return HandleRet(kInvalidHandle);
  if (!g_std_handles[idx])
    g_std_handles[idx] = NewHandle(std::make_shared<StdIoObject>(idx));
  return HandleRet(g_std_handles[idx]);
}

HLE64("kernel32.dll", SetStdHandle, 2) { return 1; }

// LockFileEx(h, flags, reserved, len lo, len hi, overlapped) and
// UnlockFileEx(h, reserved, len lo, len hi, overlapped): byte-range locks
// guard against other processes, and there are none, so every lock is
// granted at once (and an OVERLAPPED caller sees it complete).
bool IsFileHandle(gaddr h) {
  auto o = GetHandle(h);
  return o && o->type == KObject::Type::File;
}
// LockFile(file, offset lo, hi, length lo, hi): byte-range locks are
// accepted and not enforced (one process uses the files), as LockFileEx.
HLE64("kernel32.dll", LockFile, 5) {
  if (!IsFileHandle(Arg(c, 0))) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  return 1;
}
HLE64("kernel32.dll", UnlockFile, 5) {
  if (!IsFileHandle(Arg(c, 0))) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  return 1;
}
HLE64("kernel32.dll", LockFileEx, 6) {
  if (!IsFileHandle(Arg(c, 0))) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  return 1;
}
HLE64("kernel32.dll", UnlockFileEx, 5) {
  if (!IsFileHandle(Arg(c, 0))) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  return 1;
}

HLE64("kernel32.dll", GetFileType, 1) {
  auto o = GetHandle(Arg(c, 0));
  if (!o || (o->type != KObject::Type::File && o->type != KObject::Type::StdIo)) {
    SetLastError(kErrorInvalidHandle);
    return 0;  // FILE_TYPE_UNKNOWN
  }
  return o->type == KObject::Type::StdIo ? 2 : 1;  // CHAR : DISK
}

HLE64("kernel32.dll", CloseHandle, 1) {
  // Closing a pseudo handle does nothing and succeeds (NtClose).
  const uint32_t h = uint32_t(Arg(c, 0));
  if (h == 0xFFFFFFFF || h == 0xFFFFFFFE) return 1;
  if (!CloseHandleValue(h)) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  return 1;
}

// DuplicateHandle(source process, source, target process, &target,
// access, inherit, options). Both processes are this one; a pseudo handle
// as the source gives a real handle to the process or thread.
// DUPLICATE_CLOSE_SOURCE closes the source whatever the outcome.
HLE("kernel32.dll", DuplicateHandle, 7) {
  auto is_process = [](uint32_t h) {
    auto p = GetHandle(h);
    return p && p->type == KObject::Type::Process;
  };
  const uint32_t src = Arg(c, 1);
  if (!is_process(Arg(c, 0))) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  auto o = GetHandle(src);
  if (Arg(c, 6) & 1) CloseHandleValue(src);
  if (!o || !is_process(Arg(c, 2))) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  if (Arg(c, 3)) Wr32(Arg(c, 3), NewHandle(o));
  return 1;
}

// CreateFile(path, access, share, security, disposition, flags, template).
uint32_t CreateFile(Ctx& c, const std::string& gpath) {
  uint32_t access = uint32_t(Arg(c, 1)), disp = uint32_t(Arg(c, 4));
  std::string path = HostPath(gpath);
  bool write = access & 0x40000000;  // GENERIC_WRITE
  bool read = access & 0x80000000;   // GENERIC_READ
  int flags = write ? (read ? O_RDWR : O_WRONLY) : O_RDONLY;
  struct stat st;
  bool exists = stat(path.c_str(), &st) == 0;
  // Writes never touch the install (see HostPath): truncating opens get a
  // fresh overlay file, others a copy of the install's.
  if (write && !(exists && S_ISDIR(st.st_mode)))
    path = HostPath(gpath, disp == 2 || disp == 5 || !exists ? PathAccess::kWrite
                                                             : PathAccess::kUpdate);
  switch (disp) {
    case 1: if (exists) { SetLastError(kErrorFileExists); return kInvalidHandle; }
            flags |= O_CREAT; break;                  // CREATE_NEW
    case 2: flags |= O_CREAT | O_TRUNC; break;        // CREATE_ALWAYS
    case 3: break;                                    // OPEN_EXISTING
    case 4: flags |= O_CREAT; break;                  // OPEN_ALWAYS
    case 5: flags |= O_TRUNC; break;                  // TRUNCATE_EXISTING
  }
  // A directory opens only with FILE_FLAG_BACKUP_SEMANTICS (0x02000000),
  // and then only for queries (attributes, its final path); data access
  // to it fails as on Windows.
  if (exists && S_ISDIR(st.st_mode)) {
    if (!(uint32_t(Arg(c, 5)) & 0x02000000) || write) {
      SetLastError(kErrorAccessDenied);
      return kInvalidHandle;
    }
    int dfd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
      SetLastError(ErrnoToWin(errno));
      return kInvalidHandle;
    }
    RECOMP_TRACE("  CreateFile(%s) -> directory %s\n", gpath.c_str(), path.c_str());
    auto d = std::make_shared<FileObject>();
    d->fd = dfd;
    d->guest = AbsGuest(gpath);
    SetLastError(0);
    return NewHandle(d);
  }
  if (flags & O_CREAT) {
    // Make sure the parent exists in the per-user tree.
    MakeDirs(path.substr(0, path.find_last_of('/')));
  }
  int fd = open(path.c_str(), flags | O_CLOEXEC, 0644);
  if (fd < 0) {
    RECOMP_TRACE("  CreateFile(%s) -> %s: %s\n", gpath.c_str(), path.c_str(),
              std::strerror(errno));
    SetLastError(ErrnoToWin(errno));
    return kInvalidHandle;
  }
  RECOMP_TRACE("  CreateFile(%s) -> %s\n", gpath.c_str(), path.c_str());
  auto f = std::make_shared<FileObject>();
  f->fd = fd;
  f->guest = AbsGuest(gpath);
  SetLastError(exists && (disp == 2 || disp == 4) ? kErrorAlreadyExists : 0);
  return NewHandle(f);
}

HLE64("kernel32.dll", CreateFileA, 7) { return HandleRet(CreateFile(c, GStr(Arg(c, 0)))); }

// OpenFile(path, OFSTRUCT*, style), the 16-bit-era open: OF_READ (0),
// OF_WRITE (1), OF_READWRITE (2), with OF_CREATE (0x1000, create or
// truncate), OF_EXIST (0x4000: open, close, report), OF_PARSE (0x100: the
// full path only), OF_DELETE (0x200). OFSTRUCT: cBytes, fFixedDisk,
// nErrCode (u16), 4 reserved bytes, szPathName[128]. The handle is a file
// handle (HFILE_ERROR, -1, on failure).
HLE("kernel32.dll", OpenFile, 3) {
  std::string g = AbsGuest(GStr(Arg(c, 0)));
  uint32_t ofs = Arg(c, 1), style = Arg(c, 2);
  std::memset(GPtr(ofs), 0, 136);
  Wr8(ofs + 0, 136);
  Wr8(ofs + 1, 1);
  PutStr(ofs + 8, g.substr(0, 127));
  auto fail = [&](uint32_t err) {
    Wr16(ofs + 2, uint16_t(err));
    SetLastError(err);
    return 0xFFFFFFFFu;
  };
  if (style & 0x100) return 0;  // OF_PARSE
  struct stat st;
  if (style & 0x200) {  // OF_DELETE
    std::string host = HostPath(g, PathAccess::kWrite);
    return unlink(host.c_str()) == 0 ? 1u : fail(ErrnoToWin(errno));
  }
  const uint32_t mode = style & 3;
  const bool create = style & 0x1000, write = create || mode != 0;
  std::string host = HostPath(g, !write ? PathAccess::kRead
                                        : create ? PathAccess::kWrite : PathAccess::kUpdate);
  if (!create && (stat(host.c_str(), &st) != 0 || !S_ISREG(st.st_mode))) return fail(2);
  int flags = mode == 2 ? O_RDWR : mode == 1 || create ? O_WRONLY : O_RDONLY;
  if (create) {
    flags |= O_CREAT | O_TRUNC;
    if (mode == 2) flags = (flags & ~O_WRONLY) | O_RDWR;
    MakeDirs(host.substr(0, host.find_last_of('/')));
  }
  int fd = open(host.c_str(), flags | O_CLOEXEC, 0644);
  if (fd < 0) return fail(ErrnoToWin(errno));
  if (style & 0x4000) {  // OF_EXIST
    close(fd);
    return 1;
  }
  auto f = std::make_shared<FileObject>();
  f->fd = fd;
  f->guest = g;
  return NewHandle(f);
}

// _lclose(hfile): CloseHandle, with HFILE_ERROR (-1) for failure.
HLE("kernel32.dll", _lclose, 1) {
  const uint32_t h = Arg(c, 0);
  if (!CloseHandleValue(h)) {
    SetLastError(kErrorInvalidHandle);
    return 0xFFFFFFFF;
  }
  return 0;
}
HLE64("kernel32.dll", CreateFileW, 7) { return HandleRet(CreateFile(c, WidePath(Arg(c, 0)))); }

int FdOf(uint64_t h) {
  auto o = GetHandle(h);
  return o ? FileFd(o.get()) : -1;
}

// Overlapped I/O (an OVERLAPPED given to ReadFile/WriteFile): done at once,
// at the OVERLAPPED's offset (Offset, OffsetHigh), as Windows does for a
// request it can complete without waiting. The OVERLAPPED gets the outcome
// (Internal: an NTSTATUS, InternalHigh: the bytes moved) and its event, if
// any, is signalled; GetOverlappedResult reads them back. The layout is
// {ULONG_PTR Internal, ULONG_PTR InternalHigh, DWORD Offset, DWORD
// OffsetHigh, HANDLE hEvent}, pointer-sized fields 8 bytes on x64.
constexpr uint32_t kStatusEndOfFile = 0xC0000011;
constexpr uint32_t kErrorHandleEof = 38;

uint64_t OverlappedOffset(gaddr ov) {
  const uint32_t o = g_proc->x64 ? 16 : 8;
  return uint64_t(Rd32(ov + o)) | uint64_t(Rd32(ov + o + 4)) << 32;
}

void CompleteOverlapped(gaddr ov, uint32_t status, uint32_t bytes) {
  const uint32_t ptr = g_proc->x64 ? 8 : 4;
  WrPtr(ov, status);
  WrPtr(ov + ptr, bytes);
  if (gaddr ev = RdPtr(ov + (g_proc->x64 ? 24 : 16))) SignalEventHandle(uint32_t(ev), false);
}

HLE64("kernel32.dll", ReadFile, 5) {
  int fd = FdOf(Arg(c, 0));
  gaddr buf = Arg(c, 1), out = Arg(c, 3), ov = Arg(c, 4);
  uint32_t n = uint32_t(Arg(c, 2));
  if (fd < 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  lazy::Touch(buf, n);  // the kernel does not fault on lazy pages
  ssize_t r = !n ? 0 : ov ? pread(fd, GPtr(buf), n, off_t(OverlappedOffset(ov)))
                          : read(fd, GPtr(buf), n);
  if (r < 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  if (out) Wr32(out, uint32_t(r));
  if (ov) {
    // A read that starts at or past the end fails with ERROR_HANDLE_EOF.
    bool eof = n && r == 0;
    CompleteOverlapped(ov, eof ? kStatusEndOfFile : 0, uint32_t(r));
    if (eof) {
      SetLastError(kErrorHandleEof);
      return 0;
    }
  }
  return 1;
}

// GetOverlappedResult(file, OVERLAPPED*, DWORD* bytes, wait): every
// request has completed already (ReadFile above).
HLE64("kernel32.dll", GetOverlappedResult, 4) {
  gaddr ov = Arg(c, 1);
  const uint32_t ptr = g_proc->x64 ? 8 : 4;
  uint32_t status = uint32_t(RdPtr(ov)), bytes = uint32_t(RdPtr(ov + ptr));
  if (Arg(c, 2)) Wr32(Arg(c, 2), bytes);
  if (status == kStatusEndOfFile) {
    SetLastError(kErrorHandleEof);
    return 0;
  }
  return 1;
}

// Nothing is ever pending to cancel.
HLE64("kernel32.dll", CancelIo, 1) { return 1; }
HLE64("kernel32.dll", CancelIoEx, 2) {
  SetLastError(1168);  // ERROR_NOT_FOUND
  return 0;
}

HLE64("kernel32.dll", WriteFile, 5) {
  int fd = FdOf(Arg(c, 0));
  gaddr buf = Arg(c, 1), out = Arg(c, 3), ov = Arg(c, 4);
  uint32_t n = uint32_t(Arg(c, 2));
  if (fd < 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  lazy::Touch(buf, n);
  ssize_t r = !n ? 0 : ov ? pwrite(fd, GPtr(buf), n, off_t(OverlappedOffset(ov)))
                          : write(fd, GPtr(buf), n);
  if (r >= 0 && ov) CompleteOverlapped(ov, 0, uint32_t(r));
  if (r < 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  if (out) Wr32(out, uint32_t(r));
  return 1;
}

HLE64("kernel32.dll", SetFilePointer, 4) {
  int fd = FdOf(Arg(c, 0));
  int64_t dist = int32_t(Arg(c, 1));
  gaddr hi = Arg(c, 2);
  uint32_t method = uint32_t(Arg(c, 3));
  if (hi) dist = int64_t((uint64_t(Rd32(hi)) << 32) | uint32_t(Arg(c, 1)));
  if (fd < 0) {
    SetLastError(kErrorInvalidHandle);
    return 0xFFFFFFFF;
  }
  off_t r = lseek(fd, dist, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
  if (r < 0) {
    SetLastError(kErrorInvalidParameter);
    return 0xFFFFFFFF;
  }
  if (hi) Wr32(hi, uint32_t(uint64_t(r) >> 32));
  SetLastError(0);
  return uint32_t(r);
}

// (hFile, LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER lpNewFilePointer,
// dwMoveMethod). The LARGE_INTEGER takes two stack slots on x86, one
// register on x64.
HLE64("kernel32.dll", SetFilePointerEx, 5) {
  int fd = FdOf(Arg(c, 0));
  const int k = g_proc->x64 ? 0 : 1;
  int64_t dist = g_proc->x64 ? int64_t(Arg(c, 1))
                             : int64_t((uint64_t(Arg(c, 2)) << 32) | uint32_t(Arg(c, 1)));
  gaddr out = Arg(c, 2 + k);
  uint32_t method = uint32_t(Arg(c, 3 + k));
  if (fd < 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  off_t r = lseek(fd, dist, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
  if (r < 0) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  if (out) {
    Wr32(out, uint32_t(r));
    Wr32(out + 4, uint32_t(uint64_t(r) >> 32));
  }
  return 1;
}

HLE64("kernel32.dll", SetEndOfFile, 1) {
  int fd = FdOf(Arg(c, 0));
  off_t pos = fd < 0 ? -1 : lseek(fd, 0, SEEK_CUR);
  if (pos < 0 || ftruncate(fd, pos) != 0) {
    SetLastError(fd < 0 ? kErrorInvalidHandle : ErrnoToWin(errno));
    return 0;
  }
  return 1;
}

HLE64("kernel32.dll", GetFileSize, 2) {
  int fd = FdOf(Arg(c, 0));
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0) {
    SetLastError(kErrorInvalidHandle);
    return 0xFFFFFFFF;
  }
  if (Arg(c, 1)) Wr32(Arg(c, 1), uint32_t(uint64_t(st.st_size) >> 32));
  return uint32_t(st.st_size);
}

// GetFileSizeEx(handle, LARGE_INTEGER* size).
HLE("kernel32.dll", GetFileSizeEx, 2) {
  int fd = FdOf(Arg(c, 0));
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  Wr64(Arg(c, 1), uint64_t(st.st_size));
  return 1;
}

// ---- console -------------------------------------------------------------
// A GUI process has no console: its standard handles, when it has any,
// are files or pipes. GetConsoleMode is how a C runtime tells a console
// from a file, and it fails for those with ERROR_INVALID_HANDLE, so the
// console paths (WriteConsoleW, ReadConsoleW) are never taken; they fail
// the same way if called.
HLE64("kernel32.dll", GetConsoleMode, 2) {
  SetLastError(kErrorInvalidHandle);
  return 0;
}
HLE64("kernel32.dll", WriteConsoleW, 5) {
  SetLastError(kErrorInvalidHandle);
  return 0;
}
HLE64("kernel32.dll", ReadConsoleW, 5) {
  SetLastError(kErrorInvalidHandle);
  return 0;
}
// Without a console the code pages are the system's OEM one.
HLE("kernel32.dll", GetConsoleOutputCP, 0) { return 437; }
HLE64("kernel32.dll", GetConsoleCP, 0) { return 437; }

HLE("kernel32.dll", GetFileTime, 4) {
  int fd = FdOf(Arg(c, 0));
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  if (Arg(c, 1)) PutFileTime(Arg(c, 1), platform::StatModifyTime(st));
  if (Arg(c, 2)) PutFileTime(Arg(c, 2), platform::StatAccessTime(st));
  if (Arg(c, 3)) PutFileTime(Arg(c, 3), platform::StatModifyTime(st));
  return 1;
}

HLE64("kernel32.dll", FlushFileBuffers, 1) {
  int fd = FdOf(Arg(c, 0));
  if (fd >= 0) fsync(fd);
  return 1;
}

// The host file behind guest path g: false (last error set) if none.
bool StatGuest(const std::string& g, struct stat* st, uint32_t* attrs) {
  std::string path = HostPath(g);
  if (stat(path.c_str(), st) != 0) {
    SetLastError(ErrnoToWin(errno));
    return false;
  }
  *attrs = AttributesOf(*st, path.substr(path.find_last_of('/') + 1));
  return true;
}

uint32_t GetFileAttributes(const std::string& g) {
  struct stat st;
  uint32_t a;
  return StatGuest(g, &st, &a) ? a : 0xFFFFFFFF;
}
HLE64("kernel32.dll", GetFileAttributesA, 1) { return GetFileAttributes(GStr(Arg(c, 0))); }

// GetShortPathNameA(long, short, size): the guest's volumes have no 8.3
// names (as an NTFS volume with their creation turned off), so the path
// comes back as given if it exists; 0 with the stat's error if not. The
// return is CopyOut's: the length, or the size needed.
HLE("kernel32.dll", GetShortPathNameA, 3) {
  std::string path = GStr(Arg(c, 0));
  struct stat st;
  uint32_t attrs;
  if (!StatGuest(path, &st, &attrs)) return 0;
  return CopyOut(Arg(c, 1), Arg(c, 2), path);
}
HLE64("kernel32.dll", GetFileAttributesW, 1) { return GetFileAttributes(WidePath(Arg(c, 0))); }

// GetFileAttributesEx{A,W}(path, GetFileExInfoStandard,
// WIN32_FILE_ATTRIBUTE_DATA*).
uint32_t GetFileAttributesEx(Ctx& c, const std::string& path) {
  if (Arg(c, 1) != 0) FatalF("GetFileAttributesEx: info level %llu", (unsigned long long)Arg(c, 1));
  struct stat st;
  uint32_t a;
  if (!StatGuest(path, &st, &a)) return 0;
  gaddr d = Arg(c, 2);
  Wr32(d + 0, a);
  PutFileTime(d + 4, platform::StatModifyTime(st));   // creation
  PutFileTime(d + 12, platform::StatAccessTime(st));  // access
  PutFileTime(d + 20, platform::StatModifyTime(st));  // write
  Wr32(d + 28, uint32_t(uint64_t(st.st_size) >> 32));
  Wr32(d + 32, uint32_t(st.st_size));
  return 1;
}
HLE64("kernel32.dll", GetFileAttributesExA, 3) { return GetFileAttributesEx(c, GStr(Arg(c, 0))); }
HLE64("kernel32.dll", GetFileAttributesExW, 3) { return GetFileAttributesEx(c, WidePath(Arg(c, 0))); }

// GetFileInformationByHandle(h, BY_HANDLE_FILE_INFORMATION*). The volume
// serial is GetVolumeInformation's; the file index the host inode.
// GetFileInformationByHandleEx(h, class, buf, size): the classes the C++
// library's <filesystem> asks for.
uint32_t RemoveGuest(const std::string& g, bool dir);

// SetFileInformationByHandle(file, class, info, size): the end of file
// (FileEndOfFileInfo, 6) and allocation size (FileAllocationInfo, 5) set
// the file's length; times (FileBasicInfo, 0) are accepted and not kept.
// Deletion: FileDispositionInfo (4, {BOOLEAN DeleteFile}) and
// FileDispositionInfoEx (21, {ULONG Flags}: DELETE 1, POSIX_SEMANTICS 2,
// IGNORE_READONLY_ATTRIBUTE 0x10) mark the file or directory to be deleted
// when its last handle closes, or clear the mark; with POSIX semantics its
// name goes at once (the open handles keep working). As on Windows, a
// read-only file is refused unless IGNORE_READONLY is given, and so is a
// directory that isn't empty. The C++ library's std::filesystem::remove
// deletes this way. Other classes (renames...) are refused with
// ERROR_INVALID_PARAMETER.
HLE64("kernel32.dll", SetFileInformationByHandle, 4) {
  auto obj = GetHandle(Arg(c, 0));
  int fd = obj ? FileFd(obj.get()) : -1;
  uint32_t cls = uint32_t(Arg(c, 1)), size = uint32_t(Arg(c, 3));
  gaddr d = Arg(c, 2);
  if (fd < 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  switch (cls) {
    case 0:  // FileBasicInfo
      return size >= 40 ? 1 : (SetLastError(24), 0);
    case 5:  // FileAllocationInfo: only grows the file
    case 6: {  // FileEndOfFileInfo
      if (size < 8) {
        SetLastError(24);  // ERROR_BAD_LENGTH
        return 0;
      }
      off_t want = off_t(Rd64(d));
      struct stat st;
      if (cls == 5 && fstat(fd, &st) == 0 && st.st_size >= want) return 1;
      if (ftruncate(fd, want) != 0) {
        SetLastError(ErrnoToWin(errno));
        return 0;
      }
      return 1;
    }
    case 4:     // FileDispositionInfo
    case 21: {  // FileDispositionInfoEx
      if (size < (cls == 4 ? 1u : 4u) || obj->type != KObject::Type::File) {
        SetLastError(cls == 4 ? 24 : kErrorInvalidParameter);
        return 0;
      }
      auto* f = static_cast<FileObject*>(obj.get());
      const uint32_t flags = cls == 4 ? (Rd8(d) ? 1u : 0u) : Rd32(d);
      if (!(flags & 1)) {
        f->delete_on_close = false;
        return 1;
      }
      struct stat st;
      if (fstat(fd, &st) != 0) {
        SetLastError(ErrnoToWin(errno));
        return 0;
      }
      f->dir = S_ISDIR(st.st_mode);
      if (!f->dir && !(flags & 0x10) && !(st.st_mode & S_IWUSR)) {
        SetLastError(kErrorAccessDenied);  // read-only
        return 0;
      }
      if (f->dir) {  // Windows refuses a non-empty directory now, not at close
        DIR* dd = fdopendir(dup(fd));
        bool empty = true;
        if (dd) {
          while (dirent* e = readdir(dd))
            if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) {
              empty = false;
              break;
            }
          closedir(dd);
        }
        if (!empty) {
          SetLastError(145);  // ERROR_DIR_NOT_EMPTY
          return 0;
        }
      }
      RECOMP_TRACE("  SetFileInformationByHandle(%s): delete%s\n", f->guest.c_str(),
                   flags & 2 ? " now (POSIX semantics)" : " on close");
      if (flags & 2) {
        f->delete_on_close = false;
        if (!RemoveGuest(f->guest, f->dir)) return 0;
        return 1;
      }
      f->delete_on_close = true;
      return 1;
    }
    default:
      WarnF("SetFileInformationByHandle: class %u not supported\n", cls);
      SetLastError(kErrorInvalidParameter);
      return 0;
  }
}

HLE64("kernel32.dll", GetFileInformationByHandleEx, 4) {
  auto o = GetHandle(uint32_t(Arg(c, 0)));
  struct stat st;
  if (!o || o->type != KObject::Type::File || fstat(FileFd(o.get()), &st) != 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  const std::string& g = static_cast<FileObject*>(o.get())->guest;
  std::string name = g.substr(g.find_last_of('\\') + 1);
  uint32_t cls = uint32_t(Arg(c, 1)), size = uint32_t(Arg(c, 3));
  gaddr d = Arg(c, 2);
  auto need = [&](uint32_t n) {
    if (size >= n) return true;
    SetLastError(24);  // ERROR_BAD_LENGTH
    return false;
  };
  switch (cls) {
    case 0:  // FileBasicInfo: creation, access, write, change times; attributes
      if (!need(40)) return 0;
      PutFileTime(d + 0, platform::StatModifyTime(st));
      PutFileTime(d + 8, platform::StatAccessTime(st));
      PutFileTime(d + 16, platform::StatModifyTime(st));
      PutFileTime(d + 24, platform::StatModifyTime(st));
      Wr32(d + 32, AttributesOf(st, name));
      Wr32(d + 36, 0);
      return 1;
    case 1:  // FileStandardInfo: allocation size, end of file, links, flags
      if (!need(24)) return 0;
      Wr64(d + 0, S_ISDIR(st.st_mode) ? 0 : (uint64_t(st.st_size) + 4095) & ~uint64_t(4095));
      Wr64(d + 8, S_ISDIR(st.st_mode) ? 0 : uint64_t(st.st_size));
      Wr32(d + 16, uint32_t(st.st_nlink));
      Wr8(d + 20, 0);                           // DeletePending
      Wr8(d + 21, S_ISDIR(st.st_mode) ? 1 : 0); // Directory
      Wr16(d + 22, 0);
      return 1;
    case 9:  // FileAttributeTagInfo: attributes, reparse tag
      if (!need(8)) return 0;
      Wr32(d + 0, AttributesOf(st, name));
      Wr32(d + 4, 0);
      return 1;
    case 18:  // FileIdInfo: volume serial, 128-bit file id
      if (!need(24)) return 0;
      Wr64(d + 0, kVolumeSerial);
      Wr64(d + 8, uint64_t(st.st_ino));
      Wr64(d + 16, 0);
      return 1;
    default:
      WarnF("GetFileInformationByHandleEx: class %u not supported\n", cls);
      SetLastError(kErrorInvalidParameter);
      return 0;
  }
}

uint32_t CopyOutW(gaddr buf, uint32_t size, const std::string& s);

// GetFinalPathNameByHandleW(h, buf, cch, flags): the path the handle was
// opened by (there are no links to resolve in the guest's view). Only
// VOLUME_NAME_DOS (flags & 6 == 0), as "\\?\C:\...".
HLE("kernel32.dll", GetFinalPathNameByHandleW, 4) {
  auto o = GetHandle(Arg(c, 0));
  if (!o || o->type != KObject::Type::File) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  if (Arg(c, 3) & 6) {
    WarnF("GetFinalPathNameByHandleW: flags %X not supported\n", Arg(c, 3));
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  std::string p = "\\\\?\\" + static_cast<FileObject*>(o.get())->guest;
  return CopyOutW(Arg(c, 1), Arg(c, 2), p);
}

HLE64("kernel32.dll", GetFileInformationByHandle, 2) {
  auto o = GetHandle(Arg(c, 0));
  struct stat st;
  if (!o || o->type != KObject::Type::File || fstat(FileFd(o.get()), &st) != 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  gaddr d = Arg(c, 1);
  Wr32(d + 0, AttributesOf(st, {}));
  PutFileTime(d + 4, platform::StatModifyTime(st));
  PutFileTime(d + 12, platform::StatAccessTime(st));
  PutFileTime(d + 20, platform::StatModifyTime(st));
  Wr32(d + 28, kVolumeSerial);
  Wr32(d + 32, uint32_t(uint64_t(st.st_size) >> 32));
  Wr32(d + 36, uint32_t(st.st_size));
  Wr32(d + 40, uint32_t(st.st_nlink));
  Wr32(d + 44, uint32_t(uint64_t(st.st_ino) >> 32));
  Wr32(d + 48, uint32_t(st.st_ino));
  return 1;
}

HLE("kernel32.dll", SetFileAttributesA, 2) { return 1; }
HLE64("kernel32.dll", SetFileAttributesW, 2) { return 1; }

// A file that exists only in the (read-only) install cannot be deleted
// or moved away.
bool InstallOnly(const std::string& gpath) {
  return HostPath(gpath) != HostPath(gpath, PathAccess::kWrite);
}

// Deletes a guest file or (empty) directory: what DeleteFile and
// RemoveDirectory do, for deletion through a handle.
uint32_t RemoveGuest(const std::string& g, bool dir) {
  if (InstallOnly(g) && Exists(HostPath(g))) {
    SetLastError(kErrorAccessDenied);
    return 0;
  }
  const std::string path = HostPath(g, PathAccess::kWrite);
  if ((dir ? rmdir(path.c_str()) : unlink(path.c_str())) != 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  return 1;
}

uint32_t DeleteFile(const std::string& g) {
  if (InstallOnly(g) && Exists(HostPath(g))) {
    SetLastError(kErrorAccessDenied);
    return 0;
  }
  if (unlink(HostPath(g, PathAccess::kWrite).c_str()) != 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  return 1;
}
HLE64("kernel32.dll", DeleteFileA, 1) { return DeleteFile(GStr(Arg(c, 0))); }
HLE64("kernel32.dll", DeleteFileW, 1) { return DeleteFile(WidePath(Arg(c, 0))); }

HLE("kernel32.dll", CopyFileA, 3) {
  std::string from = HostPath(GStr(Arg(c, 0)));
  std::string to = HostPath(GStr(Arg(c, 1)), PathAccess::kWrite);
  int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  int flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | (Arg(c, 2) ? O_EXCL : 0);
  int out = open(to.c_str(), flags, 0644);
  if (out < 0) {
    SetLastError(errno == EEXIST ? 80 /* ERROR_FILE_EXISTS */ : ErrnoToWin(errno));
    close(in);
    return 0;
  }
  char buf[65536];
  ssize_t n;
  bool ok = true;
  while ((n = read(in, buf, sizeof buf)) > 0)
    if (write(out, buf, size_t(n)) != n) ok = false;
  if (n < 0) ok = false;
  close(in);
  close(out);
  if (!ok) SetLastError(ErrnoToWin(errno));
  return ok ? 1 : 0;
}

uint32_t MoveFile(const std::string& gfrom, const std::string& gto, bool replace) {
  if (InstallOnly(gfrom) && Exists(HostPath(gfrom))) {
    SetLastError(kErrorAccessDenied);
    return 0;
  }
  std::string from = HostPath(gfrom, PathAccess::kWrite);
  std::string to = HostPath(gto, PathAccess::kWrite);
  struct stat st;
  if (stat(HostPath(gto).c_str(), &st) == 0 && (!replace || S_ISDIR(st.st_mode))) {
    SetLastError(replace ? kErrorAccessDenied : kErrorAlreadyExists);
    return 0;
  }
  if (rename(from.c_str(), to.c_str()) != 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  return 1;
}

HLE64("kernel32.dll", MoveFileA, 2) { return MoveFile(GStr(Arg(c, 0)), GStr(Arg(c, 1)), false); }
HLE64("kernel32.dll", MoveFileW, 2) {
  return MoveFile(WidePath(Arg(c, 0)), WidePath(Arg(c, 1)), false);
}

// MoveFileExW(from, to, flags): MOVEFILE_REPLACE_EXISTING, and
// MOVEFILE_COPY_ALLOWED / MOVEFILE_WRITE_THROUGH, which change nothing
// here (one volume; host renames are durable once they return).
HLE64("kernel32.dll", MoveFileExW, 3) {
  uint32_t flags = uint32_t(Arg(c, 2));
  if (flags & ~0xBu) FatalF("MoveFileExW: flags %X not supported", flags);
  return MoveFile(WidePath(Arg(c, 0)), WidePath(Arg(c, 1)), flags & 1);
}

uint32_t CreateDirectory(const std::string& g) {
  if (Exists(HostPath(g))) {
    SetLastError(kErrorAlreadyExists);
    return 0;
  }
  std::string path = HostPath(g, PathAccess::kWrite);
  MakeDirs(path.substr(0, path.find_last_of('/')));
  if (mkdir(path.c_str(), 0755) != 0) {
    SetLastError(errno == EEXIST ? kErrorAlreadyExists : ErrnoToWin(errno));
    return 0;
  }
  return 1;
}
HLE64("kernel32.dll", CreateDirectoryA, 2) { return CreateDirectory(GStr(Arg(c, 0))); }
HLE64("kernel32.dll", CreateDirectoryW, 2) { return CreateDirectory(WidePath(Arg(c, 0))); }

uint32_t RemoveDirectory(const std::string& g) {
  if (rmdir(HostPath(g, PathAccess::kWrite).c_str()) != 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  return 1;
}
HLE64("kernel32.dll", RemoveDirectoryA, 1) { return RemoveDirectory(GStr(Arg(c, 0))); }
HLE64("kernel32.dll", RemoveDirectoryW, 1) { return RemoveDirectory(WidePath(Arg(c, 0))); }

HLE("kernel32.dll", GetCurrentDirectoryA, 2) {
  uint32_t size = Arg(c, 0), buf = Arg(c, 1);
  std::string d = GuestCurrentDir();
  return CopyOut(buf, size, d);
}

HLE64("kernel32.dll", GetFullPathNameA, 4) {
  std::string full = AbsGuest(GStr(Arg(c, 0)));
  uint32_t size = uint32_t(Arg(c, 1));
  gaddr buf = Arg(c, 2), filepart = Arg(c, 3);
  uint32_t r = CopyOut(buf, size, full);
  if (r == full.size() && filepart) {
    size_t s = full.find_last_of('\\');
    WrPtr(filepart, s == std::string::npos || s + 1 == full.size() ? 0 : buf + (s + 1));
  }
  return r;
}

// SearchPathA(path, file, extension, cap, buf, &file part): the first
// directory holding the file, in `path` (';'-separated) or, without one,
// Windows' order: the program's directory, the current directory, the
// system and Windows directories. The extension is added only to a name
// without one. A name with a directory part is only looked up as given.
HLE("kernel32.dll", SearchPathA, 6) {
  std::string name = GStr(Arg(c, 1));
  // The file name after any directory part (npos + 1 is 0: no directory).
  const std::string base = name.substr(name.find_last_of("\\/") + 1);
  if (Arg(c, 2) && base.find('.') == std::string::npos) name += GStr(Arg(c, 2));
  std::vector<std::string> dirs;
  if (name.find_first_of("\\/") != std::string::npos) {
    dirs.push_back("");
  } else if (Arg(c, 0)) {
    std::string p = GStr(Arg(c, 0));
    for (size_t a = 0, b; a <= p.size(); a = b + 1) {
      b = p.find(';', a);
      if (b == std::string::npos) b = p.size();
      if (b > a) dirs.push_back(p.substr(a, b - a));
    }
  } else {
    std::string exe = GuestExePath();
    dirs = {exe.substr(0, exe.find_last_of('\\')), AbsGuest("."), "C:\\WINDOWS\\system32",
            "C:\\WINDOWS"};
  }
  for (const std::string& d : dirs) {
    std::string g = AbsGuest(d.empty() ? name : d + "\\" + name);
    struct stat st;
    if (stat(HostPath(g).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
    uint32_t cap = Arg(c, 3), buf = Arg(c, 4), filepart = Arg(c, 5);
    uint32_t r = CopyOut(buf, cap, g);
    if (r == g.size() && filepart) Wr32(filepart, buf + uint32_t(g.find_last_of('\\') + 1));
    return r;
  }
  SetLastError(2);  // ERROR_FILE_NOT_FOUND
  return 0;
}

HLE("kernel32.dll", GetLongPathNameA, 3) {
  std::string full = GStr(Arg(c, 0));
  uint32_t buf = Arg(c, 1), size = Arg(c, 2);
  return CopyOut(buf, size, full);
}

HLE64("kernel32.dll", GetTempPathA, 2) {
  std::string t = "C:\\TEMP\\";
  MakeDirs(UserDataRoot() + "/TEMP");
  return CopyOut(Arg(c, 1), Arg(c, 0), t);
}

HLE("kernel32.dll", GetTempFileNameA, 4) {
  static uint32_t counter = 1;
  std::string dir = GStr(Arg(c, 0)), prefix = GStr(Arg(c, 1)).substr(0, 3);
  uint32_t unique = Arg(c, 2) ? Arg(c, 2) : counter++;
  char name[32];
  std::snprintf(name, sizeof name, "%s%X.TMP", prefix.c_str(), unique & 0xFFFF);
  std::string full = dir;
  if (!full.empty() && full.back() != '\\') full += '\\';
  full += name;
  PutStr(Arg(c, 3), full);
  if (!Arg(c, 2)) {
    int fd = open(HostPath(full, PathAccess::kWrite).c_str(),
                  O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    if (fd >= 0) close(fd);
  }
  return unique;
}

// ---- find ------------------------------------------------------------------

// WIN32_FIND_DATAA (320 bytes) or WIN32_FIND_DATAW (592): the same
// fields, then cFileName[260] and cAlternateFileName[14] in CHARs or WCHARs.
void FillFindData(gaddr fd, const std::string& host_dir, const std::string& name,
                  bool wide = false) {
  struct stat st{};
  stat((host_dir + "/" + name).c_str(), &st);
  std::memset(GPtr(fd), 0, wide ? 592 : 320);
  Wr32(fd + 0, AttributesOf(st, name));
  PutFileTime(fd + 4, platform::StatModifyTime(st));   // creation
  PutFileTime(fd + 12, platform::StatAccessTime(st));  // access
  PutFileTime(fd + 20, platform::StatModifyTime(st));  // write
  Wr32(fd + 28, uint32_t(uint64_t(st.st_size) >> 32));
  Wr32(fd + 32, uint32_t(st.st_size));
  if (wide) {
    std::u16string w = Widen(name.substr(0, 259));  // names are Windows-1252
    std::memcpy(GPtr(fd + 44), w.data(), w.size() * 2);
  } else {
    PutStr(fd + 44, name.substr(0, 259));  // cFileName
  }
  // cAlternateFileName (8.3) left empty.
}

// Test hook: <ID>_TEST_HIDE="piece;piece..." leaves out of directory
// listings every entry whose guest path (lower case, backslashes) contains
// a piece, e.g. "resource\\video\\intro\\": a game that plays what a folder
// holds (logo videos) then finds nothing there. Nothing on disk changes.
bool HiddenForTest(const std::string& guest_path) {
  static const std::vector<std::string> pieces = [] {
    std::vector<std::string> v;
    if (const char* e = Env("TEST_HIDE")) {
      std::string all = Lower(e);
      for (char& ch : all)
        if (ch == '/') ch = '\\';
      for (size_t a = 0; a <= all.size();) {
        size_t b = all.find(';', a);
        if (b == std::string::npos) b = all.size();
        if (b > a) v.push_back(all.substr(a, b - a));
        a = b + 1;
      }
    }
    return v;
  }();
  if (pieces.empty()) return false;
  const std::string p = Lower(guest_path);
  for (const std::string& piece : pieces)
    if (p.find(piece) != std::string::npos) return true;
  return false;
}

// Opens a search for `pattern` (a guest path whose last component may hold
// wildcards) and fills the first entry; INVALID_HANDLE_VALUE and
// ERROR_FILE_NOT_FOUND if nothing matches.
uint64_t FindFirst(const std::string& guest_pattern, gaddr fd, bool wide) {
  std::string pattern = AbsGuest(guest_pattern);
  size_t s = pattern.find_last_of('\\');
  std::string gdir = pattern.substr(0, s), mask = pattern.substr(s + 1);
  if (gdir.size() == 2) gdir += '\\';
  auto f = std::make_shared<FindObject>();
  std::set<std::string> seen;  // lower case: the overlay hides the install
  for (const std::string& hdir : HostDirs(gdir)) {
    DIR* d = opendir(hdir.c_str());
    if (!d) continue;
    while (dirent* e = readdir(d)) {
      std::string ln = Lower(e->d_name);
      if (MatchFileMask(mask, e->d_name) && !HiddenForTest(gdir + "\\" + e->d_name) &&
          seen.insert(ln).second)
        f->entries.push_back({hdir, e->d_name});
    }
    closedir(d);
  }
  RECOMP_TRACE("  FindFirst %s: %zu found\n", pattern.c_str(), f->entries.size());
  if (f->entries.empty()) {
    SetLastError(kErrorFileNotFound);
    return HandleRet(kInvalidHandle);
  }
  FillFindData(fd, f->entries[0].first, f->entries[0].second, wide);
  f->next = 1;
  return HandleRet(NewHandle(f));
}

uint32_t FindNext(uint32_t h, gaddr fd, bool wide) {
  auto o = GetHandle(h);
  if (!o || o->type != KObject::Type::Find) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  auto* f = static_cast<FindObject*>(o.get());
  if (f->next >= f->entries.size()) {
    SetLastError(kErrorNoMoreFiles);
    return 0;
  }
  const auto& [hdir, name] = f->entries[f->next++];
  FillFindData(fd, hdir, name, wide);
  return 1;
}

HLE("kernel32.dll", FindFirstFileA, 2) { return FindFirst(GStr(Arg(c, 0)), Arg(c, 1), false); }
HLE64("kernel32.dll", FindNextFileA, 2) { return FindNext(Arg(c, 0), Arg(c, 1), false); }

// FindFirstFileExW(name, info level, data, search op, filter, flags). Both
// info levels (FindExInfoStandard, FindExInfoBasic) fill the same fields
// here, as cAlternateFileName stays empty anyway. The search ops only
// narrow what a file system may return (FindExSearchLimitToDirectories is
// advisory), and FIND_FIRST_EX_CASE_SENSITIVE does not apply to guest
// paths, which are case-insensitive.
HLE64("kernel32.dll", FindFirstFileExW, 6) {
  uint32_t level = Arg(c, 1);
  if (level > 1 || Arg(c, 3) > 1) {  // FindExSearchLimitToDevices: not supported
    SetLastError(kErrorInvalidParameter);
    return HandleRet(kInvalidHandle);
  }
  return FindFirst(WidePath(Arg(c, 0)), Arg(c, 2), true);
}
HLE64("kernel32.dll", FindFirstFileW, 2) { return FindFirst(WidePath(Arg(c, 0)), Arg(c, 1), true); }
HLE64("kernel32.dll", FindNextFileW, 2) { return FindNext(Arg(c, 0), Arg(c, 1), true); }

HLE64("kernel32.dll", FindClose, 1) {
  return CloseHandleValue(Arg(c, 0)) ? 1 : 0;
}

// ---- volumes -----------------------------------------------------------------

HLE64("kernel32.dll", GetDriveTypeA, 1) { return 3; }  // DRIVE_FIXED
HLE64("kernel32.dll", GetDriveTypeW, 1) { return 3; }
HLE64("kernel32.dll", GetSystemDirectoryW, 2) {
  return CopyOutW(Arg(c, 0), uint32_t(Arg(c, 1)), "C:\\WINDOWS\\system32");
}

HLE("kernel32.dll", GetLogicalDriveStringsA, 2) {
  const char s[] = "C:\\\0";  // double-NUL terminated list
  if (Arg(c, 0) < sizeof s) return sizeof s;
  std::memcpy(GPtr(Arg(c, 1)), s, sizeof s);
  return sizeof s - 1;
}

HLE("kernel32.dll", GetVolumeInformationA, 8) {
  if (Arg(c, 1) && Arg(c, 2)) PutStr(Arg(c, 1), win32::Upper(Game().id));
  if (Arg(c, 3)) Wr32(Arg(c, 3), kVolumeSerial);
  if (Arg(c, 4)) Wr32(Arg(c, 4), 255);
  if (Arg(c, 5)) Wr32(Arg(c, 5), 0x700FF);
  if (Arg(c, 6) && Arg(c, 7)) PutStr(Arg(c, 6), "NTFS");
  return 1;
}

HLE("kernel32.dll", GetDiskFreeSpaceExA, 4) {
  // 8 GiB free of 64 GiB, reported in 64-bit ULARGE_INTEGERs.
  if (Arg(c, 1)) Wr64(Arg(c, 1), 8ull << 30);
  if (Arg(c, 2)) Wr64(Arg(c, 2), 64ull << 30);
  if (Arg(c, 3)) Wr64(Arg(c, 3), 8ull << 30);
  return 1;
}


// ---- the W forms of the path queries --------------------------------------------

// CopyOut for a WCHAR buffer of `size` characters.
uint32_t CopyOutW(gaddr buf, uint32_t size, const std::string& s) {
  if (size > s.size()) {
    PutWStr(buf, s);
    return uint32_t(s.size());
  }
  return uint32_t(s.size() + 1);
}

HLE64("kernel32.dll", GetCurrentDirectoryW, 2) { return CopyOutW(Arg(c, 1), Arg(c, 0), GuestCurrentDir()); }

// SetCurrentDirectory: relative paths resolve against the new directory.
uint32_t SetCurrentDirectory(const std::string& g) {
  std::string full = AbsGuest(g);
  while (full.size() > 3 && full.back() == '\\') full.pop_back();
  struct stat st;
  if (stat(HostPath(full).c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    SetLastError(3);  // ERROR_PATH_NOT_FOUND
    return 0;
  }
  std::lock_guard<std::mutex> l(g_cwd_mu);
  g_cwd = full;
  return 1;
}
HLE("kernel32.dll", SetCurrentDirectoryA, 1) { return SetCurrentDirectory(GStr(Arg(c, 0))); }
HLE("kernel32.dll", SetCurrentDirectoryW, 1) { return SetCurrentDirectory(WidePath(Arg(c, 0))); }

HLE64("kernel32.dll", GetFullPathNameW, 4) {
  std::string full = AbsGuest(WidePath(Arg(c, 0)));
  uint32_t size = uint32_t(Arg(c, 1));
  gaddr buf = Arg(c, 2), filepart = Arg(c, 3);
  uint32_t r = CopyOutW(buf, size, full);
  if (r == full.size() && filepart) {
    size_t s = full.find_last_of('\\');
    WrPtr(filepart, s == std::string::npos || s + 1 == full.size() ? 0 : buf + 2 * (s + 1));
  }
  return r;
}

HLE64("kernel32.dll", GetTempPathW, 2) {
  MakeDirs(UserDataRoot() + "/TEMP");
  return CopyOutW(Arg(c, 1), Arg(c, 0), "C:\\TEMP\\");
}

HLE("kernel32.dll", GetWindowsDirectoryA, 2) { return CopyOut(Arg(c, 0), Arg(c, 1), "C:\\WINDOWS"); }
HLE("kernel32.dll", GetWindowsDirectoryW, 2) { return CopyOutW(Arg(c, 0), Arg(c, 1), "C:\\WINDOWS"); }

HLE64("kernel32.dll", GetDiskFreeSpaceExW, 4) {
  if (Arg(c, 1)) Wr64(Arg(c, 1), 8ull << 30);
  if (Arg(c, 2)) Wr64(Arg(c, 2), 64ull << 30);
  if (Arg(c, 3)) Wr64(Arg(c, 3), 8ull << 30);
  return 1;
}

// CreateDirectoryExW(template, new, security): the template's attributes
// are not copied.
HLE("kernel32.dll", CreateDirectoryExW, 3) { return CreateDirectory(WidePath(Arg(c, 1))); }

HLE64("kernel32.dll", FindFirstFileExA, 6) {
  if (Arg(c, 1) > 1 || Arg(c, 3) > 1) {
    SetLastError(kErrorInvalidParameter);
    return HandleRet(kInvalidHandle);
  }
  return FindFirst(GStr(Arg(c, 0)), Arg(c, 2), false);
}

// CopyFileW(from, to, failIfExists)
HLE64("kernel32.dll", CopyFileW, 3) {
  std::string from = HostPath(WidePath(Arg(c, 0)));
  std::string to = HostPath(WidePath(Arg(c, 1)), PathAccess::kWrite);
  int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | (Arg(c, 2) ? O_EXCL : 0), 0644);
  if (out < 0) {
    SetLastError(errno == EEXIST ? 80 /* ERROR_FILE_EXISTS */ : ErrnoToWin(errno));
    close(in);
    return 0;
  }
  char buf[65536];
  ssize_t n;
  bool ok = true;
  while ((n = read(in, buf, sizeof buf)) > 0) ok &= write(out, buf, size_t(n)) == n;
  close(in);
  close(out);
  if (!ok) SetLastError(39);  // ERROR_HANDLE_DISK_FULL
  return ok ? 1 : 0;
}

// SetFileTime(file, creation, access, write): file times are the host's;
// the request succeeds without changing them.
HLE64("kernel32.dll", SetFileTime, 4) { return 1; }

}  // namespace
}  // namespace hle

namespace {
// A handle's deletion on close (FileObject), when its last handle closes.
void RemoveOnClose(const std::string& guest, bool dir) {
  if (!hle::RemoveGuest(guest, dir))
    RECOMP_TRACE("  delete on close of %s failed\n", guest.c_str());
}
}  // namespace
}  // namespace recomp

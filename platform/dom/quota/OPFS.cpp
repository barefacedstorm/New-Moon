/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "OPFS.h"

#include "mozilla/Mutex.h"
#include "mozilla/dom/BlobBinding.h"
#include "mozilla/dom/File.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/ToJSValue.h"
#include "mozilla/dom/WorkerPrivate.h"
#include "mozilla/dom/WorkerRunnable.h"
#include "nsAppDirectoryServiceDefs.h"
#include "nsDirectoryServiceUtils.h"
#include "nsIDocument.h"
#include "nsIFile.h"
#include "nsIInputStream.h"
#include "nsIPrincipal.h"
#include "nsJSUtils.h"
#include "nsPIDOMWindow.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace mozilla::dom::workers;

namespace mozilla {
namespace dom {

namespace {

static StaticMutex sOPFSPathMutex;
static nsCString sCachedProfilePath;

static bool
EnsureDirectoryRecursive(const nsACString& aPath)
{
  nsCString path(aPath);
  if (path.IsEmpty()) {
    return false;
  }
  struct stat st;
  if (stat(path.get(), &st) == 0) {
    return S_ISDIR(st.st_mode);
  }
  char* buf = strdup(path.get());
  if (!buf) {
    return false;
  }
  size_t len = strlen(buf);
  for (size_t i = 1; i < len; ++i) {
    if (buf[i] == '/') {
      buf[i] = '\0';
      mkdir(buf, 0700);
      buf[i] = '/';
    }
  }
  bool ok = (mkdir(buf, 0700) == 0 || errno == EEXIST);
  free(buf);
  return ok;
}

static bool
RemovePathRecursive(const nsACString& aPath)
{
  nsCString path(aPath);
  struct stat st;
  if (lstat(path.get(), &st) != 0) {
    return false;
  }
  if (S_ISDIR(st.st_mode)) {
    DIR* dir = opendir(path.get());
    if (!dir) {
      return false;
    }
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
        continue;
      }
      nsCString child = path;
      child.Append('/');
      child.Append(ent->d_name);
      if (!RemovePathRecursive(child)) {
        closedir(dir);
        return false;
      }
    }
    closedir(dir);
    return rmdir(path.get()) == 0;
  }
  return unlink(path.get()) == 0;
}

static bool
IsValidEntryName(const nsAString& aName, nsCString& aOutUtf8)
{
  if (aName.IsEmpty() || aName.EqualsLiteral(".") || aName.EqualsLiteral("..")) {
    return false;
  }
  for (uint32_t i = 0; i < aName.Length(); ++i) {
    char16_t c = aName[i];
    if (c == '/' || c == '\\' || c == 0) {
      return false;
    }
  }
  CopyUTF16toUTF8(aName, aOutUtf8);
  return !aOutUtf8.IsEmpty();
}

static void
SanitizeOrigin(const nsACString& aOrigin, nsCString& aOut)
{
  aOut.Truncate();
  const char* data = aOrigin.BeginReading();
  const char* end = aOrigin.EndReading();
  for (; data < end; ++data) {
    char c = *data;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '.' || c == '-') {
      aOut.Append(c);
    } else {
      aOut.Append('_');
    }
  }
  if (aOut.IsEmpty()) {
    aOut.AssignLiteral("default");
  }
}

class GetProfilePathRunnable final : public WorkerMainThreadRunnable
{
public:
  nsCString mPath;

  explicit GetProfilePathRunnable(WorkerPrivate* aWorkerPrivate)
    : WorkerMainThreadRunnable(aWorkerPrivate, NS_LITERAL_CSTRING("OPFS::GetProfilePath"))
  {}

  bool MainThreadRun() override
  {
    nsCOMPtr<nsIFile> profDir;
    if (NS_SUCCEEDED(NS_GetSpecialDirectory(NS_APP_USER_PROFILE_50_DIR, getter_AddRefs(profDir))) && profDir) {
      profDir->GetNativePath(mPath);
    }
    return true;
  }
};

static nsresult
GetProfilePath(nsCString& aOutPath)
{
  {
    StaticMutexAutoLock lock(sOPFSPathMutex);
    if (!sCachedProfilePath.IsEmpty()) {
      aOutPath = sCachedProfilePath;
      return NS_OK;
    }
  }

  nsCString path;
  if (NS_IsMainThread()) {
    nsCOMPtr<nsIFile> profDir;
    nsresult rv = NS_GetSpecialDirectory(NS_APP_USER_PROFILE_50_DIR, getter_AddRefs(profDir));
    if (NS_SUCCEEDED(rv) && profDir) {
      profDir->GetNativePath(path);
    }
  } else {
    WorkerPrivate* wp = GetCurrentThreadWorkerPrivate();
    if (wp) {
      RefPtr<GetProfilePathRunnable> r = new GetProfilePathRunnable(wp);
      ErrorResult rv;
      r->Dispatch(Terminating, rv);
      if (!rv.Failed()) {
        path = r->mPath;
      } else {
        rv.SuppressException();
      }
    }
  }

  if (path.IsEmpty()) {
    const char* home = getenv("HOME");
    if (home && home[0]) {
      path.Assign(home);
      path.AppendLiteral("/.palemoon-opfs");
    } else {
      path.AssignLiteral("/tmp/palemoon-opfs");
    }
  }

  {
    StaticMutexAutoLock lock(sOPFSPathMutex);
    sCachedProfilePath = path;
  }
  aOutPath = path;
  return NS_OK;
}

static bool
CopyFileContents(const char* aSrc, int aDstFd)
{
  int srcFd = open(aSrc, O_RDONLY);
  if (srcFd < 0) {
    return false;
  }
  uint8_t buf[16384];
  ssize_t n;
  while ((n = read(srcFd, buf, sizeof(buf))) > 0) {
    ssize_t off = 0;
    while (off < n) {
      ssize_t w = write(aDstFd, buf + off, n - off);
      if (w <= 0) {
        close(srcFd);
        return false;
      }
      off += w;
    }
  }
  close(srcFd);
  return n == 0;
}

} // anonymous namespace

nsresult
ResolveOPFSOriginRoot(nsIGlobalObject* aGlobal, nsCString& aOutRootPath)
{
  nsCString origin;
  if (NS_IsMainThread()) {
    nsCOMPtr<nsPIDOMWindowInner> win = do_QueryInterface(aGlobal);
    nsCOMPtr<nsIDocument> doc = win ? win->GetExtantDoc() : nullptr;
    nsCOMPtr<nsIPrincipal> principal = doc ? doc->NodePrincipal() : nullptr;
    if (principal) {
      principal->GetOrigin(origin);
    }
  } else {
    WorkerPrivate* wp = GetCurrentThreadWorkerPrivate();
    if (wp && wp->GetPrincipal()) {
      wp->GetPrincipal()->GetOrigin(origin);
    }
  }

  nsCString sanitized;
  SanitizeOrigin(origin, sanitized);

  nsCString profPath;
  nsresult rv = GetProfilePath(profPath);
  if (NS_FAILED(rv)) {
    return rv;
  }

  aOutRootPath = profPath;
  aOutRootPath.AppendLiteral("/storage/opfs/");
  aOutRootPath.Append(sanitized);

  if (!EnsureDirectoryRecursive(aOutRootPath)) {
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

/*******************************************************************************
 * FileSystemHandle
 ******************************************************************************/

FileSystemHandle::FileSystemHandle(nsIGlobalObject* aGlobal,
                                   FileSystemHandleKind aKind,
                                   const nsAString& aName,
                                   const nsACString& aDiskPath,
                                   const nsACString& aRootPath)
  : mOwner(aGlobal)
  , mKind(aKind)
  , mName(aName)
  , mDiskPath(aDiskPath)
  , mRootPath(aRootPath)
{
}

already_AddRefed<Promise>
FileSystemHandle::IsSameEntry(FileSystemHandle& aOther, ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  bool same = (mKind == aOther.mKind) && mDiskPath.Equals(aOther.mDiskPath);
  promise->MaybeResolve(same);
  return promise.forget();
}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(FileSystemHandle, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(FileSystemHandle)
NS_IMPL_CYCLE_COLLECTING_RELEASE(FileSystemHandle)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(FileSystemHandle)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

JSObject*
FileSystemHandle::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return FileSystemHandleBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * FileSystemFileHandle
 ******************************************************************************/

FileSystemFileHandle::FileSystemFileHandle(nsIGlobalObject* aGlobal,
                                           const nsAString& aName,
                                           const nsACString& aDiskPath,
                                           const nsACString& aRootPath)
  : FileSystemHandle(aGlobal, FileSystemHandleKind::File, aName, aDiskPath, aRootPath)
{
}

already_AddRefed<Promise>
FileSystemFileHandle::GetFile(ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  struct stat st;
  if (stat(mDiskPath.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
    promise->MaybeReject(NS_ERROR_DOM_NOT_FOUND_ERR);
    return promise.forget();
  }

  uint64_t size = static_cast<uint64_t>(st.st_size);
  void* mem = malloc(size > 0 ? size : 1);
  if (!mem) {
    promise->MaybeReject(NS_ERROR_OUT_OF_MEMORY);
    return promise.forget();
  }

  if (size > 0) {
    int fd = open(mDiskPath.get(), O_RDONLY);
    if (fd < 0) {
      free(mem);
      promise->MaybeReject(NS_ERROR_DOM_FILE_NOT_READABLE_ERR);
      return promise.forget();
    }
    uint64_t total = 0;
    while (total < size) {
      ssize_t n = read(fd, static_cast<uint8_t*>(mem) + total, size - total);
      if (n <= 0) {
        break;
      }
      total += static_cast<uint64_t>(n);
    }
    close(fd);
    size = total;
  }

  int64_t lastModMs = static_cast<int64_t>(st.st_mtime) * 1000;
  RefPtr<File> file = File::CreateMemoryFile(
    mOwner, mem, size, mName, EmptyString(), lastModMs);
  if (!file) {
    free(mem);
    promise->MaybeReject(NS_ERROR_FAILURE);
    return promise.forget();
  }

  promise->MaybeResolve(file);
  return promise.forget();
}

already_AddRefed<Promise>
FileSystemFileHandle::CreateWritable(
  const FileSystemCreateWritableOptions& aOptions,
  ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  RefPtr<FileSystemWritableFileStream> stream =
    new FileSystemWritableFileStream(mOwner, mDiskPath, aOptions.mKeepExistingData);
  promise->MaybeResolve(stream);
  return promise.forget();
}

already_AddRefed<Promise>
FileSystemFileHandle::CreateSyncAccessHandle(ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  int fd = open(mDiskPath.get(), O_RDWR | O_CREAT, 0600);
  if (fd < 0) {
    promise->MaybeReject(NS_ERROR_DOM_NO_MODIFICATION_ALLOWED_ERR);
    return promise.forget();
  }
  RefPtr<FileSystemSyncAccessHandle> handle =
    new FileSystemSyncAccessHandle(mOwner, fd);
  promise->MaybeResolve(handle);
  return promise.forget();
}

already_AddRefed<Promise>
FileSystemFileHandle::Move(const nsAString& aNewName, ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  nsCString utf8Name;
  if (!IsValidEntryName(aNewName, utf8Name)) {
    promise->MaybeReject(NS_ERROR_DOM_TYPE_ERR);
    return promise.forget();
  }
  int32_t slash = mDiskPath.RFindChar('/');
  nsCString newPath;
  if (slash >= 0) {
    newPath.Assign(Substring(mDiskPath, 0, slash + 1));
  }
  newPath.Append(utf8Name);
  if (rename(mDiskPath.get(), newPath.get()) != 0) {
    promise->MaybeReject(errno == ENOENT ? NS_ERROR_DOM_NOT_FOUND_ERR : NS_ERROR_FAILURE);
    return promise.forget();
  }
  mDiskPath = newPath;
  mName = aNewName;
  promise->MaybeResolveWithUndefined();
  return promise.forget();
}

already_AddRefed<Promise>
FileSystemFileHandle::Move(FileSystemDirectoryHandle& aDestination,
                           const Optional<nsAString>& aNewName,
                           ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  nsAutoString targetName(aNewName.WasPassed() ? aNewName.Value() : mName);
  nsCString utf8Name;
  if (!IsValidEntryName(targetName, utf8Name)) {
    promise->MaybeReject(NS_ERROR_DOM_TYPE_ERR);
    return promise.forget();
  }
  nsCString newPath(aDestination.DiskPath());
  newPath.Append('/');
  newPath.Append(utf8Name);
  if (rename(mDiskPath.get(), newPath.get()) != 0) {
    promise->MaybeReject(errno == ENOENT ? NS_ERROR_DOM_NOT_FOUND_ERR : NS_ERROR_FAILURE);
    return promise.forget();
  }
  mDiskPath = newPath;
  mName = targetName;
  promise->MaybeResolveWithUndefined();
  return promise.forget();
}

JSObject*
FileSystemFileHandle::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return FileSystemFileHandleBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * FileSystemDirectoryHandle
 ******************************************************************************/

FileSystemDirectoryHandle::FileSystemDirectoryHandle(nsIGlobalObject* aGlobal,
                                                     const nsAString& aName,
                                                     const nsACString& aDiskPath,
                                                     const nsACString& aRootPath)
  : FileSystemHandle(aGlobal, FileSystemHandleKind::Directory, aName, aDiskPath, aRootPath)
{
}

already_AddRefed<Promise>
FileSystemDirectoryHandle::GetFileHandle(
  const nsAString& aName,
  const FileSystemGetFileOptions& aOptions,
  ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  nsCString utf8Name;
  if (!IsValidEntryName(aName, utf8Name)) {
    promise->MaybeReject(NS_ERROR_DOM_TYPE_ERR);
    return promise.forget();
  }

  nsCString childPath = mDiskPath;
  childPath.Append('/');
  childPath.Append(utf8Name);

  struct stat st;
  if (stat(childPath.get(), &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      promise->MaybeReject(NS_ERROR_DOM_TYPE_MISMATCH_ERR);
      return promise.forget();
    }
  } else {
    if (!aOptions.mCreate) {
      promise->MaybeReject(NS_ERROR_DOM_NOT_FOUND_ERR);
      return promise.forget();
    }
    int fd = open(childPath.get(), O_WRONLY | O_CREAT, 0600);
    if (fd < 0) {
      promise->MaybeReject(NS_ERROR_DOM_NO_MODIFICATION_ALLOWED_ERR);
      return promise.forget();
    }
    close(fd);
  }

  RefPtr<FileSystemFileHandle> handle =
    new FileSystemFileHandle(mOwner, aName, childPath, mRootPath);
  promise->MaybeResolve(handle);
  return promise.forget();
}

already_AddRefed<Promise>
FileSystemDirectoryHandle::GetDirectoryHandle(
  const nsAString& aName,
  const FileSystemGetDirectoryOptions& aOptions,
  ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  nsCString utf8Name;
  if (!IsValidEntryName(aName, utf8Name)) {
    promise->MaybeReject(NS_ERROR_DOM_TYPE_ERR);
    return promise.forget();
  }

  nsCString childPath = mDiskPath;
  childPath.Append('/');
  childPath.Append(utf8Name);

  struct stat st;
  if (stat(childPath.get(), &st) == 0) {
    if (!S_ISDIR(st.st_mode)) {
      promise->MaybeReject(NS_ERROR_DOM_TYPE_MISMATCH_ERR);
      return promise.forget();
    }
  } else {
    if (!aOptions.mCreate) {
      promise->MaybeReject(NS_ERROR_DOM_NOT_FOUND_ERR);
      return promise.forget();
    }
    if (mkdir(childPath.get(), 0700) != 0 && errno != EEXIST) {
      promise->MaybeReject(NS_ERROR_DOM_NO_MODIFICATION_ALLOWED_ERR);
      return promise.forget();
    }
  }

  RefPtr<FileSystemDirectoryHandle> handle =
    new FileSystemDirectoryHandle(mOwner, aName, childPath, mRootPath);
  promise->MaybeResolve(handle);
  return promise.forget();
}

already_AddRefed<Promise>
FileSystemDirectoryHandle::RemoveEntry(
  const nsAString& aName,
  const FileSystemRemoveOptions& aOptions,
  ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  nsCString utf8Name;
  if (!IsValidEntryName(aName, utf8Name)) {
    promise->MaybeReject(NS_ERROR_DOM_TYPE_ERR);
    return promise.forget();
  }

  nsCString childPath = mDiskPath;
  childPath.Append('/');
  childPath.Append(utf8Name);

  struct stat st;
  if (lstat(childPath.get(), &st) != 0) {
    promise->MaybeReject(NS_ERROR_DOM_NOT_FOUND_ERR);
    return promise.forget();
  }

  bool ok = false;
  if (S_ISDIR(st.st_mode)) {
    if (aOptions.mRecursive) {
      ok = RemovePathRecursive(childPath);
    } else {
      ok = (rmdir(childPath.get()) == 0);
    }
  } else {
    ok = (unlink(childPath.get()) == 0);
  }

  if (!ok) {
    promise->MaybeReject(NS_ERROR_DOM_INVALID_MODIFICATION_ERR);
    return promise.forget();
  }

  promise->MaybeResolveWithUndefined();
  return promise.forget();
}

already_AddRefed<Promise>
FileSystemDirectoryHandle::Resolve(
  FileSystemHandle& aPossibleDescendant,
  ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  const nsCString& target = aPossibleDescendant.DiskPath();
  if (target.Equals(mDiskPath)) {
    nsTArray<nsString> empty;
    promise->MaybeResolve(empty);
    return promise.forget();
  }

  nsCString prefix = mDiskPath;
  prefix.Append('/');
  if (!StringBeginsWith(target, prefix)) {
    promise->MaybeResolve(JS::NullHandleValue);
    return promise.forget();
  }

  nsCString rel(Substring(target, prefix.Length()));
  nsTArray<nsString> parts;
  int32_t start = 0;
  while (start < static_cast<int32_t>(rel.Length())) {
    int32_t slash = rel.FindChar('/', start);
    nsCString seg((slash == kNotFound)
      ? Substring(rel, start)
      : Substring(rel, start, slash - start));
    if (!seg.IsEmpty()) {
      parts.AppendElement(NS_ConvertUTF8toUTF16(seg));
    }
    if (slash == kNotFound) {
      break;
    }
    start = slash + 1;
  }

  promise->MaybeResolve(parts);
  return promise.forget();
}

void
FileSystemDirectoryHandle::BuildAsyncIterator(JSContext* aCx,
                                              int aMode,
                                              JS::MutableHandle<JS::Value> aRetval,
                                              ErrorResult& aRv)
{
  // aMode: 0 = entries ([name, handle]), 1 = keys (name), 2 = values (handle)
  JS::Rooted<JSObject*> itemsArr(aCx, JS_NewArrayObject(aCx, 0));
  if (!itemsArr) {
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
    return;
  }

  DIR* dir = opendir(mDiskPath.get());
  uint32_t idx = 0;
  if (dir) {
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
        continue;
      }
      // Skip internal temp files created by FileSystemWritableFileStream
      if (strncmp(ent->d_name, ".opfs-tmp-", 10) == 0) {
        continue;
      }
      nsCString childPath = mDiskPath;
      childPath.Append('/');
      childPath.Append(ent->d_name);
      struct stat st;
      if (stat(childPath.get(), &st) != 0) {
        continue;
      }
      nsString u16Name = NS_ConvertUTF8toUTF16(ent->d_name);
      JS::Rooted<JS::Value> itemVal(aCx);

      if (aMode == 1) {
        if (!ToJSValue(aCx, u16Name, &itemVal)) {
          continue;
        }
      } else {
        RefPtr<FileSystemHandle> h;
        if (S_ISDIR(st.st_mode)) {
          h = new FileSystemDirectoryHandle(mOwner, u16Name, childPath, mRootPath);
        } else {
          h = new FileSystemFileHandle(mOwner, u16Name, childPath, mRootPath);
        }
        JS::Rooted<JS::Value> handleVal(aCx);
        if (!ToJSValue(aCx, h, &handleVal)) {
          continue;
        }
        if (aMode == 2) {
          itemVal = handleVal;
        } else {
          JS::Rooted<JSObject*> pair(aCx, JS_NewArrayObject(aCx, 2));
          JS::Rooted<JS::Value> nameVal(aCx);
          if (!pair || !ToJSValue(aCx, u16Name, &nameVal) ||
              !JS_DefineElement(aCx, pair, 0, nameVal, JSPROP_ENUMERATE) ||
              !JS_DefineElement(aCx, pair, 1, handleVal, JSPROP_ENUMERATE)) {
            continue;
          }
          itemVal.setObject(*pair);
        }
      }

      JS_DefineElement(aCx, itemsArr, idx++, itemVal, JSPROP_ENUMERATE);
    }
    closedir(dir);
  }

  // Create a lightweight async+sync iterable wrapper object in JS.
  static const char* kIterFactory =
    "(function(items) {"
    "  var i = 0;"
    "  var it = {"
    "    next: function() {"
    "      if (i < items.length) return Promise.resolve({ value: items[i++], done: false });"
    "      return Promise.resolve({ value: undefined, done: true });"
    "    }"
    "  };"
    "  if (typeof Symbol !== 'undefined' && Symbol.asyncIterator) {"
    "    it[Symbol.asyncIterator] = function() { return it; };"
    "  }"
    "  return it;"
    "})";

  JS::CompileOptions opts(aCx);
  opts.setFileAndLine("opfs-iterator", 1);
  JS::Rooted<JS::Value> fnVal(aCx);
  if (!JS::Evaluate(aCx, opts, kIterFactory, strlen(kIterFactory), &fnVal) ||
      !fnVal.isObject()) {
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }

  JS::AutoValueArray<1> args(aCx);
  args[0].setObject(*itemsArr);
  if (!JS_CallFunctionValue(aCx, nullptr, fnVal, args, aRetval)) {
    aRv.Throw(NS_ERROR_FAILURE);
  }
}

void
FileSystemDirectoryHandle::Entries(JSContext* aCx, JS::MutableHandle<JS::Value> aRetval, ErrorResult& aRv)
{
  BuildAsyncIterator(aCx, 0, aRetval, aRv);
}

void
FileSystemDirectoryHandle::Keys(JSContext* aCx, JS::MutableHandle<JS::Value> aRetval, ErrorResult& aRv)
{
  BuildAsyncIterator(aCx, 1, aRetval, aRv);
}

void
FileSystemDirectoryHandle::Values(JSContext* aCx, JS::MutableHandle<JS::Value> aRetval, ErrorResult& aRv)
{
  BuildAsyncIterator(aCx, 2, aRetval, aRv);
}

JSObject*
FileSystemDirectoryHandle::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return FileSystemDirectoryHandleBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * FileSystemWritableFileStream
 ******************************************************************************/

FileSystemWritableFileStream::FileSystemWritableFileStream(nsIGlobalObject* aGlobal,
                                                           const nsACString& aDiskPath,
                                                           bool aKeepExistingData)
  : mOwner(aGlobal)
  , mDiskPath(aDiskPath)
  , mFd(-1)
  , mPosition(0)
  , mClosed(false)
{
  // Place temp file in same directory so rename() is atomic.
  int32_t slash = mDiskPath.RFindChar('/');
  nsCString dir;
  if (slash != kNotFound) {
    dir.Assign(Substring(mDiskPath, 0, slash + 1));
  }
  mTempPath = dir;
  mTempPath.AppendPrintf(".opfs-tmp-%p-%d", static_cast<void*>(this), static_cast<int>(getpid()));

  mFd = open(mTempPath.get(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (mFd >= 0 && aKeepExistingData) {
    CopyFileContents(mDiskPath.get(), mFd);
    lseek(mFd, 0, SEEK_SET);
  }
}

FileSystemWritableFileStream::~FileSystemWritableFileStream()
{
  if (mFd >= 0) {
    close(mFd);
    mFd = -1;
  }
  if (!mClosed && !mTempPath.IsEmpty()) {
    unlink(mTempPath.get());
  }
}

nsresult
FileSystemWritableFileStream::WriteChunk(JSContext* aCx, JS::Handle<JS::Value> aVal)
{
  if (mFd < 0 || mClosed) {
    return NS_ERROR_DOM_INVALID_STATE_ERR;
  }

  if (aVal.isString()) {
    nsAutoJSString str;
    if (!str.init(aCx, aVal.toString())) {
      return NS_ERROR_FAILURE;
    }
    nsCString utf8 = NS_ConvertUTF16toUTF8(str);
    ssize_t w = pwrite(mFd, utf8.get(), utf8.Length(), static_cast<off_t>(mPosition));
    if (w < 0) {
      return NS_ERROR_FAILURE;
    }
    mPosition += static_cast<uint64_t>(w);
    return NS_OK;
  }

  if (!aVal.isObject()) {
    return NS_ERROR_DOM_TYPE_ERR;
  }

  JS::Rooted<JSObject*> obj(aCx, &aVal.toObject());

  // ArrayBuffer
  if (JS_IsArrayBufferObject(obj)) {
    uint32_t len = JS_GetArrayBufferByteLength(obj);
    JS::AutoCheckCannotGC nogc;
    bool isShared = false;
    uint8_t* data = JS_GetArrayBufferData(obj, &isShared, nogc);
    if (len > 0 && data) {
      ssize_t w = pwrite(mFd, data, len, static_cast<off_t>(mPosition));
      if (w < 0) {
        return NS_ERROR_FAILURE;
      }
      mPosition += static_cast<uint64_t>(w);
    }
    return NS_OK;
  }

  // ArrayBufferView (TypedArray / DataView)
  if (JS_IsArrayBufferViewObject(obj)) {
    uint32_t len = JS_GetArrayBufferViewByteLength(obj);
    JS::AutoCheckCannotGC nogc;
    bool isShared = false;
    void* data = JS_GetArrayBufferViewData(obj, &isShared, nogc);
    if (len > 0 && data) {
      ssize_t w = pwrite(mFd, data, len, static_cast<off_t>(mPosition));
      if (w < 0) {
        return NS_ERROR_FAILURE;
      }
      mPosition += static_cast<uint64_t>(w);
    }
    return NS_OK;
  }

  // Blob / File
  RefPtr<Blob> blob;
  if (NS_SUCCEEDED(UNWRAP_OBJECT(Blob, obj, blob)) && blob) {
    ErrorResult rv;
    nsCOMPtr<nsIInputStream> stream;
    blob->GetInternalStream(getter_AddRefs(stream), rv);
    if (rv.Failed() || !stream) {
      rv.SuppressException();
      return NS_ERROR_FAILURE;
    }
    char buf[16384];
    uint32_t n = 0;
    while (NS_SUCCEEDED(stream->Read(buf, sizeof(buf), &n)) && n > 0) {
      ssize_t w = pwrite(mFd, buf, n, static_cast<off_t>(mPosition));
      if (w < 0) {
        return NS_ERROR_FAILURE;
      }
      mPosition += static_cast<uint64_t>(w);
    }
    return NS_OK;
  }

  // WriteParams dictionary: { type: "write" | "seek" | "truncate", data, position, size }
  JS::Rooted<JS::Value> typeVal(aCx);
  if (JS_GetProperty(aCx, obj, "type", &typeVal) && typeVal.isString()) {
    nsAutoJSString typeStr;
    if (!typeStr.init(aCx, typeVal.toString())) {
      return NS_ERROR_FAILURE;
    }
    if (typeStr.EqualsLiteral("seek")) {
      JS::Rooted<JS::Value> posVal(aCx);
      if (JS_GetProperty(aCx, obj, "position", &posVal) && posVal.isNumber()) {
        mPosition = static_cast<uint64_t>(posVal.toNumber());
        return NS_OK;
      }
      return NS_ERROR_DOM_SYNTAX_ERR;
    }
    if (typeStr.EqualsLiteral("truncate")) {
      JS::Rooted<JS::Value> sizeVal(aCx);
      if (JS_GetProperty(aCx, obj, "size", &sizeVal) && sizeVal.isNumber()) {
        uint64_t sz = static_cast<uint64_t>(sizeVal.toNumber());
        if (ftruncate(mFd, static_cast<off_t>(sz)) != 0) {
          return NS_ERROR_FAILURE;
        }
        if (mPosition > sz) {
          mPosition = sz;
        }
        return NS_OK;
      }
      return NS_ERROR_DOM_SYNTAX_ERR;
    }
    if (typeStr.EqualsLiteral("write")) {
      JS::Rooted<JS::Value> posVal(aCx);
      if (JS_GetProperty(aCx, obj, "position", &posVal) && posVal.isNumber()) {
        mPosition = static_cast<uint64_t>(posVal.toNumber());
      }
      JS::Rooted<JS::Value> dataVal(aCx);
      if (JS_GetProperty(aCx, obj, "data", &dataVal)) {
        return WriteChunk(aCx, dataVal);
      }
      return NS_ERROR_DOM_SYNTAX_ERR;
    }
  }

  return NS_ERROR_DOM_TYPE_ERR;
}

already_AddRefed<Promise>
FileSystemWritableFileStream::Write(JSContext* aCx, JS::Handle<JS::Value> aData, ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  nsresult rv = WriteChunk(aCx, aData);
  if (NS_FAILED(rv)) {
    promise->MaybeReject(rv);
  } else {
    promise->MaybeResolveWithUndefined();
  }
  return promise.forget();
}

already_AddRefed<Promise>
FileSystemWritableFileStream::Seek(uint64_t aPosition, ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  mPosition = aPosition;
  promise->MaybeResolveWithUndefined();
  return promise.forget();
}

already_AddRefed<Promise>
FileSystemWritableFileStream::Truncate(uint64_t aSize, ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  if (mFd < 0 || mClosed || ftruncate(mFd, static_cast<off_t>(aSize)) != 0) {
    promise->MaybeReject(NS_ERROR_FAILURE);
  } else {
    if (mPosition > aSize) {
      mPosition = aSize;
    }
    promise->MaybeResolveWithUndefined();
  }
  return promise.forget();
}

already_AddRefed<Promise>
FileSystemWritableFileStream::Close(ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  if (!mClosed) {
    mClosed = true;
    if (mFd >= 0) {
      fsync(mFd);
      close(mFd);
      mFd = -1;
    }
    if (rename(mTempPath.get(), mDiskPath.get()) != 0) {
      promise->MaybeReject(NS_ERROR_FAILURE);
      return promise.forget();
    }
  }
  promise->MaybeResolveWithUndefined();
  return promise.forget();
}

already_AddRefed<Promise>
FileSystemWritableFileStream::Abort(JSContext* aCx, JS::Handle<JS::Value> aReason, ErrorResult& aRv)
{
  RefPtr<Promise> promise = Promise::Create(mOwner, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  if (!mClosed) {
    mClosed = true;
    if (mFd >= 0) {
      close(mFd);
      mFd = -1;
    }
    unlink(mTempPath.get());
  }
  promise->MaybeResolveWithUndefined();
  return promise.forget();
}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(FileSystemWritableFileStream, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(FileSystemWritableFileStream)
NS_IMPL_CYCLE_COLLECTING_RELEASE(FileSystemWritableFileStream)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(FileSystemWritableFileStream)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

JSObject*
FileSystemWritableFileStream::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return FileSystemWritableFileStreamBinding::Wrap(aCx, this, aGivenProto);
}

/*******************************************************************************
 * FileSystemSyncAccessHandle
 ******************************************************************************/

FileSystemSyncAccessHandle::FileSystemSyncAccessHandle(nsIGlobalObject* aGlobal, int aFd)
  : mOwner(aGlobal)
  , mFd(aFd)
  , mPosition(0)
{
}

FileSystemSyncAccessHandle::~FileSystemSyncAccessHandle()
{
  Close();
}

static bool
ExtractSyncBuffer(JSContext* aCx, JS::Handle<JS::Value> aVal, uint8_t*& aOutPtr, uint32_t& aOutLen)
{
  if (!aVal.isObject()) {
    return false;
  }
  JS::Rooted<JSObject*> obj(aCx, js::CheckedUnwrap(&aVal.toObject()));
  if (!obj) {
    return false;
  }
  JS::AutoCheckCannotGC nogc;
  bool isShared = false;
  if (JS_IsArrayBufferViewObject(obj)) {
    aOutLen = JS_GetArrayBufferViewByteLength(obj);
    aOutPtr = static_cast<uint8_t*>(JS_GetArrayBufferViewData(obj, &isShared, nogc));
    return aOutPtr != nullptr || aOutLen == 0;
  }
  if (JS_IsArrayBufferObject(obj)) {
    aOutLen = JS_GetArrayBufferByteLength(obj);
    aOutPtr = JS_GetArrayBufferData(obj, &isShared, nogc);
    return aOutPtr != nullptr || aOutLen == 0;
  }
  if (JS_IsSharedArrayBufferObject(obj)) {
    aOutLen = JS_GetSharedArrayBufferByteLength(obj);
    aOutPtr = JS_GetSharedArrayBufferData(obj, &isShared, nogc);
    return aOutPtr != nullptr || aOutLen == 0;
  }
  return false;
}

uint64_t
FileSystemSyncAccessHandle::Read(JSContext* aCx,
                                 JS::Handle<JS::Value> aBuffer,
                                 const FileSystemReadWriteOptions& aOptions,
                                 ErrorResult& aRv)
{
  if (mFd < 0) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return 0;
  }
  uint64_t offset = aOptions.mAt.WasPassed() ? aOptions.mAt.Value() : mPosition;
  uint8_t* ptr = nullptr;
  uint32_t len = 0;
  if (!ExtractSyncBuffer(aCx, aBuffer, ptr, len)) {
    aRv.Throw(NS_ERROR_DOM_TYPE_ERR);
    return 0;
  }
  if (!ptr || len == 0) {
    return 0;
  }
  ssize_t n = pread(mFd, ptr, len, static_cast<off_t>(offset));
  if (n < 0) {
    aRv.Throw(NS_ERROR_FAILURE);
    return 0;
  }
  mPosition = offset + static_cast<uint64_t>(n);
  return static_cast<uint64_t>(n);
}

uint64_t
FileSystemSyncAccessHandle::Write(JSContext* aCx,
                                  JS::Handle<JS::Value> aBuffer,
                                  const FileSystemReadWriteOptions& aOptions,
                                  ErrorResult& aRv)
{
  if (mFd < 0) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return 0;
  }
  uint64_t offset = aOptions.mAt.WasPassed() ? aOptions.mAt.Value() : mPosition;
  uint8_t* ptr = nullptr;
  uint32_t len = 0;
  if (!ExtractSyncBuffer(aCx, aBuffer, ptr, len)) {
    aRv.Throw(NS_ERROR_DOM_TYPE_ERR);
    return 0;
  }
  if (!ptr || len == 0) {
    return 0;
  }
  ssize_t n = pwrite(mFd, ptr, len, static_cast<off_t>(offset));
  if (n < 0) {
    aRv.Throw(NS_ERROR_FAILURE);
    return 0;
  }
  mPosition = offset + static_cast<uint64_t>(n);
  return static_cast<uint64_t>(n);
}

void
FileSystemSyncAccessHandle::Truncate(uint64_t aNewSize, ErrorResult& aRv)
{
  if (mFd < 0) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return;
  }
  if (ftruncate(mFd, static_cast<off_t>(aNewSize)) != 0) {
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }
  if (mPosition > aNewSize) {
    mPosition = aNewSize;
  }
}

uint64_t
FileSystemSyncAccessHandle::GetSize(ErrorResult& aRv)
{
  if (mFd < 0) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return 0;
  }
  struct stat st;
  if (fstat(mFd, &st) != 0) {
    aRv.Throw(NS_ERROR_FAILURE);
    return 0;
  }
  return static_cast<uint64_t>(st.st_size);
}

void
FileSystemSyncAccessHandle::Flush(ErrorResult& aRv)
{
  if (mFd < 0) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return;
  }
  if (fsync(mFd) != 0) {
    aRv.Throw(NS_ERROR_FAILURE);
  }
}

void
FileSystemSyncAccessHandle::Close()
{
  if (mFd >= 0) {
    close(mFd);
    mFd = -1;
  }
}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(FileSystemSyncAccessHandle, mOwner)
NS_IMPL_CYCLE_COLLECTING_ADDREF(FileSystemSyncAccessHandle)
NS_IMPL_CYCLE_COLLECTING_RELEASE(FileSystemSyncAccessHandle)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(FileSystemSyncAccessHandle)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

JSObject*
FileSystemSyncAccessHandle::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return FileSystemSyncAccessHandleBinding::Wrap(aCx, this, aGivenProto);
}

} // namespace dom
} // namespace mozilla

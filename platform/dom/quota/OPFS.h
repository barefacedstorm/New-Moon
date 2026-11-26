/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_OPFS_h
#define mozilla_dom_OPFS_h

#include "mozilla/ErrorResult.h"
#include "mozilla/dom/OPFSBinding.h"
#include "mozilla/dom/TypedArray.h"
#include "mozilla/dom/UnionTypes.h"
#include "nsCOMPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsIGlobalObject.h"
#include "nsISupports.h"
#include "nsString.h"
#include "nsWrapperCache.h"

namespace mozilla {
namespace dom {

class Promise;
class File;
class FileSystemWritableFileStream;
class FileSystemSyncAccessHandle;

// Resolve and ensure the origin-isolated OPFS root directory on disk.
nsresult ResolveOPFSOriginRoot(nsIGlobalObject* aGlobal, nsCString& aOutRootPath);

class FileSystemHandle
  : public nsISupports
  , public nsWrapperCache
{
public:
  FileSystemHandle(nsIGlobalObject* aGlobal,
                   FileSystemHandleKind aKind,
                   const nsAString& aName,
                   const nsACString& aDiskPath,
                   const nsACString& aRootPath);

  nsIGlobalObject* GetParentObject() const { return mOwner; }
  FileSystemHandleKind Kind() const { return mKind; }
  void GetName(nsAString& aName) const { aName = mName; }
  const nsCString& DiskPath() const { return mDiskPath; }
  const nsCString& RootPath() const { return mRootPath; }

  already_AddRefed<Promise> IsSameEntry(FileSystemHandle& aOther, ErrorResult& aRv);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(FileSystemHandle)

  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

protected:
  virtual ~FileSystemHandle() = default;

  nsCOMPtr<nsIGlobalObject> mOwner;
  FileSystemHandleKind mKind;
  nsString mName;
  nsCString mDiskPath;
  nsCString mRootPath;
};

class FileSystemFileHandle final : public FileSystemHandle
{
public:
  FileSystemFileHandle(nsIGlobalObject* aGlobal,
                       const nsAString& aName,
                       const nsACString& aDiskPath,
                       const nsACString& aRootPath);

  already_AddRefed<Promise> GetFile(ErrorResult& aRv);
  already_AddRefed<Promise> CreateWritable(
    const FileSystemCreateWritableOptions& aOptions,
    ErrorResult& aRv);
  already_AddRefed<Promise> CreateSyncAccessHandle(ErrorResult& aRv);
  already_AddRefed<Promise> Move(const nsAString& aNewName, ErrorResult& aRv);
  already_AddRefed<Promise> Move(FileSystemDirectoryHandle& aDestination,
                                 const Optional<nsAString>& aNewName,
                                 ErrorResult& aRv);

  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~FileSystemFileHandle() = default;
};

class FileSystemDirectoryHandle final : public FileSystemHandle
{
public:
  FileSystemDirectoryHandle(nsIGlobalObject* aGlobal,
                            const nsAString& aName,
                            const nsACString& aDiskPath,
                            const nsACString& aRootPath);

  already_AddRefed<Promise> GetFileHandle(
    const nsAString& aName,
    const FileSystemGetFileOptions& aOptions,
    ErrorResult& aRv);

  already_AddRefed<Promise> GetDirectoryHandle(
    const nsAString& aName,
    const FileSystemGetDirectoryOptions& aOptions,
    ErrorResult& aRv);

  already_AddRefed<Promise> RemoveEntry(
    const nsAString& aName,
    const FileSystemRemoveOptions& aOptions,
    ErrorResult& aRv);

  already_AddRefed<Promise> Resolve(
    FileSystemHandle& aPossibleDescendant,
    ErrorResult& aRv);

  void Entries(JSContext* aCx, JS::MutableHandle<JS::Value> aRetval, ErrorResult& aRv);
  void Keys(JSContext* aCx, JS::MutableHandle<JS::Value> aRetval, ErrorResult& aRv);
  void Values(JSContext* aCx, JS::MutableHandle<JS::Value> aRetval, ErrorResult& aRv);

  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~FileSystemDirectoryHandle() = default;
  void BuildAsyncIterator(JSContext* aCx, int aMode, JS::MutableHandle<JS::Value> aRetval, ErrorResult& aRv);
};

class FileSystemWritableFileStream final
  : public nsISupports
  , public nsWrapperCache
{
public:
  FileSystemWritableFileStream(nsIGlobalObject* aGlobal,
                               const nsACString& aDiskPath,
                               bool aKeepExistingData);

  nsIGlobalObject* GetParentObject() const { return mOwner; }

  already_AddRefed<Promise> Write(JSContext* aCx, JS::Handle<JS::Value> aData, ErrorResult& aRv);
  already_AddRefed<Promise> Seek(uint64_t aPosition, ErrorResult& aRv);
  already_AddRefed<Promise> Truncate(uint64_t aSize, ErrorResult& aRv);
  already_AddRefed<Promise> Close(ErrorResult& aRv);
  already_AddRefed<Promise> Abort(JSContext* aCx, JS::Handle<JS::Value> aReason, ErrorResult& aRv);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(FileSystemWritableFileStream)

  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~FileSystemWritableFileStream();
  nsresult WriteChunk(JSContext* aCx, JS::Handle<JS::Value> aVal);

  nsCOMPtr<nsIGlobalObject> mOwner;
  nsCString mDiskPath;
  nsCString mTempPath;
  int mFd;
  uint64_t mPosition;
  bool mClosed;
};

class FileSystemSyncAccessHandle final
  : public nsISupports
  , public nsWrapperCache
{
public:
  FileSystemSyncAccessHandle(nsIGlobalObject* aGlobal, int aFd);

  nsIGlobalObject* GetParentObject() const { return mOwner; }

  uint64_t Read(JSContext* aCx,
                JS::Handle<JS::Value> aBuffer,
                const FileSystemReadWriteOptions& aOptions,
                ErrorResult& aRv);
  uint64_t Write(JSContext* aCx,
                 JS::Handle<JS::Value> aBuffer,
                 const FileSystemReadWriteOptions& aOptions,
                 ErrorResult& aRv);
  void Truncate(uint64_t aNewSize, ErrorResult& aRv);
  uint64_t GetSize(ErrorResult& aRv);
  void Flush(ErrorResult& aRv);
  void Close();

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(FileSystemSyncAccessHandle)

  virtual JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

private:
  ~FileSystemSyncAccessHandle();

  nsCOMPtr<nsIGlobalObject> mOwner;
  int mFd;
  uint64_t mPosition;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_OPFS_h

/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

enum FileSystemHandleKind {
  "file",
  "directory"
};

dictionary FileSystemGetFileOptions {
  boolean create = false;
};

dictionary FileSystemGetDirectoryOptions {
  boolean create = false;
};

dictionary FileSystemRemoveOptions {
  boolean recursive = false;
};

dictionary FileSystemCreateWritableOptions {
  boolean keepExistingData = false;
};

dictionary FileSystemReadWriteOptions {
  [EnforceRange] unsigned long long at;
};

[Exposed=(Window,Worker), SecureContext]
interface FileSystemHandle {
  readonly attribute FileSystemHandleKind kind;
  readonly attribute USVString name;

  [Throws]
  Promise<boolean> isSameEntry(FileSystemHandle other);
};

[Exposed=(Window,Worker), SecureContext]
interface FileSystemFileHandle : FileSystemHandle {
  [Throws]
  Promise<File> getFile();

  [Throws]
  Promise<FileSystemWritableFileStream> createWritable(
    optional FileSystemCreateWritableOptions options);

  [Exposed=DedicatedWorker, Throws]
  Promise<FileSystemSyncAccessHandle> createSyncAccessHandle();

  [Throws]
  Promise<void> move(USVString newName);

  [Throws]
  Promise<void> move(FileSystemDirectoryHandle destination, optional USVString newName);
};

[Exposed=(Window,Worker), SecureContext]
interface FileSystemDirectoryHandle : FileSystemHandle {
  [Throws]
  Promise<FileSystemFileHandle> getFileHandle(
    USVString name,
    optional FileSystemGetFileOptions options);

  [Throws]
  Promise<FileSystemDirectoryHandle> getDirectoryHandle(
    USVString name,
    optional FileSystemGetDirectoryOptions options);

  [Throws]
  Promise<void> removeEntry(
    USVString name,
    optional FileSystemRemoveOptions options);

  [Throws]
  Promise<sequence<USVString>?> resolve(FileSystemHandle possibleDescendant);

  [Throws]
  any entries();

  [Throws]
  any keys();

  [Throws]
  any values();
};

[Exposed=(Window,Worker), SecureContext]
interface FileSystemWritableFileStream {
  [Throws]
  Promise<void> write(any data);

  [Throws]
  Promise<void> seek(unsigned long long position);

  [Throws]
  Promise<void> truncate(unsigned long long size);

  [Throws]
  Promise<void> close();

  [Throws]
  Promise<void> abort(optional any reason);
};

[Exposed=DedicatedWorker, SecureContext]
interface FileSystemSyncAccessHandle {
  [Throws]
  unsigned long long read(any buffer,
                          optional FileSystemReadWriteOptions options);

  [Throws]
  unsigned long long write(any buffer,
                           optional FileSystemReadWriteOptions options);

  [Throws]
  void truncate([EnforceRange] unsigned long long newSize);

  [Throws]
  unsigned long long getSize();

  [Throws]
  void flush();

  void close();
};

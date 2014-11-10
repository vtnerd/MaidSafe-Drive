Nfs Drive API
=========

Design document 0.1 - discussion stage

Introduction
============

Drive is represented as a cross patform cirtual filesystem at the moment. This requires the drive library interface with FUSE, OSXFUSE and CBFS for Linux, OSX and Windows repspectively. It also provides the back end network storage and retrieval (handled vis Nfs and Encrypt). This design requires this duality to be split into seperate libraries. 

Motivation
==========

The motivation for this proposal is twofold, to clear up the responsibility of libraries into a more defined structure and provide users of the Nfs library to create appplications without the need to install any kernel level drivers. This implementaion will also relax the requirements of the system that a drive object requires a complete routing node attached. This should reduce the required machine resources. 

Perhaps the greatest motivation though is to allow direct access to data and content from an application. As private shared drives are required the locking mechanism will be one of 'lock free'. Instead the system will provide eventual consistency, this is best suited for such netwworks and will require applicaitons are built with this in mind. Providing a virtual filesystem does not lend itself to this limitation/opportunity very well. 


Overview
=========

PLEASE DISCUSS - Best outcome is this interface can be used as directly as possible with fuse/osxfuse/cbfs as well as implementing a fielsystem that moves towards posix complience (forgiving the eventual consistency of course)


The API to be made available to Nfs (public interface) is below.
```C++

class NfsFile {
 public:
  typedef std::basic_future<std::expected<void, NfsError> Event;
  typedef std::basic_future<std::expected<std::string, NfsError> DataEvent;
  
  DataEvent Read(std::string buffer, off_t offset);
  DataEvent Write(std::string contents, off_t offset);
  Event Truncate(off_t size);
};

template <typename IdType>
class NfsDrive {
 public:
  NfsDrive(IdType id_type); // retrieve root dir from network) ?? template or pass the root dir ??
  
  typedef std::basic_future<std::expected<std:shared_ptr<NfsFile>, NfsError> Event;
  
  NfsFile::DataEvent Put(std::string path, std::string contents);
  NfsFile::DataEvent Get(std::string path, std::string buffer);
  NfsFile::Event Delete(std::string path);
  
  Event Create(std::string path);
  Event Open(std::string path);
  
  // Need an abstraction for file metadata, but not opened file ...
  std::basic_future<std::vector<std::shared_ptr<NfsFile>>, NfsError> ListDirectory(std::string path);
};
```


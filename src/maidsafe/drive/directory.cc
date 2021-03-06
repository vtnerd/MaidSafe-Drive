/*  Copyright 2011 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.maidsafe.net/licenses

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

#include "maidsafe/drive/directory.h"

#include <algorithm>
#include <iterator>

#include "boost/asio/placeholders.hpp"

#include "maidsafe/common/profiler.h"

#include "maidsafe/drive/meta_data.h"
#include "maidsafe/drive/utils.h"
#include "maidsafe/drive/proto_structs.pb.h"

namespace fs = boost::filesystem;

namespace maidsafe {

namespace drive {

namespace detail {

Directory::Directory(ParentId parent_id, DirectoryId directory_id,
                     boost::asio::io_service& io_service,
                     std::weak_ptr<Directory::Listener> listener,
                     const boost::filesystem::path& path)
    : Path(fs::directory_file),
      parent_id_(std::move(parent_id)),
      directory_id_(std::move(directory_id)),
      timer_(io_service),
      path_(path),
      versions_(),
      max_versions_(kMaxVersions),
      children_(),
      children_count_position_(0),
      store_state_(StoreState::kComplete),
      listener_(listener),
      newParent_(),
      pending_count_(0),
      mutex_() {}

Directory::Directory(ParentId parent_id, const std::string&,
                     const std::vector<StructuredDataVersions::VersionName>& versions,
                     boost::asio::io_service& io_service,
                     std::weak_ptr<Directory::Listener> listener,
                     const boost::filesystem::path& path)
    : Path(fs::directory_file),
      parent_id_(std::move(parent_id)),
      directory_id_(),
      timer_(io_service),
      path_(path),
      versions_(std::begin(versions), std::end(versions)),
      max_versions_(kMaxVersions),
      children_(),
      children_count_position_(0),
      store_state_(StoreState::kComplete),
      listener_(listener),
      newParent_(),
      pending_count_(0),
      mutex_() {}

Directory::~Directory() {
  try {
    StoreImmediatelyIfPending();
  } catch (...) {
  }
}

void Directory::Initialise(const ParentId&, const DirectoryId&, boost::asio::io_service&,
                           std::weak_ptr<Directory::Listener>, const boost::filesystem::path&) {
  const std::lock_guard<std::mutex> lock(mutex_);
  DoScheduleForStoring();
}

void Directory::Initialise(const ParentId&, const std::string& serialised_directory,
                           const std::vector<StructuredDataVersions::VersionName>&,
                           boost::asio::io_service& io_service, std::weak_ptr<Directory::Listener>,
                           const boost::filesystem::path&) {
  const std::lock_guard<std::mutex> lock(mutex_);
  protobuf::Directory proto_directory;
  if (!proto_directory.ParseFromString(serialised_directory)) {
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::parsing_error));
  }

  directory_id_ = Identity(proto_directory.directory_id());
  max_versions_ = MaxVersions(proto_directory.max_versions());

  children_.reserve(proto_directory.children_size());
  for (int i(0); i != proto_directory.children_size(); ++i) {
    children_.emplace_back(
        File::Create(io_service, MetaData(proto_directory.children(i)), shared_from_this()));
  }
  SortAndResetChildrenCounter();
}

std::string Directory::Serialise() {
  protobuf::Directory proto_directory;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    proto_directory.set_directory_id(directory_id_.string());
    proto_directory.set_max_versions(max_versions_.data);
  }

  std::vector<ImmutableData::Name> chunks_to_be_incremented;
  Serialise(proto_directory, chunks_to_be_incremented);
  return proto_directory.SerializeAsString();
}

void Directory::Serialise(protobuf::Directory& proto_directory,
                          std::vector<ImmutableData::Name>& chunks) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& child : children_) {
      child->Serialise(proto_directory, chunks);
    }
  }

  const std::shared_ptr<Listener> listener(GetListener());
  if (listener) {
    listener->IncrementChunks(chunks);
  }
  chunks.clear();
  store_state_ = StoreState::kOngoing;
}

size_t Directory::VersionsCount() const { return versions_.size(); }

std::tuple<DirectoryId, StructuredDataVersions::VersionName> Directory::InitialiseVersions(
    ImmutableData::Name version_id) {
  std::tuple<DirectoryId, StructuredDataVersions::VersionName> result;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    store_state_ = StoreState::kComplete;
    if (versions_.empty()) {
      versions_.emplace_back(0, version_id);
      result = std::make_tuple(directory_id_, versions_[0]);
    } else {
      BOOST_THROW_EXCEPTION(MakeError(CommonErrors::uninitialised));
    }
  }
  return result;
}

std::tuple<DirectoryId, StructuredDataVersions::VersionName, StructuredDataVersions::VersionName>
    Directory::AddNewVersion(ImmutableData::Name version_id) {
  std::tuple<DirectoryId, StructuredDataVersions::VersionName, StructuredDataVersions::VersionName>
      result;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    store_state_ = StoreState::kComplete;
    if (versions_.empty()) {
      versions_.emplace_back(0, version_id);
      result = std::make_tuple(directory_id_, StructuredDataVersions::VersionName(), versions_[0]);
    } else {
      versions_.emplace_front(versions_.front().index + 1, version_id);
      auto itr(std::begin(versions_));
      result = std::make_tuple(directory_id_, *(itr + 1), *itr);
      if (versions_.size() > max_versions_)
        versions_.pop_back();
    }
  }
  return result;
}

Directory::Children::iterator Directory::Find(const fs::path& name) {
  return std::find_if(
      std::begin(children_), std::end(children_),
      [&name](const Children::value_type& file) { return file->meta_data.name() == name; });
}

Directory::Children::const_iterator Directory::Find(const fs::path& name) const {
  return std::find_if(
      std::begin(children_), std::end(children_),
      [&name](const Children::value_type& file) { return file->meta_data.name() == name; });
}

void Directory::SortAndResetChildrenCounter() {
  std::sort(std::begin(children_), std::end(children_),
            [](const std::shared_ptr<Path>& lhs, const std::shared_ptr<Path>& rhs) {
    return *lhs < *rhs;
  });
  children_count_position_ = 0;
}

void Directory::DoScheduleForStoring() {
  auto cancelled_count(timer_.expires_from_now(kDirectoryInactivityDelay));
#ifndef NDEBUG
  if (cancelled_count > 0 && store_state_ != StoreState::kComplete) {
    LOG(kInfo) << "Successfully cancelled " << cancelled_count << " store functor.";
    assert(cancelled_count == 1);
  } else if (store_state_ != StoreState::kComplete) {
    LOG(kWarning) << "Failed to cancel store functor.";
  }
#endif
  static_cast<void>(cancelled_count);
  timer_.async_wait(std::bind(&Directory::ProcessTimer, shared_from_this(), std::placeholders::_1));
  ++pending_count_;
  store_state_ = StoreState::kPending;
}

void Directory::ProcessTimer(const boost::system::error_code& ec) {
  std::shared_ptr<Listener> listener;
  {
    const std::unique_lock<std::mutex> lock(mutex_);
    switch (ec.value()) {
      case 0: {
        LOG(kInfo) << "Storing " << path_;
        listener = GetListener();
        break;
      }
      case boost::asio::error::operation_aborted:
        LOG(kInfo) << "Timer was cancelled - not storing " << path_;
        break;
      default:
        LOG(kWarning) << "Timer aborted with error code " << ec;
        break;
    }
  }

  if (listener) {
    listener->Put(shared_from_this());
  }

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    // Update pending parent change
    if (newParent_) {
      parent_id_ = newParent_->parent_id_;
      path_ = newParent_->path_;
      newParent_ = nullptr;
    }
    --pending_count_;
  }
}

std::shared_ptr<Directory::Listener> Directory::GetListener() const { return listener_.lock(); }

bool Directory::HasChild(const fs::path& name) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return std::any_of(
      std::begin(children_), std::end(children_),
      [&name](const Children::value_type& file) { return file->meta_data.name() == name; });
}

std::shared_ptr<const Path> Directory::GetChildAndIncrementCounter() {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (children_count_position_ < children_.size()) {
    auto file(children_[children_count_position_]);
    ++children_count_position_;
    return file;
  }
  return std::shared_ptr<const File>();
}

void Directory::AddChild(std::shared_ptr<Path> child) {
  const std::lock_guard<std::mutex> lock(mutex_);
  auto itr(Find(child->meta_data.name()));
  if (itr != std::end(children_))
    BOOST_THROW_EXCEPTION(MakeError(DriveErrors::file_exists));
  child->SetParent(shared_from_this());
  children_.emplace_back(child);
  SortAndResetChildrenCounter();
  DoScheduleForStoring();
}

std::shared_ptr<Path> Directory::RemoveChild(const fs::path& name) {
  const std::lock_guard<std::mutex> lock(mutex_);
  auto itr(Find(name));
  if (itr == std::end(children_))
    BOOST_THROW_EXCEPTION(MakeError(DriveErrors::no_such_file));
  auto file(*itr);
  children_.erase(itr);
  SortAndResetChildrenCounter();
  DoScheduleForStoring();
  return file;
}

void Directory::RenameChild(const fs::path& old_name, const fs::path& new_name) {
  const std::lock_guard<std::mutex> lock(mutex_);
  assert(Find(new_name) == std::end(children_));
  auto itr(Find(old_name));
  if (itr == std::end(children_))
    BOOST_THROW_EXCEPTION(MakeError(DriveErrors::no_such_file));
  (*itr)->meta_data.set_name(new_name);
  SortAndResetChildrenCounter();
  DoScheduleForStoring();
}

void Directory::ResetChildrenCounter() {
  const std::lock_guard<std::mutex> lock(mutex_);
  children_count_position_ = 0;
}

bool Directory::empty() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return children_.empty();
}

ParentId Directory::parent_id() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return parent_id_;
}

void Directory::SetNewParent(const ParentId parent_id, const boost::filesystem::path& path) {
  const std::lock_guard<std::mutex> lock(mutex_);
  newParent_.reset(new NewParent(parent_id, path));
}

DirectoryId Directory::directory_id() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return directory_id_;
}

void Directory::ScheduleForStoring() {
  const std::lock_guard<std::mutex> lock(mutex_);
  DoScheduleForStoring();
}

void Directory::StoreImmediatelyIfPending() {
  /* The implication is that we should only store if there's already a pending
     store waiting - i.e. we're just bringing forward the deadline of any
     outstanding store. */
  {
    const std::lock_guard<std::mutex> lock(mutex_);

    if (store_state_ != StoreState::kPending) {
      LOG(kInfo) << "No store functor pending.";
      return;
    }

    const auto cancelled_count(timer_.cancel());
    if (cancelled_count <= 0) {
      LOG(kWarning) << "Failed to cancel store functor.";
      return;
    }

    LOG(kInfo) << "Successfully brought forward schedule for " << cancelled_count
               << " store functor.";
    assert(cancelled_count == 1);
    ++pending_count_;
  }

  ProcessTimer(boost::system::error_code());
}

bool Directory::HasPending() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return (pending_count_ != 0);
}

bool operator<(const Directory& lhs, const Directory& rhs) {
  return lhs.directory_id() < rhs.directory_id();
}

}  // namespace detail

}  // namespace drive

}  // namespace maidsafe

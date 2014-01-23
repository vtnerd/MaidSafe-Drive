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

#ifdef USES_WINMAIN
#include <Windows.h>
#include <shellapi.h>
#endif
#include <signal.h>

#include <functional>
#include <iostream>  // NOLINT
#include <memory>
#include <string>
#include <fstream>  // NOLINT
#include <iterator>

#include "boost/filesystem.hpp"
#include "boost/program_options.hpp"
#include "boost/preprocessor/stringize.hpp"
#include "boost/system/error_code.hpp"

#include "maidsafe/common/crypto.h"
#include "maidsafe/common/log.h"
#include "maidsafe/common/rsa.h"
#include "maidsafe/common/utils.h"

#include "maidsafe/data_store/local_store.h"

#ifdef MAIDSAFE_WIN32
#include "maidsafe/drive/win_drive.h"
#else
#include "maidsafe/drive/unix_drive.h"
#endif


namespace fs = boost::filesystem;
namespace po = boost::program_options;

namespace maidsafe {
namespace drive {

namespace {

// std::function<void()> g_unmount_functor;

// void CtrlCHandler(int /*value*/) {
//   g_unmount_functor();
// }

}  // unnamed namespace

template<typename Storage>
struct GetDrive {
#ifdef MAIDSAFE_WIN32
  typedef CbfsDrive<Storage> type;
#else
  typedef FuseDrive<Storage> type;
#endif
};

int Mount(const fs::path &/*mount_dir*/, const fs::path &chunk_dir) {
  fs::path storage_path(chunk_dir / "store");
  DiskUsage disk_usage(1048576000);
  MemoryUsage memory_usage(0);
  std::shared_ptr<maidsafe::data_store::LocalStore>
      storage(new maidsafe::data_store::LocalStore(storage_path, disk_usage));

  boost::system::error_code error_code;
  if (!fs::exists(chunk_dir, error_code))
    return error_code.value();

  std::string root_parent_id_str;
  fs::path id_path(storage_path / "root_parent_id");
  bool first_run(!fs::exists(id_path, error_code));
  if (!first_run)
    BOOST_VERIFY(ReadFile(id_path, &root_parent_id_str));

  // The following values are passed in and returned on unmount.
  Identity unique_user_id(std::string(64, 'a'));
  Identity root_parent_id(root_parent_id_str.empty() ? Identity() : Identity(root_parent_id_str));
  std::string product_id;

  // typedef GetDrive<maidsafe::data_store::LocalStore>::type Drive;
  // Drive drive(storage,
  //             unique_user_id,
  //             root_parent_id,
  //             mount_dir,
  //             product_id,
  //             "MaidSafeDrive");
  // if (first_run)
  //   BOOST_VERIFY(WriteFile(id_path, drive.root_parent_id().string()));

  // g_unmount_functor = [&] { drive.Unmount(); };  // NOLINT
  // signal(SIGINT, CtrlCHandler);
  // drive.Mount();

  return 0;
}

}  // namespace drive
}  // namespace maidsafe


fs::path GetPathFromProgramOption(const std::string &option_name,
                                  po::variables_map *variables_map,
                                  bool must_exist) {
  if (variables_map->count(option_name)) {
    boost::system::error_code error_code;
    fs::path option_path(variables_map->at(option_name).as<std::string>());
    if (must_exist) {
      if (!fs::exists(option_path, error_code) || error_code) {
        LOG(kError) << "Invalid " << option_name << " option.  " << option_path
                    << " doesn't exist or can't be accessed (error message: "
                    << error_code.message() << ")";
        return fs::path();
      }
      if (!fs::is_directory(option_path, error_code) || error_code) {
        LOG(kError) << "Invalid " << option_name << " option.  " << option_path
                    << " is not a directory (error message: "
                    << error_code.message() << ")";
        return fs::path();
      }
    } else {
      if (fs::exists(option_path, error_code)) {
        LOG(kError) << "Invalid " << option_name << " option.  " << option_path
                    << " already exists (error message: "
                    << error_code.message() << ")";
        return fs::path();
      }
    }
    LOG(kInfo) << option_name << " set to " << option_path;
    return option_path;
  } else {
    LOG(kWarning) << "You must set the " << option_name << " option to a"
                 << (must_exist ? "n " : " non-") << "existing directory.";
    return fs::path();
  }
}


#ifdef USES_WINMAIN
int CALLBACK wWinMain(HINSTANCE /*handle_to_instance*/, HINSTANCE /*handle_to_previous_instance*/,
                      PWSTR /*command_line_args_without_program_name*/, int /*command_show*/) {
  int argc(0);
  LPWSTR* argv(nullptr);
  argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  typedef po::wcommand_line_parser CommandLineParser;
#else
int main(int argc, char* argv[]) {
  typedef po::command_line_parser CommandLineParser;
  maidsafe::log::Logging::Instance().Initialise(argc, argv);
#endif
  boost::system::error_code error_code;
#ifdef MAIDSAFE_WIN32
  fs::path logging_dir("C:\\ProgramData\\MaidSafeDrive\\logs");
#else
  fs::path logging_dir(fs::temp_directory_path(error_code) / "maidsafe_drive/logs");
  if (error_code) {
    LOG(kError) << error_code.message();
    return 1;
  }
#endif
  if (!fs::exists(logging_dir, error_code))
    fs::create_directories(logging_dir, error_code);
  if (error_code)
    LOG(kError) << error_code.message();
  if (!fs::exists(logging_dir, error_code))
    LOG(kError) << "Couldn't create logging directory at " << logging_dir;
  fs::path log_path(logging_dir / "maidsafe_drive");
  // All command line parameters are only for this run. To allow persistance, update the config
  // file. Command line overrides any config file settings.
  try {
    po::options_description options_description("Allowed options");
    options_description.add_options()
        ("help,h", "print this help message")
        ("chunkdir,C", po::value<std::string>(), "set directory to store chunks")
        ("mountdir,D", po::value<std::string>(), "set virtual drive name")
        ("checkdata", "check all data (metadata and chunks)");

    po::variables_map variables_map;
    po::store(CommandLineParser(argc, argv).options(options_description).allow_unregistered().
                                            run(), variables_map);
    po::notify(variables_map);

    // set up options for config file
    po::options_description config_file_options;
    config_file_options.add(options_description);

    // try open some config options
    std::ifstream local_config_file("maidsafe_drive.conf");
#ifdef MAIDSAFE_WIN32
    fs::path main_config_path("C:/ProgramData/MaidSafeDrive/maidsafe_drive.conf");
#else
    fs::path main_config_path("/etc/maidsafe_drive.conf");
#endif
    std::ifstream main_config_file(main_config_path.string().c_str());

    // try local first for testing
    if (local_config_file) {
      LOG(kInfo) << "Using local config file \"maidsafe_drive.conf\"";
      store(parse_config_file(local_config_file, config_file_options), variables_map);
      notify(variables_map);
    } else if (main_config_file) {
      LOG(kInfo) << "Using main config file " << main_config_path;
      store(parse_config_file(main_config_file, config_file_options), variables_map);
      notify(variables_map);
    } else {
      LOG(kWarning) << "No configuration file found at " << main_config_path;
    }

    if (variables_map.count("help")) {
      std::cout << options_description << '\n';
      return 1;
    }

    fs::path chunkstore_path(GetPathFromProgramOption("chunkdir", &variables_map, true));
#ifdef MAIDSAFE_WIN32
    fs::path mount_path(GetPathFromProgramOption("mountdir", &variables_map, false));
#else
    fs::path mount_path(GetPathFromProgramOption("mountdir", &variables_map, true));
#endif

    if (variables_map.count("stop")) {
      LOG(kInfo) << "Trying to stop.";
      return 0;
    }

    if (chunkstore_path == fs::path() || mount_path == fs::path()) {
      LOG(kWarning) << options_description;
      return 1;
    }

    int result(maidsafe::drive::Mount(mount_path, chunkstore_path));
    return result;
  }
  catch(const std::exception& e) {
    LOG(kError) << "Exception: " << e.what();
    return 1;
  }
  catch(...) {
    LOG(kError) << "Exception of unknown type!";
  }
  return 0;
}

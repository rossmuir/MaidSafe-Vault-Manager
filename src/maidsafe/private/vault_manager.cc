/***************************************************************************************************
 *  Copyright 2012 maidsafe.net limited                                                            *
 *                                                                                                 *
 *  The following source code is property of MaidSafe.net limited and is not meant for external    *
 *  use. The use of this code is governed by the licence file licence.txt found in the root of     *
 *  this directory and also on www.maidsafe.net.                                                   *
 *                                                                                                 *
 *  You are not free to copy, amend or otherwise use this source code without the explicit written *
 *  permission of the board of directors of MaidSafe.net.                                          *
 **************************************************************************************************/

#include "maidsafe/private/vault_manager.h"

#include <chrono>
#include <iostream>

#include "boost/filesystem/path.hpp"
#include "boost/lexical_cast.hpp"

#include "maidsafe/common/config.h"
#include "maidsafe/common/log.h"
#include "maidsafe/common/utils.h"

#include "maidsafe/private/controller_messages_pb.h"
#include "maidsafe/private/local_tcp_transport.h"
#include "maidsafe/private/utils.h"
#include "maidsafe/private/return_codes.h"
#include "maidsafe/private/vault_info_pb.h"


namespace bptime = boost::posix_time;
namespace fs = boost::filesystem;

namespace maidsafe {

namespace priv {

namespace {

bool HandleBootstrapFile(const std::string& short_vault_id, const fs::path& parent_dir) {
  fs::path vault_bootstrap_path(parent_dir / ("bootstrap-" + short_vault_id + ".dat"));

  boost::system::error_code error_code;
  // false returned from create_directories implies the directory pre-existed
  fs::create_directories(parent_dir, error_code);
  if (error_code) {
    LOG(kError) << "Error creating vault directory " << parent_dir << "   " << error_code.message();
    return false;
  }

  // Copy global bootstrap file
  if (!fs::exists(vault_bootstrap_path, error_code)) {
    if (error_code.value() != boost::system::errc::no_such_file_or_directory)
      LOG(kError) << error_code.value() << error_code.message();
    fs::copy_file(parent_dir / "bootstrap-global.dat", vault_bootstrap_path, error_code);
    if (error_code) {
      LOG(kError) << "Failed to copy global bootstrap file: " << error_code.message();
      return false;
    }
  }

  // TODO(Phil) set permissions to give vault exclusive access

  return true;
}

}  // unnamed namespace


VaultManager::VaultInfo::VaultInfo()
    : process_index(),
      account_name(),
      keys(),
      chunkstore_path(),
      chunkstore_capacity(0),
      client_port(0),
      vault_port(0),
      mutex(),
      cond_var(),
      requested_to_run(false),
      vault_requested(false) {}

void VaultManager::VaultInfo::ToProtobuf(protobuf::VaultInfo* pb_vault_info) const {
  pb_vault_info->set_account_name(account_name);
  std::string serialized_keys;
  asymm::SerialiseKeys(keys, serialized_keys);
  pb_vault_info->set_keys(serialized_keys);
  pb_vault_info->set_chunkstore_path(chunkstore_path);
  pb_vault_info->set_chunkstore_capacity(chunkstore_capacity);
  pb_vault_info->set_requested_to_run(requested_to_run);
}

void VaultManager::VaultInfo::FromProtobuf(const protobuf::VaultInfo& pb_vault_info) {
  account_name = pb_vault_info.account_name();
  asymm::ParseKeys(pb_vault_info.keys(), keys);
  chunkstore_path = pb_vault_info.chunkstore_path();
  chunkstore_capacity = pb_vault_info.chunkstore_capacity();
  requested_to_run = pb_vault_info.requested_to_run();
}


VaultManager::VaultManager()
    : process_manager_(),
      download_manager_("http", "dash.maidsafe.net", "~phil"),  // TODO(Fraser#5#): 2012-08-12 - Provide proper path to server as constants
      asio_service_(3),
      update_interval_(bptime::hours(24)),
      update_timer_(asio_service_.service()),
      update_mutex_(),
      transport_(new LocalTcpTransport(asio_service_.service())),
      local_port_(kMinPort()),
      vault_infos_(),
      vault_infos_mutex_(),
      cond_var_(),
      stop_listening_for_updates_(false),
      shutdown_requested_(false),
      config_file_path_() {
  if (!EstablishConfigFilePath() && !WriteConfigFile()) {
    LOG(kError) << "VaultManager failed to start - failed to find existing config file in "
                << fs::current_path() << " or in " << GetSystemAppDir()
                << " and failed to write new one at " << config_file_path_;
    return;
  }

  asio_service_.Start();
  transport_->on_message_received().connect(
      [this](const std::string& message, Port peer_port) {
        HandleReceivedMessage(message, peer_port);
      });
  transport_->on_error().connect([](const int& error) {
    LOG(kError) << "Transport reported error code " << error;
  });

  if (!ReadConfigFile()) {
    LOG(kError) << "VaultManager failed to start - failed to read existing config file at "
                << config_file_path_;
    return;
  }

  // Invoke update immediately.  Thereafter, invoked every update_interval_.
  boost::system::error_code ec;
  CheckForUpdates(ec);
  ListenForMessages();
  LOG(kInfo) << "VaultManager started successfully.  Using config file at "
             << (InTestMode() ? fs::current_path() / kConfigFileName() : config_file_path_);
}

VaultManager::~VaultManager() {
  process_manager_.LetAllProcessesDie();
                                                                                                        {
                                                                                                          std::lock_guard<std::mutex> lock(vault_infos_mutex_);
                                                                                                          stop_listening_for_updates_ = true;
                                                                                                          shutdown_requested_ = true;
                                                                                                          cond_var_.notify_all();
                                                                                                        }
  {
    std::lock_guard<std::mutex> lock(update_mutex_);
    update_timer_.cancel();
  }
  transport_->StopListening();
  asio_service_.Stop();
}

boost::posix_time::time_duration VaultManager::kMinUpdateInterval() {
  return bptime::minutes(5);
}

boost::posix_time::time_duration VaultManager::kMaxUpdateInterval() {
  return bptime::hours(24 * 7);
}

void VaultManager::RestartVaultManager(const std::string& latest_file,
                                       const std::string& executable_name) const {
  // TODO(Fraser#5#): 2012-08-12 - Define command in constant.  Do we need 2 shell scripts?  Do we need 2 parameters to unix script?
#ifdef MAIDSAFE_WIN32
  std::string command("restart_vm_windows.bat " + latest_file + " " + executable_name);
#else
  std::string command("./restart_vm_linux.sh " + latest_file + " " + executable_name);
#endif
  // system("/etc/init.d/mvm restart");
  int result(system(command.c_str()));
  if (result != 0)
    LOG(kWarning) << "Result: " << result;
}

bool VaultManager::EstablishConfigFilePath() {
  assert(config_file_path_.empty());
  // Favour config file in ./
  fs::path local_config_file_path(fs::path(".") / kConfigFileName());
  boost::system::error_code error_code;
  if (!fs::exists(local_config_file_path, error_code) || error_code) {
    // Try for one in system app dir
    config_file_path_ = fs::path(GetSystemAppDir() / kConfigFileName());
    return (fs::exists(config_file_path_, error_code) && !error_code);
  } else {
    config_file_path_ = local_config_file_path;
  }
  return true;
}

bool VaultManager::ReadConfigFile() {
  std::string content;
  if (!ReadFile(config_file_path_, &content)) {
    LOG(kError) << "Failed to read config file " << config_file_path_;
    return false;
  }

  // Handle first run with 1 byte config file in local dir (for use in tests)
  if (content.size() == 1U && InTestMode())
    return true;

  protobuf::VaultManagerConfig config;
  if (!config.ParseFromString(content) || !config.IsInitialized()) {
    LOG(kError) << "Failed to parse config file " << config_file_path_;
    return false;
  }

  update_interval_ = bptime::seconds(config.update_interval());

  for (int i(0); i != config.vault_info_size(); ++i) {
    std::shared_ptr<VaultInfo> vault_info(new VaultInfo);
    vault_info->FromProtobuf(config.vault_info(i));
    vault_info->process_index = AddVaultToProcesses(vault_info->chunkstore_path,
                                                    vault_info->chunkstore_capacity,
                                                    "");
    if (vault_info->process_index == ProcessManager::kInvalidIndex())
      continue;
    if (vault_info->requested_to_run)
      process_manager_.StartProcess(vault_info->process_index);
    vault_infos_.push_back(vault_info);
  }

  return true;
}

bool VaultManager::WriteConfigFile() {
  protobuf::VaultManagerConfig config;
  {
    std::lock_guard<std::mutex> lock(update_mutex_);
    config.set_update_interval(update_interval_.total_seconds());
  }

  std::lock_guard<std::mutex> lock(vault_infos_mutex_);
  for (auto& vault_info : vault_infos_) {
    protobuf::VaultInfo* pb_vault_info = config.add_vault_info();
    vault_info->ToProtobuf(pb_vault_info);
  }

  if (!WriteFile(config_file_path_, config.SerializeAsString())) {
    LOG(kError) << "Failed to write config file " << config_file_path_;
    return false;
  }

  return true;
}

void VaultManager::ListenForMessages() {
  while (transport_->StartListening(local_port_) != kSuccess) {
    ++local_port_;
    if (local_port_ > kMaxPort()) {
      LOG(kError) << "Listening failed on all ports in range " << kMinPort() << " - " << kMaxPort();
      return;
    }
  }
}

void VaultManager::HandleReceivedMessage(const std::string& message, Port peer_port) {
  MessageType type;
  std::string payload;
  if (!detail::UnwrapMessage(message, type, payload)) {
    LOG(kError) << "Failed to handle incoming message.";
    return;
  }
  LOG(kVerbose) << "HandleReceivedMessage: message type " << static_cast<int>(type) << " received.";
  std::string response;
  switch (type) {
    case MessageType::kPing:
      HandlePing(payload, response);
      break;
    case MessageType::kStartVaultRequest:
      HandleStartVaultRequest(payload, response);
      break;
    case MessageType::kVaultIdentityRequest:
      HandleVaultIdentityRequest(payload, response);
      break;
    case MessageType::kStopVaultRequest:
      HandleStopVaultRequest(payload, response);
      break;
    case MessageType::kUpdateIntervalRequest:
      HandleUpdateIntervalRequest(payload, response);
      break;
    default:
      LOG(kError) << "Invalid message type";
      return;
  }
  transport_->Send(response, peer_port);
}

void VaultManager::HandlePing(const std::string& request, std::string& response) {
  protobuf::Ping ping;
  if (!ping.ParseFromString(request) || !ping.IsInitialized()) {  // Silently drop
    LOG(kError) << "Failed to parse ping.";
    return;
  }
  response = detail::WrapMessage(MessageType::kPing, request);
}

void VaultManager::HandleStartVaultRequest(const std::string& request, std::string& response) {
  protobuf::StartVaultRequest start_vault_request;
  if (!start_vault_request.ParseFromString(request) || !start_vault_request.IsInitialized()) {
    // Silently drop
    LOG(kError) << "Failed to parse StartVaultRequest.";
    return;
  }

  auto set_response([&response](bool result) {
    protobuf::StartVaultResponse start_vault_response;
    start_vault_response.set_result(result);
    response = detail::WrapMessage(MessageType::kStartVaultResponse,
                                   start_vault_response.SerializeAsString());
  });

  std::shared_ptr<VaultInfo> vault_info(new VaultInfo);
  vault_info->account_name = start_vault_request.account_name();
  asymm::ParseKeys(start_vault_request.keys(), vault_info->keys);
  std::string short_vault_id(EncodeToBase32(crypto::Hash<crypto::SHA1>(vault_info->keys.identity)));
  vault_info->chunkstore_path = (config_file_path_.parent_path() / short_vault_id).string();
  if (!HandleBootstrapFile(short_vault_id, config_file_path_.parent_path())) {
    LOG(kError) << "Failed to set bootstrap file for vault "
                << HexSubstr(vault_info->keys.identity);
    return set_response(false);
  }

  vault_info->chunkstore_capacity = 0;
  LOG(kVerbose) << "Bootstrap endpoint is " << start_vault_request.bootstrap_endpoint();
  vault_info->process_index = AddVaultToProcesses(vault_info->chunkstore_path,
                                                  vault_info->chunkstore_capacity,
                                                  start_vault_request.bootstrap_endpoint());
  if (vault_info->process_index == ProcessManager::kInvalidIndex())
    return set_response(false);

  process_manager_.StartProcess(vault_info->process_index);
  {
    std::lock_guard<std::mutex> lock(vault_infos_mutex_);
    vault_infos_.push_back(vault_info);
  }

  WriteConfigFile();

  // Need to block here until new vault has sent VaultIdentityRequest, since response to client will
  // be sent once this function exits.
  std::unique_lock<std::mutex> lock(vault_info->mutex);
  LOG(kVerbose) << "Waiting for Vault " << vault_info->process_index;
  if (!vault_info->cond_var.wait_for(
          lock,
          std::chrono::seconds(3),
          [&] { return vault_info->vault_requested; })) {  // NOLINT (Philip)
    LOG(kError) << "HandleClientStartVaultRequest: wait for Vault timed out";
    return set_response(false);
  }

  set_response(true);
}

void VaultManager::HandleVaultIdentityRequest(const std::string& request, std::string& response) {
  protobuf::VaultIdentityRequest vault_identity_request;
  if (!vault_identity_request.ParseFromString(request) || !vault_identity_request.IsInitialized()) {
    // Silently drop
    LOG(kError) << "Failed to parse VaultIdentityRequest.";
    return;
  }

  protobuf::VaultIdentityResponse vault_identity_response;
  std::lock_guard<std::mutex> lock(vault_infos_mutex_);
  auto itr(std::find_if(
      vault_infos_.begin(),
      vault_infos_.end(),
      [&vault_identity_request](const std::shared_ptr<VaultInfo>& vault_info) {
        return vault_info->process_index == vault_identity_request.process_index();
      }));
  if (itr == vault_infos_.end()) {
    LOG(kError) << "Vault with process_index " << vault_identity_request.process_index()
                << " hasn't been added.";
    vault_identity_response.set_account_name("");
    vault_identity_response.set_keys("");
  } else {
    std::string serialised_keys;
    if (!asymm::SerialiseKeys((*itr)->keys, serialised_keys)) {
      LOG(kError) << "Failed to serialise keys of vault with process_index "
                  << vault_identity_request.process_index();
      vault_identity_response.set_account_name("");
      vault_identity_response.set_keys("");
    } else {
      vault_identity_response.set_account_name((*itr)->account_name);
      vault_identity_response.set_keys(serialised_keys);
      // Notify so that waiting client StartVaultResponse can be sent
      std::lock_guard<std::mutex> lock((*itr)->mutex);
      (*itr)->vault_requested = true;
      (*itr)->cond_var.notify_one();
    }
  }

  response = detail::WrapMessage(MessageType::kVaultIdentityResponse,
                                 vault_identity_response.SerializeAsString());
}

void VaultManager::HandleStopVaultRequest(const std::string& request, std::string& response) {
  protobuf::StopVaultRequest stop_vault_request;
  if (!stop_vault_request.ParseFromString(request) || !stop_vault_request.IsInitialized()) {
    // Silently drop
    LOG(kError) << "Failed to parse StopVaultRequest.";
    return;
  }

  protobuf::StopVaultResponse stop_vault_response;
  std::lock_guard<std::mutex> lock(vault_infos_mutex_);
  auto itr(std::find_if(vault_infos_.begin(),
                        vault_infos_.end(),
                        [&stop_vault_request](const std::shared_ptr<VaultInfo>& vault_info) {
                          return vault_info->keys.identity == stop_vault_request.identity();
                          // TODO(Fraser#5#): 2012-08-16 - Check client port is same as peer_port for this request
                        }));
  if (itr == vault_infos_.end()) {
    LOG(kError) << "Vault with identity " << HexSubstr(stop_vault_request.identity())
                << " hasn't been added.";
    stop_vault_response.set_result(false);
  } else if (!asymm::Validate(stop_vault_request.data(),
                              stop_vault_request.signature(),
                              (*itr)->keys.public_key)) {
    LOG(kError) << "Vault with identity " << HexSubstr(stop_vault_request.identity())
                << " hasn't been added.";
    stop_vault_response.set_result(false);
  } else {
    stop_vault_response.set_result(true);
  }

  response = detail::WrapMessage(MessageType::kVaultShutdownResponse,
                                 stop_vault_response.SerializeAsString());

  LOG(kInfo) << "Shutting down vault with identity " << HexSubstr(stop_vault_request.identity());
  StopVault(stop_vault_request.identity());
                                                    // TODO(Fraser#5#): 2012-08-13 - Do we need this cond_var_ call?
  cond_var_.notify_all();
}

void VaultManager::HandleUpdateIntervalRequest(const std::string& request, std::string& response) {
  protobuf::UpdateIntervalRequest update_interval_request;
  if (!update_interval_request.ParseFromString(request) ||
      !update_interval_request.IsInitialized()) {  // Silently drop
    LOG(kError) << "Failed to parse UpdateIntervalRequest.";
    return;
  }

  protobuf::UpdateIntervalResponse update_interval_response;
  if (update_interval_request.has_new_update_interval()) {
    if (SetUpdateInterval(bptime::seconds(update_interval_request.new_update_interval())))
      update_interval_response.set_update_interval(GetUpdateInterval().total_seconds());
    else
      update_interval_response.set_update_interval(0);
  } else {
    update_interval_response.set_update_interval(GetUpdateInterval().total_seconds());
  }

  response = detail::WrapMessage(MessageType::kUpdateIntervalResponse,
                                 update_interval_response.SerializeAsString());
}

bool VaultManager::SetUpdateInterval(const bptime::time_duration& update_interval) {
  if (update_interval < kMinUpdateInterval() || update_interval > kMaxUpdateInterval()) {
    LOG(kError) << "Invalid update interval of " << update_interval;
    return false;
  }
  std::lock_guard<std::mutex> lock(update_mutex_);
  update_interval_ = update_interval;
  update_timer_.expires_from_now(update_interval_);
  update_timer_.async_wait([this](const boost::system::error_code& ec) { CheckForUpdates(ec); });  // NOLINT (Fraser)
  return true;
}

bptime::time_duration VaultManager::GetUpdateInterval() const {
  std::lock_guard<std::mutex> lock(update_mutex_);
  return update_interval_;
}

std::string VaultManager::FindLatestLocalVersion(const std::string& application) const {
  std::string app;
  detail::Platform platform(detail::Platform::Type::kUnknown);
  int latest_version(detail::kInvalidVersion), version(detail::kInvalidVersion);
  std::string latest_file;
  fs::path search_dir(config_file_path_.parent_path());
  // Disable logging temporarily
  log::FilterMap filter_map_before(log::Logging::instance().Filter());
  log::FilterMap disable_logging;
  disable_logging["*"] = log::kFatal;
  log::Logging::instance().SetFilter(disable_logging);
  try {
    for (fs::directory_iterator itr(search_dir); itr != fs::directory_iterator(); ++itr) {
      // Allow directory iteration to be interrupted
      boost::this_thread::interruption_point();
      std::string file_name((*itr).path().stem().string());
      if (detail::TokeniseFileName(file_name, &app, &platform, &version) &&
          app == application &&
          platform.type() == detail::kThisPlatform().type() &&
          version > latest_version) {
        latest_version = version;
        latest_file = file_name;
      }
    }
    log::Logging::instance().SetFilter(filter_map_before);
  }
  catch(const std::exception& e) {
    log::Logging::instance().SetFilter(filter_map_before);
    LOG(kError) << e.what();
    latest_file.clear();
  }
  if (latest_file.empty()) {
    LOG(kInfo) << "Couldn't find any version of " << application << " in " << search_dir;
    latest_file = detail::GenerateFileName(application, detail::kThisPlatform(), "0.00.00");
  }
  return latest_file;
}

void VaultManager::CheckForUpdates(const boost::system::error_code& ec) {
  if (ec) {
    if (ec != boost::asio::error::operation_aborted)
      LOG(kError) << ec.message();
    return;
  }

  if (download_manager_.UpdateAndVerify("bootstrap-global.dat", config_file_path_.parent_path()) !=
      "bootstrap-global.dat") {
    LOG(kError) << "Failed to update bootstrap-global.dat";
  }

  std::lock_guard<std::mutex> lock(update_mutex_);
  std::vector<std::string> applications;
  applications.push_back(kApplicationName);
  applications.push_back(kVaultName());
  applications.push_back(kVaultManagerName());

  for (auto application : applications) {
    std::string latest_local(FindLatestLocalVersion(application));
    LOG(kVerbose) << "Latest local version is " << latest_local;
    std::string updated_file(download_manager_.UpdateAndVerify(latest_local,
                                                               config_file_path_.parent_path()));
    if (!updated_file.empty()) {  // A new version was downloaded
      boost::system::error_code error_code;
#ifndef MAIDSAFE_WIN32
      fs::path symlink(GetSystemAppDir() / application);
      if (!fs::remove(symlink, error_code) || error_code)
        LOG(kWarning) << "Failed to remove symlink " << symlink << ": " << error_code.message();

      fs::create_symlink(updated_file, symlink, error_code);
      LOG(kVerbose) << "Symbolic link " << symlink << " to " << updated_file
                    << (error_code ? " failed to be created: " + error_code.message() : " created");
#endif
      //// Remove the previous file
      //while (fs::exists(current_path / latest_local_file)) {
      //  if (fs::remove(current_path / latest_local_file)) {
      //    continue;
      //  }
      //  boost::mutex::scoped_lock lock(mutex_);
      //  cond_var_.timed_wait(lock, boost::posix_time::minutes(2),
      //                        [&] { return stop_listening_for_updates_; });
      //  if (stop_listening_for_updates_)
      //    return;
      //}
      //if (application == kVaultManagerName() || application == kVaultName())
      //  RestartVaultManager(updated_file, application);
    } else {
      LOG(kVerbose) << "No newer file has been found";
    }
      //if (stop_listening_for_updates_)
      //  return;
  }

  update_timer_.expires_from_now(update_interval_);
  update_timer_.async_wait([this](const boost::system::error_code& ec) { CheckForUpdates(ec); });  // NOLINT (Fraser)
}

bool VaultManager::InTestMode() const {
  return config_file_path_ == fs::path(".") / kConfigFileName();
}

std::vector<std::shared_ptr<VaultManager::VaultInfo>>::const_iterator
    VaultManager::FindFromIdentity(const std::string& identity) const {
  return std::find_if(vault_infos_.begin(),
                      vault_infos_.end(),
                      [identity](const std::shared_ptr<VaultInfo>& vault_info) {
                        return vault_info->keys.identity == identity;
                      });
}

ProcessIndex VaultManager::AddVaultToProcesses(const std::string& chunkstore_path,
                                               const uintmax_t& chunkstore_capacity,
                                               const std::string& bootstrap_endpoint) {
  Process process;
  LOG(kInfo) << "Creating a vault at " << chunkstore_path << ", with capacity: "
             << chunkstore_capacity;
  if (!process.SetExecutablePath(config_file_path_.parent_path() /
                                 (kVaultName() + detail::kThisPlatform().executable_extension()))) {
    return ProcessManager::kInvalidIndex();
  }

  if (!bootstrap_endpoint.empty()) {
    process.AddArgument("--peer");
    process.AddArgument(bootstrap_endpoint);
  }
  process.AddArgument("--chunk_path");
  process.AddArgument(chunkstore_path);
  process.AddArgument("--chunk_capacity");
  process.AddArgument(boost::lexical_cast<std::string>(chunkstore_capacity));
  process.AddArgument("--start");
  LOG(kInfo) << "Process Name: " << process.name();
  return process_manager_.AddProcess(process, local_port_);
}

void VaultManager::RestartVault(const std::string& identity) {
  std::lock_guard<std::mutex> lock(vault_infos_mutex_);
  auto itr(FindFromIdentity(identity));
  if (itr == vault_infos_.end()) {
    LOG(kError) << "Vault with identity " << HexSubstr(identity) << " hasn't been added.";
    return;
  }
  process_manager_.RestartProcess((*itr)->process_index);
}

void VaultManager::StopVault(const std::string& identity) {
  std::lock_guard<std::mutex> lock(vault_infos_mutex_);
  auto itr(FindFromIdentity(identity));
  if (itr == vault_infos_.end()) {
    LOG(kError) << "Vault with identity " << HexSubstr(identity) << " hasn't been added.";
    return;
  }
  process_manager_.StopProcess((*itr)->process_index);
                  // TODO(Fraser#5#): 2012-08-16 - Need to send VaultShutdownRequest here and block until response arrives?
  (*itr)->requested_to_run = false;
  WriteConfigFile();
}

//  void VaultManager::EraseVault(const std::string& account_name) {
//    if (index < static_cast<int32_t>(processes_.size())) {
//      auto itr(processes_.begin() + (index - 1));
//      process_manager_.KillProcess((*itr).second);
//      processes_.erase(itr);
//      LOG(kInfo) << "Erasing vault...";
//      if (WriteConfig()) {
//        LOG(kInfo) << "Done!";
//      }
//    } else {
//      LOG(kError) << "Invalid index of " << index << " for processes container with size "
//                  << processes_.size();
//    }
//  }

//  int32_t VaultManager::ListVaults(bool select) const {
//    fs::path path((GetSystemAppDir() / "config.txt"));
//
//    std::string content;
//    ReadFile(path, &content);
//
//    typedef boost::tokenizer<boost::char_separator<char>> vault_tokenizer;
//    boost::char_separator<char> delimiter("\n", "", boost::keep_empty_tokens);
//    vault_tokenizer tok(content, delimiter);
//
//    int32_t i = 1;
//    LOG(kInfo) << "************************************************************";
//    for (vault_tokenizer::iterator iterator = tok.begin(); iterator != tok.end(); ++iterator) {
//      LOG(kInfo) << i << ". " << *iterator;
//      i++;
//    }
//    LOG(kInfo) << "************************************************************";
//
//    if (select) {
//      int32_t option;
//      LOG(kInfo) << "Select an item: ";
//      std::cin >> option;
//      return option;
//    }
//
//    return 0;
//  }

}  // namespace priv

}  // namespace maidsafe

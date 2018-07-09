/*
 *  Copyright (c) 2016-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <boost/filesystem.hpp>
#include <folly/Conv.h>
#include <folly/Optional.h>
#include <folly/experimental/FunctionScheduler.h>
#include <folly/init/Init.h>
#include <folly/logging/Init.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>
#include <pwd.h>
#include <sysexits.h>
#include <unistd.h>
#include "EdenServer.h"
#include "eden/fs/config/EdenConfig.h"
#include "eden/fs/fuse/privhelper/PrivHelper.h"
#include "eden/fs/fuse/privhelper/UserInfo.h"
#include "eden/fs/service/StartupLogger.h"

DEFINE_bool(allowRoot, false, "Allow running eden directly as root");
DEFINE_bool(
    foreground,
    false,
    "Run edenfs in the foreground, rather than daemonizing "
    "as a background process");
DEFINE_string(edenDir, "", "The path to the .eden directory");
DEFINE_string(
    etcEdenDir,
    "/etc/eden",
    "The directory holding all system configuration files");
DEFINE_string(configPath, "", "The path of the ~/.edenrc config file");
DEFINE_string(
    logPath,
    "",
    "If set, redirects stdout and stderr to the log file given.");

constexpr folly::StringPiece kDefaultUserConfigFile{".edenrc"};
constexpr folly::StringPiece kEdenfsConfigFile{"edenfs.rc"};

using namespace facebook::eden;

namespace facebook {
namespace eden {
std::string getEdenfsBuildName();
} // namespace eden
} // namespace facebook

// Set the default log level for all eden logs to DBG2
// Also change the "default" log handler (which logs to stderr) to log
// messages asynchronously rather than blocking in the logging thread.
FOLLY_INIT_LOGGING_CONFIG("eden=DBG2; default:async=true");

namespace {

std::shared_ptr<StartupLogger> daemonizeIfRequested(
    folly::StringPiece logPath) {
  auto startupLogger = std::make_shared<StartupLogger>();
  if (FLAGS_foreground) {
    return startupLogger;
  }

  startupLogger->daemonize(logPath);
  return startupLogger;
}

std::string getLogPath(AbsolutePathPiece edenDir) {
  // If a log path was explicitly specified as a command line argument use that
  if (!FLAGS_logPath.empty()) {
    return FLAGS_logPath;
  }

  // If we are running in the foreground default to an empty log path
  // (just log directly to stderr)
  if (FLAGS_foreground) {
    return "";
  }

  // When running in the background default to logging to
  // <edenDir>/logs/edenfs.log
  // Create the logs/ directory first in case it does not exist.
  auto logDir = edenDir + "logs"_pc;
  ensureDirectoryExists(logDir);
  return (logDir + "edenfs.log"_pc).value();
}

AbsolutePath ensureEdenDirExists(folly::StringPiece path) {
  // Call boost::filesystem::create_directories() directly on the user-supplied
  // argument before we try canonicalizing it.  We'll do so with realpath()
  // later, but this requires the directory exist first.
  boost::filesystem::path boostPath(path.begin(), path.end());
  boost::filesystem::create_directories(boostPath);

  // Call realpath now that we know the directory exists.
  return facebook::eden::realpath(path);
}

} // namespace

int main(int argc, char** argv) {
  // Fork the privhelper process, then drop privileges in the main process.
  // This should be done as early as possible, so that everything else we do
  // runs only with normal user privileges.
  //
  // We do this even before calling folly::init().  The privhelper server
  // process will call folly::init() on its own.
  auto identity = UserInfo::lookup();
  auto originalEUID = geteuid();
  auto privHelper = startPrivHelper(identity);
  identity.dropPrivileges();

  // Make sure to run this before any flag values are read.
  folly::init(&argc, &argv);

  // Fail if we were not started as root.  The privhelper needs root
  // privileges in order to perform mount and unmount operations.
  // We check this after calling folly::init() so that non-root users
  // can use the --help argument.
  if (originalEUID != 0) {
    fprintf(stderr, "error: edenfs must be started as root\n");
    return EX_NOPERM;
  }

  if (identity.getUid() == 0 && !FLAGS_allowRoot) {
    fprintf(
        stderr,
        "error: you appear to be running eden as root, "
        "rather than using\n"
        "sudo or a setuid binary.  This is normally undesirable.\n"
        "Pass in the --allowRoot flag if you really mean to run "
        "eden as root.\n");
    return EX_USAGE;
  }

  // normalizeBestEffort() to try resolving symlinks in these paths but don't
  // fail if they don't exist.
  AbsolutePath systemConfigDir;
  try {
    systemConfigDir = normalizeBestEffort(FLAGS_etcEdenDir);
  } catch (const std::exception& ex) {
    fprintf(
        stderr,
        "invalid flag value: %s: %s\n",
        FLAGS_edenDir.c_str(),
        folly::exceptionStr(ex).c_str());
    return EX_SOFTWARE;
  }
  const auto systemConfigPath =
      systemConfigDir + PathComponentPiece{kEdenfsConfigFile};

  const std::string configPathStr = FLAGS_configPath;
  AbsolutePath userConfigPath;
  if (configPathStr.empty()) {
    userConfigPath = identity.getHomeDirectory() +
        PathComponentPiece{kDefaultUserConfigFile};
  } else {
    try {
      userConfigPath = normalizeBestEffort(configPathStr);
    } catch (const std::exception& ex) {
      fprintf(
          stderr,
          "invalid flag value: %s: %s\n",
          FLAGS_configPath.c_str(),
          folly::exceptionStr(ex).c_str());
      return EX_SOFTWARE;
    }
  }
  // Create the default EdenConfig. Next, update with command line arguments.
  // Command line areguments will take precedence over config file settings.
  auto edenConfig = std::make_unique<EdenConfig>(
      identity.getHomeDirectory(),
      userConfigPath,
      systemConfigDir,
      systemConfigPath);

  // Load system and user configurations
  edenConfig->loadSystemConfig();
  edenConfig->loadUserConfig();

  // We will set the edenDir using ConfigSource COMMAND_LINE so that it cannot
  // be over-ridden by subsequent config file updates.
  AbsolutePath edenDir;
  try {
    if (!FLAGS_edenDir.empty()) {
      edenDir = ensureEdenDirExists(folly::to<std::string>(FLAGS_edenDir));
    } else {
      edenDir =
          ensureEdenDirExists(folly::to<std::string>(edenConfig->getEdenDir()));
    }
    edenConfig->setEdenDir(edenDir, facebook::eden::COMMAND_LINE);
  } catch (const std::exception& ex) {
    fprintf(
        stderr,
        "error creating %s: %s\n",
        FLAGS_edenDir.c_str(),
        folly::exceptionStr(ex).c_str());
    return EX_SOFTWARE;
  }

  auto logPath = getLogPath(edenDir);
  auto startupLogger = daemonizeIfRequested(logPath);
  XLOG(DBG3) << edenConfig->toString();
  folly::Optional<EdenServer> server;
  auto prepareFuture = folly::Future<folly::Unit>::makeEmpty();
  try {
    // If stderr was redirected to a log file, inform the privhelper
    // to make sure it logs to our current stderr.
    if (!logPath.empty()) {
      privHelper->setLogFileBlocking(
          folly::File(STDERR_FILENO, /*ownsFd=*/false));
    }

    // Since we are a daemon, and we don't ever want to be in a situation
    // where we hold any open descriptors through a fuse mount that points
    // to ourselves (which can happen during takeover), we chdir to `/`
    // to avoid having our cwd reference ourselves if the user runs
    // `eden daemon --takeover` from within an eden mount
    folly::checkPosixError(chdir("/"), "failed to chdir(/)");

    // Set some default glog settings, to be applied unless overridden on the
    // command line
    gflags::SetCommandLineOptionWithMode(
        "logtostderr", "1", gflags::SET_FLAGS_DEFAULT);
    gflags::SetCommandLineOptionWithMode(
        "minloglevel", "0", gflags::SET_FLAGS_DEFAULT);

    startupLogger->log("Starting ", getEdenfsBuildName(), ", pid ", getpid());

    server.emplace(
        std::move(identity), std::move(privHelper), std::move(edenConfig));

    prepareFuture = server->prepare(startupLogger);
  } catch (const std::exception& ex) {
    startupLogger->exitUnsuccessfully(
        EX_SOFTWARE, "error starting edenfs: ", folly::exceptionStr(ex));
  }

  prepareFuture.then([startupLogger](folly::Try<folly::Unit>&& result) {
    // If an error occurred this means that we failed to mount all of the mount
    // points.  However, we have still started and will continue running, so we
    // report successful startup here no matter what.
    if (result.hasException()) {
      // Log an overall error message here.
      // We will have already logged more detailed messages for each mount
      // failure when it occurred.
      startupLogger->warn(
          "did not successfully remount all repositories: ",
          result.exception().what());
    }
    startupLogger->success();
  });

  server->run();

  XLOG(INFO) << "edenfs exiting successfully";
  return EX_OK;
}

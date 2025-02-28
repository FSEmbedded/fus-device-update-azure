/**
 * @file fusfirmware_tasks.cpp
 * @brief Implements functions related to fus/firmware update tasks.
 *
 * @copyright Copyright (c) F&S Systeme Elektronik GmbH.
 * Licensed under the MIT License.
 */

#include <unordered_map>

#include "fusupdate_tasks.hpp"
#include "common_tasks.hpp"

#include "aduc/logging.h"
#include "aduc/process_utils.hpp"
#include "aduc/string_utils.hpp"
#include "aduc/system_utils.h"

namespace Adu
{
namespace Shell
{
namespace Tasks
{
namespace FUSUpdate
{

const char* updater_command = UPDATER_CLI_FULL_CMD;
const char* updater_option_update_install = "--update_file";
const char* updater_option_commit_update = "--commit_update";
const char* updater_option_get_update_state = "--update_reboot_state";
const char* updater_option_rollback_update = "--rollback_update";
const char* updater_option_update_type = "--update_type";

/**
 * @brief Runs "<updater command> --update_file <path>" command in  a child process or
 * "<updater command> --update_file <path> --update_type (app or fw)"
 *
 * @param launchArgs An adu-shell launch arguments.
 * @return A result from child process.
 */
ADUShellTaskResult Install(const ADUShell_LaunchArguments& launchArgs)
{
    ADUShellTaskResult taskResult;
    std::vector<std::string> args;

    Log_Info("Installing image. Path: %s", launchArgs.targetData);

    args.emplace_back(updater_option_update_install);
    args.emplace_back(launchArgs.targetData);

    if (!launchArgs.targetOptions.empty())
    {
        if ((std::string("app").compare(launchArgs.targetOptions.front()) == 0)
            || (std::string("fw").compare(launchArgs.targetOptions.front()) == 0))
        {
            args.emplace_back(updater_option_update_type);
            args.emplace_back(launchArgs.targetOptions.front());
        }
    }

    taskResult.SetExitStatus(ADUC_LaunchChildProcess(updater_command, args, taskResult.Output()));

    return taskResult;
}

/**
* @brief Runs "<updater command> --commit_update" command in  a child process.
*
* @param launchArgs An adu-shell launch arguments.
* @return A result from child process.
*/
ADUShellTaskResult CommitUpdate(const ADUShell_LaunchArguments& launchArgs)
{
    ADUShellTaskResult taskResult;

    Log_Info("Apply image: commit update state");

    std::vector<std::string> args;

    args.emplace_back(updater_option_commit_update);

    taskResult.SetExitStatus(ADUC_LaunchChildProcess(updater_command, args, taskResult.Output()));

    return taskResult;
}

/**
* @brief Runs "<updater command> [--rollback_update]" command in  a child process.
*
* @param launchArgs An adu-shell launch arguments.
* @return A result from child process.
*/
ADUShellTaskResult Cancel(const ADUShell_LaunchArguments& launchArgs)
{
    ADUShellTaskResult taskResult;

    Log_Info("Rollback update install");

    std::vector<std::string> args;

    args.emplace_back(updater_option_rollback_update);

    taskResult.SetExitStatus(ADUC_LaunchChildProcess(updater_command, args, taskResult.Output()));

    return taskResult;
}

/**
* @brief Runs "<updater command> [--update_reboot_state]" command in  a child process.
*
* @param launchArgs An adu-shell launch arguments.
* @return A result from child process.
*/
ADUShellTaskResult Execute(const ADUShell_LaunchArguments& launchArgs)
{
    ADUShellTaskResult taskResult;
    std::vector<std::string> args;

    if (launchArgs.targetOptions.size() > 2)
    {
        Log_Error("Wrong number of target options.");
        taskResult.SetExitStatus(EXIT_FAILURE);
        return taskResult;
    }

    for (const std::string& option : launchArgs.targetOptions)
    {
        Log_Debug("args: %s", option.c_str());
        args.emplace_back(option);
    }

    taskResult.SetExitStatus(ADUC_LaunchChildProcess(updater_command, args, taskResult.Output()));

    for (const std::string& option : launchArgs.targetOptions)
    {
        if(option.compare("--firmware_version") == 0)
            printf("--firmware_version %s", taskResult.Output().c_str());
        if(option.compare("--application_version") == 0)
            printf("--application_version %s", taskResult.Output().c_str());
    }

    return taskResult;
}

/**
 * @brief Runs appropriate command based on an action and other arguments in launchArgs.
 *
 * This could resulted in one or more packaged installed or removed from the system.
 *
 * @param launchArgs The adu-shell launch command-line arguments that has been parsed.
 * @return A result from child process.
 */
ADUShellTaskResult DoFUSUpdateTask(const ADUShell_LaunchArguments& launchArgs)
{
    ADUShellTaskResult taskResult;
    ADUShellTaskFuncType taskProc = nullptr;

    try
    {
        // clang-format off

        static const std::unordered_map<ADUShellAction, ADUShellTaskFuncType> actionMap = {
            { ADUShellAction::Install, Install },
            { ADUShellAction::Execute, Execute },
            { ADUShellAction::Apply, CommitUpdate },
            { ADUShellAction::Cancel, Cancel },
            { ADUShellAction::Reboot, Adu::Shell::Tasks::Common::Reboot }
        };

        // clang-format on

        taskProc = actionMap.at(launchArgs.action);
    }
    catch (const std::exception& ex)
    {
        Log_Error("Unsupported action: '%s'", launchArgs.action);
        taskResult.SetExitStatus(ADUSHELL_EXIT_UNSUPPORTED);
    }

    try
    {
        taskResult = taskProc(launchArgs);
    }
    catch (const std::exception& ex)
    {
        Log_Error("Exception occurred while running task: '%s'", ex.what());
        taskResult.SetExitStatus(EXIT_FAILURE);
    }

    return taskResult;
}

} // namespace FUSUpdate
} // namespace Tasks
} // namespace Shell
} // namespace Adu

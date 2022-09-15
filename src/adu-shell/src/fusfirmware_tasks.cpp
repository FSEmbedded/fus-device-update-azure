/**
 * @file fusfirmware_tasks.cpp
 * @brief Implements functions related to fus/firmware update tasks.
 *
 * @copyright Copyright (c) F&S Systeme Elektronik GmbH.
 * Licensed under the MIT License.
 */

#include <unordered_map>

#include "fusfirmware_tasks.hpp"
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
namespace FUSFirmware
{

const char* fsazure_command = "fs-azure";
const char* fsazure_option_firmware_install = "--firmware_file";
const char* fsazure_option_commit_update = "--commit_update";
const char* fsazure_option_get_update_state = "--update_reboot_state";
const char* fsazure_option_rollback_firmware = "--rollback_firmware";

/**
 * @brief Runs "fs-azure --firmware_file <path>" command in  a child process.
 *
 * @param launchArgs An adu-shell launch arguments.
 * @return A result from child process.
 */
ADUShellTaskResult Install(const ADUShell_LaunchArguments& launchArgs)
{
    ADUShellTaskResult taskResult;
    std::vector<std::string> args;

    Log_Info("Installing image. Path: %s", launchArgs.targetData);

    args.emplace_back(fsazure_option_firmware_install);
    args.emplace_back(launchArgs.targetData);

    taskResult.SetExitStatus(ADUC_LaunchChildProcess(fsazure_command, args, taskResult.Output()));

    return taskResult;
}

/**
* @brief Runs "fs-azure --commit_update" command in  a child process.
*
* @param launchArgs An adu-shell launch arguments.
* @return A result from child process.
*/
ADUShellTaskResult CommitUpdate(const ADUShell_LaunchArguments& launchArgs)
{
    ADUShellTaskResult taskResult;

    Log_Info("Apply image: commit update state");

    std::vector<std::string> args;

    args.emplace_back(fsazure_option_commit_update);

    taskResult.SetExitStatus(ADUC_LaunchChildProcess(fsazure_command, args, taskResult.Output()));

    return taskResult;
}

/**
* @brief Runs "fs-azure [--rollback_firmware]" command in  a child process.
*
* @param launchArgs An adu-shell launch arguments.
* @return A result from child process.
*/
ADUShellTaskResult Cancel(const ADUShell_LaunchArguments& launchArgs)
{
    ADUShellTaskResult taskResult;

    Log_Info("Rollback firmware install");

    std::vector<std::string> args;

    args.emplace_back(fsazure_option_rollback_firmware);

    taskResult.SetExitStatus(ADUC_LaunchChildProcess(fsazure_command, args, taskResult.Output()));

    return taskResult;
}

/**
* @brief Runs "fs-azure [--update_reboot_state]" command in  a child process.
*
* @param launchArgs An adu-shell launch arguments.
* @return A result from child process.
*/
ADUShellTaskResult Execute(const ADUShell_LaunchArguments& launchArgs)
{
    ADUShellTaskResult taskResult;

    Log_Info("Get current install status");

    std::vector<std::string> args;

    if (launchArgs.targetOptions.size() != 1)
    {
        Log_Error("Not exact one target option is set");
        taskResult.SetExitStatus(EXIT_FAILURE);
        return taskResult;
    }

    if (launchArgs.targetOptions.at(0) != std::string("update_state"))
    {
        Log_Error("Target option is not \"update_state\": '%s'", launchArgs.targetOptions.at(0));
        taskResult.SetExitStatus(EXIT_FAILURE);
        return taskResult;
    }

    args.emplace_back(fsazure_option_get_update_state);

    taskResult.SetExitStatus(ADUC_LaunchChildProcess(fsazure_command, args, taskResult.Output()));

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
ADUShellTaskResult DoFUSFirmwareUpdateTask(const ADUShell_LaunchArguments& launchArgs)
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

} // namespace FUSFirmware
} // namespace Tasks
} // namespace Shell
} // namespace Adu

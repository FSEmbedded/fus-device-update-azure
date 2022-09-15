/**
 * @file aptget_tasks.hpp
 * @brief Implements functions related to microsoft/apt update type.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */
#ifndef ADU_SHELL_FUSFIRMWARE_TASKS_HPP
#define ADU_SHELL_FUSFIRMWARE_TASKS_HPP

#include "adushell.hpp"

namespace Adu
{
namespace Shell
{
namespace Tasks
{
namespace FUSFirmware
{
/**
* @brief Runs "fs-azure --firmware_file <path>" command in  a child process.
*
* @param launchArgs An adu-shell launch arguments.
* @return A result from child process.
*/
ADUShellTaskResult Install(const ADUShell_LaunchArguments& launchArgs);

/**
* @brief Runs "fs-azure --commit_update" command in  a child process.
*
* @param launchArgs An adu-shell launch arguments.
* @return A result from child process.
*/
ADUShellTaskResult CommitUpdate(const ADUShell_LaunchArguments& launchArgs);

/**
* @brief Runs "fs-azure [--update_reboot_state]" command in  a child process.
*
* @param launchArgs An adu-shell launch arguments.
* @return A result from child process.
*/
ADUShellTaskResult Execute(const ADUShell_LaunchArguments& launchArgs);

/**
* @brief Runs "fs-azure [--rollback_firmware]" command in  a child process.
*
* @param launchArgs An adu-shell launch arguments.
* @return A result from child process.
*/
ADUShellTaskResult Cancel(const ADUShell_LaunchArguments& launchArgs);

/**
 * @brief Runs appropriate command based on an action and other arguments in launchArgs.
 *
 * @param launchArgs An adu-shell launch arguments.
 * @return A result from child process.
 */
ADUShellTaskResult DoFUSFirmwareUpdateTask(const ADUShell_LaunchArguments& launchArgs);

} // namespace FUSFirmware
} // namespace Tasks
} // namespace Shell
} // namespace Adu

#endif // ADU_SHELL_FUSFIRMWARE_TASKS_HPP

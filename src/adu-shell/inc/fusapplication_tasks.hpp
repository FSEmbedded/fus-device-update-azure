/**
 * @file aptget_tasks.hpp
 * @brief Implements functions related to microsoft/apt update type.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */
#ifndef ADU_SHELL_FUSAPPLICATION_TASKS_HPP
#define ADU_SHELL_FUSAPPLICATION_TASKS_HPP

#include "adushell.hpp"

namespace Adu
{
namespace Shell
{
namespace Tasks
{
namespace FUSApplication
{
/**
* @brief Runs "fs-azure --application_file <path>" command in  a child process.
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
* @brief Runs "fs-azure [--rollback_application]" command in  a child process.
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
ADUShellTaskResult DoFUSApplicationUpdateTask(const ADUShell_LaunchArguments& launchArgs);

} // namespace FUSApplication
} // namespace Tasks
} // namespace Shell
} // namespace Adu

#endif // ADU_SHELL_FUSAPPLICATION_TASKS_HPP

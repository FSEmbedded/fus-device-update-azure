/**
 * @file fsupdate_handler.cpp
 * @brief Implementation of ContentHandler API for fs-updater.
 *
 * Will call into wrapper script for fsupdate to install image files.
 *
 * fus/fsupdate
 * v1:
 *   Description:
 *   Initial revision.
 *
 *   Expected files:
 *   .fsimage - contains fs-update application update image.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */
#include "aduc/fsupdate_application_handler.hpp"

#include "aduc/adu_core_exports.h"
#include "aduc/extension_manager.hpp"
#include "aduc/logging.h"
#include "aduc/process_utils.hpp"
#include "aduc/string_c_utils.h"
#include "aduc/string_utils.hpp"
#include "aduc/system_utils.h"
#include "aduc/types/update_content.h"
#include "aduc/workflow_data_utils.h"
#include "aduc/workflow_utils.h"
#include "adushell_const.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>

#include <dirent.h>

#include <fs_updater_error.h>

namespace adushconst = Adu::Shell::Const;

EXTERN_C_BEGIN
/**
 * @brief Instantiates an Update Content Handler for 'fus/application:1' update type.
 */
ContentHandler* CreateUpdateContentHandlerExtension(ADUC_LOG_SEVERITY logLevel)
{
    ADUC_Logging_Init(logLevel, "fsupdate-application-handler");
    Log_Info("Instantiating an Update Content Handler for 'fus/application:1'");
    try
    {
        return FSUpdateApplicationHandlerImpl::CreateContentHandler();
    }
    catch (const std::exception& e)
    {
        const char* what = e.what();
        Log_Error("Unhandled std exception: %s", what);
    }
    catch (...)
    {
        Log_Error("Unhandled exception");
    }

    return nullptr;
}
EXTERN_C_END

/**
 * @brief Destructor for the FSUpdate Handler Impl class.
 */
FSUpdateApplicationHandlerImpl::~FSUpdateApplicationHandlerImpl() // override
{
    ADUC_Logging_Uninit();
}

/**
 * @brief Creates a new FSUpdateApplicationHandlerImpl object and casts to a ContentHandler.
 * Note that there is no way to create a FSUpdateApplicationHandlerImpl directly.
 *
 * @return ContentHandler* SimulatorHandlerImpl object as a ContentHandler.
 */
ContentHandler* FSUpdateApplicationHandlerImpl::CreateContentHandler()
{
    return new FSUpdateApplicationHandlerImpl();
}

static ADUC_Result HandleFSUpdateRebootState()
{
    ADUC_Result result = { ADUC_Result_Failure };
    std::string command = adushconst::adu_shell;
    std::vector<std::string> args{ adushconst::update_type_opt,
                                adushconst::update_type_fus_firmware,
                                adushconst::update_action_opt,
                                adushconst::update_action_execute,
                                adushconst::target_options_opt,
                                "update_state" };

    Log_Info("Verify current_update_state");

    std::string output;
    result.ExtendedResultCode = ADUC_LaunchChildProcess(command, args, output);

    return result;
}

static ADUC_Result CommitUpdateState()
{
    ADUC_Result result = { ADUC_Result_Failure };
    Log_Info("Applying application update.");

    std::string command = adushconst::adu_shell;
    std::vector<std::string> args{ adushconst::update_type_opt,
                                   adushconst::update_type_fus_application,
                                   adushconst::update_action_opt,
                                   adushconst::update_action_apply };

    std::string output;

    result.ExtendedResultCode = ADUC_LaunchChildProcess(command, args, output);

    return result;
}

/**
 * @brief Performs 'Download' task.
 *
 * @return ADUC_Result The result of the download (always success)
 */
ADUC_Result FSUpdateApplicationHandlerImpl::Download(const tagADUC_WorkflowData* workflowData)
{
    std::stringstream updateFilename;
    ADUC_Result result = { ADUC_Result_Failure };
    ADUC_FileEntity* entity = nullptr;
    ADUC_WorkflowHandle workflowHandle = workflowData->WorkflowHandle;
    char* workflowId = workflow_get_id(workflowHandle);
    char* workFolder = workflow_get_workfolder(workflowHandle);
    int fileCount = 0;

    char* updateType = workflow_get_update_type(workflowHandle);
    char* updateName = nullptr;
    unsigned int updateTypeVersion = 0;
    bool updateTypeOk = ADUC_ParseUpdateType(updateType, &updateName, &updateTypeVersion);

    if (!updateTypeOk)
    {
        Log_Error("FSUpdate packages download failed. Unknown Handler Version (UpdateDateType:%s)", updateType);
        result.ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_DOWNLOAD_FAILURE_UNKNOW_UPDATE_VERSION;
        goto done;
    }

    if (updateTypeVersion != 1)
    {
        Log_Error("FSUpdate packages download failed. Wrong Handler Version %d", updateTypeVersion);
        result.ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_DOWNLOAD_FAILURE_WRONG_UPDATE_VERSION;
        goto done;
    }

    // For 'fus/application:1', we're expecting 1 payload file.
    fileCount = workflow_get_update_files_count(workflowHandle);
    if (fileCount != 1)
    {
        Log_Error("FSUpdate expecting one file. (%d)", fileCount);
        result.ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_DOWNLOAD_FAILURE_WRONG_FILECOUNT;
        goto done;
    }

    if (!workflow_get_update_file(workflowHandle, 0, &entity))
    {
        result.ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_DOWNLOAD_BAD_FILE_ENTITY;
        goto done;
    }

    updateFilename << workFolder << "/" << entity->TargetFilename;

    result = ExtensionManager::Download(entity, workflowId, workFolder, DO_RETRY_TIMEOUT_DEFAULT, nullptr);

done:
    workflow_free_string(workflowId);
    workflow_free_string(workFolder);
    workflow_free_string(updateType);
    workflow_free_file_entity(entity);
    free(updateName);

    return result;
}

/**
 * @brief Install implementation for fsupdate.
 * Calls into the fus-device-update-azure library handler to install an image file.
 *
 * @return ADUC_Result The result of the install.
 */
ADUC_Result FSUpdateApplicationHandlerImpl::Install(const tagADUC_WorkflowData* workflowData)
{
    ADUC_Result result = { ADUC_Result_Failure };
    ADUC_FileEntity* entity = nullptr;
    ADUC_WorkflowHandle workflowHandle = workflowData->WorkflowHandle;
    char* workFolder = workflow_get_workfolder(workflowHandle);

    Log_Info("Installing from %s", workFolder);
    std::unique_ptr<DIR, std::function<int(DIR*)>> directory(
        opendir(workFolder), [](DIR* dirp) -> int { return closedir(dirp); });
    if (directory == nullptr)
    {
        Log_Error("opendir failed, errno = %d", errno);

        result = { .ResultCode = ADUC_Result_Failure,
                   .ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_INSTALL_FAILURE_CANNOT_OPEN_WORKFOLDER };
        goto done;
    }

    if (!workflow_get_update_file(workflowHandle, 0, &entity))
    {
        result.ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_INSTALL_FAILURE_BAD_FILE_ENTITY;
        goto done;
    }

    {
        std::string command = adushconst::adu_shell;
        std::vector<std::string> args{ adushconst::update_type_opt,
                                       adushconst::update_type_fus_application,
                                       adushconst::update_action_opt,
                                       adushconst::update_action_install };

        std::stringstream data;
        data << workFolder << "/" << entity->TargetFilename;

        args.emplace_back(adushconst::target_data_opt);
        args.emplace_back(data.str().c_str());

        Log_Info("Install application image: '%s'", data.str().c_str());

        std::string output;
        const int exitCode = ADUC_LaunchChildProcess(command, args, output);

        if (exitCode != static_cast<int>(AZURE_APPLICATION_STATE::UPDATE_SUCCESSFUL))
        {
            Log_Error("Install application failed, extendedResultCode = %d", exitCode);

            result = CommitUpdateState();
            if (result.ExtendedResultCode == static_cast<int>(AZURE_COMMIT_STATE::SUCCESSFUL))
            {
                Log_Info("Commit of failed application update.");
                result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_INSTALL_FAILURE_APPLICATION_UPDATE };
            }
            else
            {
                Log_Error("Failed to commit missing application update.");
                result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_INSTALL_FAILURE_COMMIT_UPDATE };
            }
        }
    }

    Log_Info("Install succeeded");
    // Always require a reboot after successful install
    result = { ADUC_Result_Install_RequiredImmediateReboot };

done:
    workflow_free_string(workFolder);
    workflow_free_file_entity(entity);

    return result;
}

/**
 * @brief Apply implementation for fsupdate.
 * Calls into the fsupdate wrapper script to perform apply.
 * Will flip bootloader flag to boot into update partition for A/B update.
 *
 * @return ADUC_Result The result of the apply.
 */
ADUC_Result FSUpdateApplicationHandlerImpl::Apply(const tagADUC_WorkflowData* workflowData)
{
    ADUC_Result result = { ADUC_Result_Failure };
    result = CommitUpdateState();

    if (result.ExtendedResultCode == static_cast<int>(AZURE_COMMIT_STATE::SUCCESSFUL))
    {
        result  = HandleFSUpdateRebootState();

        if (result.ExtendedResultCode == static_cast<int>(AZURE_UPDATE_REBOOT_STATE::INCOMPLETE_APP_UPDATE))
        {
            Log_Info("Incomplete application update; reboot is mandatory");
            result = { ADUC_Result_Apply_RequiredImmediateReboot };
        }
        else if (result.ExtendedResultCode == static_cast<int>(AZURE_UPDATE_REBOOT_STATE::NO_UPDATE_REBOOT_PENDING))
        {
            Log_Info("Application update is installed");
            result = { ADUC_Result_Apply_Success };
        }
        else
        {
            Log_Error("Unknown error during retrieving current update state.");
            result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_APPLY_FAILURE_UNKNOWN_ERROR };
        }
    }
    else if (result.ExtendedResultCode == static_cast<int>(AZURE_COMMIT_STATE::UPDATE_NOT_NEEDED))
    {
        Log_Info("Apply not needed.");
        result = { ADUC_Result_Apply_Success };
    }
    else if (result.ExtendedResultCode == static_cast<int>(AZURE_COMMIT_STATE::UPDATE_SYSTEM_ERROR))
    {
        Log_Info("Missing reboot.");
        result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_APPLY_FAILURE_UPDATE_SYSTEM_ERROR };
    }
    else
    {
        Log_Error("Unknown error during apply phase, extendedResultCode = %d", result.ExtendedResultCode);
        result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_APPLY_FAILURE_UNKNOWN_ERROR };
    }

    return result;
}

/**
 * @brief Cancel implementation for fsupdate.
 * We don't have already implemented posibility to revert a ongoing update.
 * The functionality is implemented but not tested.
 * Cancel after or during any other operation is a no-op.
 *
 * @return ADUC_Result The result of the cancel.
 */
ADUC_Result FSUpdateApplicationHandlerImpl::Cancel(const tagADUC_WorkflowData* workflowData)
{
    UNREFERENCED_PARAMETER(workflowData);
    return ADUC_Result{ ADUC_Result_Cancel_Success };
}


/**
 * @brief Checks if the installed content matches the installed criteria.
 *
 * @param installedCriteria The installed criteria string. e.g. The application version.
 *  installedCriteria has already been checked to be non-empty before this call.
 *
 * @return ADUC_Result
 */
ADUC_Result FSUpdateApplicationHandlerImpl::IsInstalled(const tagADUC_WorkflowData* workflowData)
{
    char* installedCriteria = ADUC_WorkflowData_GetInstalledCriteria(workflowData);
    ADUC_Result result;

    const std::string command("/usr/sbin/fs-azure");
    std::vector<std::string> args {"--application_version"};
    std::string output;

    const int exitCode = ADUC_LaunchChildProcess(command, args, output);

    if (exitCode != 0)
    {
        Log_Error("IsInstalled failed, extendedResultCode = %d", exitCode);
        result = { ADUC_Result_Failure, exitCode };
        goto done;
    }

    if (output.empty())
    {
        Log_Error("Version of fs-azure could not be read.");
        result = { ADUC_Result_Failure };
        goto done;
    }

    output.erase(output.end()-1);
    if (output.compare(installedCriteria) == 0)
    {
        Log_Info("Expected and installed application version are the same: '%s'", installedCriteria);
        result = HandleFSUpdateRebootState();

        if (result.ExtendedResultCode == static_cast<int>(AZURE_UPDATE_REBOOT_STATE::INCOMPLETE_APP_UPDATE))
        {
            Log_Info("Incomplete application update; apply is mandatory");
            result = { ADUC_Result_IsInstalled_MissingCommit };
        }
        else if (result.ExtendedResultCode == static_cast<int>(AZURE_UPDATE_REBOOT_STATE::INCOMPLETE_FW_UPDATE))
        {
            Log_Info("Incomplete firmware update; apply is mandatory");
            result = { ADUC_Result_IsInstalled_MissingCommit };
        }
        else if (result.ExtendedResultCode == static_cast<int>(AZURE_UPDATE_REBOOT_STATE::NO_UPDATE_REBOOT_PENDING))
        {
            Log_Info("Application update is already installed, expected version matches with current installed: '%s'", installedCriteria);
            result = { ADUC_Result_IsInstalled_Installed };
        }
        else
        {
            Log_Error("Unknown error during retrieving current update state.");
            result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_ISINSTALLED_FAILURE_UNKNOWN_STATE };
        }
        goto done;
    }

    result = HandleFSUpdateRebootState();

    if (result.ExtendedResultCode == static_cast<int>(AZURE_UPDATE_REBOOT_STATE::FAILED_APP_UPDATE))
    {
        Log_Info("IsInstall based of failed application update successful -> commit failed update.");
        result = CommitUpdateState();

        if (result.ExtendedResultCode == static_cast<int>(AZURE_COMMIT_STATE::SUCCESSFUL))
        {
            Log_Info("Commit of failed application update.");
            result = { ADUC_Result_IsInstalled_Installed };
        }
        else
        {
            Log_Error("Failed to commit missing application update.");
            result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_ISINSTALLED_FAILURE_COMMIT_PREVIOUS_FAILED_UPDATE };
        }
    }
    else if (result.ExtendedResultCode == static_cast<int>(AZURE_UPDATE_REBOOT_STATE::FAILED_FW_UPDATE))
    {
        Log_Info("IsInstall based of failed firmware update successful -> commit failed update.");
        result = CommitUpdateState();

        if (result.ExtendedResultCode == static_cast<int>(AZURE_COMMIT_STATE::SUCCESSFUL))
        {
            Log_Info("Commit of failed firmware update.");
            result = { ADUC_Result_IsInstalled_Installed };
        }
        else
        {
            Log_Error("Failed to commit missing firmware update.");
            result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_ISINSTALLED_FAILURE_COMMIT_PREVIOUS_FAILED_UPDATE };
        }
    }
    else if (result.ExtendedResultCode == static_cast<int>(AZURE_UPDATE_REBOOT_STATE::FW_UPDATE_REBOOT_FAILED))
    {
        Log_Info("Failed firmware update reboot");
        result = { ADUC_Result_IsInstalled_Installed };
    }

    Log_Info("Installed criteria %s was not satisfied, the current version is %s", installedCriteria, output.c_str());

    result = { ADUC_Result_IsInstalled_NotInstalled };

done:
    workflow_free_string(installedCriteria);
    return result;
}

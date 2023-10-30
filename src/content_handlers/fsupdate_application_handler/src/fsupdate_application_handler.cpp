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
#include <azure_c_shared_utility/threadapi.h> // ThreadAPI_Sleep

#include <algorithm>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>

#include <dirent.h>

#include <fs_updater_error.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

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
                                adushconst::update_type_fus_application,
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
    {
        char* installedCriteria = ADUC_WorkflowData_GetInstalledCriteria(workflowData);
        char* updateType = ADUC_WorkflowData_GetUpdateType(workflowData);
        int updateSize = ADUC_WorkflowData_GetUpdateSize(workflowData);

        mkdir("/tmp/adu/.work", 0777);

        int fd = open("/tmp/adu/.work/application_version", O_RDWR | O_CREAT, S_IRUSR | S_IRGRP | S_IROTH);
        if (write(fd, installedCriteria, strlen(installedCriteria)) < 0)
            Log_Info("Error, could not create server version stamp");
        close(fd);

		fd = open("/tmp/adu/.work/application_type", O_RDWR | O_CREAT, S_IRUSR | S_IRGRP | S_IROTH);
		if (write(fd, updateType, strlen(updateType)) < 0)
			Log_Info("Error, could not create server version stamp");
		close(fd);

		int fd2 = open("/tmp/adu/.work/application_size", O_RDWR | O_CREAT, S_IRUSR | S_IRGRP | S_IROTH);
		FILE *fd3 = fdopen(fd2, "r+");
		if (fprintf(fd3, "%d", updateSize) < 0)
			Log_Info("Error, could not create server size stamp");
		fflush(fd3);
		fclose(fd3);

		while(access("/tmp/adu/.work/downloadApplication", F_OK) < 0)
		{
			ThreadAPI_Sleep(100);
		}

		fd = open("/tmp/adu/.work/application_location", O_RDWR | O_CREAT, S_IRUSR | S_IRGRP | S_IROTH);
		if(write(fd, updateFilename.str().c_str(), strlen(updateFilename.str().c_str())));
			Log_Info("Error, could not create download location stamp");
		close(fd);
	}

	//--------------------------------------------------

    Log_Info("Download file firmware update file to download '%s'", updateFilename.str().c_str());

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

        Log_Info("Application image can be installed: '%s'", data.str().c_str());

        while(access("/tmp/adu/.work/installApplication", F_OK) < 0)
        {
            ThreadAPI_Sleep(100);
            Log_Debug("Waiting for install command");
        }

        Log_Info("Install application image: '%s'", data.str().c_str());

        std::string output;
        const int exitCode = ADUC_LaunchChildProcess(command, args, output);

        if (exitCode != static_cast<int>(UPDATER_APPLICATION_STATE::UPDATE_SUCCESSFUL))
        {
            Log_Error("Install application failed, extendedResultCode = %d", exitCode);

            result = CommitUpdateState();
            if (result.ExtendedResultCode == static_cast<int>(UPDATER_COMMIT_STATE::SUCCESSFUL))
            {
                Log_Info("Commit of failed application update.");
                result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_INSTALL_FAILURE_APPLICATION_UPDATE };
            }
            else
            {
                Log_Error("Failed to commit missing application update.");
                result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_INSTALL_FAILURE_COMMIT_UPDATE };
            }
        } else {
            /* create applicationInstalled state file */
            int fd = open("/tmp/adu/.work/applicationInstalled", O_RDONLY | O_CREAT, S_IRUSR | S_IRGRP | S_IROTH);
            if(fd < 0) {
                Log_Info("Create file for state application installed fails.");
            }
            close(fd);
        }
    }

    Log_Info("Install succeeded");
    result = { ADUC_Result_Install_Success};

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
    ADUC_Result result;

    result = HandleFSUpdateRebootState();

    switch (result.ExtendedResultCode)
    {
    case static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::UPDATE_REBOOT_PENDING):

        while (access("/tmp/adu/.work/applyApplication", F_OK) < 0)
        {
            ThreadAPI_Sleep(100);
        }

        workflow_request_immediate_reboot(workflowData->WorkflowHandle);
        result = { ADUC_Result_Apply_RequiredImmediateReboot };
        break;
    case static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::INCOMPLETE_APP_UPDATE):
        Log_Info("Incomplete application update; reboot is mandatory");
        while (access("/tmp/adu/.work/applyApplication", F_OK) < 0)
        {
            ThreadAPI_Sleep(100);
        }

        workflow_request_immediate_reboot(workflowData->WorkflowHandle);
        result = { ADUC_Result_Apply_RequiredImmediateReboot };
        break;

    case static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::NO_UPDATE_REBOOT_PENDING):
        Log_Info("Application update is installed");
        result = { ADUC_Result_Apply_Success };
        break;
    case static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::ROLLBACK_APP_REBOOT_PENDING):
        Log_Info("Incomplete application rollback; need reboot");
        workflow_request_immediate_reboot(workflowData->WorkflowHandle);
        result = { ADUC_Result_Apply_RequiredImmediateReboot };
        break;
    default:
        Log_Error("Unknown error during retrieving current application update state.");
        result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_APPLY_FAILURE_UNKNOWN_ERROR };
        break;
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
    ADUC_Result result = { ADUC_Result_Failure };

    result  = HandleFSUpdateRebootState();
    if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::INCOMPLETE_APP_UPDATE))
    {
        Log_Info("Incomplete application update -> proceed rollback");

        std::string command = adushconst::adu_shell;
        std::vector<std::string> args{ adushconst::update_type_opt,
                                    adushconst::update_type_fus_application,
                                    adushconst::update_action_opt,
                                    adushconst::update_action_cancel };

        std::string output;

        result.ExtendedResultCode = ADUC_LaunchChildProcess(command, args, output);

        if (result.ExtendedResultCode != static_cast<int>(UPDATER_APPLICATION_STATE::ROLLBACK_SUCCESSFUL))
        {
            std::string error_msg = "Rollback application failed: ";
            error_msg += std::to_string(result.ExtendedResultCode);
            Log_Error(error_msg.c_str());

            result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_CANCEL_ROLLBACK_APPLICATION_ERROR };
            return result;
        }

        result  = HandleFSUpdateRebootState();
        if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::ROLLBACK_APP_REBOOT_PENDING))
        {
            Log_Info("Incomplete application rollback update -> proceed reboot");
            workflow_request_immediate_reboot(workflowData->WorkflowHandle);
            result = { ADUC_Result_Cancel_RequiredImmediateReboot };
        }
        else if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::NO_UPDATE_REBOOT_PENDING))
        {
            Log_Info("Complete application rollback update");
            result = { ADUC_Result_Cancel_Success };
        }
        else
        {
            Log_Error("No permitted rollback state");
            result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_CANCEL_NOT_ALLOWED_STATE_ERROR };
        }

    }
    else if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::ROLLBACK_APP_REBOOT_PENDING))
    {
        Log_Info("Incomplete application rollback update -> reboot processed");
        result = CommitUpdateState();

        if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::NO_UPDATE_REBOOT_PENDING))
        {
            Log_Info("Reboot of application update processed -> commited");
            result = { ADUC_Result_Cancel_Success };
        }
        else
        {
            Log_Error("Reboot of cancelled application not successed processed");
            result = { ADUC_Result_Cancel_Success, ADUC_ERC_FSUPDATE_HANDLER_CANCEL_NOT_ALLOWED_STATE_ERROR };
        }
    }
    else if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::NO_UPDATE_REBOOT_PENDING))
    {
        Log_Info("No cancel is possible update already installed");
        result = { ADUC_Result_Failure_Cancelled };
    }
    else
    {
        Log_Error("Unknown error during retrieving current update state.");
        result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_CANCEL_NOT_ALLOWED_STATE_ERROR };
    }
    return result;
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

    const std::string command(UPDATER_CLI_FULL_CMD);
    std::vector<std::string> args {"--application_version"};
    std::string output;
    std::string special_chars = "\n\t";

    const int exitCode = ADUC_LaunchChildProcess(command, args, output);

    if (exitCode != 0)
    {
        Log_Error("IsInstalled failed, extendedResultCode = %d", exitCode);
        result = { ADUC_Result_Failure, exitCode };
        goto done;
    }

    if (output.empty())
    {
        Log_Error("Version of updater command could not be read.");
        result = { ADUC_Result_Failure };
        goto done;
    }

    /* remove special character like word wrap not */
    for (char c: special_chars) {
        output.erase(std::remove(output.begin(), output.end(), c), output.end());
    }

    if (output.compare(installedCriteria) == 0)
    {
        Log_Info("Expected and installed application version are the same: '%s'", installedCriteria);
        result = HandleFSUpdateRebootState();

        if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::INCOMPLETE_APP_UPDATE))
        {
            Log_Info("Incomplete application update; apply is mandatory");
            result = { ADUC_Result_IsInstalled_MissingCommit };
        }
        else if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::INCOMPLETE_FW_UPDATE))
        {
            Log_Info("Incomplete firmware update; apply is mandatory");
            result = { ADUC_Result_IsInstalled_MissingCommit };
        }
        else if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::NO_UPDATE_REBOOT_PENDING))
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

    if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::FAILED_APP_UPDATE))
    {
        Log_Info("IsInstall based of failed application update successful -> commit failed update.");
        result = CommitUpdateState();

        if (result.ExtendedResultCode == static_cast<int>(UPDATER_COMMIT_STATE::SUCCESSFUL))
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
    else if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::FAILED_FW_UPDATE))
    {
        Log_Info("IsInstall based of failed firmware update successful -> commit failed update.");
        result = CommitUpdateState();

        if (result.ExtendedResultCode == static_cast<int>(UPDATER_COMMIT_STATE::SUCCESSFUL))
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
    else if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::FW_UPDATE_REBOOT_FAILED))
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

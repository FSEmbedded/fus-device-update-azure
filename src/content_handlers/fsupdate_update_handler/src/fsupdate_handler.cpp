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
 *   .fsimage - contains fs-update firmware update image.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */
#include "aduc/fsupdate_handler.hpp"

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

#define HANDLER_PROPERTIES_UPDATE_TYPE "updateType"

/* if not defined in configuration file set to default value */
#ifndef TEMP_ADU_WORK_DIR
#define TEMP_ADU_WORK_DIR "/tmp/adu/.work"
#endif

namespace adushconst = Adu::Shell::Const;

EXTERN_C_BEGIN
/**
 * @brief Instantiates an Update Content Handler for 'fus/update:1' update type.
 */
ContentHandler* CreateUpdateContentHandlerExtension(ADUC_LOG_SEVERITY logLevel)
{
    ADUC_Logging_Init(logLevel, "fsupdate-handler");
    Log_Info("Instantiating an Update Content Handler for 'fus/update:1'");
    try
    {
        return FSUpdateHandlerImpl::CreateContentHandler();
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

FSUpdateHandlerImpl::FSUpdateHandlerImpl() :
    work_dir(TEMP_ADU_WORK_DIR), work_dir_perms(std::filesystem::perms::all)
{
}

/**
 * @brief Destructor for the FSUpdate Handler Impl class.
 */
FSUpdateHandlerImpl::~FSUpdateHandlerImpl() // override
{
    ADUC_Logging_Uninit();
}

/**
 * @brief Creates a new FSUpdateHandlerImpl object and casts to a ContentHandler.
 * Note that there is no way to create a FSUpdateHandlerImpl directly.
 *
 * @return ContentHandler* SimulatorHandlerImpl object as a ContentHandler.
 */
ContentHandler* FSUpdateHandlerImpl::CreateContentHandler()
{
    return new FSUpdateHandlerImpl();
}

update_type_t FSUpdateHandlerImpl::getUpdateType()
{
    return this->update_type;
}
void FSUpdateHandlerImpl::setUpdateType(update_type_t up_type)
{
    this->update_type = up_type;
}

update_type_t FSUpdateHandlerImpl::getUpdateType(std::string & updateTypeName)
{
    update_type_t up_type;

    if (updateTypeName.compare("firmware") == 0)
    {
        up_type = UPDATE_FIRMWARE;
    }
    else if (updateTypeName.compare("application") == 0)
    {
        up_type = UPDATE_APPLICATION;
    }
    else if (updateTypeName.compare("both") == 0)
    {
         up_type = UPDATE_COMMON;
    }
    else
    {
        up_type = UPDATE_UNKNOWN;
    }

    return up_type;

}

bool FSUpdateHandlerImpl::create_work_dir()
{
    std::string msg(work_dir);

    if (std::filesystem::exists(work_dir))
    {
        msg += " does exist.";
        Log_Debug("FSUpdate %s", msg.c_str());
        /* remove all */
        std::filesystem::remove_all(work_dir);
    }

    try
    {
        std::filesystem::create_directory(work_dir);
        std::filesystem::permissions(work_dir, work_dir_perms, std::filesystem::perm_options::replace);
    }
    catch (std::filesystem::filesystem_error const& ex)
    {
        Log_Warn("FSUpdate %s", ex.what());
    }
    msg += " created.";
    Log_Debug("FSUpdate %s", msg.c_str());
    return true;
}

static ADUC_Result HandleFSUpdateRebootState()
{
    ADUC_Result result = { ADUC_Result_Failure };
    std::string command = adushconst::adu_shell;
    std::vector<std::string> args{ adushconst::update_type_opt,
                                   adushconst::update_type_fus_update,
                                   adushconst::update_action_opt,
                                   adushconst::update_action_execute,
                                   adushconst::target_options_opt,
                                   "update_state" };

    Log_Info("Verify current_update_state");

    std::string output;
    result.ExtendedResultCode = ADUC_LaunchChildProcess(command, args, output);

    return result;
}

static ADUC_Result CommitUpdateState(const char* update_type)
{
    ADUC_Result result = { ADUC_Result_Failure };
    Log_Info("Applying update.");

    std::string command = adushconst::adu_shell;
    std::vector<std::string> args{ adushconst::update_type_opt,
                                   update_type,
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
ADUC_Result FSUpdateHandlerImpl::Download(const tagADUC_WorkflowData* workflowData)
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

    // For 'fus/update:1', we're expecting 1 payload file.
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
        const char* updateType = this->update_type_names[this->update_type];
        int updateSize = ADUC_WorkflowData_GetUpdateSize(workflowData);

        this->create_work_dir();
        std::filesystem::path file_path = (work_dir / "update_version");
        std::ofstream update_version(file_path);
        if (!update_version.is_open())
        {
            Log_Error("Could not create %s.", file_path.string().c_str());
            result = { .ResultCode = ADUC_Result_Failure,
                   .ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_DOWNLOAD_FAILURE_CREATE_FAILED_UPDATE_VERSION };
            goto done;
        }
        update_version << installedCriteria;
        update_version.close();

        std::filesystem::permissions(
            file_path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
                | std::filesystem::perms::group_read | std::filesystem::perms::others_read,
            std::filesystem::perm_options::replace);

        file_path = work_dir / "update_type";
        std::ofstream update_type(file_path);
        if (!update_type.is_open())
        {
            Log_Error("Could not create %s.", file_path.string().c_str());
            result = { .ResultCode = ADUC_Result_Failure,
                   .ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_DOWNLOAD_FAILURE_CREATE_FAILED_UPDATE_TYPE };
            goto done;
        }
        update_type << updateType;
        update_type.close();
        std::filesystem::permissions(
            file_path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
                | std::filesystem::perms::group_read | std::filesystem::perms::others_read,
            std::filesystem::perm_options::replace);

        file_path = work_dir / "update_size";
        std::ofstream update_size(file_path);
        if (!update_size.is_open())
        {
            result = { .ResultCode = ADUC_Result_Failure,
                   .ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_DOWNLOAD_FAILURE_CREATE_FAILED_UPDATE_SIZE };
            Log_Error("Could not create %s.", file_path.string().c_str());
            goto done;
        }
        update_size << updateSize;
        update_size.close();
        std::filesystem::permissions(
            file_path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
                | std::filesystem::perms::group_read | std::filesystem::perms::others_read,
            std::filesystem::perm_options::replace);

        while (std::filesystem::exists(work_dir / "downloadUpdate") == false)
        {
            ThreadAPI_Sleep(100);
        }

        file_path = work_dir / "update_location";
        std::ofstream update_location(file_path);
        if (!update_location.is_open())
        {
            result = { .ResultCode = ADUC_Result_Failure,
                   .ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_DOWNLOAD_FAILURE_CREATE_FAILED_UPDATE_LOCATION };
            Log_Error("Could not create %s.", file_path.string().c_str());
            goto done;
        }

        update_location << updateFilename.str().c_str();
        update_location.close();
        std::filesystem::permissions(
            file_path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
                | std::filesystem::perms::group_read | std::filesystem::perms::others_read,
            std::filesystem::perm_options::replace);
    }

    //--------------------------------------------------

    Log_Info("Download file update file to download '%s'", updateFilename.str().c_str());

    result = ExtensionManager::Download(entity, workflowId, workFolder, DO_RETRY_TIMEOUT_DEFAULT, nullptr);

    Log_Info("Download result code: '%d' and extended result code '%d'", result.ResultCode, result.ExtendedResultCode);

done:
    workflow_free_string(workflowId);
    workflow_free_string(workFolder);
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
ADUC_Result FSUpdateHandlerImpl::Install(const tagADUC_WorkflowData* workflowData)
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
                                       adushconst::update_type_fus_update,
                                       adushconst::update_action_opt,
                                       adushconst::update_action_install };

        std::stringstream data;
        data << workFolder << "/" << entity->TargetFilename;
        args.emplace_back(adushconst::target_data_opt);
        args.emplace_back(data.str().c_str());

        while (std::filesystem::exists(work_dir / "installUpdate") == false)
        {
            ThreadAPI_Sleep(100);
            Log_Debug("Waiting for install command");
        }

        Log_Info("Install update image: '%s'", data.str().c_str());

        std::string output;
        const int exitCode = ADUC_LaunchChildProcess(command, args, output);

        if ((exitCode == static_cast<int>(UPDATER_FIRMWARE_STATE::UPDATE_SUCCESSFUL))
            || (exitCode == static_cast<int>(UPDATER_APPLICATION_STATE::UPDATE_SUCCESSFUL))
            || (exitCode == static_cast<int>(UPDATER_FIRMWARE_AND_APPLICATION_STATE::UPDATE_SUCCESSFUL)))
        {
            Log_Info("Install succeeded");
            result = { ADUC_Result_Install_Success };
        }
        else
        {
            Log_Error("Install failed, extendedResultCode = %d", exitCode);
            if (getUpdateType() == UPDATE_FIRMWARE)
            {
                result = { .ResultCode = ADUC_Result_Failure,
                           .ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_INSTALL_FAILURE_FIRMWARE_UPDATE };
            }
            else if (getUpdateType() == UPDATE_APPLICATION)
            {
                result = { .ResultCode = ADUC_Result_Failure,
                           .ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_INSTALL_FAILURE_APPLICATION_UPDATE };
            }
            else if (getUpdateType() == UPDATE_COMMON)
            {
                result = { .ResultCode = ADUC_Result_Failure,
                           .ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_INSTALL_FAILURE_BAD_FILE_ENTITY };
            }
            else
            {
                /* update type is unkown */
                result = { .ResultCode = ADUC_Result_Failure,
                           .ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_INSTALL_FAILURE_BAD_FILE_ENTITY };
            }
        }
    }

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
ADUC_Result FSUpdateHandlerImpl::Apply(const tagADUC_WorkflowData* workflowData)
{
    ADUC_Result result;

    result = HandleFSUpdateRebootState();

    switch (result.ExtendedResultCode)
    {
    case static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::UPDATE_REBOOT_PENDING):
        Log_Debug("Update reboot pending; wait for apply.");
        while (std::filesystem::exists(work_dir / "applyUpdate") == false)
        {
            ThreadAPI_Sleep(100);
        }

        workflow_request_immediate_reboot(workflowData->WorkflowHandle);
        result = { ADUC_Result_Apply_RequiredImmediateReboot };
        break;

    case static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::INCOMPLETE_FW_UPDATE):
    case static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::INCOMPLETE_APP_UPDATE):
    case static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::INCOMPLETE_APP_FW_UPDATE):
        Log_Debug("Incomplete update; commit is mandatory");

        while (std::filesystem::exists(work_dir / "applyUpdate") == false)
        {
            ThreadAPI_Sleep(100);
        }
        break;

    case static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::NO_UPDATE_REBOOT_PENDING):
        Log_Debug("Update is installed");
        result = { ADUC_Result_Apply_Success };
        break;

    case static_cast<int>(UPDATER_COMMIT_STATE::UPDATE_NOT_NEEDED):
        Log_Debug("Apply not needed.");
        result = { ADUC_Result_Apply_Success };
        break;

    default:
        Log_Error("Unknown error during retrieving current firmware update state.");
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
ADUC_Result FSUpdateHandlerImpl::Cancel(const tagADUC_WorkflowData* workflowData)
{
    ADUC_Result result = { ADUC_Result_Failure };

    result = HandleFSUpdateRebootState();
    if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::INCOMPLETE_APP_UPDATE))
    {
        Log_Info("Incomplete application update -> proceed rollback");

        std::string command = adushconst::adu_shell;
        std::vector<std::string> args{ adushconst::update_type_opt,
                                    adushconst::update_type_fus_update,
                                    adushconst::update_action_opt,
                                    adushconst::update_action_cancel };

        std::string output;

        result.ExtendedResultCode = ADUC_LaunchChildProcess(command, args, output);

        if (result.ExtendedResultCode != static_cast<int>(UPDATER_UPDATE_ROLLBACK_STATE::UPDATE_ROLLBACK_SUCCESSFUL))
        {
            std::string error_msg = "Rollback firmware failed: ";
            error_msg += std::to_string(result.ExtendedResultCode);
            Log_Error(error_msg.c_str());

            result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_CANCEL_ROLLBACK_FIRMWARE_ERROR };
            return result;
        }

        result  = HandleFSUpdateRebootState();
        if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::ROLLBACK_FW_REBOOT_PENDING))
        {
            Log_Info("Incomplete firmware rollback update -> proceed reboot");
            workflow_request_immediate_reboot(workflowData->WorkflowHandle);
            result = { ADUC_Result_Cancel_RequiredImmediateReboot };
        }
        else if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::NO_UPDATE_REBOOT_PENDING))
        {
            Log_Info("Complete firmware rollback update");
            result = { ADUC_Result_Cancel_Success };
        }
        else
        {
            Log_Error("No permitted rollback state");
            result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_CANCEL_NOT_ALLOWED_STATE_ERROR };
        }

    }
    else if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::ROLLBACK_FW_REBOOT_PENDING))
    {
        Log_Info("Incomplete firmware rollback update -> reboot processed");
        result = CommitUpdateState(adushconst::update_type_fus_firmware);

        if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::NO_UPDATE_REBOOT_PENDING))
        {
            Log_Info("Reboot of firmware update processed -> commited");
            result = { ADUC_Result_Cancel_Success };
        }
        else
        {
            Log_Error("Reboot of cancelled firmware not successed processed");
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
 * @param installedCriteria The installed criteria string. e.g. The firmware version.
 *  installedCriteria has already been checked to be non-empty before this call.
 *
 * @return ADUC_Result
 */
ADUC_Result FSUpdateHandlerImpl::IsInstalled(const tagADUC_WorkflowData* workflowData)
{
    char* installedCriteria = ADUC_WorkflowData_GetInstalledCriteria(workflowData);
    ADUC_Result result;
    ADUC_WorkflowHandle workflowHandle = workflowData->WorkflowHandle;
    bool both_versions = false;
    std::string cmd_output;
    std::string special_chars = "\n\t";
    int exitCode = 0;
    update_type_t up_type;
    const std::string command(UPDATER_CLI_FULL_CMD);
    std::vector<std::string> args(1);

    // read update type from handler properties node.
    std::string update_type_name =
        workflow_peek_update_manifest_handler_properties_string(workflowHandle, HANDLER_PROPERTIES_UPDATE_TYPE);

    up_type = this->getUpdateType(update_type_name);

    Log_Info("IsInstalled  update_type_name = %s", update_type_name.c_str());

    args[0] = "--firmware_version";
    if (up_type == UPDATE_APPLICATION)
    {
        args[0] = "--application_version";
    }
    else if (up_type == UPDATE_UNKNOWN)
    {
        Log_Error("IsInstalled failed, %s is wrong update type.", update_type_name.c_str());
        result = { ADUC_Result_Failure,
                   static_cast<int>(UPDATER_FIRMWARE_AND_APPLICATION_STATE::UPDATE_INTERNAL_ERROR) };
        goto done;
    }

    exitCode = ADUC_LaunchChildProcess(command, args, cmd_output);

    if (exitCode != 0)
    {
        Log_Error("IsInstalled failed, extendedResultCode = %d", exitCode);
        result = { ADUC_Result_Failure, exitCode };
        goto done;
    }

    if (cmd_output.empty())
    {
        Log_Error("Version of updater command could not be read.");
        result = { ADUC_Result_Failure };
        goto done;
    }

    /* remove special character like word wrap not */
    for (char c : special_chars)
    {
        cmd_output.erase(std::remove(cmd_output.begin(), cmd_output.end(), c), cmd_output.end());
    }

    Log_Info(
        "Compare %s version %s and installedCriteria %s",
        update_type_name.c_str(),
        cmd_output.c_str(),
        installedCriteria);

    if (cmd_output.compare(installedCriteria) == 0)
    {
        result = HandleFSUpdateRebootState();

        if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::INCOMPLETE_APP_FW_UPDATE))
        {
            Log_Info("Incomplete firmware and ...");
            result = { ADUC_Result_IsInstalled_MissingCommit };
        }
        else if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::NO_UPDATE_REBOOT_PENDING))
        {
            std::string updatename("Firmware");

            if (up_type == UPDATE_APPLICATION)
                updatename = "Application";

            Log_Info(
                "%s update is already installed, expected version matches with current installed: '%s'",
                updatename.c_str(),
                installedCriteria);
            result = { ADUC_Result_IsInstalled_Installed };
            /* In case of common update application update state need to be checked too. */
            if (up_type != UPDATE_COMMON)
            {
                goto done;
            }
        }
        else
        {
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
            else
            {
                Log_Error("Unknown error during retrieving current update state.");
                result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_ISINSTALLED_FAILURE_UNKNOWN_STATE };
            }
            goto done;
        }
    }

    if (up_type == UPDATE_COMMON)
    {
        /* In case of common update, application update has to be checked too. */
        args[0] = "--application_version";
        /* clear old value of firmware_version */
        cmd_output.clear();
        exitCode = ADUC_LaunchChildProcess(command, args, cmd_output);

        if (exitCode != 0)
        {
            Log_Error("IsInstalled failed, extendedResultCode = %d", exitCode);
            result = { ADUC_Result_Failure, exitCode };
            goto done;
        }

        if (cmd_output.empty())
        {
            Log_Error("Version of updater command could not be read.");
            result = { ADUC_Result_Failure };
            goto done;
        }

        /* remove special character like word wrap not */
        for (char c : special_chars)
        {
            cmd_output.erase(std::remove(cmd_output.begin(), cmd_output.end(), c), cmd_output.end());
        }

        if (cmd_output.compare(installedCriteria) == 0)
        {
            result = HandleFSUpdateRebootState();

            if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::INCOMPLETE_APP_UPDATE))
            {
                Log_Info("Incomplete application update; apply is mandatory");
                result = { ADUC_Result_IsInstalled_MissingCommit };
            }
            else if (
                result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::INCOMPLETE_APP_FW_UPDATE))
            {
                Log_Info("... application update; apply is mandatory");
                result = { ADUC_Result_IsInstalled_MissingCommit };
            }
            else if (
                result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::NO_UPDATE_REBOOT_PENDING))
            {
                Log_Info(
                    " Application update is already installed, expected version matches with current installed: '%s'",
                    installedCriteria);
                result = { ADUC_Result_IsInstalled_Installed };
            }
            else
            {
                Log_Error("Unknown error during retrieving current update state.");
                result = { ADUC_Result_Failure, ADUC_ERC_FSUPDATE_HANDLER_ISINSTALLED_FAILURE_UNKNOWN_STATE };
            }
            goto done;
        }
    }

    result = HandleFSUpdateRebootState();

    if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::FAILED_APP_UPDATE))
    {
        Log_Info("IsInstalled based of failed application update successful -> commit failed update.");
        result = CommitUpdateState(adushconst::update_type_fus_application);

        if (result.ExtendedResultCode == static_cast<int>(UPDATER_COMMIT_STATE::UPDATE_COMMIT_SUCCESSFUL))
        {
            Log_Info("Commit of failed application update.");
            result = { ADUC_Result_IsInstalled_Installed };
        }
        else
        {
            Log_Error("Failed to commit missing application update.");
            result = { ADUC_Result_Failure,
                       ADUC_ERC_FSUPDATE_HANDLER_ISINSTALLED_FAILURE_COMMIT_PREVIOUS_FAILED_UPDATE };
        }
        goto done;
    }
    else if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::FAILED_FW_UPDATE))
    {
        Log_Info("IsInstalled based of failed firmware update successful -> commit failed update.");
        result = CommitUpdateState(adushconst::update_type_fus_firmware);

        if (result.ExtendedResultCode == static_cast<int>(UPDATER_COMMIT_STATE::UPDATE_COMMIT_SUCCESSFUL))
        {
            Log_Info("Commit of failed firmware update.");
            result = { ADUC_Result_IsInstalled_Installed };
        }
        else
        {
            Log_Error("Failed to commit missing firmware update.");
            result = { ADUC_Result_Failure,
                       ADUC_ERC_FSUPDATE_HANDLER_ISINSTALLED_FAILURE_COMMIT_PREVIOUS_FAILED_UPDATE };
        }
        goto done;
    }
    else if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::FW_UPDATE_REBOOT_FAILED))
    {
        Log_Info("Failed update reboot");
        result = { ADUC_Result_IsInstalled_Installed };
        goto done;
    }

    Log_Info(
        "Installed criteria %s was not satisfied, the current version is %s", installedCriteria, cmd_output.c_str());

    setUpdateType(up_type);

    result = { ADUC_Result_IsInstalled_NotInstalled };

done:
    workflow_free_string(installedCriteria);
    return result;
}

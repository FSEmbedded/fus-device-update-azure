/**
 * @file fsupdate_handler.cpp
 * @brief Implementation of ContentHandler API for fs-updater.
 *
 * Will call into wrapper script for fsupdate to install image files.
 *
 * fus/update
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
#include "aduc/fsupdate_result.h" /* fsupdate result codes */

#include "aduc/adu_core_exports.h"
#include "aduc/config_utils.h"
#include "aduc/extension_manager.hpp"
#include "aduc/logging.h"
#include "aduc/parser_utils.h" // ADUC_FileEntity_Uninit
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
#define UPDATE_TYPE_APP "app"
#define UPDATE_TYPE_FW "fw"

/* if not defined in configuration file set to default value */
#ifndef TEMP_ADU_WORK_DIR
#define TEMP_ADU_WORK_DIR "/tmp/adu/.work"
#endif

namespace adushconst = Adu::Shell::Const;

EXTERN_C_BEGIN

// BEGIN Shared Library Export Functions
//
// These are the function symbols that the device update agent will
// lookup and call.
//

/**
 * @brief Instantiates an Update Content Handler for 'fus/update:1' update type.
 */
EXPORTED_METHOD ContentHandler* CreateUpdateContentHandlerExtension(ADUC_LOG_SEVERITY logLevel)
{
    ADUC_Logging_Init(logLevel, "fsupdate-handler");
    Log_Info("Instantiating a Step Handler for 'fus/update:1'");
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

/**
 * @brief Gets the extension contract info.
 *
 * @param[out] contractInfo The extension contract info.
 * @return ADUC_Result The result.
 */
EXPORTED_METHOD ADUC_Result GetContractInfo(ADUC_ExtensionContractInfo* contractInfo)
{
    contractInfo->majorVer = ADUC_V1_CONTRACT_MAJOR_VER;
    contractInfo->minorVer = ADUC_V1_CONTRACT_MINOR_VER;
    return ADUC_Result{ ADUC_GeneralResult_Success, 0 };
}

//
// END Shared Library Export Functions
/////////////////////////////////////////////////////////////////////////////

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
    else if (updateTypeName.compare("common-application") == 0)
    {
         up_type = UPDATE_COMMON_APPLICATION;
    }
    else if (updateTypeName.compare("common-firmware") == 0)
    {
         up_type = UPDATE_COMMON_FIRMWARE;
    }
    else if (updateTypeName.compare("common-both") == 0)
    {
         up_type = UPDATE_COMMON_BOTH;
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

static ADUC_Result HandleExecuteAction(const std::string& targetaction, std::string& output)
{
    int exitCode = 0;
    ADUC_Result result = { ADUC_Result_Failure };
    std::string command = ADUSHELL_FILE_PATH;
    const char* logevel = "-l 3";
    std::vector<std::string> args{ adushconst::update_type_opt,
                                   adushconst::update_type_fus_update,
                                   adushconst::update_action_opt,
                                   adushconst::update_action_execute,
                                   adushconst::target_options_opt,
                                   targetaction, logevel };
    result.ExtendedResultCode = ADUC_LaunchChildProcess(command, args, output);

    return result;
}

static void WriteErrorState(const std::filesystem::path& error_file, ADUC_Result& error_code)
{
    std::ofstream error_state(error_file);
    if (!error_state.is_open())
    {
        Log_Error("Could not create %s.", error_file.string().c_str());
    }
    error_state << error_code.ResultCode;
    error_state << error_code.ExtendedResultCode;
    error_state.close();
}

static bool GetNextSubstringFromString(std::string& fullstr, const std::string& substr)
{
    std::string next_substr;
    std::string special_chars = "\n\t";
    /* search for substring in in target_option*/
    size_t pos = fullstr.find(substr);
    if (pos != std::string::npos)
    {
        /* Calculate the start position of the next substring after the space */
        size_t next_pos = fullstr.find_first_not_of(' ', pos + substr.length());

        /* Find the next space or the end of the string */
        size_t end_pos = fullstr.find(' ', next_pos);
        if (end_pos == std::string::npos)
        {
            end_pos = fullstr.length();
        }

        /* Extract the next substring */
        next_substr = fullstr.substr(next_pos, end_pos - next_pos);
        /* remove special character like word wrap not */
        for (char c : special_chars)
        {
            next_substr.erase(std::remove(next_substr.begin(), next_substr.end(), c), next_substr.end());
        }
        fullstr.clear();
        fullstr = next_substr;
        return true;
    }
    return false;
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
    ADUC_FileEntity fileEntity;
    ADUC_WorkflowHandle workflowHandle = workflowData->WorkflowHandle;
    char* workflowId = workflow_get_id(workflowHandle);
    char* workFolder = workflow_get_workfolder(workflowHandle);
    size_t fileCount = 0;

    memset(&fileEntity, 0, sizeof(fileEntity));
    // For 'fus/update:1', we're expecting 1 payload file.
    fileCount = workflow_get_update_files_count(workflowHandle);
    if (fileCount != 1)
    {
        Log_Error("FSUpdate expecting one file. (%d)", fileCount);
        result.ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_DOWNLOAD_FAILURE_WRONG_FILECOUNT;
        goto done;
    }

    if (!workflow_get_update_file(workflowHandle, 0, &fileEntity))
    {
        result.ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_DOWNLOAD_BAD_FILE_ENTITY;
        goto done;
    }

    updateFilename << workFolder << "/" << fileEntity.TargetFilename;
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

    Log_Info("Start download update file: '%s'", updateFilename.str().c_str());

    result = ExtensionManager::Download(
                &fileEntity, workflowHandle, &Default_ExtensionManager_Download_Options, nullptr);

    Log_Info("Download result code: '%d' and extended result code '%d'", result.ResultCode, result.ExtendedResultCode);

done:
    workflow_free_string(workflowId);
    workflow_free_string(workFolder);

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
    ADUC_FileEntity fileEntity;
    ADUC_WorkflowHandle workflowHandle = workflowData->WorkflowHandle;
    char* workFolder = workflow_get_workfolder(workflowHandle);
    update_type_t up_type = UPDATE_UNKNOWN;
    memset(&fileEntity, 0, sizeof(fileEntity));

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

    if (!workflow_get_update_file(workflowHandle, 0, &fileEntity))
    {
        result.ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_INSTALL_FAILURE_BAD_FILE_ENTITY;
        goto done;
    }

    {
        // read update type from handler properties node.
        const char* type_name =
            workflow_peek_update_manifest_handler_properties_string(workflowHandle, HANDLER_PROPERTIES_UPDATE_TYPE);
        if (IsNullOrEmpty(type_name))
        {
            result = { .ResultCode = ADUC_Result_Failure,
                       .ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_MISSING_UPDATE_TYPE_PROPERTY };
            goto done;
        }

        while (std::filesystem::exists(work_dir / "installUpdate") == false)
        {
            ThreadAPI_Sleep(100);
            Log_Debug("Waiting for install command");
        }

        std::string command = ADUSHELL_FILE_PATH;
        std::vector<std::string> args{ adushconst::update_type_opt,
                                       adushconst::update_type_fus_update,
                                       adushconst::update_action_opt,
                                       adushconst::update_action_install,
                                       adushconst::target_data_opt};

        std::stringstream data;
        data << workFolder << "/" << fileEntity.TargetFilename;
        args.emplace_back(data.str().c_str());

        std::string update_type_name = type_name;
        up_type = this->getUpdateType(update_type_name);
        if (up_type == UPDATE_APPLICATION)
        {
            args.emplace_back(adushconst::target_options_opt);
            args.emplace_back(UPDATE_TYPE_APP);
        }
        else  if (up_type == UPDATE_FIRMWARE )
        {
            args.emplace_back(adushconst::target_options_opt);
            args.emplace_back(UPDATE_TYPE_FW);
        }
        Log_Debug("Install update image: '%s'", data.str().c_str());

        std::string output;
        const int exitCode = ADUC_LaunchChildProcess(command, args, output);

        if ((exitCode == static_cast<int>(UPDATER_FIRMWARE_STATE::UPDATE_SUCCESSFUL))
            || (exitCode == static_cast<int>(UPDATER_APPLICATION_STATE::UPDATE_SUCCESSFUL))
            || (exitCode == static_cast<int>(UPDATER_FIRMWARE_AND_APPLICATION_STATE::UPDATE_SUCCESSFUL)))
        {
            Log_Debug("Install succeeded");
            result = { ADUC_Result_Install_Success };
        }
        else
        {
            Log_Error("Install failed, extendedResultCode = %d, %d", exitCode, errno);
            if (getUpdateType() == UPDATE_FIRMWARE || getUpdateType() == UPDATE_COMMON_FIRMWARE)
            {
                result = { .ResultCode = ADUC_Result_Failure,
                           .ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_INSTALL_FAILURE_FIRMWARE_UPDATE };
            }
            else if (getUpdateType() == UPDATE_APPLICATION || getUpdateType() == UPDATE_COMMON_APPLICATION)
            {
                result = { .ResultCode = ADUC_Result_Failure,
                           .ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_INSTALL_FAILURE_APPLICATION_UPDATE };
            }
            else if (getUpdateType() == UPDATE_COMMON_BOTH)
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
    if(result.ResultCode != ADUC_Result_Install_Success)
    {
        /* remove installUpdate file because installation fails.*/
        std::filesystem::remove( work_dir / "installUpdate");
    }
    WriteErrorState(work_dir / "errorState", result);
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
    std::string result_msg;

    result = HandleExecuteAction("--update_reboot_state", result_msg);

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
    std::string result_msg;

    result = HandleExecuteAction("--update_reboot_state", result_msg);
    if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::INCOMPLETE_APP_UPDATE))
    {
        Log_Info("Incomplete application update -> proceed rollback");

        std::string command = ADUSHELL_FILE_PATH;
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

        result = HandleExecuteAction("--update_reboot_state", result_msg);
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
        std::string result_msg;
        Log_Info("Incomplete firmware rollback update -> reboot processed");
        result = HandleExecuteAction("--commit_update", result_msg);

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
    update_type_t up_type = UPDATE_UNKNOWN;
    std::string update_type_name;
    std::string result_msg;
    std::string target_option;

    // read update type from handler properties node.
    const char* type_name =
        workflow_peek_update_manifest_handler_properties_string(workflowHandle, HANDLER_PROPERTIES_UPDATE_TYPE);
    if (IsNullOrEmpty(type_name))
    {
        result.ResultCode = ADUC_Result_Failure;
        result.ExtendedResultCode = ADUC_ERC_FSUPDATE_HANDLER_MISSING_UPDATE_TYPE_PROPERTY;
        goto done;
    }
    update_type_name = type_name;
    up_type = this->getUpdateType(update_type_name);

    printf("IsInstalled  update_type_name = %s\n", update_type_name.c_str());

    target_option = "--firmware_version";
    if (up_type == UPDATE_APPLICATION || up_type == UPDATE_COMMON_APPLICATION)
    {
        target_option = "--application_version";
    }
    else if (up_type == UPDATE_UNKNOWN)
    {
        Log_Error("IsInstalled failed, %s is wrong update type.", update_type_name.c_str());
        result = { ADUC_Result_Failure,
                   static_cast<int>(UPDATER_FIRMWARE_AND_APPLICATION_STATE::UPDATE_INTERNAL_ERROR) };
        goto done;
    }

    result = HandleExecuteAction(target_option, cmd_output);
    if (result.ExtendedResultCode != 0)
    {
        Log_Error("IsInstalled failed, extendedResultCode = %d", result.ExtendedResultCode);
        goto done;
    }

    if (cmd_output.empty())
    {
        Log_Error("Version of updater command could not be read.");
        result = { ADUC_Result_Failure };
        goto done;
    }
    /* Unfortunaly the adu shell return full output
     * Part of the full log string is --firmware_version <value>
     * or --application_version <value>. The <value> is a version
     * string and would extract from full output string.
     * TODO: Check to get version strings directly.
    */
    GetNextSubstringFromString(cmd_output, target_option);

    Log_Info(
        "Compare %s version %s and installedCriteria %s",
        update_type_name.c_str(),
        cmd_output.c_str(),
        installedCriteria);

    if (cmd_output.compare(installedCriteria) == 0)
    {
        result = HandleExecuteAction("--update_reboot_state", result_msg);

        if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::INCOMPLETE_APP_FW_UPDATE))
        {
            Log_Info("Incomplete firmware and ...");
            result = { ADUC_Result_IsInstalled_MissingCommit };
        }
        else if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::NO_UPDATE_REBOOT_PENDING))
        {
            std::string updatename("Firmware");

            if (up_type == UPDATE_APPLICATION || up_type == UPDATE_COMMON_APPLICATION)
                updatename = "Application";

            Log_Info(
                "%s update is already installed, expected version matches with current installed: '%s'",
                updatename.c_str(),
                installedCriteria);
            result = { ADUC_Result_IsInstalled_Installed };
            /* In case of common update application update state need to be checked too. */
            if (up_type != UPDATE_COMMON_BOTH)
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

    if (up_type == UPDATE_COMMON_BOTH)
    {
        /* In case of common update, application update has to be checked too. */
        target_option = "--application_version";
        /* clear old value of firmware_version */
        cmd_output.clear();
        result = HandleExecuteAction(target_option, cmd_output);

        if (result.ExtendedResultCode != 0)
        {
            Log_Error("IsInstalled failed, extendedResultCode = %d", result.ExtendedResultCode);
            goto done;
        }

        if (cmd_output.empty())
        {
            Log_Error("Version of updater command could not be read.");
            result = { ADUC_Result_Failure };
            goto done;
        }

        GetNextSubstringFromString(cmd_output, target_option);

        if (cmd_output.compare(installedCriteria) == 0)
        {
            result = HandleExecuteAction("--update_reboot_state", result_msg);

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

    result = HandleExecuteAction("--update_reboot_state", result_msg);

    if (result.ExtendedResultCode == static_cast<int>(UPDATER_UPDATE_REBOOT_STATE::FAILED_APP_UPDATE))
    {
        std::string result_msg;
        Log_Info("IsInstalled based of failed application update successful -> commit failed update.");
        result = HandleExecuteAction("--commit_update", result_msg);

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
        std::string result_msg;
        Log_Info("IsInstalled based of failed firmware update successful -> commit failed update.");
        result = HandleExecuteAction("--commit_update", result_msg);

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

/**
 * @brief Backup implementation for fsupdate.
 *
 * @return ADUC_Result The result of the backup.
 * It will always return ADUC_Result_Backup_Success.
 */
ADUC_Result FSUpdateHandlerImpl::Backup(const tagADUC_WorkflowData* workflowData)
{
    ADUC_Result result = { ADUC_Result_Backup_Success };
    Log_Info("FSUpdate doesn't require a specific operation to backup. (no-op) ");
    return result;
}


/**
 * @brief Restore implementation for fsupdate.
 *
 * @return ADUC_Result The result of the restore.
 * ADUC_Result_Restore_Success - success
 * ADUC_Result_Restore_Success_Unsupported - no-op
 */
ADUC_Result FSUpdateHandlerImpl::Restore(const tagADUC_WorkflowData* workflowData)
{
    ADUC_Result result = { ADUC_Result_Restore_Success_Unsupported };
    UNREFERENCED_PARAMETER(workflowData);
    Log_Info("FSUpdate update backup & restore is not supported. (no-op)");
    return result;
}

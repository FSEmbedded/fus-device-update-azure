#include "aduc/fsupdate_handler.hpp"

#include "aduc/adu_core_exports.h"
#include "aduc/logging.h"
#include "aduc/process_utils.hpp"
#include "aduc/string_utils.hpp"
#include "aduc/system_utils.h"

#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

/**
 * @brief handler creation function
 * This function calls  CreateContentHandler from handler factory
 */
std::unique_ptr<ContentHandler> fus_fsupdate_CreateFunc(const ContentHandlerCreateData& data)
{
    Log_Info("fsupdate_handler_create-called.");
    return std::unique_ptr<ContentHandler>{ FSUpdateHandlerImpl::CreateContentHandler(
        data.WorkFolder(), data.LogFolder(), data.Filename(), data.FileType()) };
}

// Forward declarations.
static ADUC_Result CancelApply(const char* logFolder);

/**
 * @brief Creates a new FSUpdateHandlerImpl object and casts to a ContentHandler.
 * Note that there is no way to create a FSUpdateHandlerImpl directly.
 *
 * @param workFolder The folder where content will be downloaded.
 * @param logFolder The folder where operational logs can be placed.
 * @param filename The .fsu image file to be installed by fsupdate.
 * @return std::unique_ptr<ContentHandler> SimulatorHandlerImpl object as a ContentHandler.
 */
std::unique_ptr<ContentHandler> FSUpdateHandlerImpl::CreateContentHandler(
    const std::string& workFolder,
    const std::string& logFolder,
    const std::string& filename,
    const std::string& fileType)
{
    return std::unique_ptr<ContentHandler>{ new FSUpdateHandlerImpl(workFolder, logFolder, filename, fileType) };
}

/**
 * @brief Validate meta data including file count and handler version.
 * @param prepareInfo.
 * @return bool
 */
ADUC_Result FSUpdateHandlerImpl::Prepare(const ADUC_PrepareInfo* prepareInfo)
{
    if (prepareInfo->updateTypeVersion != _applicationFile && prepareInfo->updateTypeVersion != _firmwareFile)
    {
        Log_Error(
            "FsUpdate packages prepare failed. Wrong Handler Version '%s'. Select 'application' or 'firmware' ",
            prepareInfo->updateTypeVersion);
        return ADUC_Result{ ADUC_PrepareResult_Failure,
                            ADUC_ERC_SWUPDATE_HANDLER_PACKAGE_PREPARE_FAILURE_WRONG_VERSION };
    }

    if (prepareInfo->fileCount != 1)
    {
        Log_Error("FsUpdate packages prepare failed. Wrong File Count %d", prepareInfo->fileCount);
        return ADUC_Result{ ADUC_PrepareResult_Failure,
                            ADUC_ERC_SWUPDATE_HANDLER_PACKAGE_PREPARE_FAILURE_WRONG_FILECOUNT };
    }

    Log_Info("Prepare succeeded.");
    return ADUC_Result{ ADUC_PrepareResult_Success };
}

/**
 * @brief Download implementation for fsupdate (no-op)
 * fsupdate does not need to download additional content.
 * This method is a no-op.
 *
 * @return ADUC_Result The result of the download (always success)
 */
ADUC_Result FSUpdateHandlerImpl::Download()
{
    _isApply = false;
    Log_Info("Download called - no-op for fsupdate");
    return ADUC_Result{ ADUC_DownloadResult_Success };
}

/**
 * @brief Install implementation for fsupdate.
 * Calls into the fsupdate wrapper script to install an image file.
 *
 * @return ADUC_Result The result of the install.
 */
ADUC_Result FSUpdateHandlerImpl::Install()
{
    _isApply = false;
    Log_Info("Installing from %s", _workFolder.c_str());

    std::unique_ptr<DIR, std::function<int(DIR*)>> directory(
        opendir(_workFolder.c_str()), [](DIR* dirp) -> int { return closedir(dirp); });

    if (directory == nullptr)
    {
        Log_Error("opendir failed, errno = %d", errno);

        return ADUC_Result{ ADUC_InstallResult_Failure, MAKE_ADUC_ERRNO_EXTENDEDRESULTCODE(errno) };
    }

    const char* filename = nullptr;
    const dirent* entry = nullptr;

    while ((entry = readdir(directory.get())) != nullptr)
    {
        if (entry->d_type == DT_REG)
        {
            if (filename == nullptr)
            {
                filename = entry->d_name;
            }
            else
            {
                Log_Error("More than one file in work folder");
                return ADUC_Result{ ADUC_InstallResult_Failure, ADUC_ERC_NOTPERMITTED };
            }
        }
    }

    if (filename == nullptr)
    {
        Log_Error("No file in work folder");
        return ADUC_Result{ ADUC_InstallResult_Failure, ADUC_ERC_NOTRECOVERABLE };
    }

    if (_filename != filename)
    {
        Log_Warn("Specified filename %s does not match actual filename %s.", _filename.c_str(), filename);
    }

    Log_Info("Installing image file: '%s' type: '%s'", filename, _fileType.c_str());

    std::string command = _pathToFsUpdate;
    std::vector<std::string> args{};

    if (_fileType == _applicationFile)
    {
        args.emplace_back(_installApplicationFile);
    }
    else if (_fileType == _firmwareFile)
    {
        args.emplace_back(_installFirmwareFile);
    }
    else if (_fileType.empty())
    {
        Log_Error("No Update Type provided");
        return ADUC_Result{ ADUC_InstallResult_Failure };
    }
    else
    {
        Log_Error("Invaliede Update Type");
        return ADUC_Result{ ADUC_InstallResult_Failure };
    }

    std::stringstream data;
    data << _workFolder << "/" << filename;
    args.emplace_back(data.str().c_str());
    args.emplace_back(_debugMode);
    std::string output;

    const int exitCode = ADUC_LaunchChildProcess(command, args, output);

    if (exitCode != 0)
    {
        Log_Error("Install failed, extendedResultCode = %d", exitCode);
        return ADUC_Result{ ADUC_InstallResult_Failure, exitCode };
    }

    Log_Info("Install succeeded");
    return ADUC_Result{ ADUC_InstallResult_Success };
}

/**
 * @brief Apply implementation for FS-Update
 * Calls into the FS-Update wrapper script to perform apply.
 * Will validate a successfull reboot and
 * flip U-Boot flags update and update-reboot-state to 0
 * to mark the newly bootet partition as good and the olde on as inactive
 *
 * @return ADUC_Result The result of the apply.
 */
ADUC_Result FSUpdateHandlerImpl::Apply()
{
    Log_Info("Apply action called");
    _isApply = true;

    std::string command = _pathToFsUpdate;
    std::vector<std::string> args{ _commitUpdate, _debugMode };
    std::string output;

    const int exitCode = ADUC_LaunchChildProcess(command, args, output);

    if (exitCode != 0)
    {
        Log_Error("Apply failed, extendedResultCode = %d", exitCode);
        return ADUC_Result{ ADUC_ApplyResult_Failure, exitCode };
    }

    return ADUC_Result{ ADUC_ApplyResult_Success };
}

/**
 * @brief Cancel implementation for fsupdate.
 * We don't have many hooks into fsupdate to cancel an ongoing install.
 * We can cancel apply by reverting the bootloader flag to boot into the original partition.
 * Calls into the fsupdate wrapper script to cancel apply.
 * Cancel after or during any other operation is a no-op.
 *
 * @return ADUC_Result The result of the cancel.
 */
ADUC_Result FSUpdateHandlerImpl::Cancel()
{
    Log_Info("Cancel called - no-op for fsupdate");
    return ADUC_Result{ ADUC_CancelResult_Success };
}

/**
 * @brief Reads a first line of a file, trims trailing whitespace, and returns as string.
 *
 * @param filePath Path to the file to read value from.
 * @return std::string Returns the value from the file. Returns empty string if there was an error.
 */
/*static*/
std::string FSUpdateHandlerImpl::ReadValueFromFile(const std::string& filePath)
{
    if (filePath.empty())
    {
        Log_Error("Empty file path.");
        return std::string{};
    }

    if ((filePath.length()) + 1 > PATH_MAX)
    {
        Log_Error("Path is too long.");
        return std::string{};
    }

    std::ifstream file(filePath);
    if (!file.is_open())
    {
        Log_Error("File %s failed to open, error: %d", filePath.c_str(), errno);
        return std::string{};
    }

    std::string result;
    std::getline(file, result);

    if (file.bad())
    {
        Log_Error("Unable to read from file %s, error: %d", filePath.c_str(), errno);
        return std::string{};
    }

    // Trim whitespace
    ADUC::StringUtils::Trim(result);
    return result;
}

ADUC_Result FSUpdateHandlerImpl::GetUpdateRebootState()
{
    Log_Info("GetUpdateRebootState");
    _isApply = false;

    ADUC_Result result;

    std::string command = _pathToFsUpdate;
    std::vector<std::string> args{ _getRebootState };
    std::string output;

    const int exitCode = ADUC_LaunchChildProcess(command, args, output);

    switch (exitCode)
    {
    case 0:
        result = { ADUC_GetUpdateRebootStateResult_NO_UPDATE_REBOOT_PENDING };
        Log_Info("FS-Update returned NO_UPDATE_REBOOT_PENDING");
        break;

    case 1:
        result = { ADUC_GetUpdateRebootStateResult_FW_UPDATE_REBOOT_FAILED };
        Log_Info("FS-Update returned FW_UPDATE_REBOOT_FAILED");
        break;

    case 2:
        result = { ADUC_GetUpdateRebootStateResult_INCOMPLETE_FW_UPDATE };
        Log_Info("FS-Update returned INCOMPLETE_FW_UPDATE");
        break;

    case 3:
        result = { ADUC_GetUpdateRebootStateResult_INCOMPLETE_APP_UPDATE };
        Log_Info("FS-Update returned INCOMPLETE_APP_UPDATE");
        break;

    case 4:
        result = { ADUC_GetUpdateRebootStateResult_INCOMPLETE_APP_FW_UPDATE };
        Log_Info("FS-Update returned INCOMPLETE_APP_AND_UPDATE");
        break;

    case 5:
        result = { ADUC_GetUpdateRebootStateResult_FAILED_FW_UPDATE };
        Log_Info("FS-Update returned FAILED_FW_UPDATE");
        break;

    case 6:
        result = { ADUC_GetUpdateRebootStateResult_FAILED_APP_UPDATE };
        Log_Info("FS-Update returned FAILED_APP_UPDATE");
        break;

    default:
        result = { ADUC_GetUpdateRebootStateResult_FAILURE };
        Log_Info("FS-Update -urs failed");
        break;
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
ADUC_Result FSUpdateHandlerImpl::IsInstalled(const std::string& installedCriteria)
{
    std::string version;

    if (_fileType == _firmwareFile)
    {
        version = ReadValueFromFile(FIRMWARE_VERSION_FILE);
    }
    else if (_fileType == _applicationFile)
    {
        version = ReadValueFromFile(APP_VERSION_FILE);
    }
    else
    {
        Log_Error("Faield to read Version file. Invaliede filetype '%s'", _fileType.c_str());
        return ADUC_Result{ ADUC_IsInstalledResult_Failure };
    }

    if (version.empty())
    {
        Log_Error("Version file %s did not contain a version or could not be read.", _fileType.c_str());
        return ADUC_Result{ ADUC_IsInstalledResult_Failure };
    }

    if (version == installedCriteria)
    {
        Log_Info("Installed criteria %s was installed.", installedCriteria.c_str());
        return ADUC_Result{ ADUC_IsInstalledResult_Installed };
    }

    Log_Info(
        "Installed criteria %s was not installed, the current version is %s",
        installedCriteria.c_str(),
        version.c_str());
    return ADUC_Result{ ADUC_IsInstalledResult_NotInstalled };
}

/**
 * @brief Helper function to perform cancel when we are doing an apply.
 *
 * @return ADUC_Result The result of the cancel.
 */
static ADUC_Result CancelApply(const char* logFolder)
{
    Log_Info("CancelApply called - no-op for fsupdate");
    return ADUC_Result{ ADUC_ApplyResult_Cancelled };
}

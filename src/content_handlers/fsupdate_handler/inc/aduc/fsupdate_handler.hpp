/**
 * @file fsupdate_handler.hpp
 * @brief Defines fsUpdateHandlerImpl.
 *
 * @copyright Copyright (c) 2019, Microsoft Corp.
 */
#ifndef ADUC_FSUPDATE_HANDLER_HPP
#define ADUC_FSUPDATE_HANDLER_HPP

#include "aduc/content_handler.hpp"
#include "aduc/content_handler_factory.hpp"
#include <aduc/result.h>
#include <memory>
#include <string>

/**
 * @brief handler creation function
 * This function calls  CreateContentHandler from handler factory
 */
std::unique_ptr<ContentHandler> fus_fsupdate_CreateFunc(const ContentHandlerCreateData& data);

/**
 * @class fsUpdateHandlerImpl
 * @brief The fsupdate specific implementation of ContentHandler interface.
 */
class FSUpdateHandlerImpl : public ContentHandler
{
public:
    static std::unique_ptr<ContentHandler> CreateContentHandler(
        const std::string& workFolder,
        const std::string& logFolder,
        const std::string& filename,
        const std::string& fileType);

    // Delete copy ctor, copy assignment, move ctor and move assignment operators.
    FSUpdateHandlerImpl(const FSUpdateHandlerImpl&) = delete;
    FSUpdateHandlerImpl& operator=(const FSUpdateHandlerImpl&) = delete;
    FSUpdateHandlerImpl(FSUpdateHandlerImpl&&) = delete;
    FSUpdateHandlerImpl& operator=(FSUpdateHandlerImpl&&) = delete;

    ~FSUpdateHandlerImpl() override = default;

    ADUC_Result Prepare(const ADUC_PrepareInfo* prepareInfo) override;
    ADUC_Result Download() override;
    ADUC_Result Install(/*const std::string& updateType*/) override;
    ADUC_Result Apply() override;
    ADUC_Result Cancel() override;
    ADUC_Result IsInstalled(const std::string& installedCriteria) override;
    ADUC_Result GetUpdateRebootState() override;
    static std::string ReadValueFromFile(const std::string& filePath);

protected:
    // Protected constructor, must call CreateContentHandler factory method or from derived simulator class
    FSUpdateHandlerImpl(
        const std::string& workFolder,
        const std::string& logFolder,
        const std::string& filename,
        const std::string& fileType) :
        _workFolder{ workFolder },
        _logFolder{ logFolder }, _filename{ filename }, _fileType{ fileType }
    {
    }

private:
    std::string _workFolder;
    std::string _logFolder;
    std::string _filename;
    std::string _fileType;
    bool _isApply{ false };

    const std::string _pathToFsUpdate = "/usr/bin/FS-Update";
    const std::string _installFirmwareFile = "-ff";
    const std::string _firmwareFile = "firmware";
    const std::string _installApplicationFile = "-af";
    const std::string _applicationFile = "application";
    const std::string _commitUpdate = "-cu";
    const std::string _getRebootState = "-urs";
    const std::string _debugMode = "--debug";
};

#endif // ADUC_FSUPDATE_HANDLER_HPP

/**
 * @file fsupdate_handler.hpp
 * @brief Defines FSUpdateHandlerImpl.
 *
 * @copyright Copyright (c) F&S Elektronik Systeme.
 * Licensed under the MIT License.
 */
#ifndef ADUC_FSUPDATE_HANDLER_HPP
#define ADUC_FSUPDATE_HANDLER_HPP

#include "aduc/content_handler.hpp"
#include "aduc/logging.h"
#include <filesystem>
#include <memory>
#include <string>

typedef enum UpdateTypeT {
    UPDATE_FIRMWARE = 0, // raw single firmware update
    UPDATE_APPLICATION, // raw single application update
    UPDATE_COMMON_FIRMWARE, // common firmware update
    UPDATE_COMMON_APPLICATION, // common application update
    UPDATE_COMMON_BOTH, // common application and firmware update
    UPDATE_UNKNOWN = -1,
} update_type_t;

EXTERN_C_BEGIN

/**
 * @brief Instantiates an Update Content Handler for 'fus/update:1' update type.
 * @return A pointer to an instantiated Update Content Handler object.
 */
EXPORTED_METHOD ContentHandler* CreateUpdateContentHandlerExtension(ADUC_LOG_SEVERITY logLevel);

/**
 * @brief Gets the extension contract info.
 *
 * @param[out] contractInfo The extension contract info.
 * @return ADUC_Result The result.
 */
EXPORTED_METHOD ADUC_Result GetContractInfo(ADUC_ExtensionContractInfo* contractInfo);

EXTERN_C_END

/**
 * @class FSUpdateHandlerImpl
 * @brief The fs-updater specific implementation of ContentHandler interface.
 */
class FSUpdateHandlerImpl : public ContentHandler
{
private:
    update_type_t update_type;
    const char* update_type_names[5] = {"firmware", "application", "common-firmware", "common-application", "common-both"};
    /* path to default work directory */
    std::filesystem::path work_dir;
    /* default permissions of work directory */
    std::filesystem::perms work_dir_perms;
    bool create_work_dir();
public:
    static ContentHandler* CreateContentHandler();

    // Delete copy ctor, copy assignment, move ctor and move assignment operators.
    FSUpdateHandlerImpl(const FSUpdateHandlerImpl&) = delete;
    FSUpdateHandlerImpl& operator=(const FSUpdateHandlerImpl&) = delete;
    FSUpdateHandlerImpl(FSUpdateHandlerImpl&&) = delete;
    FSUpdateHandlerImpl& operator=(FSUpdateHandlerImpl&&) = delete;

    ~FSUpdateHandlerImpl() override;

    ADUC_Result Download(const tagADUC_WorkflowData* workflowData) override;
    ADUC_Result Backup(const tagADUC_WorkflowData* workflowData) override;
    ADUC_Result Install(const tagADUC_WorkflowData* workflowData) override;
    ADUC_Result Apply(const tagADUC_WorkflowData* workflowData) override;
    ADUC_Result Cancel(const tagADUC_WorkflowData* workflowData) override;
    ADUC_Result Restore(const tagADUC_WorkflowData* workflowData) override;
    ADUC_Result IsInstalled(const tagADUC_WorkflowData* workflowData) override;

    update_type_t getUpdateType();
    update_type_t getUpdateType(std::string & updateTypeName);
    void setUpdateType(update_type_t up_type);

protected:
    // Protected constructor, must call CreateContentHandler factory method or from derived simulator class
    FSUpdateHandlerImpl();
};

#endif // ADUC_FSUPDATE_HANDLER_HPP

/**
 * @file fsupdate_handler.hpp
 * @brief Defines FSUpdateApplicationHandlerImpl.
 *
 * @copyright Copyright (c) F&S Elektronik Systeme.
 * Licensed under the MIT License.
 */
#ifndef ADUC_FSUPDATE_APPLICATION_HANDLER_HPP
#define ADUC_FSUPDATE_APPLICATION_HANDLER_HPP

#include "aduc/content_handler.hpp"
#include "aduc/logging.h"
#include <aduc/result.h>
#include <memory>
#include <string>

EXTERN_C_BEGIN

/**
 * @brief Instantiates an Update Content Handler for 'fus/application:1' update type.
 * @return A pointer to an instantiated Update Content Handler object.
 */
ContentHandler* CreateUpdateContentHandlerExtension(ADUC_LOG_SEVERITY logLevel);

EXTERN_C_END

/**
 * @class FSUpdateApplicationHandlerImpl
 * @brief The fs-updater specific implementation of ContentHandler interface.
 */
class FSUpdateApplicationHandlerImpl : public ContentHandler
{
public:
    static ContentHandler* CreateContentHandler();

    // Delete copy ctor, copy assignment, move ctor and move assignment operators.
    FSUpdateApplicationHandlerImpl(const FSUpdateApplicationHandlerImpl&) = delete;
    FSUpdateApplicationHandlerImpl& operator=(const FSUpdateApplicationHandlerImpl&) = delete;
    FSUpdateApplicationHandlerImpl(FSUpdateApplicationHandlerImpl&&) = delete;
    FSUpdateApplicationHandlerImpl& operator=(FSUpdateApplicationHandlerImpl&&) = delete;

    ~FSUpdateApplicationHandlerImpl() override;

    ADUC_Result Download(const tagADUC_WorkflowData* workflowData) override;
    ADUC_Result Install(const tagADUC_WorkflowData* workflowData) override;
    ADUC_Result Apply(const tagADUC_WorkflowData* workflowData) override;
    ADUC_Result Cancel(const tagADUC_WorkflowData* workflowData) override;
    ADUC_Result IsInstalled(const tagADUC_WorkflowData* workflowData) override;

protected:
    // Protected constructor, must call CreateContentHandler factory method or from derived simulator class
    FSUpdateApplicationHandlerImpl() = default;
};

#endif // ADUC_FSUPDATE_APPLICATION_HANDLER_HPP

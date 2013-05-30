/*

 Copyright (c) 2013, Ford Motor Company
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.

 Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following
 disclaimer in the documentation and/or other materials provided with the
 distribution.

 Neither the name of the Ford Motor Company nor the names of its contributors
 may be used to endorse or promote products derived from this software
 without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 POSSIBILITY OF SUCH DAMAGE.
 */

#include "application_manager/commands/delete_interaction_choice_set_command.h"
#include "application_manager/application_manager_impl.h"
#include "application_manager/message_chaining.h"
#include "application_manager/application_impl.h"
#include "JSONHandler/SDLRPCObjects/V2/HMILevel.h"
#include "utils/logger.h"

namespace application_manager {

namespace commands {

log4cxx::LoggerPtr logger_ =
  log4cxx::LoggerPtr(log4cxx::Logger::getLogger("Commands"));

DeleteInteractionChoiceSetCommand::DeleteInteractionChoiceSetCommand(
    const MessageSharedPtr& message): CommandRequestImpl(message) {
}

DeleteInteractionChoiceSetCommand::~DeleteInteractionChoiceSetCommand() {
}

void DeleteInteractionChoiceSetCommand::Run() {
  LOG4CXX_INFO(logger_, "DeleteInteractionChoiceSetCommand::Run ");

  ApplicationImpl* app = static_cast<ApplicationImpl*>(
      ApplicationManagerImpl::instance()->
      application((*message_)[strings::params][strings::connection_key]));

  if (NULL == app) {
    LOG4CXX_ERROR_EXT(logger_, "No application associated with session key ");
    SendResponse(false,
                 NsSmartDeviceLinkRPC::V2::Result::APPLICATION_NOT_REGISTERED);
    return;
  }

  const int choise_set_id =
  (*message_)[strings::msg_params][strings::interaction_choice_set_id].asInt();

  if (!app->FindChoiceSet(choise_set_id)) {
    SendResponse(false, NsSmartDeviceLinkRPC::V2::Result::INVALID_ID);
    return;
  }

  const int corellation_id =
      (*message_)[strings::params][strings::correlation_id];
  const int connection_key =
      (*message_)[strings::params][strings::connection_key];

  MessageChaining * chain = NULL;

  // create HMI request
  smart_objects::CSmartObject* p_smrt_ui  = new smart_objects::CSmartObject();
  // TODO(DK): HMI Request Id
  const int ui_cmd_id = 21;
  (*p_smrt_ui)[strings::params][strings::function_id] =
      ui_cmd_id;

  (*p_smrt_ui)[strings::params][strings::message_type] =
      MessageType::kRequest;

  (*p_smrt_ui)[strings::msg_params][strings::cmd_id] =
      (*message_)[strings::msg_params][strings::cmd_id];

  (*p_smrt_ui)[strings::msg_params][strings::interaction_choice_set_id] =
      choise_set_id;

  (*p_smrt_ui)[strings::msg_params][strings::app_id] =
      app->app_id();

  chain = ApplicationManagerImpl::instance()->AddMessageChain(chain,
      connection_key, corellation_id, ui_cmd_id, p_smrt_ui);

  ApplicationManagerImpl::instance()->SendMessageToHMI(p_smrt_ui);
}

}  // namespace commands

}  // namespace application_manager

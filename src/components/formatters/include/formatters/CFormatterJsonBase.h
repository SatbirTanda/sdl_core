/**
 * @file CFormatterJsonBase.hpp
 * @brief CFormatterJsonBase header file.
 */
// Copyright (c) 2013, Ford Motor Company
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following
// disclaimer in the documentation and/or other materials provided with the
// distribution.
//
// Neither the name of the Ford Motor Company nor the names of its contributors
// may be used to endorse or promote products derived from this software
// without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR 'A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef SRC_COMPONENTS_FORMATTERS_INCLUDE_FORMATTERS_CFORMATTERJSONBASE_H_
#define SRC_COMPONENTS_FORMATTERS_INCLUDE_FORMATTERS_CFORMATTERJSONBASE_H_

#include "smart_objects/smart_object.h"
#include "json/json.h"

namespace ns_smart_device_link {
namespace ns_json_handler {
namespace formatters {

namespace meta_formatter_error_code {
/**
  * @brief Error codes of MetaFormatter represented as bitmask
  **/
typedef long tMetaFormatterErrorCode;

/**
  * @brief OK, no error
  */
static const tMetaFormatterErrorCode kErrorOk = 0x0;

/**
  * @brief origin smart object is not function
  */
static const tMetaFormatterErrorCode kErrorObjectIsNotFunction = 0x01;

/**
  * @brief smart shema describes object which is not function
  */
static const tMetaFormatterErrorCode kErrorSchemaIsNotFunction = 0x02;

/**
  * @brief result smart object has invalid type (SmartType_Invalid)
  *        before passing to MetaFormatter, i.e. result object can not
  *        be changed, i.e. result object can not be built
  *
  */
static const tMetaFormatterErrorCode kErrorFailedCreateObjectBySchema = 0x04;
}

/**
 * @brief The base class for all JSON based formatters.
 */
class CFormatterJsonBase {
 private:
  /**
   * @brief Constructor.
   */
  CFormatterJsonBase();

  /**
   * @brief Copy constructor.
   *
   * @param obj Object to copy.
   */
  CFormatterJsonBase(const CFormatterJsonBase& obj);

 protected:
 public:
  /**
   * @brief The method constructs a SmartObject from the input JSON object
   *
   * @param value Input JSON object.
   * @param obj The resulting SmartObject.
   */
  static void jsonValueToObj(
      const Json::Value& value,
      ns_smart_device_link::ns_smart_objects::SmartObject& obj);

  /**
    * @brief The method constructs a JSON object from the input SmartObject
    *
    * @param obj Input SmartObject. Can contain a complex structure of objects.
    * @param value The resulting JSON object. It has the same structure as the
    *input SmartObject.
    */
  static void objToJsonValue(
      const ns_smart_device_link::ns_smart_objects::SmartObject& obj,
      Json::Value& value);
};
}
}
}  // namespace ns_smart_device_link::ns_json_handler::formatters

#endif  // SRC_COMPONENTS_FORMATTERS_INCLUDE_FORMATTERS_CFORMATTERJSONBASE_H_

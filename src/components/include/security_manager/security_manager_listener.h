/*
 * Copyright (c) 2014, Ford Motor Company
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following
 * disclaimer in the documentation and/or other materials provided with the
 * distribution.
 *
 * Neither the name of the Ford Motor Company nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef SRC_COMPONENTS_INCLUDE_SECURITY_MANAGER_SECURITY_MANAGER_LISTENER_H_
#define SRC_COMPONENTS_INCLUDE_SECURITY_MANAGER_SECURITY_MANAGER_LISTENER_H_

#include <string>

namespace security_manager {

class SecurityManagerListener {
 public:
  /**
   * \brief Notification about protection result
   * \param connection_key Unique key used by other components as session
   * identifier
   * \param success result of connection protection
   * \return \c true on success notification or \c false otherwise
   */
  virtual bool OnHandshakeDone(uint32_t connection_key,
                               SSLContext::HandshakeResult result) = 0;

  /**
   * @brief Notification about handshake failure
   * @return true on success notification handling or false otherwise
   */
  virtual bool OnGetSystemTimeFailed() = 0;

  /**
   * @brief Notify listeners that certificate update is required.
   */
  virtual void OnCertificateUpdateRequired() = 0;

  virtual bool OnPTUFailed() = 0;

#ifdef EXTERNAL_PROPRIETARY_MODE
  /**
   * @brief OnCertDecryptFailed is called when certificate decryption fails in
   * external flow
   * @return since this callback is a part of SecurityManagerListener, bool
   * return value is used to indicate whether listener instance can be deleted
   * by calling entity. if true - listener can be deleted and removed from
   * listeners by SecurityManager, false - listener retains its place within
   * SecurityManager.
   */
  virtual bool OnCertDecryptFailed() = 0;
#endif

  /**
   * @brief Get certificate data from policy
   * @param reference to string where to save certificate data
   * @return true if listener saved some data to string otherwise false
   */
  virtual bool GetPolicyCertificateData(std::string& data) const = 0;

  virtual ~SecurityManagerListener() {}
};
}  // namespace security_manager
#endif  // SRC_COMPONENTS_INCLUDE_SECURITY_MANAGER_SECURITY_MANAGER_LISTENER_H_

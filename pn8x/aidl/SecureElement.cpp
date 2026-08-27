#define LOG_TAG "P73SecureElement"
#include "SecureElement.h"

#include <LsClient.h>
#include <log/log.h>

namespace aidl::android::hardware::secure_element {
namespace {
constexpr size_t kMaxAidLength = 16;
constexpr uint8_t kBasicChannel = 0;

int statusFromResponse(const std::vector<uint8_t>& response) {
  if (response.size() < 2) return ISecureElement::IOERROR;
  const uint8_t sw1 = response[response.size() - 2];
  const uint8_t sw2 = response.back();
  if ((sw1 == 0x90 && sw2 == 0x00) || sw1 == 0x62 || sw1 == 0x63) return 0;
  if (sw1 == 0x6a && sw2 == 0x81) return ISecureElement::CHANNEL_NOT_AVAILABLE;
  if ((sw1 == 0x6a && sw2 == 0x82) ||
      (sw1 == 0x69 && (sw2 == 0x99 || sw2 == 0x85)))
    return ISecureElement::NO_SUCH_ELEMENT_ERROR;
  if ((sw1 == 0x6a && sw2 == 0x86) ||
      ((sw1 == 0x6e || sw1 == 0x6d) && sw2 == 0x00))
    return ISecureElement::UNSUPPORTED_OPERATION;
  return ISecureElement::FAILED;
}

}  // namespace

bool SecureElement::isInitializedLocked() const { return phNxpEse_isOpen(); }

ESESTATUS SecureElement::seHalInitLocked() {
  if (isInitializedLocked()) return ESESTATUS_SUCCESS;

  phNxpEse_initParams params{};
  params.initMode = ESE_MODE_NORMAL;
  ESESTATUS status = phNxpEse_open(params);
  if (status != ESESTATUS_SUCCESS) {
    ALOGE("SecureElement open failed");
    return status;
  }
  status = phNxpEse_init(params);
  if (status != ESESTATUS_SUCCESS) {
    ALOGE("SecureElement init failed");
    phNxpEse_close();
  }
  return status;
}

bool SecureElement::seHalDeInitLocked() {
  if (!isInitializedLocked()) {
    opened_channels_.fill(false);
    opened_channel_count_ = 0;
    return true;
  }

  const ESESTATUS deinit_status = phNxpEse_deInit();
  const ESESTATUS close_status = phNxpEse_close();
  if (deinit_status != ESESTATUS_SUCCESS || close_status != ESESTATUS_SUCCESS) {
    ALOGE("SecureElement deinit failed: deinit=%d close=%d", deinit_status,
          close_status);
    return false;
  }
  opened_channels_.fill(false);
  opened_channel_count_ = 0;
  return true;
}

bool SecureElement::transceiveLocked(const std::vector<uint8_t>& command,
                                     std::vector<uint8_t>* response) {
  if (command.empty() || !isInitializedLocked()) return false;
  phNxpEse_data tx{static_cast<uint32_t>(command.size()),
                   const_cast<uint8_t*>(command.data())};
  phNxpEse_data rx{};
  const ESESTATUS status = phNxpEse_Transceive(&tx, &rx);
  if (status == ESESTATUS_SUCCESS && rx.p_data != nullptr)
    response->assign(rx.p_data, rx.p_data + rx.len);
  if (rx.p_data != nullptr) phNxpEse_free(rx.p_data);
  return status == ESESTATUS_SUCCESS;
}

void SecureElement::onLoaderServiceCompleted(bool result, std::string reason,
                                             void* context) {
  static_cast<SecureElement*>(context)->notifyLoaderCompletion(result,
                                                               reason.c_str());
}

void SecureElement::notifyLoaderCompletion(bool result, const char* reason) {
  std::shared_ptr<ISecureElementCallback> callback;
  {
    std::lock_guard guard(lock_);
    initializing_ = false;
    callback = callback_;
  }
  if (callback) callback->onStateChange(result, reason);
}

::ndk::ScopedAStatus SecureElement::init(
    const std::shared_ptr<ISecureElementCallback>& callback) {
  if (!callback)
    return ::ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);

  bool notify = false;
  bool ready = false;
  const char* reason = "PN81T P73 initialization failed";
  {
    std::lock_guard guard(lock_);
    callback_ = callback;
    if (isInitializedLocked()) {
      notify = true;
      ready = true;
      reason = "NXP SE HAL init ok";
    } else if (!initializing_) {
      initializing_ = true;
      if (seHalInitLocked() == ESESTATUS_SUCCESS) {
        if (LSC_doDownload(SecureElement::onLoaderServiceCompleted, this) ==
            LSCSTATUS_SUCCESS)
          return ::ndk::ScopedAStatus::ok();
        ALOGE("LS download thread creation failed");
        seHalDeInitLocked();
      }
      initializing_ = false;
      notify = true;
    }
  }
  if (notify) callback->onStateChange(ready, reason);
  return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus SecureElement::isCardPresent(bool* present) {
  *present = true;
  return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus SecureElement::getAtr(std::vector<uint8_t>* response) {
  response->clear();
  return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus SecureElement::transmit(
    const std::vector<uint8_t>& command, std::vector<uint8_t>* response) {
  std::lock_guard guard(lock_);
  response->clear();
  if (!transceiveLocked(command, response))
    return ::ndk::ScopedAStatus::fromServiceSpecificError(
        ISecureElement::IOERROR);
  return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus SecureElement::openBasicChannel(
    const std::vector<uint8_t>& aid, int8_t p2,
    std::vector<uint8_t>* response) {
  std::lock_guard guard(lock_);
  response->clear();
  if (aid.size() > kMaxAidLength)
    return ::ndk::ScopedAStatus::fromServiceSpecificError(
        ISecureElement::FAILED);
  if (seHalInitLocked() != ESESTATUS_SUCCESS)
    return ::ndk::ScopedAStatus::fromServiceSpecificError(
        ISecureElement::IOERROR);

  std::vector<uint8_t> select{kBasicChannel, 0xa4, 0x04,
                              static_cast<uint8_t>(p2),
                              static_cast<uint8_t>(aid.size())};
  select.insert(select.end(), aid.begin(), aid.end());
  if (!transceiveLocked(select, response)) {
    if (opened_channel_count_ == 0) seHalDeInitLocked();
    return ::ndk::ScopedAStatus::fromServiceSpecificError(
        ISecureElement::IOERROR);
  }
  const int status = statusFromResponse(*response);
  if (status) {
    if (opened_channel_count_ == 0) seHalDeInitLocked();
    return ::ndk::ScopedAStatus::fromServiceSpecificError(status);
  }
  if (!opened_channels_[kBasicChannel]) {
    opened_channels_[kBasicChannel] = true;
    ++opened_channel_count_;
  }
  return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus SecureElement::openLogicalChannel(
    const std::vector<uint8_t>& aid, int8_t p2,
    LogicalChannelResponse* response) {
  std::lock_guard guard(lock_);
  response->channelNumber = -1;
  response->selectResponse.clear();
  if (aid.size() > kMaxAidLength)
    return ::ndk::ScopedAStatus::fromServiceSpecificError(
        ISecureElement::FAILED);
  if (seHalInitLocked() != ESESTATUS_SUCCESS)
    return ::ndk::ScopedAStatus::fromServiceSpecificError(
        ISecureElement::IOERROR);

  std::vector<uint8_t> manage_response;
  if (!transceiveLocked({0x00, 0x70, 0x00, 0x00, 0x01}, &manage_response)) {
    if (opened_channel_count_ == 0) seHalDeInitLocked();
    return ::ndk::ScopedAStatus::fromServiceSpecificError(
        ISecureElement::IOERROR);
  }
  const int manage_status = statusFromResponse(manage_response);
  if (manage_status || manage_response.size() < 3) {
    if (opened_channel_count_ == 0) seHalDeInitLocked();
    return ::ndk::ScopedAStatus::fromServiceSpecificError(
        manage_status ? manage_status : ISecureElement::IOERROR);
  }
  const uint8_t channel = manage_response.front();
  if (channel == kBasicChannel || channel >= opened_channels_.size()) {
    if (opened_channel_count_ == 0) seHalDeInitLocked();
    return ::ndk::ScopedAStatus::fromServiceSpecificError(
        ISecureElement::CHANNEL_NOT_AVAILABLE);
  }
  opened_channels_[channel] = true;
  ++opened_channel_count_;

  std::vector<uint8_t> select{channel, 0xa4, 0x04, static_cast<uint8_t>(p2),
                              static_cast<uint8_t>(aid.size())};
  select.insert(select.end(), aid.begin(), aid.end());
  if (!transceiveLocked(select, &response->selectResponse)) {
    closeChannelLocked(channel);
    return ::ndk::ScopedAStatus::fromServiceSpecificError(
        ISecureElement::IOERROR);
  }
  const int select_status = statusFromResponse(response->selectResponse);
  if (select_status) {
    closeChannelLocked(channel);
    response->selectResponse.clear();
    return ::ndk::ScopedAStatus::fromServiceSpecificError(select_status);
  }
  response->channelNumber = static_cast<int8_t>(channel);
  return ::ndk::ScopedAStatus::ok();
}

int SecureElement::closeChannelLocked(uint8_t channel) {
  if (channel >= opened_channels_.size() || !opened_channels_[channel])
    return ISecureElement::FAILED;

  int status = 0;
  if (channel != kBasicChannel) {
    std::vector<uint8_t> response;
    if (!transceiveLocked({channel, 0x70, 0x80, channel, 0x00}, &response) ||
        statusFromResponse(response) != 0)
      status = ISecureElement::FAILED;
  }
  opened_channels_[channel] = false;
  --opened_channel_count_;
  if (opened_channel_count_ == 0 && !seHalDeInitLocked())
    status = ISecureElement::FAILED;
  return status;
}

::ndk::ScopedAStatus SecureElement::closeChannel(int8_t channel_number) {
  std::lock_guard guard(lock_);
  if (channel_number < 0)
    return ::ndk::ScopedAStatus::fromServiceSpecificError(
        ISecureElement::FAILED);
  const int status = closeChannelLocked(static_cast<uint8_t>(channel_number));
  return status ? ::ndk::ScopedAStatus::fromServiceSpecificError(status)
                : ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus SecureElement::reset() {
  std::shared_ptr<ISecureElementCallback> callback;
  {
    std::lock_guard guard(lock_);
    if (seHalInitLocked() != ESESTATUS_SUCCESS ||
        phNxpEse_reset() != ESESTATUS_SUCCESS)
      return ::ndk::ScopedAStatus::fromServiceSpecificError(
          ISecureElement::FAILED);
    opened_channels_.fill(false);
    opened_channel_count_ = 0;
    callback = callback_;
  }
  if (callback) callback->onStateChange(true, "PN81T P73 reset");
  return ::ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::secure_element

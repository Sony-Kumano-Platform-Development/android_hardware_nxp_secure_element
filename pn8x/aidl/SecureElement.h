#pragma once

#include <aidl/android/hardware/secure_element/BnSecureElement.h>
#include <aidl/android/hardware/secure_element/ISecureElementCallback.h>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "phNxpEse_Api.h"

namespace aidl::android::hardware::secure_element {

class SecureElement final : public BnSecureElement {
 public:
  ::ndk::ScopedAStatus closeChannel(int8_t channelNumber) override;
  ::ndk::ScopedAStatus getAtr(std::vector<uint8_t>* response) override;
  ::ndk::ScopedAStatus init(
      const std::shared_ptr<ISecureElementCallback>& callback) override;
  ::ndk::ScopedAStatus isCardPresent(bool* present) override;
  ::ndk::ScopedAStatus openBasicChannel(
      const std::vector<uint8_t>& aid, int8_t p2,
      std::vector<uint8_t>* response) override;
  ::ndk::ScopedAStatus openLogicalChannel(
      const std::vector<uint8_t>& aid, int8_t p2,
      LogicalChannelResponse* response) override;
  ::ndk::ScopedAStatus reset() override;
  ::ndk::ScopedAStatus transmit(const std::vector<uint8_t>& command,
                                std::vector<uint8_t>* response) override;

 private:
  bool isInitializedLocked() const;
  ESESTATUS seHalInitLocked();
  bool seHalDeInitLocked();
  bool transceiveLocked(const std::vector<uint8_t>& command,
                        std::vector<uint8_t>* response);
  int closeChannelLocked(uint8_t channel);
  static void onLoaderServiceCompleted(bool result, std::string reason,
                                       void* context);
  void notifyLoaderCompletion(bool result, const char* reason);

  std::mutex lock_;
  std::shared_ptr<ISecureElementCallback> callback_;
  std::array<bool, 4> opened_channels_{};
  uint8_t opened_channel_count_ = 0;
  bool initializing_ = false;
};

}  // namespace aidl::android::hardware::secure_element

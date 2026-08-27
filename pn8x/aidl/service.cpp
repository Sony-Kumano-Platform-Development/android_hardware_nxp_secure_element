#define LOG_TAG "P73SecureElement"
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>

#include "SecureElement.h"

using aidl::android::hardware::secure_element::SecureElement;

int main() {
  ABinderProcess_setThreadPoolMaxThreadCount(1);
  ABinderProcess_startThreadPool();

  auto service = ndk::SharedRefBase::make<SecureElement>();
  const std::string name = std::string(SecureElement::descriptor) + "/eSE1";
  if (AServiceManager_addService(service->asBinder().get(), name.c_str()) !=
      STATUS_OK) {
    ALOGE("failed to register %s", name.c_str());
    return 1;
  }
  ABinderProcess_joinThreadPool();
  return 1;
}

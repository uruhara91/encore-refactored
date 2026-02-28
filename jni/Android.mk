LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := encored

LOCAL_C_INCLUDES := \
	$(LOCAL_PATH)/include \
    $(LOCAL_PATH)/external/rapidjson/include \
    $(LOCAL_PATH)/external/spdlog/include

LOCAL_STATIC_LIBRARIES := EncoreCLI GameRegistry EncoreConfig EncoreUtility DeviceInfo EncoreCustomLogic

LOCAL_SRC_FILES := Main.cpp

LOCAL_CPPFLAGS += -fexceptions -fvisibility=hidden -std=c++23 -Oz -flto
LOCAL_CPPFLAGS += -Wpedantic -Wall -Wextra -Werror -Wformat -Wuninitialized

LOCAL_LDFLAGS += -Oz -flto -Wl,--gc-sections -Wl,--icf=safe

include $(BUILD_EXECUTABLE)

include $(LOCAL_PATH)/src/Android.mk

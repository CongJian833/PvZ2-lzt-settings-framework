LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := settingsframework

# C++标准
LOCAL_CPPFLAGS := -std=c++1z -fexceptions -frtti -Wall -Wextra -Wno-unused-parameter

# 源文件（ARM64需要And64InlineHook，ARM32不需要）
LOCAL_SRC_FILES := lzt_settings_framework.cpp \
    lzt_core.cpp \
    resource_file.cpp \
    language_types.cpp \
    language_types_rton.cpp \
    language_module.cpp \
    view_angle_module.cpp \
    screen_bindings.cpp \
    cdn_load_rton.cpp \
    settings/settings_framework.cpp \
    settings/settings_widgets.cpp \
    settings/localizer.cpp \
    settings/table_loader.cpp \
    settings/game_abi.cpp \
    components/component_patches.cpp

ifeq ($(TARGET_ARCH),arm64)
    LOCAL_SRC_FILES += And64InlineHook.cpp resource_file_arm64.S
endif

# 链接库
# -llog 提供 __android_log_print
# -landroid 提供 AConfiguration API（用于获取屏幕密度计算 dp）
LOCAL_LDLIBS := -llog -landroid

include $(BUILD_SHARED_LIBRARY)

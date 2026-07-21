# RSDKv5 (Sonic Mania) on OXDK + RXDK-SDL2x.
#
# Builds the RSDKv5 decomp against the Xbox XDK via clang/lld.
# Set OXDK_DIR to your OXDK checkout and XDK_DIR to your XDK libs/headers.

OXDK_DIR ?= dependencies/xbox/oxdk
XBE_TITLE = RSDKv5
XBE_MODE  = DEBUG

OXDK_LIBCXX = 1

include $(OXDK_DIR)/third-party/SDL2x/oxdk/sdl2x.mk

SRCS  = RSDKv5/main.cpp \
        RSDKv5/RSDK/Core/RetroEngine.cpp \
        RSDKv5/RSDK/Core/Math.cpp \
        RSDKv5/RSDK/Core/Reader.cpp \
        RSDKv5/RSDK/Core/Link.cpp \
        RSDKv5/RSDK/Core/ModAPI.cpp \
        RSDKv5/RSDK/Dev/Debug.cpp \
        RSDKv5/RSDK/Storage/Storage.cpp \
        RSDKv5/RSDK/Storage/Text.cpp \
        RSDKv5/RSDK/Graphics/Drawing.cpp \
        RSDKv5/RSDK/Graphics/Scene3D.cpp \
        RSDKv5/RSDK/Graphics/Animation.cpp \
        RSDKv5/RSDK/Graphics/Sprite.cpp \
        RSDKv5/RSDK/Graphics/Palette.cpp \
        RSDKv5/RSDK/Graphics/Video.cpp \
        RSDKv5/RSDK/Audio/Audio.cpp \
        RSDKv5/RSDK/Input/Input.cpp \
        RSDKv5/RSDK/Scene/Scene.cpp \
        RSDKv5/RSDK/Scene/Collision.cpp \
        RSDKv5/RSDK/Scene/Object.cpp \
        RSDKv5/RSDK/Scene/Objects/DefaultObject.cpp \
        RSDKv5/RSDK/Scene/Objects/DevOutput.cpp \
        RSDKv5/RSDK/User/Core/UserAchievements.cpp \
        RSDKv5/RSDK/User/Core/UserCore.cpp \
        RSDKv5/RSDK/User/Core/UserLeaderboards.cpp \
        RSDKv5/RSDK/User/Core/UserPresence.cpp \
        RSDKv5/RSDK/User/Core/UserStats.cpp \
        RSDKv5/RSDK/User/Core/UserStorage.cpp \
        Game/SonicMania/Game.c \
        Game/SonicMania/Objects/All.c \
        dependencies/all/tinyxml2/tinyxml2.cpp \
        dependencies/all/iniparser/iniparser.cpp \
        dependencies/all/iniparser/dictionary.cpp \
        dependencies/all/miniz/miniz.c

SRCS += $(SDL2X_SRC_LIST)
SRCS += $(OXDK_DIR)/oxdk/libcxx-shim/libcxx_runtime.cpp
SRCS += dependencies/xbox/stubs/math_stubs.c

RSDK_DEFINES  = -DRETRO_REVISION=3 -DGAME_VERSION=6 \
                -DRETRO_STANDALONE=0 -DRETRO_USE_MOD_LOADER=0 -DRSDK_AUTOBUILD \
                -DMINIZ_NO_ARCHIVE_WRITING_APIS -DMINIZ_NO_STDIO -DMINIZ_NO_TIME \
                -DSDL_MAIN_HANDLED -D__PRFCHWINTRIN_H \
                -Dsinf=sin -Dcosf=cos \
                -Dfminf=fmin -Dfmaxf=fmax \
                -Dsnprintf=_snprintf -Dvsnprintf=_vsnprintf

RSDK_INCLUDES = -IRSDKv5 -IGame/SonicMania -IGame/SonicMania/Objects \
                -Idependencies/all -Idependencies/all/tinyxml2 \
                -Idependencies/all/iniparser -Idependencies/all/stb_vorbis \
                -Idependencies/xbox/stubs

RSDK_FLAGS = -fsigned-char -fpermissive -Wno-incompatible-pointer-types

CFLAGS   += $(SDL2X_DEFINES) $(SDL2X_INCLUDES) $(RSDK_DEFINES) $(RSDK_INCLUDES) $(RSDK_FLAGS)
CXXFLAGS += -std=c++17 $(SDL2X_DEFINES) $(SDL2X_INCLUDES) $(RSDK_DEFINES) $(RSDK_INCLUDES) $(RSDK_FLAGS)

OXDK_LIBS = $(SDL2X_LIBS)

LDFLAGS += /alternatename:_tanf@4=_tanf /alternatename:_asinf@4=_asinf

include $(OXDK_DIR)/oxdk.mk

$(OUTPUT_DIR)/default.xbe: $(OUTPUT_DIR)/Data.rsdk $(OUTPUT_DIR)/Settings.ini

$(OUTPUT_DIR)/Data.rsdk: Data.rsdk | $(OUTPUT_DIR)
	cp "$<" "$@"

$(OUTPUT_DIR)/Settings.ini: xbox/Settings.ini | $(OUTPUT_DIR)
	cp "$<" "$@"

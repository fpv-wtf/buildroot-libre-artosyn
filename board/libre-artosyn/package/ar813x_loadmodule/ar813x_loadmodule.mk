################################################################################
#
# ar813x_loadmodule
#
################################################################################

AR813X_LOADMODULE_VERSION = 1.0
AR813X_LOADMODULE_SITE = $(BR2_EXTERNAL_LIBRE_ARTOSYN_PATH)/package/ar813x_loadmodule
AR813X_LOADMODULE_SITE_METHOD = local


define AR813X_LOADMODULE_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/ar813x_loadmodule $(@D)/ar813x_loadmodule.c -lcjson -lcjson_utils
endef

define AR813X_LOADMODULE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/ar813x_loadmodule \
		$(TARGET_DIR)/usr/bin/ar813x_loadmodule
endef

$(eval $(generic-package))
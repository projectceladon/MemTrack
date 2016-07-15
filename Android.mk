ifneq ($(filter sofia3gr, $(TARGET_BOARD_PLATFORM)),)
    include $(call all-named-subdir-makefiles, mali)
else
ifneq ($(filter gsd, $(TARGET_BOARD_PLATFORM)),)
    include $(call all-named-subdir-makefiles, mali)
else
ifneq ($(filter sofia_lte, $(TARGET_BOARD_PLATFORM)),)
    include $(call all-named-subdir-makefiles, mali-midgard)
else
    include $(call all-named-subdir-makefiles, gen)
endif
endif

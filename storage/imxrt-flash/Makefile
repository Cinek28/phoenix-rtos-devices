#
# Phoenix-RTOS
#
# flash driver
#
# Copyright 2019 Phoenix Systems
# Author: Hubert Buczynski
#

NAME := libflashdrv-imxrt
LOCAL_SRCS := flashdrv.c rom_api.c flash_config.c

include $(static-lib.mk)

NAME := imxrt-flash
LOCAL_SRCS := flashsrv.c
LOCAL_HEADERS := imxrt-flashsrv.h
DEP_LIBS := libflashdrv-imxrt
LIBS := libmeterfs libptable

include $(binary.mk)

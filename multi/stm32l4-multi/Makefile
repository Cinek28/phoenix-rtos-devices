#
# Makefile for Phoenix-RTOS stm32-multi
#
# Copyright 2018, 2020 Phoenix Systems
#

LOCAL_PATH := $(call my-dir)

NAME := libstm32l4-multi
SRCS := $(wildcard $(LOCAL_PATH)libmulti/*.c)
HEADERS := $(wildcard $(LOCAL_PATH)libmulti/*.h)
DEPS := libtty

include $(static-lib.mk)

NAME := stm32-multi
SRCS := $(wildcard $(LOCAL_PATH)*.c)
LOCAL_HEADERS := stm32-multi.h
DEP_LIBS := libstm32l4-multi libtty

include $(binary.mk)

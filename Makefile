# BeatStepSync — VCV Rack 2 Plugin Makefile
RACK_DIR ?= $(HOME)/Rack2SDK

SOURCES  = src/plugin.cpp
SOURCES += src/BeatStepSeq.cpp

FLAGS   += -pthread
LDFLAGS += -pthread

DISTRIBUTABLES += $(wildcard LICENSE*)

include $(RACK_DIR)/plugin.mk

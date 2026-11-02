# 机床工位边缘安全采集网关
#
# 交叉编译（默认）：make
# 指定编译器：      make CC=/opt/gcc-arm/bin/arm-linux-gnueabihf-gcc
# 带 MQTT：         make MQTT=1 PAHO_INC=... PAHO_LIB=...
# PC 上试跑：       make host

CC      ?= arm-linux-gnueabihf-gcc
TARGET  := gateway

SRCS    := src/main.c \
           src/mailbox.c \
           src/modbus.c \
           src/canbus.c \
           src/sensor.c \
           src/store.c \
           src/alarm.c \
           src/ui.c \
           src/upload.c \
           lib/framebuffer.c \
           lib/font_8x16.c

INC      := -Iinclude -Ilib -Iinstall/include
CFLAGS   := -Wall -O2 -g $(INC) -DGW_HAVE_MQTT=0 -finput-charset=UTF-8 -fexec-charset=UTF-8
LDFLAGS  := -Linstall/lib -lsqlite3 -lpthread

ifeq ($(MQTT),1)
CFLAGS  += -DGW_HAVE_MQTT=1 $(if $(PAHO_INC),-I$(PAHO_INC),)
LDFLAGS += $(if $(PAHO_LIB),-L$(PAHO_LIB),) -lpaho-mqtt3c
endif

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(SRCS) -o $@ $(CFLAGS) $(LDFLAGS)
	@echo "---- 编译完成: $(TARGET) ----"

host: $(SRCS)
	cc $(SRCS) -o gateway_host $(CFLAGS) -lsqlite3 -lpthread
	@echo "---- 生成 PC 版: gateway_host（设备打不开属正常） ----"

clean:
	rm -f $(TARGET) gateway_host

.PHONY: all host clean


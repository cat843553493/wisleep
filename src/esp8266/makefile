PROGRAM=esp_wisleep

EXTRA_COMPONENTS=extras/stdin_uart_interrupt extras/dhcpserver

PROGRAM_ROOT=./../..
PROGRAM_SRC_DIR=./../..
PROGRAM_SRC_FILES = \
	esp_wisleep.c \
	server.c \
	server_actions.c \
	fs.c \
	octet_spiflash.c \
	systasks.c \
	bridge_esp.c \
	ntp.c \
	udputil.c \
	../umac/umac.c \
	../uweb/src/uweb.c \
	../uweb/src/uweb_codec.c \
	../../spiffs/src/spiffs_nucleus.c \
	../../spiffs/src/spiffs_hydrogen.c \
	../../spiffs/src/spiffs_gc.c \
	../../spiffs/src/spiffs_check.c \
	../../spiffs/src/spiffs_cache.c

include ../../esp-open-rtos/common.mk

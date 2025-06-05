# Makefile
CC=gcc
CFLAGS=
LDFLAGS=

TARGET_SERVER=server
TARGET_SENSOR=sensor
COMMON_OBJ=common.o

all: $(TARGET_SERVER) $(TARGET_SENSOR)

$(TARGET_SERVER): server.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_SENSOR): sensor.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c common.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o $(TARGET_SERVER) $(TARGET_SENSOR)

# Compilar sensor e server com o comando 'make' sem parâmetros adicionais [cite: 135]
.PHONY: all clean

test: clean all

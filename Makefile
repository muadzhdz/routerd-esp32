# routerd-esp32 Makefile
FQBN ?= esp32:esp32:esp32
PORT ?= /dev/ttyUSB0
BAUD ?= 115200

all: compile upload

compile:
	arduino-cli compile --fqbn $(FQBN) .

upload:
	arduino-cli upload -p $(PORT) --fqbn $(FQBN) .

monitor:
	arduino-cli monitor -p $(PORT) -c baudrate=$(BAUD)

clean:
	rm -rf build

.PHONY: all compile upload monitor clean

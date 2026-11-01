# Desktop tests, run before anything is flashed.
#
# Two suites:
#
#   opt_tests    the tag codec, against the binary fixtures published by the
#                specification. No Arduino dependencies at all.
#   logic_tests  the firmware modules that do real work -- the HX711 driver,
#                the weight maths, the spool state, the presets -- compiled
#                against the small Arduino simulator in host/.
#
#   make test      build and run both
#   make asan      run the codec suite under address/UB sanitizers
#   make dump_tag  build the decoder for tag images saved off the device
#   make clean

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Werror

FW_DIR := ../firmware/OpenPrintTagDryer
CODEC_DIR := $(FW_DIR)/src/openprinttag

CODEC_SOURCES := $(CODEC_DIR)/opt_cbor.cpp $(CODEC_DIR)/opt_tag.cpp
CODEC_HEADERS := $(CODEC_DIR)/opt_cbor.h $(CODEC_DIR)/opt_tag.h \
                 $(CODEC_DIR)/opt_enums.h

OPT_SOURCES := test_opt.cpp $(CODEC_SOURCES)
OPT_HEADERS := $(CODEC_HEADERS) fixture_expectations.h

# The reader is included so the timeout paths get exercised: the SPI simulator
# answers 0xFF and holds BUSY high, which is what an absent PN5180 looks like.
LOGIC_SOURCES := test_logic.cpp host/host_arduino.cpp host/host_webserver.cpp \
                 host/host_http.cpp \
                 $(CODEC_SOURCES) \
                 $(FW_DIR)/src/ScaleManager.cpp $(FW_DIR)/src/Ds18b20.cpp \
                 $(FW_DIR)/src/SpoolState.cpp \
                 $(FW_DIR)/src/PrusamentPresets.cpp \
                 $(FW_DIR)/src/WriteBackPolicy.cpp $(FW_DIR)/src/HaMqtt.cpp \
                 $(FW_DIR)/src/Settings.cpp $(FW_DIR)/src/App.cpp \
                 $(FW_DIR)/src/SpoolmanClient.cpp \
                 $(FW_DIR)/src/WebPortal.cpp $(FW_DIR)/src/DriftLog.cpp \
                 $(FW_DIR)/src/NfcManager.cpp $(FW_DIR)/src/Iso15693Reader.cpp

# Upstream code, compiled without -Werror: it is not ours to clean up, and the
# point of vendoring it is that it stays close to the version it was taken from.
VENDOR_SOURCES := $(FW_DIR)/src/pn5180/PN5180.cpp \
                  $(FW_DIR)/src/pn5180/PN5180ISO15693.cpp \
                  $(FW_DIR)/src/pn5180/Debug.cpp

LOGIC_HEADERS := $(CODEC_HEADERS) host/Arduino.h host/Preferences.h \
                 host/Wire.h host/SPI.h host/Client.h host/PubSubClient.h \
                 host/WebServer.h host/Update.h host/HTTPClient.h \
                 host/WiFiClient.h $(FW_DIR)/config.h

# -Ihost puts the simulator ahead of any real Arduino headers.
LOGIC_FLAGS := -Ihost

.PHONY: test asan clean regenerate


test: opt_tests logic_tests
	./opt_tests
	./logic_tests

opt_tests: $(OPT_SOURCES) $(OPT_HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ $(OPT_SOURCES)

logic_tests: $(LOGIC_SOURCES) $(VENDOR_SOURCES) $(LOGIC_HEADERS)
	$(CXX) $(CXXFLAGS) $(LOGIC_FLAGS) -c $(VENDOR_SOURCES) \
		-Wno-error -Wno-unused-variable -Wno-narrowing
	$(CXX) $(CXXFLAGS) $(LOGIC_FLAGS) -o $@ $(LOGIC_SOURCES) \
		PN5180.o PN5180ISO15693.o Debug.o
	rm -f PN5180.o PN5180ISO15693.o Debug.o

# Not part of `make test`: it takes a tag image as an argument rather than
# asserting anything by itself.
dump_tag: dump_tag.cpp $(CODEC_SOURCES) $(CODEC_HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ dump_tag.cpp $(CODEC_SOURCES)

# Malformed tag data is untrusted input, so the parser gets checked for reads
# past the end of a buffer as well as for correctness. Needs the sanitizer
# runtimes (Fedora: dnf install libasan libubsan; Debian: they ship with g++).
asan: $(OPT_SOURCES) $(OPT_HEADERS)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
		-o opt_tests_asan $(OPT_SOURCES)
	./opt_tests_asan

# Refresh the generated tables after updating spec-data/ or fixtures/.
regenerate:
	cd .. && python3 tools/gen_enums.py && python3 tools/gen_fixture_expectations.py

clean:
	rm -f opt_tests opt_tests_asan logic_tests dump_tag

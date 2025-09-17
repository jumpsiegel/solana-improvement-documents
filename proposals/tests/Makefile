CC=gcc
CXX=g++
CFLAGS=-Wall -Wextra -O2 -std=c99
CXXFLAGS=-Wall -Wextra -O2 -std=c++17
LIBS=-lssl -lcrypto -llz4
TARGET_GEN=generate_example_snapshot_streaming
TARGET_VERIFY=verify_c_snapshot
TARGET_STRUCT_TEST=test_c_struct_sizes
TARGET_CPP_GEN=generate_cpp_snapshot
TARGET_CPP_VERIFY=verify_cpp_snapshot
TARGET_CPP_STRUCT_TEST=test_cpp_struct_sizes
SOURCE_GEN=generate_example_snapshot_streaming.c fd_lthash_standalone.c
SOURCE_VERIFY=verify_c_snapshot.c fd_lthash_standalone.c
SOURCE_STRUCT_TEST=test_c_struct_sizes.c
SOURCE_CPP_GEN=generate_cpp_snapshot.cpp
SOURCE_CPP_VERIFY=verify_cpp_snapshot.cpp
SOURCE_CPP_STRUCT_TEST=test_cpp_struct_sizes.cpp

all: $(TARGET_GEN) $(TARGET_VERIFY) $(TARGET_STRUCT_TEST) $(TARGET_CPP_GEN) $(TARGET_CPP_VERIFY) $(TARGET_CPP_STRUCT_TEST)

$(TARGET_GEN): $(SOURCE_GEN)
	$(CC) $(CFLAGS) -o $(TARGET_GEN) $(SOURCE_GEN) $(LIBS)

$(TARGET_VERIFY): $(SOURCE_VERIFY)
	$(CC) $(CFLAGS) -o $(TARGET_VERIFY) $(SOURCE_VERIFY) $(LIBS)

$(TARGET_STRUCT_TEST): $(SOURCE_STRUCT_TEST)
	$(CC) $(CFLAGS) -o $(TARGET_STRUCT_TEST) $(SOURCE_STRUCT_TEST)

$(TARGET_CPP_GEN): $(SOURCE_CPP_GEN)
	$(CXX) $(CXXFLAGS) -o $(TARGET_CPP_GEN) $(SOURCE_CPP_GEN) $(LIBS)

$(TARGET_CPP_VERIFY): $(SOURCE_CPP_VERIFY)
	$(CXX) $(CXXFLAGS) -o $(TARGET_CPP_VERIFY) $(SOURCE_CPP_VERIFY) $(LIBS)

$(TARGET_CPP_STRUCT_TEST): $(SOURCE_CPP_STRUCT_TEST)
	$(CXX) $(CXXFLAGS) -o $(TARGET_CPP_STRUCT_TEST) $(SOURCE_CPP_STRUCT_TEST)

clean:
	rm -f $(TARGET_GEN) $(TARGET_VERIFY) $(TARGET_STRUCT_TEST) $(TARGET_CPP_GEN) $(TARGET_CPP_VERIFY) $(TARGET_CPP_STRUCT_TEST) cold_snapshot_chunk_*.bin rust_*_cold_snapshot_chunk_*.bin cpp_*_cold_snapshot_chunk_*.bin

generate: $(TARGET_GEN)
	./$(TARGET_GEN)

verify: $(TARGET_VERIFY)
	./$(TARGET_VERIFY) --quick *.bin

test_structs: $(TARGET_STRUCT_TEST)
	./$(TARGET_STRUCT_TEST)

generate_cpp: $(TARGET_CPP_GEN)
	./$(TARGET_CPP_GEN)

verify_cpp: $(TARGET_CPP_VERIFY)
	./$(TARGET_CPP_VERIFY)

test_cpp_structs: $(TARGET_CPP_STRUCT_TEST)
	./$(TARGET_CPP_STRUCT_TEST)

.PHONY: all clean generate verify test_structs generate_cpp verify_cpp test_cpp_structs
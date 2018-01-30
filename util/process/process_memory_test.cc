// Copyright 2017 The Crashpad Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "util/process/process_memory.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <memory>

#include "gtest/gtest.h"
#include "test/errors.h"
#include "test/multiprocess.h"
#include "test/multiprocess_exec.h"
#include "test/process_type.h"
#include "util/file/file_io.h"
#include "util/misc/from_pointer_cast.h"
#include "util/posix/scoped_mmap.h"
#include "util/process/process_memory_native.h"

namespace crashpad {
namespace test {
namespace {

void DoChildReadTestSetup(size_t* region_size,
                          std::unique_ptr<char[]>* region) {
  *region_size = 4 * getpagesize();
  region->reset(new char[*region_size]);
  for (size_t index = 0; index < *region_size; ++index) {
    (*region)[index] = index % 256;
  }
}

CRASHPAD_CHILD_TEST_MAIN(ReadTestChild) {
  size_t region_size;
  std::unique_ptr<char[]> region;
  DoChildReadTestSetup(&region_size, &region);
  FileHandle out = StdioFileHandle(StdioStream::kStandardOutput);
  CheckedWriteFile(out, &region_size, sizeof(region_size));
  VMAddress address = FromPointerCast<VMAddress>(region.get());
  CheckedWriteFile(out, &address, sizeof(address));
  CheckedReadFileAtEOF(StdioFileHandle(StdioStream::kStandardInput));
  return 0;
}

class ReadTest : public MultiprocessExec {
 public:
  ReadTest() : MultiprocessExec() {
    SetChildTestMainFunction("ReadTestChild");
  }

  void RunAgainstSelf() {
    size_t region_size;
    std::unique_ptr<char[]> region;
    DoChildReadTestSetup(&region_size, &region);
    DoTest(GetSelfProcess(),
           region_size,
           FromPointerCast<VMAddress>(region.get()));
  }

  void RunAgainstChild() { Run(); }

 private:
  void MultiprocessParent() override {
    size_t region_size;
    VMAddress region;
    ASSERT_TRUE(
        ReadFileExactly(ReadPipeHandle(), &region_size, sizeof(region_size)));
    ASSERT_TRUE(ReadFileExactly(ReadPipeHandle(), &region, sizeof(region)));
    DoTest(ChildProcess(), region_size, region);
  }

  void DoTest(ProcessType process, size_t region_size, VMAddress address) {
    ProcessMemoryNative memory;
    ASSERT_TRUE(memory.Initialize(process));

    std::unique_ptr<char[]> result(new char[region_size]);

    // Ensure that the entire region can be read.
    ASSERT_TRUE(memory.Read(address, region_size, result.get()));
    for (size_t i = 0; i < region_size; ++i) {
      EXPECT_EQ(result[i], static_cast<char>(i % 256));
    }

    // Ensure that a read of length 0 succeeds and doesn’t touch the result.
    memset(result.get(), '\0', region_size);
    ASSERT_TRUE(memory.Read(address, 0, result.get()));
    for (size_t i = 0; i < region_size; ++i) {
      EXPECT_EQ(result[i], 0);
    }

    // Ensure that a read starting at an unaligned address works.
    ASSERT_TRUE(memory.Read(address + 1, region_size - 1, result.get()));
    for (size_t i = 0; i < region_size - 1; ++i) {
      EXPECT_EQ(result[i], static_cast<char>((i + 1) % 256));
    }

    // Ensure that a read ending at an unaligned address works.
    ASSERT_TRUE(memory.Read(address, region_size - 1, result.get()));
    for (size_t i = 0; i < region_size - 1; ++i) {
      EXPECT_EQ(result[i], static_cast<char>(i % 256));
    }

    // Ensure that a read starting and ending at unaligned addresses works.
    ASSERT_TRUE(memory.Read(address + 1, region_size - 2, result.get()));
    for (size_t i = 0; i < region_size - 2; ++i) {
      EXPECT_EQ(result[i], static_cast<char>((i + 1) % 256));
    }

    // Ensure that a read of exactly one page works.
    size_t page_size = getpagesize();
    ASSERT_GE(region_size, page_size + page_size);
    ASSERT_TRUE(memory.Read(address + page_size, page_size, result.get()));
    for (size_t i = 0; i < page_size; ++i) {
      EXPECT_EQ(result[i], static_cast<char>((i + page_size) % 256));
    }

    // Ensure that reading exactly a single byte works.
    result[1] = 'J';
    ASSERT_TRUE(memory.Read(address + 2, 1, result.get()));
    EXPECT_EQ(result[0], 2);
    EXPECT_EQ(result[1], 'J');
  }

  DISALLOW_COPY_AND_ASSIGN(ReadTest);
};

TEST(ProcessMemory, ReadSelf) {
  ReadTest test;
  test.RunAgainstSelf();
}

TEST(ProcessMemory, ReadChild) {
  ReadTest test;
  test.RunAgainstChild();
}

constexpr char kConstCharEmpty[] = "";
constexpr char kConstCharShort[] = "A short const char[]";

#define SHORT_LOCAL_STRING "A short local variable char[]"

std::string MakeLongString() {
  std::string long_string;
  const size_t kStringLongSize = 4 * getpagesize();
  for (size_t index = 0; index < kStringLongSize; ++index) {
    long_string.push_back((index % 255) + 1);
  }
  EXPECT_EQ(long_string.size(), kStringLongSize);
  return long_string;
}

void DoChildCStringReadTestSetup(const char** const_empty,
                                 const char** const_short,
                                 const char** local_empty,
                                 const char** local_short,
                                 std::string* long_string) {
  *const_empty = kConstCharEmpty;
  *const_short = kConstCharShort;
  *local_empty = "";
  *local_short = SHORT_LOCAL_STRING;
  *long_string = MakeLongString();
}

CRASHPAD_CHILD_TEST_MAIN(ReadCStringTestChild) {
  const char* const_empty;
  const char* const_short;
  const char* local_empty;
  const char* local_short;
  std::string long_string;
  DoChildCStringReadTestSetup(
      &const_empty, &const_short, &local_empty, &local_short, &long_string);
  const auto write_address = [](const char* p) {
    VMAddress address = FromPointerCast<VMAddress>(p);
    CheckedWriteFile(StdioFileHandle(StdioStream::kStandardOutput),
                     &address,
                     sizeof(address));
  };
  write_address(const_empty);
  write_address(const_short);
  write_address(local_empty);
  write_address(local_short);
  write_address(long_string.c_str());
  CheckedReadFileAtEOF(StdioFileHandle(StdioStream::kStandardInput));
  return 0;
}

class ReadCStringTest : public MultiprocessExec {
 public:
  ReadCStringTest(bool limit_size)
      : MultiprocessExec(), limit_size_(limit_size) {
    SetChildTestMainFunction("ReadCStringTestChild");
  }

  void RunAgainstSelf() {
    const char* const_empty;
    const char* const_short;
    const char* local_empty;
    const char* local_short;
    std::string long_string;
    DoChildCStringReadTestSetup(
        &const_empty, &const_short, &local_empty, &local_short, &long_string);
    DoTest(GetSelfProcess(),
           FromPointerCast<VMAddress>(const_empty),
           FromPointerCast<VMAddress>(const_short),
           FromPointerCast<VMAddress>(local_empty),
           FromPointerCast<VMAddress>(local_short),
           FromPointerCast<VMAddress>(long_string.c_str()));
  }
  void RunAgainstChild() { Run(); }

 private:
  void MultiprocessParent() override {
#define DECLARE_AND_READ_ADDRESS(name) \
  VMAddress name;                      \
  ASSERT_TRUE(ReadFileExactly(ReadPipeHandle(), &name, sizeof(name)));
    DECLARE_AND_READ_ADDRESS(const_empty_address);
    DECLARE_AND_READ_ADDRESS(const_short_address);
    DECLARE_AND_READ_ADDRESS(local_empty_address);
    DECLARE_AND_READ_ADDRESS(local_short_address);
    DECLARE_AND_READ_ADDRESS(long_string_address);
#undef DECLARE_AND_READ_ADDRESS

    DoTest(ChildProcess(),
           const_empty_address,
           const_short_address,
           local_empty_address,
           local_short_address,
           long_string_address);
  }

  void DoTest(ProcessType process,
              VMAddress const_empty_address,
              VMAddress const_short_address,
              VMAddress local_empty_address,
              VMAddress local_short_address,
              VMAddress long_string_address) {
    ProcessMemoryNative memory;
    ASSERT_TRUE(memory.Initialize(process));

    std::string result;

    if (limit_size_) {
      ASSERT_TRUE(memory.ReadCStringSizeLimited(
          const_empty_address, arraysize(kConstCharEmpty), &result));
      EXPECT_EQ(result, kConstCharEmpty);

      ASSERT_TRUE(memory.ReadCStringSizeLimited(
          const_short_address, arraysize(kConstCharShort), &result));
      EXPECT_EQ(result, kConstCharShort);
      EXPECT_FALSE(memory.ReadCStringSizeLimited(
          const_short_address, arraysize(kConstCharShort) - 1, &result));

      ASSERT_TRUE(
          memory.ReadCStringSizeLimited(local_empty_address, 1, &result));
      EXPECT_EQ(result, "");

      ASSERT_TRUE(memory.ReadCStringSizeLimited(
          local_short_address, strlen(SHORT_LOCAL_STRING) + 1, &result));
      EXPECT_EQ(result, SHORT_LOCAL_STRING);
      EXPECT_FALSE(memory.ReadCStringSizeLimited(
          local_short_address, strlen(SHORT_LOCAL_STRING), &result));

      std::string long_string_for_comparison = MakeLongString();
      ASSERT_TRUE(memory.ReadCStringSizeLimited(
          long_string_address, long_string_for_comparison.size() + 1, &result));
      EXPECT_EQ(result, long_string_for_comparison);
      EXPECT_FALSE(memory.ReadCStringSizeLimited(
          long_string_address, long_string_for_comparison.size(), &result));
    } else {
      ASSERT_TRUE(memory.ReadCString(const_empty_address, &result));
      EXPECT_EQ(result, kConstCharEmpty);

      ASSERT_TRUE(memory.ReadCString(const_short_address, &result));
      EXPECT_EQ(result, kConstCharShort);

      ASSERT_TRUE(memory.ReadCString(local_empty_address, &result));
      EXPECT_EQ(result, "");

      ASSERT_TRUE(memory.ReadCString(local_short_address, &result));
      EXPECT_EQ(result, SHORT_LOCAL_STRING);

      ASSERT_TRUE(memory.ReadCString(long_string_address, &result));
      EXPECT_EQ(result, MakeLongString());
    }
  }

  const bool limit_size_;

  DISALLOW_COPY_AND_ASSIGN(ReadCStringTest);
};

TEST(ProcessMemory, ReadCStringSelf) {
  ReadCStringTest test(/* limit_size= */ false);
  test.RunAgainstSelf();
}

TEST(ProcessMemory, ReadCStringChild) {
  ReadCStringTest test(/* limit_size= */ false);
  test.RunAgainstChild();
}

TEST(ProcessMemory, ReadCStringSizeLimitedSelf) {
  ReadCStringTest test(/* limit_size= */ true);
  test.RunAgainstSelf();
}

TEST(ProcessMemory, ReadCStringSizeLimitedChild) {
  ReadCStringTest test(/* limit_size= */ true);
  test.RunAgainstChild();
}

// TODO(scottmg): Need to be ported to MultiprocessExec and not rely on fork().
#if !defined(OS_FUCHSIA)

class TargetProcessTest : public Multiprocess {
 public:
  TargetProcessTest() : Multiprocess() {}
  ~TargetProcessTest() {}

  void RunAgainstSelf() { DoTest(getpid()); }

  void RunAgainstForked() { Run(); }

 private:
  void MultiprocessParent() override { DoTest(ChildPID()); }

  void MultiprocessChild() override { CheckedReadFileAtEOF(ReadPipeHandle()); }

  virtual void DoTest(pid_t pid) = 0;

  DISALLOW_COPY_AND_ASSIGN(TargetProcessTest);
};

bool ReadCString(const ProcessMemory& memory,
                 const char* pointer,
                 std::string* result) {
  return memory.ReadCString(FromPointerCast<VMAddress>(pointer), result);
}

bool ReadCStringSizeLimited(const ProcessMemory& memory,
                            const char* pointer,
                            size_t size,
                            std::string* result) {
  return memory.ReadCStringSizeLimited(
      FromPointerCast<VMAddress>(pointer), size, result);
}

class ReadUnmappedTest : public TargetProcessTest {
 public:
  ReadUnmappedTest()
      : TargetProcessTest(),
        page_size_(getpagesize()),
        region_size_(2 * page_size_),
        result_(new char[region_size_]) {
    if (!pages_.ResetMmap(nullptr,
                          region_size_,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS,
                          -1,
                          0)) {
      ADD_FAILURE();
      return;
    }

    char* region = pages_.addr_as<char*>();
    for (size_t index = 0; index < region_size_; ++index) {
      region[index] = index % 256;
    }

    EXPECT_TRUE(pages_.ResetAddrLen(region, page_size_));
  }

 private:
  void DoTest(pid_t pid) override {
    ProcessMemoryNative memory;
    ASSERT_TRUE(memory.Initialize(pid));

    VMAddress page_addr1 = pages_.addr_as<VMAddress>();
    VMAddress page_addr2 = page_addr1 + page_size_;

    EXPECT_TRUE(memory.Read(page_addr1, page_size_, result_.get()));
    EXPECT_TRUE(memory.Read(page_addr2 - 1, 1, result_.get()));

    EXPECT_FALSE(memory.Read(page_addr1, region_size_, result_.get()));
    EXPECT_FALSE(memory.Read(page_addr2, page_size_, result_.get()));
    EXPECT_FALSE(memory.Read(page_addr2 - 1, 2, result_.get()));
  }

  ScopedMmap pages_;
  const size_t page_size_;
  const size_t region_size_;
  std::unique_ptr<char[]> result_;

  DISALLOW_COPY_AND_ASSIGN(ReadUnmappedTest);
};

TEST(ProcessMemory, ReadUnmappedSelf) {
  ReadUnmappedTest test;
  ASSERT_FALSE(testing::Test::HasFailure());
  test.RunAgainstSelf();
}

TEST(ProcessMemory, ReadUnmappedForked) {
  ReadUnmappedTest test;
  ASSERT_FALSE(testing::Test::HasFailure());
  test.RunAgainstForked();
}

class ReadCStringUnmappedTest : public TargetProcessTest {
 public:
  ReadCStringUnmappedTest(bool limit_size)
      : TargetProcessTest(),
        page_size_(getpagesize()),
        region_size_(2 * page_size_),
        limit_size_(limit_size) {
    if (!pages_.ResetMmap(nullptr,
                          region_size_,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS,
                          -1,
                          0)) {
      ADD_FAILURE();
      return;
    }

    char* region = pages_.addr_as<char*>();
    for (size_t index = 0; index < region_size_; ++index) {
      region[index] = 1 + index % 255;
    }

    // A string at the start of the mapped region
    string1_ = region;
    string1_[expected_length_] = '\0';

    // A string near the end of the mapped region
    string2_ = region + page_size_ - expected_length_ * 2;
    string2_[expected_length_] = '\0';

    // A string that crosses from the mapped into the unmapped region
    string3_ = region + page_size_ - expected_length_ + 1;
    string3_[expected_length_] = '\0';

    // A string entirely in the unmapped region
    string4_ = region + page_size_ + 10;
    string4_[expected_length_] = '\0';

    result_.reserve(expected_length_ + 1);

    EXPECT_TRUE(pages_.ResetAddrLen(region, page_size_));
  }

 private:
  void DoTest(pid_t pid) override {
    ProcessMemoryNative memory;
    ASSERT_TRUE(memory.Initialize(pid));

    if (limit_size_) {
      ASSERT_TRUE(ReadCStringSizeLimited(
          memory, string1_, expected_length_ + 1, &result_));
      EXPECT_EQ(result_, string1_);
      ASSERT_TRUE(ReadCStringSizeLimited(
          memory, string2_, expected_length_ + 1, &result_));
      EXPECT_EQ(result_, string2_);
      EXPECT_FALSE(ReadCStringSizeLimited(
          memory, string3_, expected_length_ + 1, &result_));
      EXPECT_FALSE(ReadCStringSizeLimited(
          memory, string4_, expected_length_ + 1, &result_));
    } else {
      ASSERT_TRUE(ReadCString(memory, string1_, &result_));
      EXPECT_EQ(result_, string1_);
      ASSERT_TRUE(ReadCString(memory, string2_, &result_));
      EXPECT_EQ(result_, string2_);
      EXPECT_FALSE(ReadCString(memory, string3_, &result_));
      EXPECT_FALSE(ReadCString(memory, string4_, &result_));
    }
  }

  std::string result_;
  ScopedMmap pages_;
  const size_t page_size_;
  const size_t region_size_;
  static const size_t expected_length_ = 10;
  char* string1_;
  char* string2_;
  char* string3_;
  char* string4_;
  const bool limit_size_;

  DISALLOW_COPY_AND_ASSIGN(ReadCStringUnmappedTest);
};

TEST(ProcessMemory, ReadCStringUnmappedSelf) {
  ReadCStringUnmappedTest test(/* limit_size= */ false);
  ASSERT_FALSE(testing::Test::HasFailure());
  test.RunAgainstSelf();
}

TEST(ProcessMemory, ReadCStringUnmappedForked) {
  ReadCStringUnmappedTest test(/* limit_size= */ false);
  ASSERT_FALSE(testing::Test::HasFailure());
  test.RunAgainstForked();
}

TEST(ProcessMemory, ReadCStringSizeLimitedUnmappedSelf) {
  ReadCStringUnmappedTest test(/* limit_size= */ true);
  ASSERT_FALSE(testing::Test::HasFailure());
  test.RunAgainstSelf();
}

TEST(ProcessMemory, ReadCStringSizeLimitedUnmappedForked) {
  ReadCStringUnmappedTest test(/* limit_size= */ true);
  ASSERT_FALSE(testing::Test::HasFailure());
  test.RunAgainstForked();
}

#endif  // !defined(OS_FUCHSIA)

}  // namespace
}  // namespace test
}  // namespace crashpad

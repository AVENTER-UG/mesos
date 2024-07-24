// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <utility>
#include <tuple>

#include <process/gtest.hpp>
#include <process/reap.hpp>

#include <stout/unreachable.hpp>
#include <stout/tests/utils.hpp>

#include "linux/cgroups2.hpp"
#include "slave/containerizer/device_manager/device_manager.hpp"

using mesos::internal::slave::DeviceManager;
using mesos::internal::slave::DeviceManagerProcess;

using process::Future;
using process::Owned;

using std::string;
using std::tuple;
using std::vector;

namespace devices = cgroups2::devices;

namespace mesos {
namespace internal {
namespace tests {

const string TEST_CGROUP = "test";


Try<vector<DeviceManager::NonWildcardEntry>> convert_to_non_wildcards(
    const vector<devices::Entry>& entries)
{
  vector<DeviceManager::NonWildcardEntry> non_wildcards = {};

  foreach (const devices::Entry& entry, entries) {
    if (entry.selector.has_wildcard()) {
      return Error("Entry cannot have wildcard");
    }
    DeviceManager::NonWildcardEntry non_wildcard;
    non_wildcard.access = entry.access;
    non_wildcard.selector.major = *entry.selector.major;
    non_wildcard.selector.minor = *entry.selector.minor;
    non_wildcard.selector.type = [&]() {
      switch (entry.selector.type) {
        case cgroups::devices::Entry::Selector::Type::BLOCK:
          return DeviceManager::NonWildcardEntry::Selector::Type::BLOCK;
        case cgroups::devices::Entry::Selector::Type::CHARACTER:
          return DeviceManager::NonWildcardEntry::Selector::Type::CHARACTER;
        case cgroups::devices::Entry::Selector::Type::ALL:
          UNREACHABLE();
      }
    }();
    non_wildcards.push_back(non_wildcard);
  }

  return non_wildcards;
}


class DeviceManagerTest : public TemporaryDirectoryTest
{
  void SetUp() override
  {
    TemporaryDirectoryTest::SetUp();

    // Cleanup the test cgroup, in case a previous test run didn't clean it
    // up properly.
    if (cgroups2::exists(TEST_CGROUP)) {
      AWAIT_READY(cgroups2::destroy(TEST_CGROUP));
    }
  }

  void TearDown() override
  {
    if (cgroups2::exists(TEST_CGROUP)) {
      AWAIT_READY(cgroups2::destroy(TEST_CGROUP));
    }

    TemporaryDirectoryTest::TearDown();
  }
};


TEST(NonWildcardEntry, NonWildcardFromWildcard)
{
  EXPECT_ERROR(convert_to_non_wildcards(
      vector<devices::Entry>{*devices::Entry::parse("c *:1 w")}));
}


TEST_F(DeviceManagerTest, ROOT_DeviceManagerConfigure_Normal)
{
  typedef std::pair<string, int> OpenArgs;
  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  slave::Flags flags;
  flags.work_dir = *sandbox;
  Owned<DeviceManager> dm =
    Owned<DeviceManager>(CHECK_NOTERROR(DeviceManager::create(flags)));

  vector<devices::Entry> allow_list = {*devices::Entry::parse("c 1:3 r")};
  vector<devices::Entry> deny_list = {*devices::Entry::parse("c 3:1 w")};

  AWAIT_ASSERT_READY(dm->configure(
      TEST_CGROUP,
      allow_list,
      CHECK_NOTERROR(convert_to_non_wildcards(deny_list))));

  Future<DeviceManager::CgroupDeviceAccess> cgroup_state =
    dm->state(TEST_CGROUP);

  AWAIT_ASSERT_READY(cgroup_state);
  EXPECT_EQ(allow_list, cgroup_state->allow_list);
  EXPECT_EQ(deny_list, cgroup_state->deny_list);

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // Move the child process into the newly created cgroup.
    Try<Nothing> assign = cgroups2::assign(TEST_CGROUP, ::getpid());
    if (assign.isError()) {
      SAFE_EXIT(EXIT_FAILURE, "Failed to assign child process to cgroup");
    }

    // Check that we can only do the "allowed_accesses".
    if (os::open(os::DEV_NULL, O_RDONLY).isError()) {
      SAFE_EXIT(EXIT_FAILURE, "Expected allowed read to succeed");
    }
    if (os::open(os::DEV_NULL, O_RDWR).isSome()) {
      SAFE_EXIT(EXIT_FAILURE, "Expected blocked read to fail");
    }

    ::_exit(EXIT_SUCCESS);
  }

  AWAIT_EXPECT_WEXITSTATUS_EQ(EXIT_SUCCESS, process::reap(pid));
}


TEST_F(DeviceManagerTest, ROOT_DeviceManagerReconfigure_Normal)
{
  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  slave::Flags flags;
  flags.work_dir = *sandbox;
  Owned<DeviceManager> dm =
    Owned<DeviceManager>(CHECK_NOTERROR(DeviceManager::create(flags)));

  vector<devices::Entry> allow_list = {*devices::Entry::parse("c 1:3 w")};
  vector<devices::Entry> deny_list = {*devices::Entry::parse("c 3:1 w")};

  AWAIT_ASSERT_READY(dm->configure(
      TEST_CGROUP,
      allow_list,
      CHECK_NOTERROR(convert_to_non_wildcards(deny_list))));

  Future<DeviceManager::CgroupDeviceAccess> cgroup_state =
    dm->state(TEST_CGROUP);

  AWAIT_ASSERT_READY(cgroup_state);
  EXPECT_EQ(allow_list, cgroup_state->allow_list);
  EXPECT_EQ(deny_list, cgroup_state->deny_list);

  vector<devices::Entry> additions = {*devices::Entry::parse("c 1:3 r")};
  vector<devices::Entry> removals = allow_list;

  AWAIT_ASSERT_READY(dm->reconfigure(
      TEST_CGROUP,
      CHECK_NOTERROR(convert_to_non_wildcards(additions)),
      CHECK_NOTERROR(convert_to_non_wildcards(removals))));

  cgroup_state = dm->state(TEST_CGROUP);

  AWAIT_ASSERT_READY(cgroup_state);
  EXPECT_EQ(additions, cgroup_state->allow_list);
  EXPECT_EQ(deny_list, cgroup_state->deny_list);

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // Move the child process into the newly created cgroup.
    Try<Nothing> assign = cgroups2::assign(TEST_CGROUP, ::getpid());
    if (assign.isError()) {
      SAFE_EXIT(EXIT_FAILURE, "Failed to assign child process to cgroup");
    }

    // Check that we can only do the "allowed_accesses".
    if (os::open(os::DEV_NULL, O_RDONLY).isError()) {
      SAFE_EXIT(EXIT_FAILURE, "Expected allowed read to succeed");
    }
    if (os::open(os::DEV_NULL, O_RDWR).isSome()) {
      SAFE_EXIT(EXIT_FAILURE, "Expected blocked read to fail");
    }

    ::_exit(EXIT_SUCCESS);
  }

  AWAIT_EXPECT_WEXITSTATUS_EQ(EXIT_SUCCESS, process::reap(pid));
}


TEST_F(DeviceManagerTest, ROOT_DeviceManagerConfigure_AllowMatchesDeny)
{
  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  slave::Flags flags;
  flags.work_dir = *sandbox;
  Owned<DeviceManager> dm =
    Owned<DeviceManager>(CHECK_NOTERROR(DeviceManager::create(flags)));

  vector<devices::Entry> allow_list = {*devices::Entry::parse("c 1:3 w")};
  vector<devices::Entry> deny_list = {
    *devices::Entry::parse("c 1:3 w"),
    *devices::Entry::parse("c 21:1 w")
  };

  AWAIT_ASSERT_FAILED(dm->configure(
      TEST_CGROUP,
      allow_list,
      CHECK_NOTERROR(convert_to_non_wildcards(deny_list))));
}


TEST_F(DeviceManagerTest, ROOT_DeviceManagerConfigure_AllowWildcard)
{
  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  slave::Flags flags;
  flags.work_dir = *sandbox;
  Owned<DeviceManager> dm =
    Owned<DeviceManager>(CHECK_NOTERROR(DeviceManager::create(flags)));

  vector<devices::Entry> allow_list = {*devices::Entry::parse("a *:* m")};
  vector<devices::Entry> deny_list = {*devices::Entry::parse("c 3:1 m")};

  AWAIT_ASSERT_READY(dm->configure(
      TEST_CGROUP,
      allow_list,
      CHECK_NOTERROR(convert_to_non_wildcards(deny_list))));

  Future<DeviceManager::CgroupDeviceAccess> cgroup_state =
    dm->state(TEST_CGROUP);

  AWAIT_ASSERT_READY(cgroup_state);
  EXPECT_EQ(allow_list, cgroup_state->allow_list);
  EXPECT_EQ(deny_list, cgroup_state->deny_list);
}


TEST_F(DeviceManagerTest, ROOT_DeviceManagerGetDiffState_AllowMatchesDeny)
{
  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  slave::Flags flags;
  flags.work_dir = *sandbox;
  Owned<DeviceManager> dm =
    Owned<DeviceManager>(CHECK_NOTERROR(DeviceManager::create(flags)));

  vector<devices::Entry> additions = {*devices::Entry::parse("c 1:3 w")};
  vector<devices::Entry> removals = {
    *devices::Entry::parse("c 1:3 w"),
    *devices::Entry::parse("c 21:1 w")
  };

  AWAIT_ASSERT_FAILED(dm->reconfigure(
      TEST_CGROUP,
      CHECK_NOTERROR(convert_to_non_wildcards(additions)),
      CHECK_NOTERROR(convert_to_non_wildcards(removals))));
}


using DeviceManagerGetDiffStateTestParams = tuple<
  vector<devices::Entry>, // Allow list for initial configure.
  vector<devices::Entry>, // Deny list for initial configure.
  vector<devices::Entry>, // Additions for reconfigure.
  vector<devices::Entry>, // Removals for reconfigure.
  vector<devices::Entry>, // Expected allow list after reconfigure.
  vector<devices::Entry>  // Expected deny list after reconfigure.
>;


class DeviceManagerGetDiffStateTestFixture
  : public DeviceManagerTest,
    public ::testing::WithParamInterface<DeviceManagerGetDiffStateTestParams>
{};


TEST_P(DeviceManagerGetDiffStateTestFixture, ROOT_DeviceManagerGetDiffState)
{
  auto params = GetParam();
  vector<devices::Entry> setup_allow = std::get<0>(params);
  vector<devices::Entry> setup_deny = std::get<1>(params);
  vector<devices::Entry> additions = std::get<2>(params);
  vector<devices::Entry> removals = std::get<3>(params);
  vector<devices::Entry> reconfigured_allow = std::get<4>(params);
  vector<devices::Entry> reconfigured_deny = std::get<5>(params);

  ASSERT_SOME(cgroups2::create(TEST_CGROUP));
  slave::Flags flags;
  flags.work_dir = *sandbox;
  Owned<DeviceManager> dm =
    Owned<DeviceManager>(CHECK_NOTERROR(DeviceManager::create(flags)));

  AWAIT_ASSERT_READY(dm->configure(
      TEST_CGROUP,
      setup_allow,
      CHECK_NOTERROR(convert_to_non_wildcards(setup_deny))));

  Future<DeviceManager::CgroupDeviceAccess> cgroup_state =
    dm->state(TEST_CGROUP);

  AWAIT_ASSERT_READY(cgroup_state);
  EXPECT_EQ(setup_allow, cgroup_state->allow_list);
  EXPECT_EQ(setup_deny, cgroup_state->deny_list);

  cgroup_state = dm->apply_diff(
      cgroup_state.get(),
      CHECK_NOTERROR(convert_to_non_wildcards(additions)),
      CHECK_NOTERROR(convert_to_non_wildcards(removals)));

  EXPECT_EQ(reconfigured_allow, cgroup_state->allow_list);
  EXPECT_EQ(reconfigured_deny, cgroup_state->deny_list);
}


INSTANTIATE_TEST_CASE_P(
  DeviceManagerGetDiffStateTestParams,
  DeviceManagerGetDiffStateTestFixture,
  ::testing::Values<DeviceManagerGetDiffStateTestParams>(
    // Remove existing allow entry accesses:
    DeviceManagerGetDiffStateTestParams{
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rwm")},
      vector<devices::Entry>{},
      vector<devices::Entry>{},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 w")},
      vector<devices::Entry>{}},
    // Remove existing deny entry accesses:
    DeviceManagerGetDiffStateTestParams{
      vector<devices::Entry>{*devices::Entry::parse("c 3:* rwm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rwm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rm")},
      vector<devices::Entry>{},
      vector<devices::Entry>{
        *devices::Entry::parse("c 3:* rwm"),
        *devices::Entry::parse("c 3:1 rm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 w")}},
    // Remove entire existing allow entry:
    DeviceManagerGetDiffStateTestParams{
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rm")},
      vector<devices::Entry>{},
      vector<devices::Entry>{},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rwm")},
      vector<devices::Entry>{},
      vector<devices::Entry>{}},
    // Remove entire existing deny entry:
    DeviceManagerGetDiffStateTestParams{
      vector<devices::Entry>{*devices::Entry::parse("c 3:* rm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rm")},
      vector<devices::Entry>{},
      vector<devices::Entry>{
        *devices::Entry::parse("c 3:* rm"), *devices::Entry::parse("c 3:1 rm")},
      vector<devices::Entry>{}},
    // Overlapping entries where none encompasses the other:
    DeviceManagerGetDiffStateTestParams{
      vector<devices::Entry>{*devices::Entry::parse("c 3:* rm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rw")},
      vector<devices::Entry>{},
      vector<devices::Entry>{
        *devices::Entry::parse("c 3:* rm"), *devices::Entry::parse("c 3:1 rw")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 m")}},
    // Overlapping with non-encompassing wildcard:
    DeviceManagerGetDiffStateTestParams{
      vector<devices::Entry>{*devices::Entry::parse("c 3:* rm")},
      vector<devices::Entry>{},
      vector<devices::Entry>{},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 rw")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:* rm")},
      vector<devices::Entry>{*devices::Entry::parse("c 3:1 r")}}));

} // namespace tests {
} // namespace internal {
} // namespace mesos {

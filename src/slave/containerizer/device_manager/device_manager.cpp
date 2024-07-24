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

#include <algorithm>
#include <string>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/stringify.hpp>

#include "slave/containerizer/device_manager/device_manager.hpp"
#include "slave/paths.hpp"
#include "linux/cgroups2.hpp"

using std::string;
using std::vector;

using process::dispatch;
using process::Failure;
using process::Future;
using process::Owned;

using cgroups::devices::Entry;

namespace mesos {
namespace internal {
namespace slave {


vector<Entry> convert_to_entries(
    const vector<DeviceManager::NonWildcardEntry>& non_wildcards_entries)
{
  vector<Entry> entries = {};
  foreach (const DeviceManager::NonWildcardEntry& non_wildcards_entry,
           non_wildcards_entries) {
    Entry entry;
    entry.access = non_wildcards_entry.access;
    entry.selector.type = [&]() {
      switch (non_wildcards_entry.selector.type) {
        case DeviceManager::NonWildcardEntry::Selector::Type::BLOCK:
          return Entry::Selector::Type::BLOCK;
        case DeviceManager::NonWildcardEntry::Selector::Type::CHARACTER:
          return Entry::Selector::Type::CHARACTER;
      }
    }();
    entry.selector.major = non_wildcards_entry.selector.major;
    entry.selector.minor = non_wildcards_entry.selector.minor;
    entries.push_back(entry);
  }
  return entries;
}


class DeviceManagerProcess : public process::Process<DeviceManagerProcess>
{
public:
  DeviceManagerProcess(const string& work_dir)
    : ProcessBase(process::ID::generate("device-manager")),
      meta_dir(paths::getMetaRootDir(work_dir)) {}

  Future<Nothing> configure(
      const string& cgroup,
      const vector<Entry>& allow_list,
      const vector<DeviceManager::NonWildcardEntry>& non_wildcard_deny_list)
  {
    vector<Entry> deny_list = convert_to_entries(non_wildcard_deny_list);
    foreach (const Entry& allow_entry, allow_list) {
      foreach (const Entry& deny_entry, deny_list) {
        if (deny_entry.encompasses(allow_entry)) {
          return Failure(
              "Failed to configure allow and deny devices:"
              " allow entry '" + stringify(allow_entry) + "' cannot be"
              " encompassed by deny entry '" + stringify(deny_entry) + "'");
        }
      }
    }

    device_access_per_cgroup[cgroup].allow_list = allow_list;
    device_access_per_cgroup[cgroup].deny_list = deny_list;

    Try<Nothing> commit = commit_device_access_changes(cgroup);
    if (commit.isError()) {
      // We do not rollback the state when something goes wrong in the
      // update because the container will be destroyed when this fails.
      return Failure("Failed to commit cgroup device access changes: "
                     + commit.error());
    }

    return Nothing();
  }

  Future<Nothing> reconfigure(
      const string& cgroup,
      const vector<DeviceManager::NonWildcardEntry>& non_wildcard_additions,
      const vector<DeviceManager::NonWildcardEntry>& non_wildcard_removals)
  {
    vector<Entry> additions = convert_to_entries(non_wildcard_additions);
    vector<Entry> removals = convert_to_entries(non_wildcard_removals);
    foreach (const Entry& addition, additions) {
      foreach (const Entry& removal, removals) {
        if (removal.encompasses(addition)) {
          return Failure(
              "Failed to configure allow and deny devices:"
              " addition '" + stringify(addition) + "' cannot be"
              " encompassed by removal '" + stringify(removal) + "'");
        }
      }
    }

    device_access_per_cgroup[cgroup] = DeviceManager::apply_diff(
        device_access_per_cgroup[cgroup],
        non_wildcard_additions,
        non_wildcard_removals);

    Try<Nothing> commit = commit_device_access_changes(cgroup);
    if (commit.isError()) {
      // We do not rollback the state when something goes wrong in the
      // update because the container will be destroyed when this fails.
      return Failure("Failed to commit cgroup device access changes: "
                     + commit.error());
    }

    return Nothing();
  }

  hashmap<string, DeviceManager::CgroupDeviceAccess> state() const
  {
    return device_access_per_cgroup;
  }

  DeviceManager::CgroupDeviceAccess state(const string& cgroup) const
  {
    return device_access_per_cgroup.contains(cgroup)
      ? device_access_per_cgroup.at(cgroup)
      : DeviceManager::CgroupDeviceAccess();
  }

private:
  const string meta_dir;

  hashmap<string, DeviceManager::CgroupDeviceAccess> device_access_per_cgroup;

  // TODO(jasonzhou): persist device_access_per_cgroup on disk.
  Try<Nothing> commit_device_access_changes(const string& cgroup) const
  {
    Try<Nothing> status = cgroups2::devices::configure(
        cgroup,
        device_access_per_cgroup.at(cgroup).allow_list,
        device_access_per_cgroup.at(cgroup).deny_list);

    if (status.isError()) {
      return Error("Failed to configure device access: " + status.error());
    }

    return Nothing();
  }
};


Try<DeviceManager*> DeviceManager::create(const Flags& flags)
{
  return new DeviceManager(
      Owned<DeviceManagerProcess>(new DeviceManagerProcess(flags.work_dir)));
}


DeviceManager::DeviceManager(
    const Owned<DeviceManagerProcess>& _process)
  : process(_process)
{
  spawn(*process);
}


DeviceManager::~DeviceManager()
{
  terminate(*process);
  process::wait(*process);
}


Future<Nothing> DeviceManager::reconfigure(
    const string& cgroup,
    const vector<DeviceManager::NonWildcardEntry>& additions,
    const vector<DeviceManager::NonWildcardEntry>& removals)
{
  return dispatch(
      *process,
      &DeviceManagerProcess::reconfigure,
      cgroup,
      additions,
      removals);
}


Future<Nothing> DeviceManager::configure(
    const string& cgroup,
    const vector<Entry>& allow_list,
    const vector<DeviceManager::NonWildcardEntry>& deny_list)
{
  return dispatch(
      *process,
      &DeviceManagerProcess::configure,
      cgroup,
      allow_list,
      deny_list);
}


Future<hashmap<string, DeviceManager::CgroupDeviceAccess>>
  DeviceManager::state() const
{
  // Necessary due to overloading of state().
  auto process_copy = process;
  return dispatch(*process, [process_copy]() {
    return process_copy->state();
  });
}


Future<DeviceManager::CgroupDeviceAccess> DeviceManager::state(
    const string& cgroup) const
{
  // Necessary due to overloading of state().
  auto process_copy = process;
  return dispatch(*process, [process_copy, cgroup]() {
    return process_copy->state(cgroup);
  });
}


DeviceManager::CgroupDeviceAccess DeviceManager::apply_diff(
    const DeviceManager::CgroupDeviceAccess& old_state,
    const vector<DeviceManager::NonWildcardEntry>& non_wildcard_additions,
    const vector<DeviceManager::NonWildcardEntry>& non_wildcard_removals)
{
  auto revoke_accesses = [](Entry* entry, const Entry& diff_entry) {
    CHECK(!entry->selector.has_wildcard());
    CHECK(!diff_entry.selector.has_wildcard());

    if (entry->selector.major == diff_entry.selector.major
        && entry->selector.minor == diff_entry.selector.minor
        && entry->selector.type == diff_entry.selector.type) {
      entry->access.mknod = entry->access.mknod && !diff_entry.access.mknod;
      entry->access.read = entry->access.read && !diff_entry.access.read;
      entry->access.write = entry->access.write && !diff_entry.access.write;
    }
  };

  DeviceManager::CgroupDeviceAccess new_state = old_state;
  vector<Entry> additions = convert_to_entries(non_wildcard_additions);
  vector<Entry> removals = convert_to_entries(non_wildcard_removals);

  foreach (const Entry& addition, additions) {
    // Go over each entry in deny list, find any entries that match the new
    // addition's major & minor numbers, remove any accesses they specify
    // that the addition also specifies.
    // Invariant: No device wildcards are allowed in the deny list.
    foreach (Entry& deny_entry, new_state.deny_list) {
      revoke_accesses(&deny_entry, addition);
    }

    new_state.allow_list.push_back(addition);
  }

  foreach (const Entry& removal, removals) {
    Entry::Access accesses_by_matching_wildcards;
    accesses_by_matching_wildcards.read = false;
    accesses_by_matching_wildcards.write = false;
    accesses_by_matching_wildcards.mknod = false;

    foreach (Entry& allow_entry, new_state.allow_list) {
      // Matching against wildcard - we cannot revoke wildcard privileges
      // so we will insert a deny entry replicating whatever privileges we
      // need to deny which the wildcard grants.
      if (allow_entry.selector.has_wildcard()) {
        // Does the allow wildcard match the removal device? Skip if not.
        if (allow_entry.selector.type != Entry::Selector::Type::ALL
            && allow_entry.selector.type != removal.selector.type) {
          continue; // Type doesn't match.
        }
        if (allow_entry.selector.major.isSome()
            && allow_entry.selector.major != removal.selector.major) {
          continue; // Major doesn't match.
        }
        if (allow_entry.selector.minor.isSome()
            && allow_entry.selector.minor != removal.selector.minor) {
          continue; // Minor doesn't match.
        }
        accesses_by_matching_wildcards.mknod |= allow_entry.access.mknod;
        accesses_by_matching_wildcards.read |= allow_entry.access.read;
        accesses_by_matching_wildcards.write |= allow_entry.access.write;
      } else {
        revoke_accesses(&allow_entry, removal);
      }
    }

    Entry::Access removal_access = removal.access;
    removal_access.mknod &= accesses_by_matching_wildcards.mknod;
    removal_access.read &= accesses_by_matching_wildcards.read;
    removal_access.write &= accesses_by_matching_wildcards.write;

    if (!removal_access.none()) {
      Entry to_push = removal;
      to_push.access = removal_access;
      new_state.deny_list.push_back(to_push);
    }
  }

  auto strip_empties = [](const vector<Entry>& entries) {
    vector<Entry> res = {};
    foreach (const Entry& entry, entries) {
      if (!entry.access.none()) {
        res.push_back(entry);
      }
    }
    return res;
  };

  new_state.allow_list = strip_empties(new_state.allow_list);
  new_state.deny_list = strip_empties(new_state.deny_list);

  return new_state;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {

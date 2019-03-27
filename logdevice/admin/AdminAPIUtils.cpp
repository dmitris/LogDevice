/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logdevice/admin/AdminAPIUtils.h"

#include "logdevice/admin/Conv.h"
#include "logdevice/common/AuthoritativeStatus.h"
#include "logdevice/common/ClusterState.h"
#include "logdevice/common/configuration/Configuration.h"
#include "logdevice/common/configuration/Node.h"
#include "logdevice/common/configuration/nodes/NodesConfigLegacyConverter.h"
#include "logdevice/common/configuration/nodes/NodesConfiguration.h"
#include "logdevice/common/event_log/EventLogRebuildingSet.h"
#include "logdevice/server/FailureDetector.h"

using namespace facebook::logdevice::configuration;

namespace facebook { namespace logdevice {

std::string toString(const thrift::SocketAddressFamily& family) {
  switch (family) {
    case thrift::SocketAddressFamily::INET:
      return "INET";
    case thrift::SocketAddressFamily::UNIX:
      return "UNIX";
  }
  ld_check(false);
  return "";
}

std::string toString(const thrift::SocketAddress& address) {
  return folly::format("{}-[{}{}]",
                       toString(address.get_address_family()),
                       address.get_address() ? *address.get_address() : "",
                       address.get_port()
                           ? ":" + std::to_string(*address.get_port())
                           : "")
      .str();
}

bool match_by_address(const configuration::nodes::NodeServiceDiscovery& node_sd,
                      const thrift::SocketAddress* address) {
  ld_check(address);
  if (node_sd.address.isUnixAddress() &&
      address->address_family == thrift::SocketAddressFamily::UNIX &&
      node_sd.address.getPath() == *address->get_address()) {
    return true;
  }
  if (!node_sd.address.isUnixAddress() &&
      address->address_family == thrift::SocketAddressFamily::INET &&
      node_sd.address.getAddress().str() == *address->get_address() &&
      node_sd.address.port() == *address->get_port()) {
    return true;
  }
  return false;
}

void forFilteredNodes(
    const configuration::nodes::NodesConfiguration& nodes_configuration,
    thrift::NodesFilter* filter,
    NodeFunctor fn) {
  folly::Optional<configuration::nodes::NodeRole> role_filter;

  if (filter && filter->get_role()) {
    configuration::nodes::NodeRole ld_role =
        toLogDevice<configuration::nodes::NodeRole>(*filter->get_role());
    role_filter.assign(ld_role);
  }

  auto matches =
      [&](node_index_t index,
          const configuration::nodes::NodeServiceDiscovery& node_sd) -> bool {
    if (!filter) {
      // We don't have a filter, we accept all nodes.
      return true;
    }
    bool res = true;
    // filter by role
    if (role_filter) {
      res &= node_sd.hasRole(*role_filter);
    }
    // filter by node
    if (filter->get_node()) {
      auto* node_identifier = filter->get_node();
      if (node_identifier->get_address()) {
        res &= match_by_address(node_sd, node_identifier->get_address());
      }
      // filter by index
      if (node_identifier->get_node_index()) {
        res &= (index == *node_identifier->get_node_index());
      }
    }
    // filter by location
    if (filter->get_location()) {
      std::string location_filter_str = *filter->get_location();
      res &= (node_sd.location &&
              node_sd.location->matchesPrefix(location_filter_str));
    }
    return res;
  };

  for (const auto& kv : *nodes_configuration.getServiceDiscovery()) {
    if (matches(kv.first, kv.second)) {
      fn(kv.first);
    }
  }
}

thrift::ShardOperationalState
toShardOperationalState(StorageState storage_state,
                        const EventLogRebuildingSet::NodeInfo* node_info) {
  switch (storage_state) {
    case StorageState::DISABLED:
      return thrift::ShardOperationalState::DRAINED;
    case StorageState::READ_ONLY:
      // The node will be in READ_ONLY if we are draining.
      if (node_info && node_info->drain) {
        if (node_info->auth_status ==
            AuthoritativeStatus::AUTHORITATIVE_EMPTY) {
          return thrift::ShardOperationalState::DRAINED;
        } else {
          // We are still draining then.
          return thrift::ShardOperationalState::MIGRATING_DATA;
        }
      }
      return thrift::ShardOperationalState::ENABLED;
    case StorageState::READ_WRITE:
      return thrift::ShardOperationalState::ENABLED;
  }
  ld_check(false);
  return thrift::ShardOperationalState::INVALID;
}

void fillNodeConfig(
    thrift::NodeConfig& out,
    node_index_t node_index,
    const configuration::nodes::NodesConfiguration& nodes_configuration) {
  out.set_node_index(node_index);

  const auto* node_sd = nodes_configuration.getNodeServiceDiscovery(node_index);
  // caller should ensure node_index exists in nodes_configuration
  ld_check(node_sd != nullptr);

  // Roles
  std::set<thrift::Role> roles;
  if (node_sd->hasRole(nodes::NodeRole::SEQUENCER)) {
    roles.insert(thrift::Role::SEQUENCER);
    const auto& seq_membership = nodes_configuration.getSequencerMembership();
    const auto result = seq_membership->getNodeState(node_index);
    if (result.first) {
      // Sequencer Config
      thrift::SequencerConfig sequencer_config;
      sequencer_config.set_weight(result.second.weight);
      out.set_sequencer(std::move(sequencer_config));
    }
  }

  if (node_sd->hasRole(nodes::NodeRole::STORAGE)) {
    roles.insert(thrift::Role::STORAGE);
    const auto* storage_attr =
        nodes_configuration.getNodeStorageAttribute(node_index);
    if (storage_attr) {
      // Storage Node Config
      thrift::StorageConfig storage_config;
      storage_config.set_weight(storage_attr->capacity);
      storage_config.set_num_shards(storage_attr->num_shards);
      out.set_storage(std::move(storage_config));
    }
  }

  out.set_roles(std::move(roles));
  out.set_location(node_sd->locationStr());

  thrift::SocketAddress data_address;
  fillSocketAddress(data_address, node_sd->address);
  out.set_data_address(std::move(data_address));

  // Other Addresses
  thrift::Addresses other_addresses;
  thrift::SocketAddress gossip_address;
  fillSocketAddress(gossip_address, node_sd->gossip_address);
  other_addresses.set_gossip(std::move(gossip_address));
  if (node_sd->ssl_address) {
    thrift::SocketAddress ssl_address;
    fillSocketAddress(ssl_address, node_sd->ssl_address.value());
    other_addresses.set_ssl(std::move(ssl_address));
  }
  out.set_other_addresses(std::move(other_addresses));
}

void fillSocketAddress(thrift::SocketAddress& out, const Sockaddr& addr) {
  if (addr.isUnixAddress()) {
    out.set_address_family(thrift::SocketAddressFamily::UNIX);
    out.set_address(addr.getPath());
  } else {
    out.set_address_family(thrift::SocketAddressFamily::INET);
    out.set_address(addr.getAddress().str());
    out.set_port(addr.port());
  }
}

void fillNodeState(
    thrift::NodeState& out,
    node_index_t node_index,
    const configuration::nodes::NodesConfiguration& nodes_configuration,
    const EventLogRebuildingSet* rebuilding_set,
    const ClusterState* cluster_state) {
  out.set_node_index(node_index);

  if (cluster_state) {
    thrift::ServiceState daemon_state = thrift::ServiceState::UNKNOWN;
    switch (cluster_state->getNodeState(node_index)) {
      case ClusterStateNodeState::DEAD:
        daemon_state = thrift::ServiceState::DEAD;
        break;
      case ClusterStateNodeState::FULLY_STARTED:
        daemon_state = thrift::ServiceState::ALIVE;
        break;
      case ClusterStateNodeState::STARTING:
        daemon_state = thrift::ServiceState::STARTING_UP;
        break;
      case ClusterStateNodeState::FAILING_OVER:
        daemon_state = thrift::ServiceState::SHUTTING_DOWN;
        break;
    }
    out.set_daemon_state(daemon_state);
  }

  const auto* node_sd = nodes_configuration.getNodeServiceDiscovery(node_index);
  // caller should ensure node_index exists in nodes_configuration
  ld_check(node_sd != nullptr);

  // Sequencer State
  if (node_sd->hasRole(nodes::NodeRole::SEQUENCER)) {
    thrift::SequencerState sequencer;
    thrift::SequencingState state = thrift::SequencingState::DISABLED;
    const auto& seq_membership = nodes_configuration.getSequencerMembership();

    if (seq_membership->isSequencingEnabled(node_index)) {
      state = thrift::SequencingState::ENABLED;
      // let's see if we have a failure-detector state about this sequencer
      if (cluster_state && cluster_state->isNodeBoycotted(node_index)) {
        state = thrift::SequencingState::BOYCOTTED;
      }
    }
    sequencer.set_state(state);
    // TODO: Fill the sequencer_state_last_updated when we have that.
    out.set_sequencer_state(std::move(sequencer));
  }

  // Storage State
  if (node_sd->hasRole(nodes::NodeRole::STORAGE)) {
    const auto& storage_membership = nodes_configuration.getStorageMembership();
    std::vector<thrift::ShardState> shard_states;
    for (int shard_index = 0;
         shard_index < nodes_configuration.getNumShards(node_index);
         shard_index++) {
      // For every shard in storage membership
      ShardID shard(node_index, shard_index);
      auto result = storage_membership->getShardState(shard);
      if (!result.first) {
        // shard does not exist in membership
        continue;
      }

      // TODO T41895204: use cluster membership in admin api thrift interfaces
      const auto legacy_storage_state =
          configuration::nodes::NodesConfigLegacyConverter::
              toLegacyStorageState(result.second.storage_state);

      thrift::ShardState state;
      auto node_info = rebuilding_set
          ? rebuilding_set->getNodeInfo(node_index, shard_index)
          : nullptr;
      state.set_current_storage_state(
          toThrift<thrift::ShardStorageState>(legacy_storage_state));
      state.set_current_operational_state(
          toShardOperationalState(legacy_storage_state, node_info));
      AuthoritativeStatus auth_status =
          AuthoritativeStatus::FULLY_AUTHORITATIVE;
      bool has_dirty_ranges = false;
      if (node_info) {
        has_dirty_ranges = !node_info->dc_dirty_ranges.empty();
        auth_status = node_info->auth_status;
      }
      state.set_data_health(toShardDataHealth(auth_status, has_dirty_ranges));
      shard_states.push_back(std::move(state));
    }
    out.set_shard_states(std::move(shard_states));
  }
}

ShardSet resolveShardOrNode(
    const thrift::ShardID& shard,
    const configuration::nodes::NodesConfiguration& nodes_configuration) {
  ShardSet output;

  const auto& serv_disc = nodes_configuration.getServiceDiscovery();
  shard_index_t shard_index = (shard.shard_index < 0) ? -1 : shard.shard_index;
  shard_size_t num_shards = 1;
  node_index_t node_index = -1;
  if (shard.get_node().get_node_index()) {
    node_index = *shard.get_node().get_node_index();
    if (node_index >= nodes_configuration.clusterSize()) {
      // We didn't find the node.
      thrift::InvalidRequest err;
      err.set_message(
          folly::format(
              "Node with index '{}' was not found in the nodes config.",
              node_index)
              .str());
      throw err;
    }
    num_shards = nodes_configuration.getNumShards(node_index);
  } else if (shard.get_node().get_address()) {
    // resolve the node index from the nodes configuration.
    for (const auto& kv : *serv_disc) {
      if (match_by_address(kv.second, shard.get_node().get_address())) {
        node_index = kv.first;
        num_shards = nodes_configuration.getNumShards(node_index);
        break;
      }
    }

    // We didn't find the node.
    thrift::InvalidRequest err;
    err.set_message(
        folly::format(
            "Node with address '{}' was not found in the nodes config.",
            toString(*shard.get_node().get_address()))
            .str());
    throw err;
  } else {
    thrift::InvalidRequest err;
    err.set_message("Cannot accept nodes or shards without specifying an "
                    "address or node index");
    throw err;
  }
  if (shard_index == -1) {
    for (int i = 0; i < num_shards; i++) {
      output.emplace(ShardID(node_index, i));
    }
  } else {
    output.emplace(ShardID(node_index, shard_index));
  }
  return output;
}

ShardSet expandShardSet(
    const thrift::ShardSet& thrift_shards,
    const configuration::nodes::NodesConfiguration& nodes_configuration) {
  ShardSet output;
  for (const auto& it : thrift_shards) {
    ShardSet expanded = resolveShardOrNode(it, nodes_configuration);
    output.merge(expanded);
  }
  return output;
}

}} // namespace facebook::logdevice

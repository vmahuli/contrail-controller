/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/routing-instance/service_chaining.h"

#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <sandesh/sandesh_types.h>
#include <sandesh/sandesh.h>
#include <sandesh/sandesh_trace.h>

#include <algorithm>

#include "base/queue_task.h"
#include "base/task.h"
#include "base/task_annotations.h"
#include "bgp/bgp_condition_listener.h"
#include "bgp/bgp_config.h"
#include "bgp/bgp_log.h"
#include "bgp/bgp_origin_vn_path.h"
#include "bgp/bgp_path.h"
#include "bgp/bgp_peer.h"
#include "bgp/bgp_peer_membership.h"
#include "bgp/bgp_route.h"
#include "bgp/bgp_sandesh.h"
#include "bgp/bgp_server.h"
#include "bgp/bgp_table.h"
#include "bgp/extended-community/site_of_origin.h"
#include "bgp/inet6vpn/inet6vpn_route.h"
#include "bgp/l3vpn/inetvpn_route.h"
#include "bgp/origin-vn/origin_vn.h"
#include "bgp/routing-instance/routing_instance.h"
#include "bgp/routing-instance/service_chaining_types.h"
#include "db/db_table_partition.h"
#include "net/address.h"
#include "net/address_util.h"

using boost::bind;
using boost::system::error_code;
using std::make_pair;
using std::sort;
using std::string;
using std::vector;

template<>
int ServiceChainMgr<ServiceChainInet>::service_chain_task_id_ = -1;
template<>
int ServiceChainMgr<ServiceChainInet6>::service_chain_task_id_ = -1;

static int GetOriginVnIndex(const BgpTable *table, const BgpRoute *route) {
    const BgpPath *path = route->BestPath();
    if (!path)
        return 0;

    if (path->IsVrfOriginated())
        return table->routing_instance()->virtual_network_index();

    const BgpAttr *attr = path->GetAttr();
    const ExtCommunity *ext_community = attr->ext_community();
    if (!ext_community)
        return 0;

    BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &comm,
                  ext_community->communities()) {
        if (!ExtCommunity::is_origin_vn(comm))
            continue;
        OriginVn origin_vn(comm);
        return origin_vn.vn_index();
    }

    return 0;
}

template <typename T>
ServiceChain<T>::ServiceChain(RoutingInstance *src, RoutingInstance *dest,
    RoutingInstance *connected,
    const vector<string> &subnets, AddressT addr)
    : src_(src),
      dest_(dest),
      connected_(connected),
      connected_route_(NULL),
      service_chain_addr_(addr),
      connected_table_unregistered_(false),
      dest_table_unregistered_(false),
      aggregate_(false),
      src_table_delete_ref_(this, src_table()->deleter()) {
    for (vector<string>::const_iterator it = subnets.begin();
         it != subnets.end(); ++it) {
        error_code ec;
        PrefixT ipam_subnet = PrefixT::FromString(*it, &ec);
        if (ec != 0)
            continue;
        prefix_to_routelist_map_[ipam_subnet] = RouteList();
    }
}

template <>
Address::Family ServiceChain<ServiceChainInet>::GetFamily() const {
    return Address::INET;
}

template <>
Address::Family ServiceChain<ServiceChainInet6>::GetFamily() const {
    return Address::INET6;
}

template <typename T>
BgpTable *ServiceChain<T>::src_table() const {
    return src_->GetTable(GetFamily());
}

template <typename T>
BgpTable *ServiceChain<T>::connected_table() const {
    return connected_->GetTable(GetFamily());
}

template <typename T>
BgpTable *ServiceChain<T>::dest_table() const {
    return dest_->GetTable(GetFamily());
}

//
// Compare this ServiceChain against the ServiceChainConfig.
// Return true if the configuration has not changed, false otherwise.
//
template <typename T>
bool ServiceChain<T>::CompareServiceChainConfig(
    const ServiceChainConfig &config) {
    if (dest_->name() != config.routing_instance)
        return false;
    if (connected_->name() != config.source_routing_instance)
        return false;
    if (service_chain_addr_.to_string() != config.service_chain_address)
        return false;

    if (prefix_to_routelist_map_.size() != config.prefix.size())
        return false;
    for (vector<string>::const_iterator it = config.prefix.begin();
         it != config.prefix.end(); ++it) {
        error_code ec;
        PrefixT ipam_subnet = PrefixT::FromString(*it, &ec);
        if (prefix_to_routelist_map_.find(ipam_subnet) ==
            prefix_to_routelist_map_.end()) {
            return false;
        }
    }
    return true;
}

//
// Match function called from BgpConditionListener
// Concurrency : db::DBTable
// For the purpose of route aggregation, two condition needs to be matched
//      1. More specific route present in any of the Dest BgpTable partition
//      2. Connected route(for nexthop) present in Src BgpTable
//
template <typename T>
bool ServiceChain<T>::Match(BgpServer *server, BgpTable *table, BgpRoute *route,
    bool deleted) {
    CHECK_CONCURRENCY("db::DBTable");

    typename ServiceChainRequestT::RequestType type;
    PrefixT aggregate_match;

    if (table == dest_table() && !dest_table_unregistered()) {
        if (IsConnectedRoute(route))
            return false;
        if (aggregate_enable() && IsAggregate(route))
            return false;

        if (aggregate_enable() && IsMoreSpecific(route, &aggregate_match)) {
            // More specific
            if (deleted) {
                type = ServiceChainRequestT::MORE_SPECIFIC_DELETE;
            } else {
                type = ServiceChainRequestT::MORE_SPECIFIC_ADD_CHG;
            }
        } else {
            // External connecting routes
            if (!deleted) {
                if (!route->BestPath() || !route->BestPath()->IsFeasible()) {
                    deleted = true;
                } else {
                    const BgpAttr *attr = route->BestPath()->GetAttr();
                    const Community *comm = attr ? attr->community() : NULL;
                    if (comm && comm->ContainsValue(Community::NoAdvertise))
                        deleted = true;

                    int vn_index = GetOriginVnIndex(table, route);
                    int src_vn_index = src_->virtual_network_index();
                    int dest_vn_index = dest_->virtual_network_index();
                    if (!vn_index || dest_vn_index != vn_index) {
                        if (src_vn_index == vn_index)
                            deleted = true;
                        if (!dest_->virtual_network_allow_transit())
                            deleted = true;
                        if (!dest_vn_index)
                            deleted = true;
                    }

                    OriginVn src_origin_vn(
                        server->autonomous_system(), src_vn_index);
                    const OriginVnPath *ovnpath =
                        attr ? attr->origin_vn_path() : NULL;
                    if (ovnpath &&
                        ovnpath->Contains(src_origin_vn.GetExtCommunity())) {
                        deleted = true;
                    }
                }
            }

            if (deleted) {
                type = ServiceChainRequestT::EXT_CONNECT_ROUTE_DELETE;
            } else {
                type = ServiceChainRequestT::EXT_CONNECT_ROUTE_ADD_CHG;
            }
        }
    } else if ((table == connected_table()) &&
               !connected_table_unregistered() &&
               IsConnectedRoute(route)) {
        if (!deleted) {
            if (!route->IsValid() ||
                route->BestPath()->GetSource() != BgpPath::BGP_XMPP) {
                deleted = true;
            }
        }

        // Connected route for service chain
        if (deleted) {
            type = ServiceChainRequestT::CONNECTED_ROUTE_DELETE;
        } else {
            type = ServiceChainRequestT::CONNECTED_ROUTE_ADD_CHG;
        }
    } else {
        return false;
    }

    BgpConditionListener *listener = server->condition_listener(GetFamily());
    ServiceChainState *state = static_cast<ServiceChainState *>(
        listener->GetMatchState(table, route, this));
    if (!deleted) {
        // MatchState is added to the Route to ensure that DBEntry is not
        // deleted before the ServiceChain module processes the WorkQueue
        // request.
        if (!state) {
            state = new ServiceChainState(ServiceChainPtr(this));
            listener->SetMatchState(table, route, this, state);
        }
    } else {
        // MatchState is set on all the Routes that matches the conditions
        // Retrieve to check and ignore delete of unseen Add Match
        if (state == NULL) {
            // Not seen ADD ignore DELETE
            return false;
        }
    }

    // The MatchState reference is taken to ensure that the route is not
    // deleted when request is still present in the queue
    // This is to handle the case where MatchState already exists and
    // deleted entry gets reused or reused entry gets deleted.
    state->IncrementRefCnt();

    // Post the Match result to ServiceChain task to take Action
    // More_Specific_Present + Connected_Route_exists ==> Add Aggregate Route
    // and stitch the nexthop from connected route
    ServiceChainRequestT *req = new ServiceChainRequestT(
        type, table, route, aggregate_match, ServiceChainPtr(this));
    server->service_chain_mgr()->Enqueue(req);
    return true;
}

template <typename T>
string ServiceChain<T>::ToString() const {
    return (string("ServiceChain " ) + service_chain_addr_.to_string());
}

template <typename T>
void ServiceChain<T>::SetConnectedRoute(BgpRoute *connected) {
    connected_route_ = connected;
    connected_path_ids_.clear();
    if (!connected_route_)
        return;

    for (Route::PathList::iterator it = connected->GetPathList().begin();
        it != connected->GetPathList().end(); ++it) {
        BgpPath *path = static_cast<BgpPath *>(it.operator->());

        // Infeasible paths are not considered.
        if (!path->IsFeasible())
            break;

        // Bail if it's not ECMP with the best path.
        if (connected_route_->BestPath()->PathCompare(*path, true))
            break;

        // Use nexthop attribute of connected path as path id.
        uint32_t path_id = path->GetAttr()->nexthop().to_v4().to_ulong();
        connected_path_ids_.insert(path_id);
    }
}

template <typename T>
bool ServiceChain<T>::IsConnectedRouteValid() const {
    return (connected_route_ && connected_route_->IsValid());
}

template <>
bool ServiceChain<ServiceChainInet>::IsMoreSpecific(
    BgpRoute *route, Ip4Prefix *aggregate_match) const {
    uint32_t broadcast = 0xFFFFFFFF;
    InetRoute *inet_route = dynamic_cast<InetRoute *>(route);
    Ip4Address address = inet_route->GetPrefix().ip4_addr();
    for (PrefixToRouteListMap::const_iterator it =
         prefix_to_route_list_map()->begin();
         it != prefix_to_route_list_map()->end(); ++it) {
        uint8_t shift_mask = (32 - it->first.prefixlen());
        if ((it->first.prefixlen() != inet_route->GetPrefix().prefixlen()) &&
            ((address.to_ulong() & (broadcast << shift_mask)) ==
             (it->first.ip4_addr().to_ulong() & (broadcast << shift_mask)))) {
            *aggregate_match = it->first;
            return true;
        }
    }
    return false;
}

template <typename T>
bool ServiceChain<T>::IsAggregate(BgpRoute *route) const {
    RouteT *ip_route = dynamic_cast<RouteT *>(route);
    for (typename PrefixToRouteListMap::const_iterator it =
         prefix_to_route_list_map()->begin();
         it != prefix_to_route_list_map()->end(); ++it) {
        if (it->first == ip_route->GetPrefix())
            return true;
    }
    return false;
}

template <typename T>
bool ServiceChain<T>::IsConnectedRoute(BgpRoute *route) const {
    RouteT *ip_route = dynamic_cast<RouteT *>(route);
    return (service_chain_addr() == ip_route->GetPrefix().addr());
}

template <typename T>
void ServiceChain<T>::RemoveMatchState(BgpRoute *route,
    ServiceChainState *state) {
    if (deleted() || route->IsDeleted()) {
        // At this point we are ready to release the MatchState on the DBEntry
        // So mark it as deleted.. Actual removal of the state is done when
        // ref count is 0
        state->set_deleted();
    }
}

// RemoveServiceChainRoute
template <typename T>
void ServiceChain<T>::RemoveServiceChainRoute(PrefixT prefix, bool aggregate) {
    CHECK_CONCURRENCY("bgp::ServiceChain");

    BgpTable *bgptable = src_table();
    RouteT rt_key(prefix);
    DBTablePartition *partition =
        static_cast<DBTablePartition *>(bgptable->GetTablePartition(&rt_key));
    BgpRoute *service_chain_route =
        static_cast<BgpRoute *>(partition->Find(&rt_key));

    if (!service_chain_route || service_chain_route->IsDeleted())
        return;

    for (ConnectedPathIdList::const_iterator it = GetConnectedPathIds().begin();
         it != GetConnectedPathIds().end(); ++it) {
        uint32_t path_id = *it;
        service_chain_route->RemovePath(BgpPath::ServiceChain, NULL, path_id);
        BGP_LOG_STR(BgpMessage, SandeshLevel::SYS_DEBUG, BGP_LOG_FLAG_TRACE,
            "Removed " << (aggregate ? "Aggregate" : "ExtConnected") <<
            " ServiceChain path " << service_chain_route->ToString() <<
            " path_id " << BgpPath::PathIdString(path_id) <<
            " in table " << bgptable->name());
    }

    if (!service_chain_route->BestPath()) {
        partition->Delete(service_chain_route);
    } else {
        partition->Notify(service_chain_route);
    }
}

// AddServiceChainRoute
template <typename T>
void ServiceChain<T>::AddServiceChainRoute(PrefixT prefix,
    const RouteT *orig_route, const ConnectedPathIdList &old_path_ids,
    bool aggregate) {
    CHECK_CONCURRENCY("bgp::ServiceChain");

    BgpTable *bgptable = src_table();
    RouteT rt_key(prefix);
    DBTablePartition *partition =
        static_cast<DBTablePartition *>(bgptable->GetTablePartition(&rt_key));
    BgpRoute *service_chain_route =
        static_cast<BgpRoute *>(partition->Find(&rt_key));

    if (service_chain_route == NULL) {
        service_chain_route = new RouteT(prefix);
        partition->Add(service_chain_route);
    } else {
        service_chain_route->ClearDelete();
    }

    int vn_index = dest_routing_instance()->virtual_network_index();
    BgpServer *server = dest_routing_instance()->server();
    OriginVn origin_vn(server->autonomous_system(), vn_index);

    SiteOfOrigin soo;
    ExtCommunity::ExtCommunityList sgid_list;
    const Community *orig_community = NULL;
    const OriginVnPath *orig_ovnpath = NULL;
    if (orig_route) {
        const BgpPath *orig_path = orig_route->BestPath();
        const BgpAttr *orig_attr = NULL;
        const ExtCommunity *ext_community = NULL;
        if (orig_path)
            orig_attr = orig_path->GetAttr();
        if (orig_attr) {
            orig_community = orig_attr->community();
            ext_community = orig_attr->ext_community();
            orig_ovnpath = orig_attr->origin_vn_path();
        }
        if (ext_community) {
            BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &comm,
                          ext_community->communities()) {
                if (ExtCommunity::is_security_group(comm))
                    sgid_list.push_back(comm);
                if (ExtCommunity::is_site_of_origin(comm) && soo.IsNull())
                    soo = SiteOfOrigin(comm);
            }
        }
    }

    BgpAttrDB *attr_db = server->attr_db();
    CommunityDB *comm_db = server->comm_db();
    CommunityPtr new_community =
        comm_db->AppendAndLocate(orig_community, Community::AcceptOwn);
    ExtCommunityDB *extcomm_db = server->extcomm_db();
    PeerRibMembershipManager *membership_mgr = server->membership_mgr();
    OriginVnPathDB *ovnpath_db = server->ovnpath_db();
    OriginVnPathPtr new_ovnpath =
        ovnpath_db->PrependAndLocate(orig_ovnpath, origin_vn.GetExtCommunity());

    ConnectedPathIdList new_path_ids;
    for (Route::PathList::iterator it =
         connected_route()->GetPathList().begin();
         it != connected_route()->GetPathList().end(); ++it) {
        BgpPath *connected_path = static_cast<BgpPath *>(it.operator->());

        // Infeasible paths are not considered
        if (!connected_path->IsFeasible())
            break;

        // take snapshot of all ECMP paths
        if (connected_route()->BestPath()->PathCompare(*connected_path, true))
            break;

        // Skip paths with duplicate forwarding information.  This ensures
        // that we generate only one path with any given next hop and label
        // when there are multiple connected paths from the original source
        // received via different peers e.g. directly via XMPP and via BGP.
        if (connected_route()->DuplicateForwardingPath(connected_path))
            continue;

        const BgpAttr *attr = connected_path->GetAttr();
        ExtCommunityPtr new_ext_community;

        // Strip any RouteTargets from the connected attributes.
        new_ext_community = extcomm_db->ReplaceRTargetAndLocate(
            attr->ext_community(), ExtCommunity::ExtCommunityList());

        // Replace the SGID list with the list from the original route.
        new_ext_community = extcomm_db->ReplaceSGIDListAndLocate(
            new_ext_community.get(), sgid_list);

        // Replace SiteOfOrigin with value from original route if any.
        if (soo.IsNull()) {
            new_ext_community = extcomm_db->RemoveSiteOfOriginAndLocate(
                new_ext_community.get());
        } else {
            new_ext_community = extcomm_db->ReplaceSiteOfOriginAndLocate(
                new_ext_community.get(), soo.GetExtCommunity());
        }

        // Replace the OriginVn with the value from the original route
        // or the value associated with the dest routing instance.
        new_ext_community = extcomm_db->ReplaceOriginVnAndLocate(
            new_ext_community.get(), origin_vn.GetExtCommunity());

        // Replace extended community, community and origin vn path.
        BgpAttrPtr new_attr = attr_db->ReplaceExtCommunityAndLocate(
            attr, new_ext_community);
        new_attr =
            attr_db->ReplaceCommunityAndLocate(new_attr.get(), new_community);
        new_attr = attr_db->ReplaceOriginVnPathAndLocate(new_attr.get(),
            new_ovnpath);

        // If the connected path is learnt via XMPP, construct RD based on
        // the id registered with source table instead of connected table.
        // This allows chaining of multiple in-network service instances
        // that are on the same compute node.
        const IPeer *peer = connected_path->GetPeer();
        if (src_ != connected_ && peer && peer->IsXmppPeer()) {
            int id = membership_mgr->GetRegistrationId(peer, bgptable);
            if (id < 0)
                continue;
            RouteDistinguisher connected_rd = attr->source_rd();
            if (connected_rd.Type() != RouteDistinguisher::TypeIpAddressBased)
                continue;
            RouteDistinguisher rd(connected_rd.GetAddress(), id);
            new_attr = attr_db->ReplaceSourceRdAndLocate(new_attr.get(), rd);
        }

        // Replace the source rd if the connected path is a secondary path
        // of a primary path in the l3vpn table. Use the RD of the primary.
        if (connected_path->IsReplicated()) {
            const BgpSecondaryPath *spath =
                static_cast<const BgpSecondaryPath *>(connected_path);
            const RoutingInstance *ri = spath->src_table()->routing_instance();
            if (ri->IsDefaultRoutingInstance()) {
                const VpnRouteT *vpn_route =
                    static_cast<const VpnRouteT *>(spath->src_rt());
                new_attr = attr_db->ReplaceSourceRdAndLocate(new_attr.get(),
                    vpn_route->GetPrefix().route_distinguisher());
            }
        }

        // Check whether we already have a path with the associated path id.
        uint32_t path_id =
            connected_path->GetAttr()->nexthop().to_v4().to_ulong();
        BgpPath *existing_path =
            service_chain_route->FindPath(BgpPath::ServiceChain, NULL,
                                          path_id);
        bool is_stale = false;
        bool path_updated = false;
        if (existing_path != NULL) {
            // Existing path can be reused.
            if ((new_attr.get() == existing_path->GetAttr()) &&
                (connected_path->GetLabel() == existing_path->GetLabel())) {
                new_path_ids.insert(path_id);
                continue;
            }

            // Remove existing path, new path will be added below.
            path_updated = true;
            is_stale = existing_path->IsStale();
            service_chain_route->RemovePath(
                BgpPath::ServiceChain, NULL, path_id);
        }

        BgpPath *new_path =
            new BgpPath(path_id, BgpPath::ServiceChain, new_attr.get(),
                        connected_path->GetFlags(), connected_path->GetLabel());
        if (is_stale)
            new_path->SetStale();

        new_path_ids.insert(path_id);
        service_chain_route->InsertPath(new_path);
        partition->Notify(service_chain_route);

        BGP_LOG_STR(BgpMessage, SandeshLevel::SYS_DEBUG, BGP_LOG_FLAG_TRACE,
            (path_updated ? "Updated " : "Added ") <<
            (aggregate ? "Aggregate" : "ExtConnected") <<
            " ServiceChain path " << service_chain_route->ToString() <<
            " path_id " << BgpPath::PathIdString(path_id) <<
            " in table " << bgptable->name());
    }

    // Remove stale paths.
    for (ConnectedPathIdList::const_iterator it = old_path_ids.begin();
         it != old_path_ids.end(); ++it) {
        uint32_t path_id = *it;
        if (new_path_ids.find(path_id) != new_path_ids.end())
            continue;
        service_chain_route->RemovePath(BgpPath::ServiceChain, NULL, path_id);
        partition->Notify(service_chain_route);

        BGP_LOG_STR(BgpMessage, SandeshLevel::SYS_DEBUG, BGP_LOG_FLAG_TRACE,
            "Removed " << (aggregate ? "Aggregate" : "ExtConnected") <<
            " ServiceChain path " << service_chain_route->ToString() <<
            " path_id " << BgpPath::PathIdString(path_id) <<
            " in table " << bgptable->name());
    }

    // Delete the route if there's no paths.
    if (!service_chain_route->BestPath())
        partition->Delete(service_chain_route);
}

template <typename T>
bool ServiceChain<T>::AddMoreSpecific(PrefixT aggregate,
    BgpRoute *more_specific) {
    typename PrefixToRouteListMap::iterator it =
        prefix_to_routelist_map_.find(aggregate);
    assert(it != prefix_to_routelist_map_.end());
    bool ret = false;
    if (it->second.empty()) {
        // Add the aggregate for the first time
        ret = true;
    }
    it->second.insert(more_specific);
    return ret;
}

template <typename T>
bool ServiceChain<T>::DeleteMoreSpecific(PrefixT aggregate,
    BgpRoute *more_specific) {
    typename PrefixToRouteListMap::iterator it =
        prefix_to_routelist_map_.find(aggregate);
    assert(it != prefix_to_routelist_map_.end());
    it->second.erase(more_specific);
    return it->second.empty();
}

template <typename T>
void ServiceChain<T>::FillServiceChainInfo(ShowServicechainInfo *info) const {
    info->set_src_rt_instance(src_routing_instance()->name());
    info->set_connected_rt_instance(connected_routing_instance()->name());
    info->set_dest_rt_instance(dest_routing_instance()->name());

    ConnectedRouteInfo connected_rt_info;
    connected_rt_info.set_service_chain_addr(service_chain_addr().to_string());
    if (connected_route()) {
        ShowRoute show_route;
        connected_route()->FillRouteInfo(connected_table(), &show_route);
        connected_rt_info.set_connected_rt(show_route);
    }
    info->set_connected_route(connected_rt_info);

    vector<PrefixToRouteListInfo> more_vec;
    for (typename PrefixToRouteListMap::const_iterator it =
         prefix_to_route_list_map()->begin();
        it != prefix_to_route_list_map()->end(); ++it) {
        PrefixToRouteListInfo prefix_list_info;
        prefix_list_info.set_prefix(it->first.ToString());

        BgpTable *bgptable = src_table();
        RouteT rt_key(it->first);
        BgpRoute *aggregate = static_cast<BgpRoute *>(bgptable->Find(&rt_key));
        if (aggregate) {
            prefix_list_info.set_aggregate(true);
            ShowRoute show_route;
            aggregate->FillRouteInfo(bgptable, &show_route);
            prefix_list_info.set_aggregate_rt(show_route);
        } else {
            prefix_list_info.set_aggregate(false);
        }

        vector<string> rt_list;
        for (RouteList::iterator rt_it = it->second.begin();
             rt_it != it->second.end(); ++rt_it) {
            rt_list.push_back((*rt_it)->ToString());
        }
        prefix_list_info.set_more_specific_list(rt_list);
        more_vec.push_back(prefix_list_info);
    }
    info->set_more_specifics(more_vec);

    vector<ExtConnectRouteInfo> ext_connecting_rt_info_list;
    for (ExtConnectRouteList::const_iterator it =
         ext_connecting_routes().begin();
         it != ext_connecting_routes().end(); ++it) {
        ExtConnectRouteInfo ext_rt_info;
        ext_rt_info.set_ext_rt_prefix((*it)->ToString());
        BgpTable *bgptable = src_table();
        RouteT *ext_route = static_cast<RouteT *>(*it);
        RouteT rt_key(ext_route->GetPrefix());
        BgpRoute *ext_connecting =
            static_cast<BgpRoute *>(bgptable->Find(&rt_key));
        if (ext_connecting) {
            ShowRoute show_route;
            ext_connecting->FillRouteInfo(bgptable, &show_route);
            ext_rt_info.set_ext_rt_svc_rt(show_route);
        }
        ext_connecting_rt_info_list.push_back(ext_rt_info);
    }
    info->set_ext_connecting_rt_info_list(ext_connecting_rt_info_list);
    info->set_aggregate_enable(aggregate_enable());
}

static void FillServiceChainConfigInfo(ShowServicechainInfo *info,
    const ServiceChainConfig &sci) {
    info->set_dest_virtual_network(
        GetVNFromRoutingInstance(sci.routing_instance));
    info->set_service_instance(sci.service_instance);
}

struct ServiceChainInfoComp {
    bool operator()(const ShowServicechainInfo &lhs,
                    const ShowServicechainInfo &rhs) {
        string lnetwork;
        string rnetwork;

        if (lhs.src_virtual_network < lhs.dest_virtual_network) {
            lnetwork = lhs.src_virtual_network + lhs.dest_virtual_network;
        } else {
            lnetwork = lhs.dest_virtual_network + lhs.src_virtual_network;
        }

        if (rhs.src_virtual_network < rhs.dest_virtual_network) {
            rnetwork = rhs.src_virtual_network + rhs.dest_virtual_network;
        } else {
            rnetwork = rhs.dest_virtual_network + rhs.src_virtual_network;
        }

        if (lnetwork != rnetwork) {
            return (lnetwork < rnetwork);
        }

        if (lhs.src_virtual_network != rhs.src_virtual_network) {
            return (lhs.src_virtual_network < rhs.src_virtual_network);
        }

        if (lhs.service_instance != rhs.service_instance) {
            return (lhs.service_instance < rhs.service_instance);
        }

        return false;
    }
};

template <typename T>
bool ServiceChainMgr<T>::RequestHandler(ServiceChainRequestT *req) {
    CHECK_CONCURRENCY("bgp::ServiceChain");
    BgpTable *table = NULL;
    BgpRoute *route = NULL;
    PrefixT aggregate_match = req->aggregate_match_;
    ServiceChainT *info = NULL;

    if (req->type_ != ServiceChainRequestT::SHOW_SERVICE_CHAIN &&
        req->type_ != ServiceChainRequestT::SHOW_PENDING_CHAIN) {
        table = req->table_;
        route = req->rt_;
        info = static_cast<ServiceChainT *>(req->info_.get());
        // Table where the aggregate route needs to be added
        aggregate_match = req->aggregate_match_;
    }

    ServiceChainState *state = NULL;
    if (route) {
        state = static_cast<ServiceChainState *>
            (listener_->GetMatchState(table, route, info));
    }

    switch (req->type_) {
        case ServiceChainRequestT::MORE_SPECIFIC_ADD_CHG: {
            assert(state);
            if (state->deleted()) {
                state->reset_deleted();
            }
            if (info->AddMoreSpecific(aggregate_match, route) &&
                info->IsConnectedRouteValid()) {
                // Add the aggregate route
                typename ServiceChainT::ConnectedPathIdList path_ids;
                info->AddServiceChainRoute(
                    aggregate_match, NULL, path_ids, true);
            }
            break;
        }
        case ServiceChainRequestT::MORE_SPECIFIC_DELETE: {
            assert(state);
            if (info->DeleteMoreSpecific(aggregate_match, route)) {
                // Delete the aggregate route
                info->RemoveServiceChainRoute(aggregate_match, true);
            }
            info->RemoveMatchState(route, state);
            break;
        }
        case ServiceChainRequestT::CONNECTED_ROUTE_ADD_CHG: {
            assert(state);
            if (route->IsDeleted() || !route->BestPath() ||
                !route->BestPath()->IsFeasible())  {
                break;
            }

            if (state->deleted()) {
                state->reset_deleted();
            }

            // Store the old path id list and populate the new one.
            typename ServiceChainT::ConnectedPathIdList path_ids =
                info->GetConnectedPathIds();
            info->SetConnectedRoute(route);

            typename ServiceChainT::PrefixToRouteListMap *vnprefix_list =
                info->prefix_to_route_list_map();
            for (typename ServiceChainT::PrefixToRouteListMap::iterator it =
                 vnprefix_list->begin(); it != vnprefix_list->end(); ++it) {
                // Add aggregate route.. Or if the route exists
                // sync the path and purge old paths
                if (!it->second.empty())
                    info->AddServiceChainRoute(it->first, NULL, path_ids, true);
            }

            for (typename ServiceChainT::ExtConnectRouteList::iterator it =
                 info->ext_connecting_routes()->begin();
                 it != info->ext_connecting_routes()->end(); ++it) {
                // Add ServiceChain route for external connecting route
                RouteT *ext_route = static_cast<RouteT *>(*it);
                info->AddServiceChainRoute(
                    ext_route->GetPrefix(), ext_route, path_ids, false);
            }
            break;
        }
        case ServiceChainRequestT::CONNECTED_ROUTE_DELETE: {
            assert(state);
            // Delete ServiceChain route for aggregate.
            typename ServiceChainT::PrefixToRouteListMap *vnprefix_list =
                info->prefix_to_route_list_map();
            for (typename ServiceChainT::PrefixToRouteListMap::iterator it =
                vnprefix_list->begin(); it != vnprefix_list->end(); ++it) {
                info->RemoveServiceChainRoute(it->first, true);
            }

            // Delete ServiceChain routes for external connecting routes.
            for (typename ServiceChainT::ExtConnectRouteList::iterator it =
                 info->ext_connecting_routes()->begin();
                 it != info->ext_connecting_routes()->end(); ++it) {
                RouteT *ext_route = static_cast<RouteT *>(*it);
                info->RemoveServiceChainRoute(ext_route->GetPrefix(), false);
            }
            info->RemoveMatchState(route, state);
            info->SetConnectedRoute(NULL);
            break;
        }
        case ServiceChainRequestT::EXT_CONNECT_ROUTE_ADD_CHG: {
            assert(state);
            if (state->deleted()) {
                state->reset_deleted();
            }
            info->ext_connecting_routes()->insert(route);
            if (info->IsConnectedRouteValid()) {
                RouteT *ext_route = dynamic_cast<RouteT *>(route);
                typename ServiceChainT::ConnectedPathIdList path_ids;
                info->AddServiceChainRoute(
                    ext_route->GetPrefix(), ext_route, path_ids, false);
            }
            break;
        }
        case ServiceChainRequestT::EXT_CONNECT_ROUTE_DELETE: {
            assert(state);
            if (info->ext_connecting_routes()->erase(route)) {
                RouteT *inet_route = dynamic_cast<RouteT *>(route);
                info->RemoveServiceChainRoute(inet_route->GetPrefix(), false);
            }
            info->RemoveMatchState(route, state);
            break;
        }
        case ServiceChainRequestT::UPDATE_ALL_ROUTES: {
            if (info->dest_table_unregistered())
                break;
            if (info->connected_table_unregistered())
                break;
            if (!info->connected_route())
                break;

            typename ServiceChainT::ConnectedPathIdList path_ids =
                info->GetConnectedPathIds();
            typename ServiceChainT::PrefixToRouteListMap *vnprefix_list =
                info->prefix_to_route_list_map();
            for (typename ServiceChainT::PrefixToRouteListMap::iterator it =
                 vnprefix_list->begin(); it != vnprefix_list->end(); ++it) {
                // Add aggregate route.. Or if the route exists
                // sync the path and purge old paths
                if (!it->second.empty())
                    info->AddServiceChainRoute(
                        it->first, NULL, path_ids, true);
            }

            for (typename ServiceChainT::ExtConnectRouteList::iterator it =
                 info->ext_connecting_routes()->begin();
                 it != info->ext_connecting_routes()->end(); ++it) {
                // Add ServiceChain route for external connecting route
                RouteT *ext_route = static_cast<RouteT *>(*it);
                info->AddServiceChainRoute(
                    ext_route->GetPrefix(), ext_route, path_ids, false);
            }
            break;
        }
        case ServiceChainRequestT::STOP_CHAIN_DONE: {
            if (table == info->connected_table()) {
                info->set_connected_table_unregistered();
                if (!info->num_matchstate()) {
                    listener_->UnregisterCondition(table, info);
                }
            }
            if (table == info->dest_table()) {
                info->set_dest_table_unregistered();
                if (!info->num_matchstate()) {
                    listener_->UnregisterCondition(table, info);
                }
            }
            if (info->unregistered()) {
                chain_set_.erase(info->src_routing_instance());
                StartResolve();
            }
            break;
        }
        case ServiceChainRequestT::SHOW_SERVICE_CHAIN: {
            ShowServiceChainResp *resp =
                static_cast<ShowServiceChainResp *>(req->snh_resp_);
            vector<ShowServicechainInfo> list;
            RoutingInstanceMgr::RoutingInstanceIterator rit =
                server()->routing_instance_mgr()->begin();
            for (; rit != server()->routing_instance_mgr()->end(); ++rit) {
                if (rit->deleted()) continue;
                if (!req->search_string_.empty() &&
                    rit->name().find(req->search_string_) == string::npos) {
                    continue;
                }
                ShowServicechainInfo info;
                const BgpInstanceConfig *rtc = rit->config();
                if (!rtc->service_chain_list().empty()) {
                    info.set_src_virtual_network(rit->virtual_network());
                    const ServiceChainConfig &sci =
                            rtc->service_chain_list().front();
                    FillServiceChainConfigInfo(&info, sci);
                    ServiceChainT *chain = FindServiceChain(rit->name());
                    if (chain) {
                        if (chain->deleted())
                            info.set_state("Deleted");
                        else
                            info.set_state("Active");
                        chain->FillServiceChainInfo(&info);
                    } else {
                        info.set_state("Pending");
                    }
                    list.push_back(info);
                }
            }
            ServiceChainInfoComp comp;
            sort(list.begin(), list.end(), comp);
            resp->set_service_chain_list(list);
            resp->Response();
            break;
        }
        case ServiceChainRequestT::SHOW_PENDING_CHAIN: {
            ShowPendingServiceChainResp *resp =
                static_cast<ShowPendingServiceChainResp *>(req->snh_resp_);
            vector<string> pending_list;
            for (UnresolvedServiceChainList::const_iterator it =
                 pending_chains().begin(); it != pending_chains().end(); ++it)
                pending_list.push_back((*it)->name());

            resp->set_pending_chains(pending_list);
            resp->Response();
            break;
        }
        default: {
            assert(false);
            break;
        }
    }

    if (state) {
        state->DecrementRefCnt();
        if (state->refcnt() == 0 && state->deleted()) {
            listener_->RemoveMatchState(table, route, info);
            delete state;
            if (!info->num_matchstate()) {
                if (info->dest_table_unregistered()) {
                    listener_->UnregisterCondition(info->dest_table(), info);
                }
                if (info->connected_table_unregistered()) {
                    listener_->UnregisterCondition(
                        info->connected_table(), info);
                }
                if (info->unregistered()) {
                    chain_set_.erase(info->src_routing_instance());
                    StartResolve();
                }
            }
        }
    }
    delete req;
    return true;
}

template <typename T>
ServiceChainMgr<T>::ServiceChainMgr(BgpServer *server)
    : server_(server),
      listener_(server_->condition_listener(GetFamily())),
      resolve_trigger_(new TaskTrigger(
          bind(&ServiceChainMgr::ResolvePendingServiceChain, this),
          TaskScheduler::GetInstance()->GetTaskId("bgp::Config"), 0)),
      aggregate_host_route_(false) {
    if (service_chain_task_id_ == -1) {
        TaskScheduler *scheduler = TaskScheduler::GetInstance();
        service_chain_task_id_ = scheduler->GetTaskId("bgp::ServiceChain");
    }

    process_queue_ =
        new WorkQueue<ServiceChainRequestT *>(service_chain_task_id_, 0,
                     bind(&ServiceChainMgr::RequestHandler, this, _1));

    id_ = server->routing_instance_mgr()->RegisterInstanceOpCallback(
        bind(&ServiceChainMgr::RoutingInstanceCallback, this, _1, _2));

    PeerRibMembershipManager *membership_mgr = server->membership_mgr();
    registration_id_ = membership_mgr->RegisterPeerRegistrationCallback(
        bind(&ServiceChainMgr::PeerRegistrationCallback, this, _1, _2, _3));
}

template <typename T>
ServiceChainMgr<T>::~ServiceChainMgr() {
    delete process_queue_;
    server()->routing_instance_mgr()->UnregisterInstanceOpCallback(id_);
    PeerRibMembershipManager *membership_mgr = server()->membership_mgr();
    membership_mgr->UnregisterPeerRegistrationCallback(registration_id_);
}

template <>
Address::Family ServiceChainMgr<ServiceChainInet>::GetFamily() const {
    return Address::INET;
}

template <>
Address::Family ServiceChainMgr<ServiceChainInet6>::GetFamily() const {
    return Address::INET6;
}

template <typename T>
void ServiceChainMgr<T>::Enqueue(ServiceChainRequestT *req) {
    process_queue_->Enqueue(req);
}

template <typename T>
bool ServiceChainMgr<T>::LocateServiceChain(RoutingInstance *rtinstance,
    const ServiceChainConfig &config) {
    CHECK_CONCURRENCY("bgp::Config");
    // Verify whether the entry already exists
    ServiceChainMap::iterator it = chain_set_.find(rtinstance);
    if (it != chain_set_.end()) {
        ServiceChainT *chain = static_cast<ServiceChainT *>(it->second.get());
        if (chain->CompareServiceChainConfig(config)) {
            BGP_LOG_STR(BgpMessage, SandeshLevel::SYS_DEBUG, BGP_LOG_FLAG_TRACE,
                "No update in ServiceChain config : " << rtinstance->name());
            return true;
        }

        // Entry already exists. Update of match condition
        // The routing instance to pending resolve such that
        // service chain is created after stop done cb
        AddPendingServiceChain(rtinstance);

        if (it->second->deleted()) {
            // Wait for the delete complete cb
            return false;
        }

        BgpConditionListener::RequestDoneCb callback =
            bind(&ServiceChainMgr::StopServiceChainDone, this, _1, _2);

        listener_->RemoveMatchCondition(
                         chain->dest_table(), it->second.get(), callback);

        listener_->RemoveMatchCondition(
                         chain->connected_table(), it->second.get(), callback);
        return true;
    }

    RoutingInstanceMgr *mgr = server()->routing_instance_mgr();
    RoutingInstance *dest = mgr->GetRoutingInstance(config.routing_instance);
    // Destination routing instance is not yet created.
    if (dest == NULL || dest->deleted()) {
        // Wait for the creation of RoutingInstance
        AddPendingServiceChain(rtinstance);
        return false;
    }

    RoutingInstance *connected_ri = NULL;
    if (config.source_routing_instance == "") {
        connected_ri = rtinstance;
        assert(!rtinstance->deleted());
    } else {
        connected_ri = mgr->GetRoutingInstance(config.source_routing_instance);
    }
    // routing instance to search for connected route is not yet created.
    if (connected_ri == NULL || connected_ri->deleted()) {
        // Wait for the creation of RoutingInstance where connected route
        // will be published
        AddPendingServiceChain(rtinstance);
        return false;
    }

    // Add to pending queue if the service chain address is invalid.
    error_code ec;
    AddressT chain_addr =
        AddressT::from_string(config.service_chain_address, ec);
    if (ec != 0) {
        AddPendingServiceChain(rtinstance);
        return false;
    }

    // Get the BGP Tables to add condition
    BgpTable *connected_table = connected_ri->GetTable(GetFamily());
    assert(connected_table);
    BgpTable *dest_table = dest->GetTable(GetFamily());
    assert(dest_table);

    // Allocate the new service chain and verify whether one already exists
    ServiceChainPtr chain = ServiceChainPtr(new ServiceChainT(
        rtinstance, dest, connected_ri, config.prefix, chain_addr));

    if (aggregate_host_route()) {
        ServiceChainT *obj = static_cast<ServiceChainT *>(chain.get());
        obj->set_aggregate_enable();
    }

    // Add the new service chain request
    chain_set_.insert(make_pair(rtinstance, chain));
    listener_->AddMatchCondition(
        connected_table, chain.get(), BgpConditionListener::RequestDoneCb());
    listener_->AddMatchCondition(
        dest_table, chain.get(), BgpConditionListener::RequestDoneCb());

    // Delete from the pending list. The instance would already have been
    // removed from the pending list if this method is called when trying
    // to resolve items in the pending list.  However, if this method is
    // called when processing a change in the service chain config, then
    // we may need to remove it from the pending list.
    DeletePendingServiceChain(rtinstance);
    return true;
}


template <typename T>
ServiceChain<T> *ServiceChainMgr<T>::FindServiceChain(const string &src) {
    RoutingInstance *rtinstance =
        server()->routing_instance_mgr()->GetRoutingInstance(src);
    if (!rtinstance) return NULL;
    ServiceChainMap::iterator it = chain_set_.find(rtinstance);
    if (it == chain_set_.end()) return NULL;
    ServiceChainT *chain = static_cast<ServiceChainT *>(it->second.get());
    return chain;
}

template <typename T>
ServiceChain<T> *ServiceChainMgr<T>::FindServiceChain(
    RoutingInstance *rtinstance) {
    ServiceChainMap::iterator it = chain_set_.find(rtinstance);
    if (it == chain_set_.end())
        return NULL;
    ServiceChainT *chain = static_cast<ServiceChainT *>(it->second.get());
    return chain;
}

template <typename T>
bool ServiceChainMgr<T>::ResolvePendingServiceChain() {
    CHECK_CONCURRENCY("bgp::Config");
    for (UnresolvedServiceChainList::iterator it = pending_chain_.begin(), next;
         it != pending_chain_.end(); it = next) {
        next = it;
        ++next;
        RoutingInstance *rtinst = *it;
        pending_chain_.erase(it);
        LocateServiceChain(rtinst,
                           rtinst->config()->service_chain_list().front());
    }
    return true;
}

template <typename T>
void ServiceChainMgr<T>::RoutingInstanceCallback(string name, int op) {
    if (op == RoutingInstanceMgr::INSTANCE_ADD) StartResolve();
}

template <typename T>
void ServiceChainMgr<T>::StartResolve() {
    if (pending_chain_.empty() == false) {
        resolve_trigger_->Set();
    }
}

template <typename T>
void ServiceChainMgr<T>::StopServiceChainDone(BgpTable *table,
                                           ConditionMatch *info) {
    // Post the RequestDone event to ServiceChain task to take Action
    ServiceChainRequestT *req =
        new ServiceChainRequestT(ServiceChainRequestT::STOP_CHAIN_DONE, table,
                                NULL, PrefixT(), ServiceChainPtr(info));

    server()->service_chain_mgr()->Enqueue(req);
    return;
}

template <typename T>
void ServiceChainMgr<T>::StopServiceChain(RoutingInstance *src) {
    // Remove the src routing instance from the pending_chain_
    pending_chain_.erase(src);

    ServiceChainMap::iterator it = chain_set_.find(src);
    if (it == chain_set_.end()) {
        return;
    }

    if (it->second->deleted()) {
        return;
    }
    BgpConditionListener::RequestDoneCb callback =
        bind(&ServiceChainMgr::StopServiceChainDone, this, _1, _2);

    ServiceChainT *obj = static_cast<ServiceChainT *>(it->second.get());
    listener_->RemoveMatchCondition(obj->dest_table(), obj, callback);
    listener_->RemoveMatchCondition(obj->connected_table(), obj, callback);
}

template <typename T>
void ServiceChainMgr<T>::PeerRegistrationCallback(IPeer *peer, BgpTable *table,
                                               bool unregister) {
    CHECK_CONCURRENCY("bgp::PeerMembership");

    // Bail if it's not an XMPP peer.
    if (!peer->IsXmppPeer())
        return;

    // Bail if there's no service chain for the instance.
    ServiceChainT *chain = FindServiceChain(table->routing_instance());
    if (!chain)
        return;

    // Post event to ServiceChain task to update all routes.
    ServiceChainRequestT *req =
        new ServiceChainRequestT(ServiceChainRequestT::UPDATE_ALL_ROUTES, NULL,
                                NULL, PrefixT(), ServiceChainPtr(chain));
    Enqueue(req);
}

template <typename T>
uint32_t ServiceChainMgr<T>::GetDownServiceChainCount() const {
    uint32_t count = 0;
    for (ServiceChainMap::const_iterator it = chain_set_.begin();
         it != chain_set_.end(); ++it) {
        const ServiceChainT *chain =
             static_cast<const ServiceChainT *>(it->second.get());
        if (!chain->IsConnectedRouteValid())
            count++;
    }
    return count;
}

void ShowServiceChainReq::HandleRequest() const {
    BgpSandeshContext *bsc = static_cast<BgpSandeshContext *>(client_context());
    ServiceChainMgr<ServiceChainInet> *mgr =
        bsc->bgp_server->service_chain_mgr();
    ShowServiceChainResp *resp = new ShowServiceChainResp;
    resp->set_context(context());
    ServiceChainRequest<ServiceChainInet> *req =
        new ServiceChainRequest<ServiceChainInet>(
            ServiceChainRequest<ServiceChainInet>::SHOW_SERVICE_CHAIN,
            resp, get_search_string());
    mgr->Enqueue(req);
}

void ShowPendingServiceChainReq::HandleRequest() const {
    BgpSandeshContext *bsc = static_cast<BgpSandeshContext *>(client_context());
    ServiceChainMgr<ServiceChainInet> *mgr =
        bsc->bgp_server->service_chain_mgr();
    ShowPendingServiceChainResp *resp = new ShowPendingServiceChainResp;
    resp->set_context(context());
    ServiceChainRequest<ServiceChainInet> *req =
        new ServiceChainRequest<ServiceChainInet>(
            ServiceChainRequest<ServiceChainInet>::SHOW_PENDING_CHAIN,
            resp , get_search_string());
    mgr->Enqueue(req);
}

template ServiceChainMgr<ServiceChainInet>::ServiceChainMgr(BgpServer *server);
template ServiceChainMgr<ServiceChainInet>::~ServiceChainMgr();
template uint32_t ServiceChainMgr<ServiceChainInet>::GetDownServiceChainCount()
    const;
template void ServiceChainMgr<ServiceChainInet>::StopServiceChain(
    RoutingInstance *src);
template bool ServiceChainMgr<ServiceChainInet>::LocateServiceChain(
    RoutingInstance *rtinstance, ServiceChainConfig const &config);

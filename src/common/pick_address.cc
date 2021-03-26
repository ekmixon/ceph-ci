// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2012 Inktank
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "common/pick_address.h"

#include <netdb.h>
#include <string>
#include <string.h>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <fmt/format.h>

#include "include/ipaddr.h"
#include "include/str_list.h"
#include "common/ceph_context.h"
#ifndef WITH_SEASTAR
#include "common/config.h"
#include "common/config_obs.h"
#endif
#include "common/debug.h"
#include "common/errno.h"
#include "common/numa.h"

#define dout_subsys ceph_subsys_

using std::string;
using std::vector;

namespace {

bool matches_with_name(const ifaddrs& ifa, const std::string& if_name)
{
  return if_name.compare(ifa.ifa_name) == 0;
}

bool is_addr_allowed(const ifaddrs& ifa, bool exclude_lo_iface)
{
  if (exclude_lo_iface && strcmp(ifa.ifa_name, "lo") == 0) {
    return false;
  }
  if (boost::starts_with(ifa.ifa_name, "lo:")) {
    return false;
  }
  return true;
}

bool matches_with_net(const ifaddrs& ifa,
                      const sockaddr* net,
                      unsigned int prefix_len,
                      unsigned ipv)
{
  switch (net->sa_family) {
  case AF_INET:
    if (ipv & CEPH_PICK_ADDRESS_IPV4) {
      return matches_ipv4_in_subnet(ifa, (struct sockaddr_in*)net, prefix_len);
    }
    break;
  case AF_INET6:
    if (ipv & CEPH_PICK_ADDRESS_IPV6) {
      return matches_ipv6_in_subnet(ifa, (struct sockaddr_in6*)net, prefix_len);
    }
    break;
  }
  return false;
}

bool matches_with_net(CephContext *cct,
                      const ifaddrs& ifa,
                      const std::string& s,
                      unsigned ipv)
{
  struct sockaddr_storage net;
  unsigned int prefix_len;
  if (!parse_network(s.c_str(), &net, &prefix_len)) {
    lderr(cct) << "unable to parse network: " << s << dendl;
    exit(1);
  }
  return matches_with_net(ifa, (sockaddr*)&net, prefix_len, ipv);
}

bool matches_with_numa_node(const ifaddrs& ifa, int numa_node)
{
#if defined(WITH_SEASTAR) || defined(_WIN32)
  return true;
#else
  if (numa_node < 0) {
    return true;
  }
  int if_node = -1;
  int r = get_iface_numa_node(ifa.ifa_name, &if_node);
  if (r < 0) {
    return false;
  }
  return if_node == numa_node;
#endif
}
}
const struct sockaddr *find_ip_in_subnet_list(
  CephContext *cct,
  const struct ifaddrs *ifa,
  unsigned ipv,
  const std::string &networks,
  const std::string &interfaces,
  bool exclude_lo_iface,
  int numa_node)
{
  const auto ifs = get_str_list(interfaces);
  const auto nets = get_str_list(networks);
  if (!ifs.empty() && nets.empty()) {
      lderr(cct) << "interface names specified but not network names" << dendl;
      exit(1);
  }

  for (const auto* addr = ifa; addr != nullptr; addr = addr->ifa_next) {
    if (!is_addr_allowed(*addr, exclude_lo_iface)) {
      continue;
    }
    if ((ifs.empty() ||
         std::any_of(std::begin(ifs), std::end(ifs),
                     [&](const auto& if_name) {
                       return matches_with_name(*addr, if_name);
                     })) &&
        (nets.empty() ||
         std::any_of(std::begin(nets), std::end(nets),
                     [&](const auto& net) {
                       return matches_with_net(cct, *addr, net, ipv);
                     })) &&
        matches_with_numa_node(*addr, numa_node)) {
      return addr->ifa_addr;
    }
  }
  return nullptr;
}

#ifndef WITH_SEASTAR
// observe this change
struct Observer : public md_config_obs_t {
  const char *keys[2];
  explicit Observer(const char *c) {
    keys[0] = c;
    keys[1] = NULL;
  }

  const char** get_tracked_conf_keys() const override {
    return (const char **)keys;
  }
  void handle_conf_change(const ConfigProxy& conf,
			  const std::set <std::string> &changed) override {
    // do nothing.
  }
};

static void fill_in_one_address(CephContext *cct,
				const struct ifaddrs *ifa,
				const string &networks,
				const string &interfaces,
				bool exclude_lo_iface,
				const char *conf_var,
				int numa_node = -1)
{
  const struct sockaddr *found = find_ip_in_subnet_list(
    cct,
    ifa,
    CEPH_PICK_ADDRESS_IPV4|CEPH_PICK_ADDRESS_IPV6,
    networks,
    interfaces,
    numa_node);
  if (!found) {
    lderr(cct) << "unable to find any IP address in networks '" << networks
	       << "' interfaces '" << interfaces << "'" << dendl;
    exit(1);
  }

  char buf[INET6_ADDRSTRLEN];
  int err;

  err = getnameinfo(found,
		    (found->sa_family == AF_INET)
		    ? sizeof(struct sockaddr_in)
		    : sizeof(struct sockaddr_in6),

		    buf, sizeof(buf),
		    nullptr, 0,
		    NI_NUMERICHOST);
  if (err != 0) {
    lderr(cct) << "unable to convert chosen address to string: " << gai_strerror(err) << dendl;
    exit(1);
  }

  Observer obs(conf_var);

  cct->_conf.add_observer(&obs);

  cct->_conf.set_val_or_die(conf_var, buf);
  cct->_conf.apply_changes(nullptr);

  cct->_conf.remove_observer(&obs);
}

void pick_addresses(CephContext *cct, int needs)
{
  auto public_addr = cct->_conf.get_val<entity_addr_t>("public_addr");
  auto public_network = cct->_conf.get_val<std::string>("public_network");
  auto public_network_interface =
    cct->_conf.get_val<std::string>("public_network_interface");
  auto cluster_addr = cct->_conf.get_val<entity_addr_t>("cluster_addr");
  auto cluster_network = cct->_conf.get_val<std::string>("cluster_network");
  auto cluster_network_interface =
    cct->_conf.get_val<std::string>("cluster_network_interface");

  struct ifaddrs *ifa;
  int r = getifaddrs(&ifa);
  if (r < 0) {
    string err = cpp_strerror(errno);
    lderr(cct) << "unable to fetch interfaces and addresses: " << err << dendl;
    exit(1);
  }
  auto free_ifa = make_scope_guard([ifa] { freeifaddrs(ifa); });

  const bool exclude_lo_iface =
    cct->_conf.get_val<bool>("ms_bind_exclude_lo_iface");
  if ((needs & CEPH_PICK_ADDRESS_PUBLIC) &&
    public_addr.is_blank_ip() && !public_network.empty()) {
    fill_in_one_address(cct, ifa, public_network, public_network_interface,
			exclude_lo_iface,
			"public_addr");
  }

  if ((needs & CEPH_PICK_ADDRESS_CLUSTER) && cluster_addr.is_blank_ip()) {
    if (!cluster_network.empty()) {
      fill_in_one_address(cct, ifa, cluster_network, cluster_network_interface,
			  exclude_lo_iface,
			  "cluster_addr");
    } else {
      if (!public_network.empty()) {
        lderr(cct) << "Public network was set, but cluster network was not set " << dendl;
        lderr(cct) << "    Using public network also for cluster network" << dendl;
        fill_in_one_address(cct, ifa, public_network, public_network_interface,
			    exclude_lo_iface,
			    "cluster_addr");
      }
    }
  }
}
#endif	// !WITH_SEASTAR

static int fill_in_one_address(
  CephContext *cct,
  const struct ifaddrs *ifa,
  unsigned ipv,
  const string &networks,
  const string &interfaces,
  bool exclude_lo_iface,
  entity_addrvec_t *addrs,
  int numa_node = -1)
{
  const struct sockaddr *found = find_ip_in_subnet_list(cct, ifa, ipv,
							networks,
							interfaces,
							exclude_lo_iface,
							numa_node);
  if (!found) {
    std::string ip_type = "";
    if ((ipv & CEPH_PICK_ADDRESS_IPV4) && (ipv & CEPH_PICK_ADDRESS_IPV6)) {
      ip_type = "IPv4 or IPv6";
    } else if (ipv & CEPH_PICK_ADDRESS_IPV4) {
      ip_type = "IPv4";
    } else {
      ip_type = "IPv6";
    }
    lderr(cct) << "unable to find any " << ip_type << " address in networks '"
               << networks << "' interfaces '" << interfaces << "'" << dendl;
    return -1;
  }

  char buf[INET6_ADDRSTRLEN];
  int err;

  err = getnameinfo(found,
		    (found->sa_family == AF_INET)
		    ? sizeof(struct sockaddr_in)
		    : sizeof(struct sockaddr_in6),

		    buf, sizeof(buf),
		    nullptr, 0,
		    NI_NUMERICHOST);
  if (err != 0) {
    lderr(cct) << "unable to convert chosen address to string: " << gai_strerror(err) << dendl;
    return -1;
  }

  entity_addr_t addr;
  const char *end = 0;
  bool r = addr.parse(buf, &end);
  if (!r) {
    return -1;
  }
  addrs->v.push_back(addr);
  return 0;
}

int pick_addresses(
  CephContext *cct,
  unsigned flags,
  struct ifaddrs *ifa,
  entity_addrvec_t *addrs,
  int preferred_numa_node)
{
  addrs->v.clear();

  unsigned addrt = (flags & (CEPH_PICK_ADDRESS_PUBLIC |
			     CEPH_PICK_ADDRESS_CLUSTER));
  if (addrt == 0 ||
      addrt == (CEPH_PICK_ADDRESS_PUBLIC |
		CEPH_PICK_ADDRESS_CLUSTER)) {
    return -EINVAL;
  }
  unsigned msgrv = flags & (CEPH_PICK_ADDRESS_MSGR1 |
			    CEPH_PICK_ADDRESS_MSGR2);
  if (msgrv == 0) {
    if (cct->_conf.get_val<bool>("ms_bind_msgr1")) {
      msgrv |= CEPH_PICK_ADDRESS_MSGR1;
    }
    if (cct->_conf.get_val<bool>("ms_bind_msgr2")) {
      msgrv |= CEPH_PICK_ADDRESS_MSGR2;
    }
    if (msgrv == 0) {
      return -EINVAL;
    }
  }
  unsigned ipv = flags & (CEPH_PICK_ADDRESS_IPV4 |
			  CEPH_PICK_ADDRESS_IPV6);
  if (ipv == 0) {
    if (cct->_conf.get_val<bool>("ms_bind_ipv4")) {
      ipv |= CEPH_PICK_ADDRESS_IPV4;
    }
    if (cct->_conf.get_val<bool>("ms_bind_ipv6")) {
      ipv |= CEPH_PICK_ADDRESS_IPV6;
    }
    if (ipv == 0) {
      return -EINVAL;
    }
    if (cct->_conf.get_val<bool>("ms_bind_prefer_ipv4")) {
      flags |= CEPH_PICK_ADDRESS_PREFER_IPV4;
    } else {
      flags &= ~CEPH_PICK_ADDRESS_PREFER_IPV4;
    }
  }

  entity_addr_t addr;
  string networks;
  string interfaces;
  if (addrt & CEPH_PICK_ADDRESS_PUBLIC) {
    addr = cct->_conf.get_val<entity_addr_t>("public_addr");
    networks = cct->_conf.get_val<std::string>("public_network");
    interfaces =
      cct->_conf.get_val<std::string>("public_network_interface");
  } else {
    addr = cct->_conf.get_val<entity_addr_t>("cluster_addr");
    networks = cct->_conf.get_val<std::string>("cluster_network");
    interfaces =
      cct->_conf.get_val<std::string>("cluster_network_interface");
    if (networks.empty()) {
      lderr(cct) << "Falling back to public interface" << dendl;
      // fall back to public_ network and interface if cluster is not set
      networks = cct->_conf.get_val<std::string>("public_network");
      interfaces =
	cct->_conf.get_val<std::string>("public_network_interface");
    }
  }
  if (addr.is_blank_ip() &&
      !networks.empty()) {
    int ipv4_r = !(ipv & CEPH_PICK_ADDRESS_IPV4) ? 0 : -1;
    int ipv6_r = !(ipv & CEPH_PICK_ADDRESS_IPV6) ? 0 : -1;
    const bool exclude_lo_iface =
      cct->_conf.get_val<bool>("ms_bind_exclude_lo_iface");
    // first try on preferred numa node (if >= 0), then anywhere.
    while (true) {
      // note: pass in ipv to filter the matching addresses
      if ((ipv & CEPH_PICK_ADDRESS_IPV4) &&
	  (flags & CEPH_PICK_ADDRESS_PREFER_IPV4)) {
	ipv4_r = fill_in_one_address(cct, ifa, CEPH_PICK_ADDRESS_IPV4,
                                     networks, interfaces,
				     exclude_lo_iface,
				     addrs,
                                     preferred_numa_node);
      }
      if (ipv & CEPH_PICK_ADDRESS_IPV6) {
	ipv6_r = fill_in_one_address(cct, ifa, CEPH_PICK_ADDRESS_IPV6,
                                     networks, interfaces,
				     exclude_lo_iface,
				     addrs,
                                     preferred_numa_node);
      }
      if ((ipv & CEPH_PICK_ADDRESS_IPV4) &&
	  !(flags & CEPH_PICK_ADDRESS_PREFER_IPV4)) {
	ipv4_r = fill_in_one_address(cct, ifa, CEPH_PICK_ADDRESS_IPV4,
                                     networks, interfaces,
				     exclude_lo_iface,
				     addrs,
                                     preferred_numa_node);
      }
      if (ipv4_r >= 0 && ipv6_r >= 0) {
	break;
      }
      if (preferred_numa_node < 0) {
	return ipv4_r >= 0 && ipv6_r >= 0 ? 0 : -1;
      }
      preferred_numa_node = -1;      // try any numa node
    }
  }

  // note: we may have a blank addr here

  // ipv4 and/or ipv6?
  if (addrs->v.empty()) {
    addr.set_type(entity_addr_t::TYPE_MSGR2);
    if ((ipv & CEPH_PICK_ADDRESS_IPV4) &&
	(flags & CEPH_PICK_ADDRESS_PREFER_IPV4)) {
      addr.set_family(AF_INET);
      addrs->v.push_back(addr);
    }
    if (ipv & CEPH_PICK_ADDRESS_IPV6) {
      addr.set_family(AF_INET6);
      addrs->v.push_back(addr);
    }
    if ((ipv & CEPH_PICK_ADDRESS_IPV4) &&
	!(flags & CEPH_PICK_ADDRESS_PREFER_IPV4)) {
      addr.set_family(AF_INET);
      addrs->v.push_back(addr);
    }
  }

  // msgr2 or legacy or both?
  if (msgrv == (CEPH_PICK_ADDRESS_MSGR1 | CEPH_PICK_ADDRESS_MSGR2)) {
    vector<entity_addr_t> v;
    v.swap(addrs->v);
    for (auto a : v) {
      a.set_type(entity_addr_t::TYPE_MSGR2);
      if (flags & CEPH_PICK_ADDRESS_DEFAULT_MON_PORTS) {
	a.set_port(CEPH_MON_PORT_IANA);
      }
      addrs->v.push_back(a);
      a.set_type(entity_addr_t::TYPE_LEGACY);
      if (flags & CEPH_PICK_ADDRESS_DEFAULT_MON_PORTS) {
	a.set_port(CEPH_MON_PORT_LEGACY);
      }
      addrs->v.push_back(a);
    }
  } else if (msgrv == CEPH_PICK_ADDRESS_MSGR1) {
    for (auto& a : addrs->v) {
      a.set_type(entity_addr_t::TYPE_LEGACY);
    }
  } else {
    for (auto& a : addrs->v) {
      a.set_type(entity_addr_t::TYPE_MSGR2);
    }
  }

  return 0;
}

int pick_addresses(
  CephContext *cct,
  unsigned flags,
  entity_addrvec_t *addrs,
  int preferred_numa_node)
{
  struct ifaddrs *ifa;
  int r = getifaddrs(&ifa);
  if (r < 0) {
    r = -errno;
    string err = cpp_strerror(r);
    lderr(cct) << "unable to fetch interfaces and addresses: "
	       <<  cpp_strerror(r) << dendl;
    return r;
  }
  r = pick_addresses(cct, flags, ifa, addrs, preferred_numa_node);
  freeifaddrs(ifa);
  return r;
}

std::string pick_iface(CephContext *cct, const struct sockaddr_storage &network)
{
  struct ifaddrs *ifa;
  int r = getifaddrs(&ifa);
  if (r < 0) {
    string err = cpp_strerror(errno);
    lderr(cct) << "unable to fetch interfaces and addresses: " << err << dendl;
    return {};
  }
  auto free_ifa = make_scope_guard([ifa] { freeifaddrs(ifa); });
  const unsigned int prefix_len = std::max(sizeof(in_addr::s_addr), sizeof(in6_addr::s6_addr)) * CHAR_BIT;
  for (auto addr = ifa; addr != nullptr; addr = addr->ifa_next) {
    if (matches_with_net(*ifa, (const struct sockaddr *) &network, prefix_len,
			 CEPH_PICK_ADDRESS_IPV4 | CEPH_PICK_ADDRESS_IPV6)) {
      return addr->ifa_name;
    }
  }
  return {};
}


bool have_local_addr(CephContext *cct, const std::list<entity_addr_t>& ls, entity_addr_t *match)
{
  struct ifaddrs *ifa;
  int r = getifaddrs(&ifa);
  if (r < 0) {
    lderr(cct) << "unable to fetch interfaces and addresses: " << cpp_strerror(errno) << dendl;
    exit(1);
  }
  auto free_ifa = make_scope_guard([ifa] { freeifaddrs(ifa); });

  for (struct ifaddrs *addrs = ifa; addrs != nullptr; addrs = addrs->ifa_next) {
    if (addrs->ifa_addr) {
      entity_addr_t a;
      a.set_sockaddr(addrs->ifa_addr);
      for (auto& p : ls) {
        if (a.is_same_host(p)) {
          *match = p;
          return true;
        }
      }
    }
  }
  return false;
}

int get_iface_numa_node(
  const std::string& iface,
  int *node)
{
  enum class iface_t {
    PHY_PORT,
    BOND_PORT
  } ifatype = iface_t::PHY_PORT;
  std::string_view ifa{iface};
  if (auto pos = ifa.find(":"); pos != ifa.npos) {
    ifa.remove_suffix(ifa.size() - pos);
  }
  string fn = fmt::format("/sys/class/net/{}/device/numa_node", ifa);
  int fd = ::open(fn.c_str(), O_RDONLY);
  if (fd < 0) {
    fn = fmt::format("/sys/class/net/{}/bonding/slaves", ifa);
    fd = ::open(fn.c_str(), O_RDONLY);
    if (fd < 0) {
      return -errno;
    }
    ifatype = iface_t::BOND_PORT;
  }

  int r = 0;
  char buf[1024];
  char *endptr = 0;
  r = safe_read(fd, &buf, sizeof(buf));
  if (r < 0) {
    goto out;
  }
  buf[r] = 0;
  while (r > 0 && ::isspace(buf[--r])) {
    buf[r] = 0;
  }

  switch (ifatype) {
  case iface_t::PHY_PORT:
    *node = strtoll(buf, &endptr, 10);
    if (endptr != buf + strlen(buf)) {
      r = -EINVAL;
      goto out;
    }
    r = 0;
    break;
  case iface_t::BOND_PORT:
    int bond_node = -1;
    std::vector<std::string> sv;
    std::string ifacestr = buf;
    get_str_vec(ifacestr, " ", sv);
    for (auto& iter : sv) {
      int bn = -1;
      r = get_iface_numa_node(iter, &bn);
      if (r >= 0) {
        if (bond_node == -1 || bn == bond_node) {
          bond_node = bn;
        } else {
          *node = -2;
          goto out;
        }
      } else {
        goto out;
      }
    }
    *node = bond_node;
    break;
  }

  out:
  ::close(fd);
  return r;
}


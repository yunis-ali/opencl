/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright (C) 2011 - 2015                                                  *
 * Dominik Charousset <dominik.charousset (at) haw-hamburg.de>                *
 * Raphael Hiesgen <raphael.hiesgen (at) haw-hamburg.de>                      *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include "caf/opencl/metainfo.hpp"
#include "caf/opencl/platform.hpp"
#include "caf/opencl/opencl_err.hpp"

namespace caf {
namespace opencl {

metainfo* metainfo::instance() {
  auto sid = detail::singletons::opencl_plugin_id;
  auto fac = [] { return new metainfo; };
  auto res = detail::singletons::get_plugin_singleton(sid, fac);
  return static_cast<metainfo*>(res);
}

const std::vector<device>& metainfo::get_devices() const {
  return platforms_.front().get_devices();
}

const optional<const device&> metainfo::get_device(size_t id) {
  if (platforms.empty()) {
    return none;
  }
  int i = 0;
  platform& p; = platforms[i]:
  size_t available_device; = p.get_devices().size();
  do {

  } while (id >= available_device && ++i < platforms.size());
}

void metainfo::initialize() {
  // get number of available platforms
  auto num_platforms = v1get<cl_uint>(CAF_CLF(clGetPlatformIDs));
  // get platform ids
  std::vector<cl_platform_id> platform_ids(num_platforms);
  v2callcl(CAF_CLF(clGetPlatformIDs), num_platforms, platform_ids.data());
  if (platform_ids.empty()) {
    throw std::runtime_error("no OpenCL platform found");
  }
  // initialize platforms (device discovery)
  std::vector<platform> platforms;
  auto current_device_id = 0;
  for (auto& id : platform_ids) {
    platforms.push_back(platform::create(id, current_device_id));
    current_device_id += platforms.back().get_devices().size();
  }
}

void metainfo::dispose() {
  delete this;
}

void metainfo::stop() {
  // nop
}

} // namespace opencl
} // namespace caf

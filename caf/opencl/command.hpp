/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright (C) 2011 - 2015                                                  *
 * Dominik Charousset <dominik.charousset (at) haw-hamburg.de>                *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#ifndef CAF_OPENCL_COMMAND_HPP
#define CAF_OPENCL_COMMAND_HPP

#include <tuple>
#include <vector>
#include <numeric>
#include <algorithm>
#include <functional>

#include "caf/detail/logging.hpp"
#include "caf/opencl/global.hpp"
#include "caf/abstract_actor.hpp"
#include "caf/response_promise.hpp"
#include "caf/opencl/smart_ptr.hpp"
#include "caf/opencl/opencl_err.hpp"
#include "caf/detail/scope_guard.hpp"

namespace caf {
namespace opencl {

template <class FacadeType, class... Ts>
class command : public ref_counted {
public:
  command(response_promise handle, intrusive_ptr<FacadeType> actor_facade,
          std::vector<cl_event> events, std::vector<mem_ptr> arguments,
          std::vector<size_t> result_sizes, message msg)
      : result_sizes_(result_sizes),
        handle_(handle),
        actor_facade_(actor_facade),
        queue_(actor_facade->queue_),
        mem_in_events_(std::move(events)),
        arguments_(std::move(arguments)),
        msg_(msg) {
    // nop
  }

  ~command() {
    for (auto& e : mem_in_events_) {
      v1callcl(CAF_CLF(clReleaseEvent),e);
    }
    for (auto& e : mem_out_events_) {
      v1callcl(CAF_CLF(clReleaseEvent),e);
    }
  }

  void enqueue() {
    // Errors in this function can not be handled by opencl_err.hpp
    // because they require non-standard error handling
    CAF_LOG_TRACE("command::enqueue()");
    this->ref(); // reference held by the OpenCL comand queue
    cl_event event_k;
    auto data_or_nullptr = [](const dim_vec& vec) {
      return vec.empty() ? nullptr : vec.data();
    };
    // OpenCL expects cl_uint (unsigned int), hence the cast
    cl_int err = clEnqueueNDRangeKernel(
      queue_.get(), actor_facade_->kernel_.get(),
      static_cast<cl_uint>(actor_facade_->config_.dimensions().size()),
      data_or_nullptr(actor_facade_->config_.offsets()),
      data_or_nullptr(actor_facade_->config_.dimensions()),
      data_or_nullptr(actor_facade_->config_.local_dimensions()),
      static_cast<cl_uint>(mem_in_events_.size()),
      (mem_in_events_.empty() ? nullptr : mem_in_events_.data()), &event_k);
    if (err != CL_SUCCESS) {
      CAF_LOGMF(CAF_ERROR, "clEnqueueNDRangeKernel: " << get_opencl_error(err));
      this->deref(); // or can anything actually happen?
      return;
    } else {
      mem_out_events_.push_back(std::move(event_k));
      enqueue_read_buffers(event_k, detail::get_indices(result_buffers_));
      cl_event marker;
#if defined (CL_VERSION_1_2)
      err = clEnqueueMarkerWithWaitList(queue_.get(), mem_out_events_.size(),
                                        mem_out_events_.data(), &marker);
#else
      err = clEnqueueMarker(queue_.get(), &marker);
#endif
      mem_out_events_.push_back(std::move(marker));
      if (err != CL_SUCCESS) {
        CAF_LOGMF(CAF_ERROR, "clSetEventCallback: " << get_opencl_error(err));
        this->deref(); // callback is not set
        return;
      }
      err = clSetEventCallback(marker, CL_COMPLETE,
                               [](cl_event, cl_int, void* data) {
                                 auto cmd = reinterpret_cast<command*>(data);
                                 cmd->handle_results();
                                 cmd->deref();
                               },
                               this);
      if (err != CL_SUCCESS) {
        CAF_LOGMF(CAF_ERROR, "clSetEventCallback: " << get_opencl_error(err));
        this->deref(); // callback is not set
        return;
      }
      err = clFlush(queue_.get());
      if (err != CL_SUCCESS) {
        CAF_LOGMF(CAF_ERROR, "clFlush: " << get_opencl_error(err));
      }
    }
  }

  void enqueue_read_buffers(cl_event&, detail::int_list<>) {
    // nop
  }

  template <long I, long... Is>
  void enqueue_read_buffers(cl_event& kernel_done, detail::int_list<I, Is...>) {
    using value_type =
      typename std::tuple_element<I, std::tuple<std::vector<Ts>...>>::type;
    cl_event event;
    auto size = sizeof(value_type) * result_sizes_[I];
    std::get<I>(result_buffers_).resize(size);
    auto err = clEnqueueReadBuffer(queue_.get(), arguments_.back().get(),
                                   CL_FALSE, 0, size,
                                   std::get<I>(result_buffers_).data(),
                                   1, &kernel_done, &event);
    if (err != CL_SUCCESS) {
      this->deref(); // failed to enqueue command
      throw std::runtime_error("clEnqueueReadBuffer: " +
                               get_opencl_error(err));
    }
    mem_out_events_.push_back(std::move(event));
    enqueue_read_buffers(kernel_done, detail::int_list<Is...>{});
  }

private:
  std::vector<size_t> result_sizes_;
  response_promise handle_;
  intrusive_ptr<FacadeType> actor_facade_;
  command_queue_ptr queue_;
  std::vector<cl_event> mem_in_events_;
  std::vector<cl_event> mem_out_events_;
  std::vector<mem_ptr> arguments_;
  std::tuple<Ts...> result_buffers_;
  message msg_; // required to keep the argument buffers alive (async copy)

  void handle_results() {
    auto& map_fun = actor_facade_->map_results_;
    auto msg = map_fun ? apply_args(map_fun,
                                    detail::get_indices(result_buffers_),
                                    result_buffers_)
                       : make_message(std::move(result_buffers_));
    handle_.deliver(std::move(msg));
  }
};

} // namespace opencl
} // namespace caf

#endif // CAF_OPENCL_COMMAND_HPP

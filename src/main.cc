/**
 * @file    main.cc
 * @author  Yawning Angel (yawning at schwanenlied dot me)
 * @brief   obfsclient main entry point
 */

/*
 * Copyright (c) 2014, Yawning Angel <yawning at schwanenlied dot me>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <iostream>
#include <list>
#include <memory>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <unistd.h>

#include <allium/allium.h>
#include <event2/event.h>

#include "ext/optionparser.h"
#include "schwanenlied/common.h"
#include "schwanenlied/socks5_server.h"
#include "schwanenlied/pt/obfs2/client.h"
#include "schwanenlied/pt/obfs3/client.h"
#include "schwanenlied/pt/scramblesuit/client.h"

_INITIALIZE_EASYLOGGINGPP

namespace {

enum kOptionIndex {
  kUNKNOWN,
  kHELP,
  kVERSION,
  kDEBUG,
  kUNSAFE_LOGS,
  kWAIT_FOR_DEBUGGER
};

const ::option::Descriptor kUsage[] = {
  { kUNKNOWN, 0, "", "", ::option::Arg::None,
    "usage: obfsclient [OPTION]" },
  { kHELP, 0, "", "help", ::option::Arg::Optional,
    "  --help              Print usage." },
  { kVERSION, 0, "", "version", ::option::Arg::Optional,
    "  --version           Print version." },
  { kDEBUG, 0, "", "debug", ::option::Arg::Optional,
    "  --debug             Enable debugging." },
  { kUNSAFE_LOGS, 0, "", "unsafe-logs", ::option::Arg::Optional,
    "  --unsafe-logs       Unsafe logging." },
  { kWAIT_FOR_DEBUGGER, 0, "", "wait-for-debugger", ::option::Arg::Optional,
    "  --wait-for-debugger Sleep after parsing command line args." },
  { 0, 0, nullptr, nullptr, 0, nullptr }
};

using Socks5Server = schwanenlied::Socks5Server;
using Socks5Factory = schwanenlied::Socks5Server::SessionFactory;
using Obfs2Factory = schwanenlied::pt::obfs2::Client::SessionFactory;
using Obfs3Factory = schwanenlied::pt::obfs3::Client::SessionFactory;
using ScrambleSuitFactory = schwanenlied::pt::scramblesuit::Client::SessionFactory;

constexpr char kLogFileName[] = "obfsclient.log";
constexpr char kLogger[] = "main";

constexpr char kObfs2MethodName[] = "obfs2";
constexpr char kObfs3MethodName[] = "obfs3";
constexpr char kScrambleSuitMethodName[] = "scramblesuit";

struct event_base* ev_base = nullptr;
int nr_sigints = 0;

bool init_statedir(const allium_ptcfg* cfg,
                   ::std::string& path) {
  size_t len = 0;
  ::allium_ptcfg_state_dir(cfg, nullptr, &len);
  ::std::unique_ptr<char> tmp(new char[len]);
  if (0 != ::allium_ptcfg_state_dir(cfg, tmp.get(), &len))
    return false;

  path.assign(tmp.get());
  return true;
}

void init_logging(const ::std::string& path,
                  const bool debug) {
  ::el::Configurations conf;

  conf.setToDefault();
  conf.setGlobally(::el::ConfigurationType::ToStandardOutput, "false");
  conf.setGlobally(::el::ConfigurationType::Filename,
                   path + ::std::string(kLogFileName));
  conf.set(::el::Level::Debug, ::el::ConfigurationType::Format,
           "%datetime %level [%logger] %msg");
  if (!debug)
    conf.set(::el::Level::Debug, ::el::ConfigurationType::Enabled, "false");
  ::el::Helpers::addFlag(el::LoggingFlag::ImmediateFlush);
  ::el::Loggers::setDefaultConfigurations(conf, true);
  (void)::el::Loggers::getLogger(kLogger);
}

bool init_libevent() {
  if (ev_base == nullptr)
    ev_base = ::event_base_new();

  return ev_base != nullptr;
}

template<class Factory>
bool init_pt(const allium_ptcfg* cfg,
             const ::std::string state_dir,
             const char* name,
             ::std::list< ::std::unique_ptr<Socks5Factory>>& factories,
             ::std::list< ::std::unique_ptr<Socks5Server>>& listeners,
             const bool scrub_addrs = true) {
  if (::allium_ptcfg_method_requested(cfg, name) != 1)
    return false;

  if (!init_libevent()) {
    CLOG(ERROR, kLogger) << "Failed to initialize a libevent event_base";
    ::allium_ptcfg_method_error(cfg, name, "event_base_new()");
    return false;
  }

  Factory* factory = new Factory;
  Socks5Server* listener = new Socks5Server(state_dir, factory, ev_base,
                                            scrub_addrs);
  if (!listener->bind()) {
    CLOG(ERROR, kLogger) << "Failed to bind() a SOCKSv5 listener";
    ::allium_ptcfg_method_error(cfg, name, "Socks5::bind()");
out_free:
    delete factory;
    delete listener;
    return false;
  }

  struct sockaddr_in socks_addr;
  if (!listener->addr(socks_addr)) {
    CLOG(ERROR, kLogger) << "Failed to query the SOCKSv5 address";
    ::allium_ptcfg_method_error(cfg, name, "Socks5::addr()");
    goto out_free;
  }

  factories.push_back(::std::unique_ptr<Socks5Factory>(factory));
  listeners.push_back(::std::unique_ptr<Socks5Server>(listener));

  CLOG(INFO, kLogger) << "SOCKSv5 Listener: "
                      << Socks5Server::addr_to_string(reinterpret_cast<struct
                                                      sockaddr*>(&socks_addr),
                                                      false)
                      << " " << name ;

  ::allium_ptcfg_cmethod_report(cfg, name, 5,
                                reinterpret_cast<struct sockaddr*>(&socks_addr),
                                sizeof(socks_addr), nullptr, nullptr);

  return true;
}

} // (Annonymous) namespace

int main(int argc, char* argv[]) {
  // Parse the commnad line arguments
  if (argc > 0) {
    argc--;
    argv++;
  }
  ::option::Stats  stats(kUsage, argc, argv);
  ::option::Option* options = new ::option::Option[stats.options_max];
  ::option::Option* buffer = new ::option::Option[stats.buffer_max];
  ::option::Parser parse(kUsage, argc, argv, options, buffer);
  if (parse.error())
    return 1;
  if (options[kHELP] || options[kUNKNOWN]) {
    ::option::printUsage(std::cout, kUsage);
    return 0;
  }
  if (options[kVERSION]) {
    ::std::cout << PACKAGE_NAME << " " << PACKAGE_VERSION << ::std::endl;
    return 0;
  }
  const bool debug = options[kDEBUG];
  const bool scrub_ips = !options[kUNSAFE_LOGS];
  volatile bool wait_for_debugger = options[kWAIT_FOR_DEBUGGER];
  delete[] options;
  delete[] buffer;

  while (wait_for_debugger)
    sleep(0);

  // Start the PT configuration
  allium_ptcfg* cfg = ::allium_ptcfg_init();
  if (!cfg)
    return -1;

  if (::allium_ptcfg_is_server(cfg)) {
    ::allium_ptcfg_methods_done(cfg);
    ::allium_ptcfg_free(cfg);
    return -1;
  }

  // Determine the state directory and initialize logging
  ::std::string state_dir;
  if (!init_statedir(cfg, state_dir)) {
    // Should NEVER happen
    ::allium_ptcfg_methods_done(cfg);
    ::allium_ptcfg_free(cfg);
    return -1;
  }
  init_logging(state_dir, debug);

  // Log a banner
  CLOG(INFO, kLogger) << "obfsclient " << PACKAGE_VERSION <<
                         " - Initialized (PID: " << ::getpid() << ")";

  // Attempt to initialize the supported PTs
  ::std::list< ::std::unique_ptr<Socks5Factory>> factories;
  ::std::list< ::std::unique_ptr<Socks5Server>> listeners;
  bool dispatch_loop = false;
  dispatch_loop |= init_pt<Obfs3Factory>(cfg, state_dir, kObfs3MethodName,
                                         factories, listeners, scrub_ips);
  dispatch_loop |= init_pt<Obfs2Factory>(cfg, state_dir, kObfs2MethodName,
                                         factories, listeners, scrub_ips);
  dispatch_loop |= init_pt<ScrambleSuitFactory>(cfg, state_dir,
                                                kScrambleSuitMethodName,
                                                factories, listeners,
                                                scrub_ips);

  // Done with the config!
  ::allium_ptcfg_methods_done(cfg);
  ::allium_ptcfg_free(cfg);

  SL_ASSERT(factories.size() == listeners.size());
  if (dispatch_loop) {
    // Install a SIGINT handler
    event_callback_fn cb = [](evutil_socket_t sock,
                              short which,
                              void* arg) {
      ::std::list< ::std::unique_ptr<Socks5Server>>* servers =
          reinterpret_cast< ::std::list< ::std::unique_ptr<Socks5Server>>*>(arg);
      nr_sigints++;
      switch (nr_sigints) {
      case 1:
        CLOG(INFO, kLogger) << "Closing all listeners";
        for (auto iter = servers->begin(); iter != servers->end(); ++iter)
          (*iter)->close();
        break;
      case 2:
        CLOG(INFO, kLogger) << "Closing all sessions";
        // Technically, don't need to do anything because the dtor will do this.
        for (auto iter = servers->begin(); iter != servers->end(); ++iter)
          (*iter)->close_sessions();
        // FALLSTHROUGH
      default:
        ::event_base_loopbreak(ev_base);
        break;
      }
    };
    struct event* ev_sigint = evsignal_new(ev_base, SIGINT, cb, &listeners);
    evsignal_add(ev_sigint, nullptr);

    // Mask off SIGPIPE
    ::signal(SIGPIPE, SIG_IGN);

    // Run the event loop
    CLOG(INFO, kLogger) << "Awaiting incoming connections";
    ::event_base_dispatch(ev_base);
  } else
    CLOG(INFO, kLogger) << "No supported transports found, exiting";

  return 0;
}
